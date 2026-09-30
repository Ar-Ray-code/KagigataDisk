// Lists the objects on a KagigataDisk, appends a line to /log.txt and prints
// /device_info.txt. For an M5Stack Core2 with KagigataDisk in its slot.
#include <M5Unified.h>
#include <emu_storage.h>

EmuStorage storage;

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  delay(500);

  if (!storage.begin()) {
    Serial.printf("KagigataDisk: %s\n", emuResultName(storage.beginResult()));
    return;
  }

  EmuDeviceInfo info;
  if (storage.getInfo(info) == EmuResult::Ok) {
    Serial.printf("%s, firmware %s, %lu bytes free\n", info.deviceName, info.firmwareVersion,
                  (unsigned long)info.availableBytes);
  }

  EmuDirectoryEntry entries[16];
  size_t n = 0;
  if (storage.list(entries, 16, n) == EmuResult::Ok) {
    for (size_t i = 0; i < n; i++) {
      Serial.printf("  %-31s %7lu%s\n", entries[i].name, (unsigned long)entries[i].size,
                    entries[i].readOnly ? "  (read-only)" : "");
    }
  }

  EmuResult r = storage.appendText("/log.txt", "hello from basic.ino\n");
  Serial.printf("append: %s\n", emuResultName(r));

  static char text[600];
  size_t len = 0;
  if (storage.readText("/device_info.txt", text, sizeof(text), len) == EmuResult::Ok) {
    Serial.println(text);
  }
}

void loop() {}
