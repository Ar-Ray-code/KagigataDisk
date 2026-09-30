// Unit tests of the host-side protocol code (frame codec, CRC, length
// checks, sequence handling, resends, error mapping). Run on the PC:
//   pio test -e native
// A simulated device stands behind the EmuTransport interface, so no
// hardware is involved.
#include <string.h>
#include <unity.h>

#include <vector>

#include "../../lib/emu_storage/src/emu_protocol.cpp"

// ---------------------------------------------------------------------------
// Simulated device: answers each request frame after `busyWindows` poll
// windows, optionally misbehaving first.
// ---------------------------------------------------------------------------
struct FakeDevice : EmuTransport {
  // Behaviour knobs.
  int busyWindows = 1;        // poll windows answered with fill only
  int damageResponses = 0;    // next N responses are sent with one bit flipped
  int staleFirst = 0;         // next N windows carry a response to another sequence
  int badFrameReplies = 0;    // next N requests are answered BAD_FRAME
  bool silent = false;        // never answer
  uint8_t status = EMU_ST_OK;
  std::vector<uint8_t> extra;  // payload after the status byte

  // What happened.
  int requests = 0, executed = 0;
  std::vector<uint8_t> lastReq;

  bool sel = false;
  std::vector<uint8_t> in;   // bytes clocked in this window
  std::vector<uint8_t> out;  // response stream for this window
  size_t outPos = 0;
  std::vector<uint8_t> resp;  // current response frame
  int windowsSinceReq = 0;
  uint8_t lastSeq = 0, lastCmd = 0;
  bool haveLast = false;

  EmuTransportResult select() override {
    sel = true;
    in.clear();
    out.clear();
    outPos = 0;
    windowsSinceReq++;
    if (!silent && !resp.empty() && windowsSinceReq > busyWindows) {
      if (staleFirst > 0) {
        staleFirst--;
        std::vector<uint8_t> s = resp;
        s[4] ^= 0x55;  // another sequence number
        uint16_t crc = emu_crc16(s.data(), s.size() - 2);
        emu_put_u16(&s[s.size() - 2], crc);
        out = s;
      } else {
        out = resp;
        if (damageResponses > 0) {
          damageResponses--;
          out[9] ^= 0x10;
        }
      }
    }
    return EmuTransportResult::Ok;
  }

  EmuTransportResult deselect() override {
    sel = false;
    if (!in.empty() && in[0] == EMU_MAGIC0) handleRequest();
    return EmuTransportResult::Ok;
  }

  EmuTransportResult transfer(const uint8_t *tx, uint8_t *rx, size_t n) override {
    for (size_t i = 0; i < n; i++) {
      in.push_back(tx ? tx[i] : EMU_FILL);
      if (rx) rx[i] = outPos < out.size() ? out[outPos++] : EMU_FILL;
    }
    return EmuTransportResult::Ok;
  }

  void handleRequest() {
    requests++;
    lastReq = in;
    windowsSinceReq = 0;
    EmuFrame f;
    std::vector<uint8_t> payload;
    uint8_t cmd = in.size() > 3 ? in[3] : 0, seq = in.size() > 4 ? in[4] : 0;
    if (emuDecodeFrame(in.data(), in.size(), f) != EmuDecode::Ok || badFrameReplies > 0) {
      if (badFrameReplies > 0) badFrameReplies--;
      payload.push_back(EMU_ST_BAD_FRAME);
    } else if (haveLast && f.command == lastCmd && f.sequence == lastSeq) {
      return;  // a resend: keep the old response, as the firmware does
    } else {
      executed++;
      haveLast = true;
      lastCmd = f.command;
      lastSeq = f.sequence;
      payload.push_back(status);
      payload.insert(payload.end(), extra.begin(), extra.end());
    }
    uint8_t buf[EMU_FRAME_MAX];
    size_t len = emuEncodeFrame(buf, (uint8_t)(cmd | EMU_RESPONSE_BIT), seq, 0, payload.data(),
                                payload.size());
    resp.assign(buf, buf + len);
  }
};

static FakeDevice *dev;
static EmuProtocol *proto;

void setUp() {
  dev = new FakeDevice;
  proto = new EmuProtocol;
  proto->attach(dev);
}

void tearDown() {
  delete proto;
  delete dev;
}

static EmuResult ping(uint8_t &st, uint32_t timeout = 200, uint8_t resends = 2) {
  const uint8_t *d;
  size_t dn;
  return proto->exchange(EMU_CMD_PING, nullptr, 0, st, d, dn, timeout, resends);
}

// ---- Codec ------------------------------------------------------------------
static void test_crc_known_vector() {
  // CRC-16/CCITT-FALSE check value.
  TEST_ASSERT_EQUAL_HEX16(0x29B1, emu_crc16((const uint8_t *)"123456789", 9));
}

static void test_encode_decode_roundtrip() {
  uint8_t p[5] = {1, 2, 3, 4, 5}, buf[EMU_FRAME_MAX];
  size_t n = emuEncodeFrame(buf, EMU_CMD_READ, 42, 0, p, sizeof(p));
  TEST_ASSERT_EQUAL(EMU_HEADER_SIZE + 5 + EMU_CRC_SIZE, n);
  TEST_ASSERT_EQUAL_HEX8('K', buf[0]);
  TEST_ASSERT_EQUAL_HEX8('G', buf[1]);
  TEST_ASSERT_EQUAL(EMU_PROTOCOL_VERSION, buf[2]);
  TEST_ASSERT_EQUAL(5, buf[6]);  // little-endian length
  TEST_ASSERT_EQUAL(0, buf[7]);
  EmuFrame f;
  TEST_ASSERT_EQUAL((int)EmuDecode::Ok, (int)emuDecodeFrame(buf, n, f));
  TEST_ASSERT_EQUAL(EMU_CMD_READ, f.command);
  TEST_ASSERT_EQUAL(42, f.sequence);
  TEST_ASSERT_EQUAL(5, f.length);
  TEST_ASSERT_EQUAL_MEMORY(p, f.payload, 5);
}

static void test_encode_rejects_oversize() {
  static uint8_t p[EMU_MAX_PAYLOAD + 1], buf[EMU_FRAME_MAX + 8];
  TEST_ASSERT_EQUAL(0, emuEncodeFrame(buf, EMU_CMD_WRITE, 1, 0, p, sizeof(p)));
  TEST_ASSERT_NOT_EQUAL(0, emuEncodeFrame(buf, EMU_CMD_WRITE, 1, 0, p, EMU_MAX_PAYLOAD));
}

static void test_decode_errors() {
  uint8_t buf[EMU_FRAME_MAX];
  size_t n = emuEncodeFrame(buf, EMU_CMD_PING, 1, 0, nullptr, 0);
  EmuFrame f;
  TEST_ASSERT_EQUAL((int)EmuDecode::Short, (int)emuDecodeFrame(buf, n - 1, f));
  TEST_ASSERT_EQUAL((int)EmuDecode::BadLength, (int)emuDecodeFrame(buf, n + 1, f));
  uint8_t bad[EMU_FRAME_MAX];
  memcpy(bad, buf, n);
  bad[1] = 'X';
  TEST_ASSERT_EQUAL((int)EmuDecode::BadMagic, (int)emuDecodeFrame(bad, n, f));
  memcpy(bad, buf, n);
  bad[4] ^= 1;  // any bit: caught by the CRC
  TEST_ASSERT_EQUAL((int)EmuDecode::BadCrc, (int)emuDecodeFrame(bad, n, f));
  memcpy(bad, buf, n);
  emu_put_u16(&bad[6], EMU_MAX_PAYLOAD + 1);
  TEST_ASSERT_EQUAL((int)EmuDecode::BadLength, (int)emuDecodeFrame(bad, n, f));
}

static void test_little_endian_helpers() {
  uint8_t b[4];
  emu_put_u32(b, 0x12345678u);
  TEST_ASSERT_EQUAL_HEX8(0x78, b[0]);
  TEST_ASSERT_EQUAL_HEX8(0x12, b[3]);
  TEST_ASSERT_EQUAL_HEX32(0x12345678u, emu_get_u32(b));
  emu_put_u16(b, 0xBEEF);
  TEST_ASSERT_EQUAL_HEX16(0xBEEF, emu_get_u16(b));
}

static void test_status_mapping() {
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)emuResultFromStatus(EMU_ST_OK));
  TEST_ASSERT_EQUAL((int)EmuResult::NotFound, (int)emuResultFromStatus(EMU_ST_NOT_FOUND));
  TEST_ASSERT_EQUAL((int)EmuResult::ReadOnly, (int)emuResultFromStatus(EMU_ST_READ_ONLY));
  TEST_ASSERT_EQUAL((int)EmuResult::NoSpace, (int)emuResultFromStatus(EMU_ST_NO_SPACE));
  TEST_ASSERT_EQUAL((int)EmuResult::Unsupported, (int)emuResultFromStatus(EMU_ST_UNSUPPORTED));
  TEST_ASSERT_EQUAL((int)EmuResult::ProtocolError, (int)emuResultFromStatus(200));
}

// ---- Exchange ---------------------------------------------------------------
static void test_exchange_ok() {
  dev->extra = {0xAA, 0xBB};
  const uint8_t *d;
  size_t dn;
  uint8_t st = 0xFF;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok,
                    (int)proto->exchange(EMU_CMD_PING, nullptr, 0, st, d, dn, 200, 0));
  TEST_ASSERT_EQUAL(EMU_ST_OK, st);
  TEST_ASSERT_EQUAL(2, dn);
  TEST_ASSERT_EQUAL_HEX8(0xAA, d[0]);
  TEST_ASSERT_EQUAL(1, dev->requests);
}

static void test_sequence_advances() {
  uint8_t st;
  ping(st);
  uint8_t s1 = dev->lastReq[4];
  ping(st);
  TEST_ASSERT_EQUAL((uint8_t)(s1 + 1), dev->lastReq[4]);
}

static void test_stale_response_ignored() {
  dev->staleFirst = 3;
  uint8_t st;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)ping(st));
  TEST_ASSERT_EQUAL(1, dev->requests);
}

static void test_damaged_response_read_again() {
  dev->damageResponses = 2;
  uint8_t st;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)ping(st));
  TEST_ASSERT_EQUAL(1, dev->requests);  // read again, not resent
  TEST_ASSERT_EQUAL(2, proto->stats().crcErrors);
}

static void test_bad_frame_resent() {
  dev->badFrameReplies = 1;
  uint8_t st;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)ping(st));
  TEST_ASSERT_EQUAL(EMU_ST_OK, st);
  TEST_ASSERT_EQUAL(2, dev->requests);
  TEST_ASSERT_EQUAL(1, dev->executed);
  TEST_ASSERT_EQUAL(1, proto->stats().resends);
}

static void test_timeout_bounded() {
  dev->silent = true;
  uint8_t st;
  uint32_t t0 = nowMs();
  TEST_ASSERT_EQUAL((int)EmuResult::Timeout, (int)ping(st, 50, 2));
  uint32_t dt = nowMs() - t0;
  TEST_ASSERT_EQUAL(3, dev->requests);  // first try + 2 resends
  TEST_ASSERT_TRUE(dt >= 150 && dt < 400);
  TEST_ASSERT_EQUAL(3, proto->stats().timeouts);
}

static void test_resend_not_executed_twice() {
  // The first answer never gets through, so the host sends the request again;
  // the (simulated) device recognises it and does not carry it out again.
  dev->busyWindows = 1000;
  uint8_t st;
  TEST_ASSERT_EQUAL((int)EmuResult::Timeout, (int)ping(st, 30, 0));
  dev->busyWindows = 1;
  uint8_t seq = dev->lastReq[4];
  // Same frame again, by hand: as the protocol would resend it.
  std::vector<uint8_t> frame = dev->lastReq;
  dev->select();
  dev->transfer(frame.data(), nullptr, frame.size());
  dev->deselect();
  TEST_ASSERT_EQUAL(2, dev->requests);
  TEST_ASSERT_EQUAL(1, dev->executed);
  TEST_ASSERT_EQUAL(seq, dev->resp[4]);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_crc_known_vector);
  RUN_TEST(test_encode_decode_roundtrip);
  RUN_TEST(test_encode_rejects_oversize);
  RUN_TEST(test_decode_errors);
  RUN_TEST(test_little_endian_helpers);
  RUN_TEST(test_status_mapping);
  RUN_TEST(test_exchange_ok);
  RUN_TEST(test_sequence_advances);
  RUN_TEST(test_stale_response_ignored);
  RUN_TEST(test_damaged_response_read_again);
  RUN_TEST(test_bad_frame_resent);
  RUN_TEST(test_timeout_bounded);
  RUN_TEST(test_resend_not_executed_twice);
  return UNITY_END();
}
