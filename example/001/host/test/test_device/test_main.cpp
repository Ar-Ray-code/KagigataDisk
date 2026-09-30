// Integration and performance tests against a real KagigataDisk in the Core2's
// slot:
//   pio test -e core2 -v        (-v shows the performance figures)
// The tests create and remove objects named t_*.bin / perf.bin; everything
// else on the device is only read.
#include <M5Unified.h>
#include <emu_storage.h>
#include <unity.h>

#include "emu_protocol.h"
#include "emu_transport_spi.h"
#include "esp32-hal-spi.h"

static EmuStorage storage;
static EmuSpiTransport raw;  // same bus and CS, for hand-made frames

static constexpr uint32_t OBJ_MAX = 1440u * 1024u;  // the device's object size limit

void setUp() {}
void tearDown() {}

// ---- Hand-made frames -----------------------------------------------------
static void rawSend(const uint8_t *f, size_t n) {
  raw.select();
  raw.write(f, n);
  raw.deselect();
}

// Polls for the response to `seq` (any command); false on timeout.
static bool rawAwait(uint8_t seq, uint8_t *resp, size_t &len, uint32_t timeoutMs = 1000) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    uint8_t w[16];
    raw.select();
    raw.read(w, sizeof(w));
    size_t k = 0;
    while (k < sizeof(w) && w[k] != EMU_MAGIC0) k++;
    if (k < sizeof(w)) {
      size_t have = sizeof(w) - k;
      memcpy(resp, &w[k], have);
      if (have < EMU_HEADER_SIZE) {
        raw.read(resp + have, EMU_HEADER_SIZE - have);
        have = EMU_HEADER_SIZE;
      }
      uint16_t n = emu_get_u16(&resp[6]);
      if (n <= EMU_MAX_PAYLOAD) {
        size_t total = EMU_HEADER_SIZE + n + EMU_CRC_SIZE;
        if (have < total) raw.read(resp + have, total - have);
        raw.deselect();
        EmuFrame f;
        if (emuDecodeFrame(resp, total, f) == EmuDecode::Ok && f.sequence == seq) {
          len = total;
          return true;
        }
        continue;
      }
    }
    raw.deselect();
    delay(1);
  }
  return false;
}

static uint8_t rawStatus(const uint8_t *resp) { return resp[EMU_HEADER_SIZE]; }

// ---- Connection -----------------------------------------------------------
static void test_begin_ok() {
  uint32_t t0 = millis();
  TEST_ASSERT_TRUE(storage.begin());
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.beginResult());
  TEST_ASSERT_TRUE(millis() - t0 < 1000);
  TEST_ASSERT_TRUE(storage.isConnected());
}

static void test_absent_device_times_out() {
  EmuStorage other;
  EmuStorageConfig cfg;
  cfg.sckPin = -1;  // the bus is already up
  cfg.csPin = 26;   // nothing selected by this pin
  uint32_t t0 = millis();
  TEST_ASSERT_FALSE(other.begin(cfg));
  uint32_t dt = millis() - t0;
  TEST_ASSERT_EQUAL((int)EmuResult::Timeout, (int)other.beginResult());
  TEST_ASSERT_TRUE_MESSAGE(dt < 1500, "gave up in time");
  other.end();
  TEST_ASSERT_EQUAL((int)EmuResult::NotInitialized, (int)other.remove("/x"));
}

static void test_incompatible_version() {
  uint8_t p[2], f[EMU_FRAME_MAX], r[EMU_FRAME_MAX];
  emu_put_u16(p, 2);
  size_t n = emuEncodeFrame(f, EMU_CMD_HELLO, 0x31, 0, p, 2);
  f[2] = 2;  // a protocol version the device does not speak
  emu_put_u16(&f[n - 2], emu_crc16(f, n - 2));
  rawSend(f, n);
  size_t rn;
  TEST_ASSERT_TRUE(rawAwait(0x31, r, rn));
  TEST_ASSERT_EQUAL(EMU_ST_UNSUPPORTED, rawStatus(r));
  TEST_ASSERT_EQUAL(EMU_PROTOCOL_VERSION, emu_get_u16(&r[EMU_HEADER_SIZE + 1]));
}

// ---- Info and list --------------------------------------------------------
static void test_get_info() {
  EmuDeviceInfo info;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.getInfo(info));
  TEST_ASSERT_EQUAL(1, info.protocolVersion);
  TEST_ASSERT_TRUE(info.capabilities & EMU_CAP_READ_BIT);
  TEST_ASSERT_TRUE(info.capabilities & EMU_CAP_WRITE_BIT);
  TEST_ASSERT_EQUAL_STRING("RP2350-KagigataDisk", info.deviceName);
  TEST_ASSERT_EQUAL_UINT32(OBJ_MAX, info.maxObjectSize);
  TEST_ASSERT_TRUE(info.availableBytes > 0);
  TEST_ASSERT_TRUE(info.maxChunk > 0);
  Serial.printf("device %s fw %s, %lu bytes free, chunk %u\n", info.deviceName,
                info.firmwareVersion, (unsigned long)info.availableBytes, info.maxChunk);
}

static const EmuDirectoryEntry *findEntry(const EmuDirectoryEntry *e, size_t n, const char *name) {
  for (size_t i = 0; i < n; i++) {
    if (strcasecmp(e[i].name, name) == 0) return &e[i];
  }
  return nullptr;
}

static void test_list() {
  static EmuDirectoryEntry e[80];
  size_t n = 0;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.list(e, 80, n));
  TEST_ASSERT_TRUE(n >= 3);
  const EmuDirectoryEntry *readme = findEntry(e, n, "README.md");
  const EmuDirectoryEntry *info = findEntry(e, n, "device_info.txt");
  TEST_ASSERT_NOT_NULL(readme);
  TEST_ASSERT_NOT_NULL(info);
  TEST_ASSERT_TRUE(readme->readOnly);
  TEST_ASSERT_TRUE(info->readOnly);
  TEST_ASSERT_FALSE(info->isDirectory);
  // Small capacity: a shorter list, no overrun.
  EmuDirectoryEntry two[2];
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.list(two, 2, n));
  TEST_ASSERT_EQUAL(2, n);
}

// ---- Read -----------------------------------------------------------------
static void test_read_zero_bytes() {
  uint8_t b[4];
  size_t got = 99;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.read("/device_info.txt", 0, b, 0, got));
  TEST_ASSERT_EQUAL(0, got);
}

static void test_read_small() {
  char b[16];
  size_t got = 0;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.read("/device_info.txt", 0, b, 12, got));
  TEST_ASSERT_EQUAL(12, got);
  TEST_ASSERT_EQUAL_MEMORY("kagigatadisk", b, 12);
}

static void test_read_multi_packet_and_eof() {
  EmuFileInfo fi;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.stat("/README.md", fi));
  TEST_ASSERT_TRUE(fi.size > 600);  // several chunks
  static uint8_t whole[8192], parts[8192];
  size_t got = 0;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.read("/README.md", 0, whole, sizeof(whole), got));
  TEST_ASSERT_EQUAL(fi.size, got);  // short read at EOF
  // The same bytes read in odd-sized pieces.
  size_t pos = 0;
  while (pos < fi.size) {
    size_t n = 0;
    TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.read("README.md", pos, parts + pos, 77, n));
    TEST_ASSERT_TRUE(n > 0);
    pos += n;
  }
  TEST_ASSERT_EQUAL_MEMORY(whole, parts, fi.size);
  size_t n = 5;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.read("/README.md", fi.size, parts, 10, n));
  TEST_ASSERT_EQUAL(0, n);
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.read("/README.md", fi.size + 1000, parts, 10, n));
  TEST_ASSERT_EQUAL(0, n);
}

static void test_read_missing() {
  uint8_t b[4];
  size_t got;
  TEST_ASSERT_EQUAL((int)EmuResult::NotFound, (int)storage.read("/no_such_file.txt", 0, b, 4, got));
  EmuFileInfo fi;
  TEST_ASSERT_EQUAL((int)EmuResult::NotFound, (int)storage.stat("/no_such_file.txt", fi));
}

static void test_bad_paths() {
  uint8_t b[4];
  size_t got;
  TEST_ASSERT_EQUAL((int)EmuResult::InvalidArgument, (int)storage.read(nullptr, 0, b, 4, got));
  TEST_ASSERT_EQUAL((int)EmuResult::InvalidArgument, (int)storage.read("/", 0, b, 4, got));
  TEST_ASSERT_EQUAL((int)EmuResult::InvalidArgument, (int)storage.read("/a/b.txt", 0, b, 4, got));
  TEST_ASSERT_EQUAL((int)EmuResult::InvalidArgument,
                    (int)storage.read("/0123456789012345678901234567890123.txt", 0, b, 4, got));
  TEST_ASSERT_EQUAL((int)EmuResult::InvalidArgument, (int)storage.appendText("/bad*name.txt", "x"));
}

// ---- Write ----------------------------------------------------------------
static void fillPattern(uint8_t *b, size_t n, uint32_t seed) {
  for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(i * 31u + seed);
}

static void test_write_new_object() {
  storage.remove("/t_new.bin");
  static uint8_t w[1000], r[1000];
  fillPattern(w, sizeof(w), 7);
  size_t n = 0;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.write("/t_new.bin", 0, w, sizeof(w), n));
  TEST_ASSERT_EQUAL(sizeof(w), n);
  EmuFileInfo fi;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.stat("/t_new.bin", fi));
  TEST_ASSERT_EQUAL(1000, fi.size);
  TEST_ASSERT_FALSE(fi.readOnly);
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.read("/t_new.bin", 0, r, sizeof(r), n));
  TEST_ASSERT_EQUAL(1000, n);
  TEST_ASSERT_EQUAL_MEMORY(w, r, 1000);
}

static void test_overwrite() {
  uint8_t w[50], r[1000];
  memset(w, 0x5A, sizeof(w));
  size_t n;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.write("/t_new.bin", 490, w, sizeof(w), n));
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.read("/t_new.bin", 0, r, sizeof(r), n));
  TEST_ASSERT_EQUAL(1000, n);  // size unchanged
  static uint8_t expect[1000];
  fillPattern(expect, 1000, 7);
  memset(expect + 490, 0x5A, 50);  // across a page boundary on the device
  TEST_ASSERT_EQUAL_MEMORY(expect, r, 1000);
}

static void test_append() {
  storage.remove("/t_app.bin");
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.appendText("/t_app.bin", "one\n"));
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.appendText("/t_app.bin", "two\n"));
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.appendText("/t_app.bin", "three\n"));
  char b[64];
  size_t n;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.readText("/t_app.bin", b, sizeof(b), n));
  TEST_ASSERT_EQUAL_STRING("one\ntwo\nthree\n", b);
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.writeText("/t_app.bin", "new"));
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.readText("/t_app.bin", b, sizeof(b), n));
  TEST_ASSERT_EQUAL_STRING("new", b);
}

static void test_sparse_and_truncate_read_zero() {
  storage.remove("/t_gap.bin");
  uint8_t w[600], r[1600];
  memset(w, 0xAA, sizeof(w));
  size_t n;
  // Write past the end: the gap reads as zeros.
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.write("/t_gap.bin", 1000, w, 600, n));
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.read("/t_gap.bin", 0, r, 1600, n));
  TEST_ASSERT_EQUAL(1600, n);
  for (int i = 0; i < 1000; i++) TEST_ASSERT_EQUAL_HEX8(0, r[i]);
  // Shrink, then grow back: what was cut off does not come back.
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.truncate("/t_gap.bin", 1100));
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.truncate("/t_gap.bin", 1600));
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.read("/t_gap.bin", 0, r, 1600, n));
  for (int i = 1000; i < 1100; i++) TEST_ASSERT_EQUAL_HEX8(0xAA, r[i]);
  for (int i = 1100; i < 1600; i++) TEST_ASSERT_EQUAL_HEX8(0, r[i]);
}

static void test_no_space() {
  uint8_t b[16] = {0};
  size_t n;
  TEST_ASSERT_EQUAL((int)EmuResult::NoSpace, (int)storage.write("/t_new.bin", OBJ_MAX, b, 16, n));
  TEST_ASSERT_EQUAL((int)EmuResult::NoSpace, (int)storage.truncate("/t_new.bin", 4u * 1024u * 1024u));
}

static void test_read_only() {
  size_t n;
  TEST_ASSERT_EQUAL((int)EmuResult::ReadOnly, (int)storage.write("/README.md", 0, "x", 1, n));
  TEST_ASSERT_EQUAL((int)EmuResult::ReadOnly, (int)storage.appendText("/device_info.txt", "x"));
  TEST_ASSERT_EQUAL((int)EmuResult::ReadOnly, (int)storage.remove("/README.md"));
  TEST_ASSERT_EQUAL((int)EmuResult::ReadOnly, (int)storage.truncate("/device_info.txt", 0));
}

// ---- Remove ---------------------------------------------------------------
static void test_remove() {
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.remove("/t_new.bin"));
  EmuFileInfo fi;
  TEST_ASSERT_EQUAL((int)EmuResult::NotFound, (int)storage.stat("/t_new.bin", fi));
  TEST_ASSERT_EQUAL((int)EmuResult::NotFound, (int)storage.remove("/t_new.bin"));
  storage.remove("/t_app.bin");
  storage.remove("/t_gap.bin");
}

static void test_sync() { TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.sync()); }

// ---- Recovery -------------------------------------------------------------
static void test_cs_interrupted_request() {
  uint8_t p[40], f[EMU_FRAME_MAX], r[EMU_FRAME_MAX];
  memset(p, 0, sizeof(p));
  size_t n = emuEncodeFrame(f, EMU_CMD_PING, 0x41, 0, p, sizeof(p));
  rawSend(f, n / 2);  // deselected halfway through
  size_t rn;
  TEST_ASSERT_TRUE(rawAwait(0x41, r, rn));
  TEST_ASSERT_EQUAL(EMU_ST_BAD_FRAME, rawStatus(r));
  TEST_ASSERT_TRUE(storage.isConnected());
}

static void test_malformed_length() {
  uint8_t f[EMU_FRAME_MAX], r[EMU_FRAME_MAX];
  size_t n = emuEncodeFrame(f, EMU_CMD_PING, 0x42, 0, nullptr, 0);
  emu_put_u16(&f[6], 0xFFFF);  // claims far more than a frame can hold
  rawSend(f, n);
  size_t rn;
  TEST_ASSERT_TRUE(rawAwait(0x42, r, rn));
  TEST_ASSERT_EQUAL(EMU_ST_BAD_FRAME, rawStatus(r));
  TEST_ASSERT_TRUE(storage.isConnected());
}

static void test_bad_crc() {
  uint8_t f[EMU_FRAME_MAX], r[EMU_FRAME_MAX];
  size_t n = emuEncodeFrame(f, EMU_CMD_PING, 0x43, 0, nullptr, 0);
  f[n - 1] ^= 0x01;
  rawSend(f, n);
  size_t rn;
  TEST_ASSERT_TRUE(rawAwait(0x43, r, rn));
  TEST_ASSERT_EQUAL(EMU_ST_BAD_FRAME, rawStatus(r));
  TEST_ASSERT_TRUE(storage.isConnected());
}

static void test_resent_append_runs_once() {
  storage.remove("/t_dup.bin");
  uint8_t p[40], f[EMU_FRAME_MAX], r[EMU_FRAME_MAX];
  const char *name = "t_dup.bin";
  size_t pn = 0;
  p[pn++] = (uint8_t)strlen(name);
  memcpy(p + pn, name, strlen(name));
  pn += strlen(name);
  memcpy(p + pn, "abc", 3);
  pn += 3;
  size_t n = emuEncodeFrame(f, EMU_CMD_APPEND, 0x44, 0, p, pn);
  size_t rn;
  rawSend(f, n);
  TEST_ASSERT_TRUE(rawAwait(0x44, r, rn));
  TEST_ASSERT_EQUAL(EMU_ST_OK, rawStatus(r));
  TEST_ASSERT_FALSE(r[5] & EMU_FLAG_REPLAY);
  rawSend(f, n);  // the same request again, as after a lost answer
  TEST_ASSERT_TRUE(rawAwait(0x44, r, rn));
  TEST_ASSERT_EQUAL(EMU_ST_OK, rawStatus(r));
  TEST_ASSERT_TRUE(r[5] & EMU_FLAG_REPLAY);
  EmuFileInfo fi;
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.stat("/t_dup.bin", fi));
  TEST_ASSERT_EQUAL(3, fi.size);
  storage.remove("/t_dup.bin");
}

static void test_reconnect_after_link_failure() {
  storage.setFrequency(40000000u);  // far too fast: answers arrive garbled or not at all
  EmuFileInfo fi;
  EmuResult r = storage.stat("/README.md", fi);
  storage.setFrequency(8000000u);
  if (r == EmuResult::Ok) TEST_IGNORE_MESSAGE("the link survived 40 MHz; nothing to recover from");
  TEST_ASSERT_TRUE(r == EmuResult::Timeout || r == EmuResult::CrcError);
  TEST_ASSERT_EQUAL((int)EmuResult::NotConnected, (int)storage.stat("/README.md", fi));
  TEST_ASSERT_TRUE(storage.begin());
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.stat("/README.md", fi));
}

// ---- Performance ------------------------------------------------------------
static void bench(uint32_t hz) {
  storage.setFrequency(hz);
  storage.clearLinkStats();
  EmuResult r = EmuResult::Ok;
  // Latency: a round trip that does nothing.
  uint32_t t0 = micros();
  const int pings = 50;
  int pingOk = 0;
  for (int i = 0; i < pings; i++) pingOk += storage.isConnected();
  uint32_t pingUs = (micros() - t0) / pings;
  // Throughput: 16 KiB written and read back.
  static uint8_t w[16384], rd[16384];
  fillPattern(w, sizeof(w), hz / 1000000u);
  storage.remove("/perf.bin");
  size_t n = 0;
  t0 = micros();
  r = storage.write("/perf.bin", 0, w, sizeof(w), n);
  uint32_t wUs = micros() - t0;
  bool wOk = r == EmuResult::Ok && n == sizeof(w);
  t0 = micros();
  r = storage.read("/perf.bin", 0, rd, sizeof(rd), n);
  uint32_t rUs = micros() - t0;
  bool rOk = r == EmuResult::Ok && n == sizeof(rd) && memcmp(w, rd, sizeof(w)) == 0;
  EmuLinkStats s = storage.linkStats();
  double actual = spiClockDivToFrequency(spiFrequencyToClockDiv(hz)) / 1e6;
  Serial.printf(
      "PERF %2lu MHz (actual %.2f): ping %4lu us (%d/%d ok) | write %5.1f KiB/s %s | read %5.1f KiB/s %s | "
      "resends %lu crc %lu timeouts %lu\n",
      (unsigned long)(hz / 1000000u), actual, (unsigned long)pingUs, pingOk, pings,
      16.0 * 1e6 / (wUs ? wUs : 1), wOk ? "ok" : "FAIL",
      16.0 * 1e6 / (rUs ? rUs : 1), rOk ? "ok" : "FAIL", (unsigned long)s.resends,
      (unsigned long)s.crcErrors, (unsigned long)s.timeouts);
  storage.setFrequency(8000000u);
  if (!storage.isConnected()) storage.begin();
  storage.remove("/perf.bin");
  char msg[32];
  snprintf(msg, sizeof(msg), "%lu MHz", (unsigned long)(hz / 1000000u));
  TEST_ASSERT_TRUE_MESSAGE(pingOk == pings && wOk && rOk, msg);
}

static void test_perf_4mhz() { bench(4000000u); }
static void test_perf_8mhz() { bench(8000000u); }
static void test_perf_12mhz() { bench(12000000u); }
// The ESP32 cannot make 15 MHz from its 80 MHz source: this asks for the
// fastest clock it can make below 16 MHz (80 / 6).
static void test_perf_15mhz() { bench(13333333u); }

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);  // the LCD shares the bus: have it initialised and idle
  delay(1500);
  UNITY_BEGIN();
  RUN_TEST(test_begin_ok);
  raw.begin(SPI, -1, -1, -1, 4, 8000000u);
  RUN_TEST(test_absent_device_times_out);
  RUN_TEST(test_incompatible_version);
  RUN_TEST(test_get_info);
  RUN_TEST(test_list);
  RUN_TEST(test_read_zero_bytes);
  RUN_TEST(test_read_small);
  RUN_TEST(test_read_multi_packet_and_eof);
  RUN_TEST(test_read_missing);
  RUN_TEST(test_bad_paths);
  RUN_TEST(test_write_new_object);
  RUN_TEST(test_overwrite);
  RUN_TEST(test_append);
  RUN_TEST(test_sparse_and_truncate_read_zero);
  RUN_TEST(test_no_space);
  RUN_TEST(test_read_only);
  RUN_TEST(test_remove);
  RUN_TEST(test_sync);
  RUN_TEST(test_cs_interrupted_request);
  RUN_TEST(test_malformed_length);
  RUN_TEST(test_bad_crc);
  RUN_TEST(test_resent_append_runs_once);
  RUN_TEST(test_reconnect_after_link_failure);
  RUN_TEST(test_perf_4mhz);
  RUN_TEST(test_perf_8mhz);
  RUN_TEST(test_perf_12mhz);
  RUN_TEST(test_perf_15mhz);
  UNITY_END();
}

void loop() {}
