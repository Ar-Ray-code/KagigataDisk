#include "emu_storage.h"

#include <string.h>

#include "emu_protocol.h"
#include "emu_transport_spi.h"

static_assert(EMU_STORAGE_NAME_MAX == EMU_NAME_MAX, "name limits must agree with the protocol");

struct EmuStorage::Impl {
  EmuSpiTransport transport;
  EmuProtocol protocol;
};

const char *emuResultName(EmuResult r) {
  switch (r) {
    case EmuResult::Ok: return "ok";
    case EmuResult::NotInitialized: return "not initialized";
    case EmuResult::NotConnected: return "not connected";
    case EmuResult::Timeout: return "timeout";
    case EmuResult::ProtocolError: return "protocol error";
    case EmuResult::CrcError: return "crc error";
    case EmuResult::InvalidArgument: return "invalid argument";
    case EmuResult::NotFound: return "not found";
    case EmuResult::AlreadyExists: return "already exists";
    case EmuResult::ReadOnly: return "read only";
    case EmuResult::NoSpace: return "no space";
    case EmuResult::Unsupported: return "unsupported";
    case EmuResult::IoError: return "i/o error";
  }
  return "?";
}

EmuStorage::EmuStorage() : impl_(new Impl) { impl_->protocol.attach(&impl_->transport); }
EmuStorage::~EmuStorage() {
  end();
  delete impl_;
}

// ---- Encoding helpers -----------------------------------------------------
namespace {

// A request payload under construction.
struct Payload {
  uint8_t b[EMU_MAX_PAYLOAD];
  size_t n = 0;
  bool ok = true;
  void u8(uint32_t v) {
    if (n + 1 > sizeof(b)) return (void)(ok = false);
    b[n++] = (uint8_t)v;
  }
  void u16(uint32_t v) {
    u8(v);
    u8(v >> 8);
  }
  void u32(uint32_t v) {
    u16(v);
    u16(v >> 16);
  }
  void bytes(const void *p, size_t len) {
    if (n + len > sizeof(b)) return (void)(ok = false);
    memcpy(&b[n], p, len);
    n += len;
  }
};

// "/name" or "name" -> the name; false if it cannot be one.
bool checkPath(const char *path, const char *&name, size_t &len) {
  if (!path) return false;
  if (path[0] == '/') path++;
  len = strnlen(path, EMU_NAME_MAX + 1);
  if (len == 0 || len > EMU_NAME_MAX || memchr(path, '/', len)) return false;
  name = path;
  return true;
}

bool putPath(Payload &p, const char *path) {
  const char *name;
  size_t len;
  if (!checkPath(path, name, len)) return false;
  p.u8(len);
  p.bytes(name, len);
  return p.ok;
}

void copyName(char *dst, size_t cap, const uint8_t *src, size_t len) {
  if (len >= cap) len = cap - 1;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

}  // namespace

// ---- Connection -----------------------------------------------------------
bool EmuStorage::begin() { return begin(EmuStorageConfig{}); }

bool EmuStorage::begin(const EmuStorageConfig &config) {
  if (!started_) {
    if (!impl_->transport.begin(*config.spi, config.sckPin, config.misoPin, config.mosiPin,
                                config.csPin, config.frequency)) {
      beginResult_ = EmuResult::InvalidArgument;
      return false;
    }
    impl_->protocol.reseed((uint8_t)esp_random());
    started_ = true;
  }
  connected_ = false;
  uint8_t req[2];
  emu_put_u16(req, EMU_PROTOCOL_VERSION);
  uint8_t st = 0;
  const uint8_t *d;
  size_t dn;
  EmuResult r = impl_->protocol.exchange(EMU_CMD_HELLO, req, sizeof(req), st, d, dn,
                                         EMU_HELLO_TIMEOUT_MS, 1);
  if (r == EmuResult::Ok && st != EMU_ST_OK) r = emuResultFromStatus(st);
  if (r == EmuResult::Ok && dn < 8) r = EmuResult::ProtocolError;
  if (r == EmuResult::Ok && emu_get_u16(d) != EMU_PROTOCOL_VERSION) r = EmuResult::Unsupported;
  if (r != EmuResult::Ok) {
    beginResult_ = r;
    return false;
  }
  chunk_ = emu_get_u16(d + 6);
  if (chunk_ == 0 || chunk_ > EMU_MAX_DATA) chunk_ = EMU_MAX_DATA;
  connected_ = true;
  // The rest of what the device says about itself.
  EmuDeviceInfo info;
  r = getInfo(info);
  beginResult_ = r;
  connected_ = r == EmuResult::Ok;
  return connected_;
}

void EmuStorage::end() {
  if (started_) impl_->transport.end();
  started_ = connected_ = false;
  beginResult_ = EmuResult::NotInitialized;
}

EmuResult EmuStorage::call(uint8_t command, const uint8_t *payload, size_t length,
                           const uint8_t *&data, size_t &dataLen, uint32_t timeoutMs) {
  if (!started_) return EmuResult::NotInitialized;
  if (!connected_) return EmuResult::NotConnected;
  uint8_t st = 0;
  EmuResult r = impl_->protocol.exchange(command, payload, length, st, data, dataLen, timeoutMs);
  if (r == EmuResult::Timeout || r == EmuResult::CrcError) {
    connected_ = false;  // begin() again to reconnect
    return r;
  }
  if (r != EmuResult::Ok) return r;
  return emuResultFromStatus(st);
}

bool EmuStorage::isConnected() {
  if (!started_ || !connected_) return false;
  const uint8_t *d;
  size_t dn;
  return call(EMU_CMD_PING, nullptr, 0, d, dn, EMU_DEFAULT_TIMEOUT_MS) == EmuResult::Ok;
}

EmuResult EmuStorage::getInfo(EmuDeviceInfo &info) {
  const uint8_t *d;
  size_t dn;
  EmuResult r = call(EMU_CMD_GET_INFO, nullptr, 0, d, dn, EMU_DEFAULT_TIMEOUT_MS);
  if (r != EmuResult::Ok) return r;
  if (dn < 16) return EmuResult::ProtocolError;
  memset(&info, 0, sizeof(info));
  info.protocolVersion = emu_get_u16(d);
  info.capabilities = emu_get_u32(d + 2);
  info.maxObjectSize = emu_get_u32(d + 6);
  info.availableBytes = emu_get_u32(d + 10);
  info.maxChunk = chunk_;
  size_t pos = 14;
  size_t n = d[pos++];
  if (pos + n > dn) return EmuResult::ProtocolError;
  copyName(info.deviceName, sizeof(info.deviceName), d + pos, n);
  pos += n;
  if (pos >= dn) return EmuResult::ProtocolError;
  n = d[pos++];
  if (pos + n > dn) return EmuResult::ProtocolError;
  copyName(info.firmwareVersion, sizeof(info.firmwareVersion), d + pos, n);
  return EmuResult::Ok;
}

// ---- Objects --------------------------------------------------------------
EmuResult EmuStorage::list(EmuDirectoryEntry *entries, size_t capacity, size_t &count) {
  count = 0;
  if (!entries && capacity) return EmuResult::InvalidArgument;
  uint32_t total = 0;
  do {
    Payload p;
    p.u16(count);
    const uint8_t *d;
    size_t dn;
    EmuResult r = call(EMU_CMD_LIST, p.b, p.n, d, dn, EMU_DEFAULT_TIMEOUT_MS);
    if (r != EmuResult::Ok) return r;
    if (dn < 3) return EmuResult::ProtocolError;
    total = emu_get_u16(d);
    size_t got = d[2], pos = 3;
    if (got == 0) break;  // the list changed under us: what we have will do
    for (size_t i = 0; i < got && count < capacity; i++) {
      if (pos + 6 > dn) return EmuResult::ProtocolError;
      EmuDirectoryEntry &e = entries[count++];
      e.size = emu_get_u32(d + pos);
      uint8_t attr = d[pos + 4];
      size_t n = d[pos + 5];
      pos += 6;
      if (pos + n > dn) return EmuResult::ProtocolError;
      copyName(e.name, sizeof(e.name), d + pos, n);
      pos += n;
      e.readOnly = (attr & EMU_ATTR_READ_ONLY) != 0;
      e.isDirectory = (attr & EMU_ATTR_DIRECTORY) != 0;
    }
  } while (count < capacity && count < total);
  return EmuResult::Ok;
}

EmuResult EmuStorage::stat(const char *path, EmuFileInfo &info) {
  Payload p;
  if (!putPath(p, path)) return EmuResult::InvalidArgument;
  const uint8_t *d;
  size_t dn;
  EmuResult r = call(EMU_CMD_STAT, p.b, p.n, d, dn, EMU_DEFAULT_TIMEOUT_MS);
  if (r != EmuResult::Ok) return r;
  if (dn < 5) return EmuResult::ProtocolError;
  info.size = emu_get_u32(d);
  info.readOnly = (d[4] & EMU_ATTR_READ_ONLY) != 0;
  info.isDirectory = (d[4] & EMU_ATTR_DIRECTORY) != 0;
  return EmuResult::Ok;
}

EmuResult EmuStorage::read(const char *path, uint32_t offset, void *buffer, size_t length,
                           size_t &bytesRead) {
  bytesRead = 0;
  if (!buffer && length) return EmuResult::InvalidArgument;
  const char *name;
  size_t nlen;
  if (!checkPath(path, name, nlen)) return EmuResult::InvalidArgument;
  uint8_t *out = (uint8_t *)buffer;
  do {
    size_t want = length - bytesRead < chunk_ ? length - bytesRead : chunk_;
    Payload p;
    p.u32(offset + bytesRead);
    p.u16(want);
    putPath(p, path);
    const uint8_t *d;
    size_t dn;
    EmuResult r = call(EMU_CMD_READ, p.b, p.n, d, dn, EMU_DEFAULT_TIMEOUT_MS);
    if (r != EmuResult::Ok) return r;
    if (dn > want) return EmuResult::ProtocolError;
    memcpy(out + bytesRead, d, dn);
    bytesRead += dn;
    if (dn < want) break;  // the end of the object
  } while (bytesRead < length);
  return EmuResult::Ok;
}

EmuResult EmuStorage::write(const char *path, uint32_t offset, const void *data, size_t length,
                            size_t &bytesWritten) {
  bytesWritten = 0;
  if (!data && length) return EmuResult::InvalidArgument;
  const char *name;
  size_t nlen;
  if (!checkPath(path, name, nlen)) return EmuResult::InvalidArgument;
  const uint8_t *in = (const uint8_t *)data;
  do {
    size_t n = length - bytesWritten < chunk_ ? length - bytesWritten : chunk_;
    Payload p;
    p.u32(offset + bytesWritten);
    p.u8(EMU_WRITE_CREATE);
    putPath(p, path);
    p.bytes(in + bytesWritten, n);
    if (!p.ok) return EmuResult::InvalidArgument;
    const uint8_t *d;
    size_t dn;
    EmuResult r = call(EMU_CMD_WRITE, p.b, p.n, d, dn, EMU_LONG_TIMEOUT_MS);
    if (r != EmuResult::Ok) return r;
    bytesWritten += n;
  } while (bytesWritten < length);
  return EmuResult::Ok;
}

EmuResult EmuStorage::append(const char *path, const void *data, size_t length,
                             size_t &bytesWritten) {
  bytesWritten = 0;
  if (!data && length) return EmuResult::InvalidArgument;
  const char *name;
  size_t nlen;
  if (!checkPath(path, name, nlen)) return EmuResult::InvalidArgument;
  const uint8_t *in = (const uint8_t *)data;
  do {
    size_t n = length - bytesWritten < chunk_ ? length - bytesWritten : chunk_;
    Payload p;
    putPath(p, path);
    p.bytes(in + bytesWritten, n);
    if (!p.ok) return EmuResult::InvalidArgument;
    const uint8_t *d;
    size_t dn;
    EmuResult r = call(EMU_CMD_APPEND, p.b, p.n, d, dn, EMU_LONG_TIMEOUT_MS);
    if (r != EmuResult::Ok) return r;
    bytesWritten += n;
  } while (bytesWritten < length);
  return EmuResult::Ok;
}

EmuResult EmuStorage::truncate(const char *path, uint32_t size) {
  Payload p;
  p.u32(size);
  if (!putPath(p, path)) return EmuResult::InvalidArgument;
  const uint8_t *d;
  size_t dn;
  return call(EMU_CMD_TRUNCATE, p.b, p.n, d, dn, EMU_LONG_TIMEOUT_MS);
}

EmuResult EmuStorage::remove(const char *path) {
  Payload p;
  if (!putPath(p, path)) return EmuResult::InvalidArgument;
  const uint8_t *d;
  size_t dn;
  return call(EMU_CMD_REMOVE, p.b, p.n, d, dn, EMU_DEFAULT_TIMEOUT_MS);
}

EmuResult EmuStorage::sync() {
  const uint8_t *d;
  size_t dn;
  return call(EMU_CMD_SYNC, nullptr, 0, d, dn, EMU_LONG_TIMEOUT_MS);
}

// ---- Text -----------------------------------------------------------------
EmuResult EmuStorage::readText(const char *path, char *buffer, size_t capacity, size_t &length) {
  length = 0;
  if (!buffer || capacity == 0) return EmuResult::InvalidArgument;
  EmuResult r = read(path, 0, buffer, capacity - 1, length);
  buffer[length] = '\0';
  return r;
}

EmuResult EmuStorage::writeText(const char *path, const char *text) {
  if (!text) return EmuResult::InvalidArgument;
  size_t n = strlen(text), w = 0;
  EmuResult r = write(path, 0, text, n, w);
  if (r != EmuResult::Ok) return r;
  return truncate(path, n);
}

EmuResult EmuStorage::appendText(const char *path, const char *text) {
  if (!text) return EmuResult::InvalidArgument;
  size_t w = 0;
  return append(path, text, strlen(text), w);
}

// ---- Diagnostics ----------------------------------------------------------
void EmuStorage::setFrequency(uint32_t hz) { impl_->transport.setFrequency(hz); }
EmuLinkStats EmuStorage::linkStats() const { return impl_->protocol.stats(); }
void EmuStorage::clearLinkStats() { impl_->protocol.clearStats(); }
