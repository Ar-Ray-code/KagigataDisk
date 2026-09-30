// Manual test of the USB drive <-> SPI host bridge. Needs someone (or a
// script) at the PC the KagigataDisk is plugged into:
//   pio test -e core2 -f test_usb_bridge -v
//   1. When "STEP A done" is printed: remount the drive and check that
//      spi_virtual_device/spi_note.txt reads "from spi" and log.txt ends with
//      "[bridge]". Then save spi_virtual_device/pc_note.txt ("from pc"),
//      overwrite spi_note.txt with "edited by pc", and sync.
//   2. When "STEP B done" is printed: delete both files there and sync.
// Each wait gives up after 90 s.
#include <M5Unified.h>
#include <emu_storage.h>
#include <unity.h>

static EmuStorage storage;

void setUp() {}
void tearDown() {}

static bool textIs(const char *path, const char *want) {
  char b[64];
  size_t n;
  return storage.readText(path, b, sizeof(b), n) == EmuResult::Ok && strcmp(b, want) == 0;
}

static bool waitFor(bool (*cond)()) {
  for (uint32_t t0 = millis(); millis() - t0 < 90000;) {
    if (!storage.isConnected()) storage.begin();
    if (cond()) return true;
    delay(500);
  }
  return false;
}

static void test_a_spi_writes() {
  TEST_ASSERT_TRUE(storage.begin());
  storage.remove("/spi_note.txt");
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.writeText("/spi_note.txt", "from spi\n"));
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.appendText("/log.txt", "[bridge]\n"));
  TEST_ASSERT_EQUAL((int)EmuResult::Ok, (int)storage.sync());
  Serial.println("STEP A done");
}

static void test_b_pc_writes_reach_spi() {
  TEST_ASSERT_TRUE(waitFor([] {
    EmuFileInfo fi;
    return storage.stat("/pc_note.txt", fi) == EmuResult::Ok && fi.readOnly &&
           textIs("/pc_note.txt", "from pc\n") && textIs("/spi_note.txt", "edited by pc\n");
  }));
  size_t n;
  TEST_ASSERT_EQUAL((int)EmuResult::ReadOnly, (int)storage.write("/pc_note.txt", 0, "x", 1, n));
  Serial.println("STEP B done");
}

static void test_c_pc_deletes_reach_spi() {
  TEST_ASSERT_TRUE(waitFor([] {
    EmuFileInfo fi;
    return storage.stat("/pc_note.txt", fi) == EmuResult::NotFound &&
           storage.stat("/spi_note.txt", fi) == EmuResult::NotFound;
  }));
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  delay(1500);
  UNITY_BEGIN();
  RUN_TEST(test_a_spi_writes);
  RUN_TEST(test_b_pc_writes_reach_spi);
  RUN_TEST(test_c_pc_deletes_reach_spi);
  UNITY_END();
}

void loop() {}
