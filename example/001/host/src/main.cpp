// M5Stack Core2 client for KagigataDisk.
//
// Everything that talks to the device goes through the emu_storage library
// (lib/emu_storage): this file only deals in paths, text and results. Layout,
// full screen height:
//   - left panel:  bordered file list, "N:NAME" (up to NAME_DISPLAY_CHARS
//                  of the name), refreshed every second; next to its FILES
//                  title a dot shows the link state (see LinkState)
//   - right panel: bordered viewer for the file selected in the list (a
//                  vertical swipe on the list moves the selection); a
//                  vertical swipe on it scrolls the text a page
//   - bottom strip: three on-screen buttons, A / B / C; tapping one appends
//                  "[uptime] : [A]" (and so on) to /log.txt
// Nothing is written unless a button is tapped: KagigataDisk keeps its files
// in flash, and a host left writing on its own would wear it out for nothing.
#include <M5Unified.h>
#include <emu_storage.h>

#include <cstring>

// The library's defaults are the Core2's slot pins and an 8 MHz clock.
static EmuStorage storage;

static constexpr int MAX_LIST = 10;
static constexpr size_t VIEW_CONTENT_MAX = 16384;  // the viewer shows this much of a file
static constexpr int VIEW_LINES_MAX = 2048;         // wrapped lines kept for scrolling
static constexpr int NAME_DISPLAY_CHARS = 9;       // list entries show up to this many

// Titles and buttons use an outline font (bundled with M5GFX), scaled a
// little down. Text that has to stay legible at a small size - the file list
// and the viewer - uses a bitmap font at its native size instead: scaling an
// outline font that small blurs the letters together.
static const lgfx::IFont *kFontMedium = &fonts::FreeSans12pt7b;
static constexpr float kFontScale = 0.8f;
static const lgfx::IFont *kFontText = &fonts::Font2;  // 16 px high, drawn at x1

static void useFont(const lgfx::IFont *font) {
  M5.Display.setFont(font);
  M5.Display.setTextSize(kFontScale);
}

static void useTextFont() {
  M5.Display.setFont(kFontText);
  M5.Display.setTextSize(1);
}

// ---------------------------------------------------------------------------
// Theme - anything but black-on-white. A cool dark base with a teal frame
// for structure and a warm amber accent for anything selected/emphasised,
// so the two never get confused with each other.
// ---------------------------------------------------------------------------
static uint16_t colBg;          // page background
static uint16_t colBorder;      // panel and per-item frames
static uint16_t colAccent;      // selection highlight, emphasis text
static uint16_t colHeaderBg;    // status bar / panel header strips
static uint16_t colTextPrimary;
static uint16_t colTextSecondary;
static uint16_t colError;
// Link state dot (see LinkState).
static uint16_t colLinkReady;
static uint16_t colLinkBusy;
static uint16_t colLinkNone;

static void initTheme() {
  colBg = M5.Display.color565(17, 21, 28);
  colBorder = M5.Display.color565(0, 168, 163);
  colAccent = M5.Display.color565(255, 176, 59);
  colHeaderBg = M5.Display.color565(21, 58, 66);
  colTextPrimary = M5.Display.color565(232, 236, 240);
  colTextSecondary = M5.Display.color565(142, 160, 178);
  colError = M5.Display.color565(255, 99, 99);
  colLinkReady = M5.Display.color565(64, 200, 96);
  colLinkBusy = M5.Display.color565(240, 200, 40);
  colLinkNone = M5.Display.color565(110, 120, 130);
}

// ---------------------------------------------------------------------------
// Layout - computed from the panel's actual size in setup() so this does not
// depend on guessing the default rotation. The list/viewer panels run from
// the top down to the button strip; the link state lives in the list's title
// strip.
// ---------------------------------------------------------------------------
static int screenW, screenH;
static int listX = 0, listY = 0, listW = 104, listH;
static int viewX, viewY, viewW, viewH;
static constexpr int PANEL_GAP = 4;
static constexpr int LIST_HEADER_H = 22;
static constexpr int VIEW_HEADER_H = 30;
static constexpr int LINK_DOT_R = 6;
static constexpr int SWIPE_MIN_PX = 20;  // vertical travel that counts as a swipe
static constexpr int BTN_H = 40;         // button strip at the bottom
static constexpr int BTN_COUNT = 3;
static constexpr char BTN_LABELS[BTN_COUNT] = {'A', 'B', 'C'};
static int btnY, btnW;
static int btnPressed = -1;  // index while a finger is down on it

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
// What the dot next to FILES shows:
//   LINK_READY - green:  the last request succeeded
//   LINK_BUSY  - yellow: a request is in progress
//   LINK_ERROR - red:    connected, but a request failed
//   LINK_NONE  - gray:   no KagigataDisk answering
enum LinkState { LINK_NONE, LINK_READY, LINK_BUSY, LINK_ERROR };
static LinkState linkState = LINK_NONE;
static int linkDotX = -1;  // centre, set once the FILES title is drawn

static bool linkUp = false;  // storage.begin() succeeded and nothing timed out since
static String fileNames[MAX_LIST];
static int fileCount = 0;

static int selectedIndex = -1;
static bool selectedLoadFailed = false;

// The selected file's text, wrapped to the viewer's width: line i is
// viewText[lineStart[i] .. lineStart[i] + lineLen[i]). scrollTop is the first
// line on screen.
static char viewText[VIEW_CONTENT_MAX + 1];
static size_t viewLen = 0;
static bool viewTruncated = false;  // the file is longer than VIEW_CONTENT_MAX
static uint16_t lineStart[VIEW_LINES_MAX];
static uint16_t lineLen[VIEW_LINES_MAX];
static int lineCount = 0;
static int scrollTop = 0;

static uint32_t lastListRefresh = 0;

static void drawLinkDot();

static void setLinkState(LinkState s) {
  if (s == linkState) return;
  linkState = s;
  drawLinkDot();
}

// State after a request that ended with `r`. A request that got no answer
// drops the link; the next refresh greets the device again.
static void settleLinkState(EmuResult r) {
  bool lost = r == EmuResult::Timeout || r == EmuResult::CrcError ||
              r == EmuResult::NotConnected || r == EmuResult::NotInitialized;
  if (r != EmuResult::Ok) Serial.printf("KagigataDisk: %s\n", emuResultName(r));
  if (lost) linkUp = false;
  setLinkState(r == EmuResult::Ok ? LINK_READY : (linkUp ? LINK_ERROR : LINK_NONE));
}

// ---------------------------------------------------------------------------
// KagigataDisk access
// ---------------------------------------------------------------------------
static void refreshFileList() {
  setLinkState(LINK_BUSY);
  if (!linkUp) linkUp = storage.begin();
  if (!linkUp) {
    fileCount = 0;
    settleLinkState(storage.beginResult());
    return;
  }
  EmuDirectoryEntry entries[MAX_LIST];
  size_t n = 0;
  EmuResult r = storage.list(entries, MAX_LIST, n);
  if (r == EmuResult::Ok) {
    fileCount = 0;
    for (size_t i = 0; i < n; i++) {
      if (!entries[i].isDirectory) fileNames[fileCount++] = String("/") + entries[i].name;
    }
  } else {
    fileCount = 0;
  }
  settleLinkState(r);
}

// Reads the start of `path` into viewText. Files hold plain ASCII: anything
// else (and control characters other than newline and tab) shows as '?'.
static bool loadFile(const String &path) {
  setLinkState(LINK_BUSY);
  size_t n = 0;
  EmuResult r = storage.read(path.c_str(), 0, viewText, VIEW_CONTENT_MAX, n);
  EmuFileInfo info;
  viewTruncated = r == EmuResult::Ok && storage.stat(path.c_str(), info) == EmuResult::Ok &&
                  info.size > VIEW_CONTENT_MAX;
  if (r != EmuResult::Ok) n = 0;
  size_t out = 0;
  for (size_t i = 0; i < n; i++) {
    char c = viewText[i];
    if (c == '\r') continue;
    if (c == '\t') c = ' ';
    if (c != '\n' && (c < 0x20 || c > 0x7E)) c = '?';
    viewText[out++] = c;
  }
  viewLen = out;
  viewText[viewLen] = '\0';
  settleLinkState(r);
  return r == EmuResult::Ok;
}

static String formatUptime() {
  uint32_t s = millis() / 1000u;
  uint32_t hh = s / 3600u;
  uint32_t mm = (s % 3600u) / 60u;
  uint32_t ss = s % 60u;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", (unsigned long)hh,
            (unsigned long)mm, (unsigned long)ss);
  return String(buf);
}

// Appends one "[uptime] : [<button>]" line to /log.txt.
static bool writeLogEntry(char button) {
  if (!linkUp) return false;
  setLinkState(LINK_BUSY);
  char line[48];
  snprintf(line, sizeof(line), "[%s] : [%c]\n", formatUptime().c_str(), button);
  EmuResult r = storage.appendText("/log.txt", line);
  settleLinkState(r);
  return r == EmuResult::Ok;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
// Redraws only the dot, so a state change never repaints the whole list.
static void drawLinkDot() {
  if (linkDotX < 0) return;
  static const uint16_t *const kColors[] = {&colLinkNone, &colLinkReady, &colLinkBusy, &colError};
  int cy = listY + 1 + LIST_HEADER_H / 2;
  M5.Display.fillCircle(linkDotX, cy, LINK_DOT_R + 1, colHeaderBg);
  M5.Display.fillCircle(linkDotX, cy, LINK_DOT_R, *kColors[linkState]);
}

static void drawList() {
  M5.Display.fillRect(listX, listY, listW, listH, colBg);
  M5.Display.drawRect(listX, listY, listW, listH, colBorder);

  // Panel title - kept fixed and short so it can never overflow the narrow
  // panel.
  M5.Display.fillRect(listX + 1, listY + 1, listW - 2, LIST_HEADER_H, colHeaderBg);
  M5.Display.setTextDatum(top_left);
  useFont(kFontMedium);
  M5.Display.setTextColor(colAccent, colHeaderBg);
  M5.Display.drawString(
      "FILES", listX + 6,
      listY + 1 + (LIST_HEADER_H - M5.Display.fontHeight()) / 2);
  linkDotX = listX + 6 + M5.Display.textWidth("FILES") + 6 + LINK_DOT_R;
  drawLinkDot();
  M5.Display.drawFastHLine(listX + 1, listY + 1 + LIST_HEADER_H, listW - 2, colBorder);

  int rowsTop = listY + 1 + LIST_HEADER_H + 1;
  int rowsH = listH - LIST_HEADER_H - 2;
  int rowH = rowsH / MAX_LIST;

  useTextFont();
  int textH = M5.Display.fontHeight();
  for (int i = 0; i < MAX_LIST; i++) {
    int y = rowsTop + i * rowH;
    bool occupied = i < fileCount;
    bool isSelected = occupied && i == selectedIndex;
    // No per-item frame or separator - only the fill colour marks the
    // selected entry, so the list doesn't turn into a grid of boxes.
    uint16_t bg = isSelected ? colAccent : colBg;
    uint16_t fg = isSelected ? colBg : colTextPrimary;

    M5.Display.fillRect(listX + 1, y, listW - 2, rowH, bg);

    if (!occupied) continue;
    String nm = fileNames[i];
    if (nm.startsWith("/")) nm = nm.substring(1);
    if ((int)nm.length() > NAME_DISPLAY_CHARS) nm = nm.substring(0, NAME_DISPLAY_CHARS);

    char label[16];
    snprintf(label, sizeof(label), "%d:%s", i + 1, nm.c_str());
    M5.Display.setClipRect(listX + 1, y, listW - 2, rowH);
    M5.Display.setTextColor(fg, bg);
    M5.Display.drawString(label, listX + 6, y + (rowH - textH) / 2);
    M5.Display.clearClipRect();
  }
}

static constexpr int SCROLLBAR_W = 4;

static int viewContentY() { return viewY + 2 + VIEW_HEADER_H + 4; }
static int viewContentH() { return viewY + viewH - viewContentY() - 4; }
static int viewTextW() { return viewW - 12 - SCROLLBAR_W - 2; }

static int lineHeight() {
  useTextFont();
  return M5.Display.fontHeight();
}

static int visibleLines() {
  int n = viewContentH() / lineHeight();
  return n > 0 ? n : 1;
}

// The wrapped lines, plus a closing note when the file was cut short.
static int totalLines() { return lineCount + (viewTruncated ? 1 : 0); }

static int maxScrollTop() {
  int m = totalLines() - visibleLines();
  return m > 0 ? m : 0;
}

// Splits viewText into lines that fit the viewer: at newlines, and at the
// last space before the width runs out (mid-word when a word alone is wider).
static void wrapText() {
  useTextFont();
  const int width = viewTextW();
  lineCount = 0;
  size_t pos = 0;
  while (pos <= viewLen && lineCount < VIEW_LINES_MAX) {
    size_t start = pos, lastSpace = SIZE_MAX, end = pos;
    int w = 0;
    while (end < viewLen && viewText[end] != '\n') {
      char ch[2] = {viewText[end], 0};
      int cw = M5.Display.textWidth(ch);
      if (w + cw > width && end > start) break;
      if (viewText[end] == ' ') lastSpace = end;
      w += cw;
      end++;
    }
    size_t next;
    if (end < viewLen && viewText[end] != '\n' && lastSpace != SIZE_MAX) {
      end = lastSpace;  // break at the space, which is not drawn
      next = lastSpace + 1;
    } else if (end < viewLen && viewText[end] == '\n') {
      next = end + 1;
    } else {
      next = end;
    }
    lineStart[lineCount] = (uint16_t)start;
    lineLen[lineCount] = (uint16_t)(end - start);
    lineCount++;
    if (end >= viewLen) break;
    pos = next;
  }
}

// Only the text area: what scrolling repaints.
static void drawViewerContent() {
  int cy = viewContentY(), ch = viewContentH();
  M5.Display.fillRect(viewX + 1, cy, viewW - 2, ch, colBg);
  M5.Display.setTextDatum(top_left);
  M5.Display.setClipRect(viewX + 6, cy, viewW - 12, ch);
  M5.Display.setCursor(viewX + 6, cy);

  if (selectedIndex < 0) {
    useTextFont();
    M5.Display.setTextColor(colTextSecondary, colBg);
    M5.Display.print(
        "Swipe the list to select a file.\n\nSwipe up / down here to scroll.\n\nTap A / B / C "
        "below to add a line to log.txt.");
  } else if (selectedLoadFailed) {
    useFont(kFontMedium);
    M5.Display.setTextColor(colError, colBg);
    M5.Display.print("Failed to load");
  } else {
    int lh = lineHeight();
    int shown = visibleLines();
    M5.Display.setTextColor(colTextPrimary, colBg);
    char line[256];
    for (int k = 0; k < shown && scrollTop + k < lineCount; k++) {
      int li = scrollTop + k;
      size_t n = lineLen[li] < sizeof(line) - 1 ? lineLen[li] : sizeof(line) - 1;
      memcpy(line, &viewText[lineStart[li]], n);
      line[n] = '\0';
      M5.Display.drawString(line, viewX + 6, cy + k * lh);
    }
    if (viewTruncated && lineCount - scrollTop < shown) {
      M5.Display.setTextColor(colTextSecondary, colBg);
      M5.Display.drawString("(first 16 KiB only)", viewX + 6, cy + (lineCount - scrollTop) * lh);
    }
  }
  M5.Display.clearClipRect();

  // Scroll bar: only when there is more than fits.
  if (selectedIndex >= 0 && !selectedLoadFailed && totalLines() > visibleLines()) {
    int trackX = viewX + viewW - 2 - SCROLLBAR_W, trackH = ch;
    int thumbH = trackH * visibleLines() / totalLines();
    if (thumbH < 12) thumbH = 12;
    int thumbY = cy + (trackH - thumbH) * scrollTop / (maxScrollTop() ? maxScrollTop() : 1);
    M5.Display.fillRect(trackX, cy, SCROLLBAR_W, trackH, colHeaderBg);
    M5.Display.fillRect(trackX, thumbY, SCROLLBAR_W, thumbH, colBorder);
  }
}

static void drawViewer() {
  M5.Display.fillRect(viewX, viewY, viewW, viewH, colBg);
  M5.Display.drawRect(viewX, viewY, viewW, viewH, colBorder);
  M5.Display.setTextDatum(top_left);

  M5.Display.fillRect(viewX + 1, viewY + 1, viewW - 2, VIEW_HEADER_H, colHeaderBg);
  M5.Display.drawFastHLine(viewX + 1, viewY + 1 + VIEW_HEADER_H, viewW - 2, colBorder);

  useFont(kFontMedium);
  M5.Display.setClipRect(viewX + 1, viewY + 1, viewW - 2, VIEW_HEADER_H);
  if (selectedIndex >= 0) {
    M5.Display.setTextColor(colAccent, colHeaderBg);
    M5.Display.drawString(
        fileNames[selectedIndex], viewX + 6,
        viewY + 1 + (VIEW_HEADER_H - M5.Display.fontHeight()) / 2);
  }
  M5.Display.clearClipRect();

  drawViewerContent();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static void drawButton(int i) {
  int x = i * (btnW + PANEL_GAP);
  bool down = i == btnPressed;
  uint16_t bg = down ? colAccent : colHeaderBg;
  M5.Display.fillRoundRect(x, btnY, btnW, BTN_H, 6, bg);
  M5.Display.drawRoundRect(x, btnY, btnW, BTN_H, 6, colBorder);
  useFont(kFontMedium);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(down ? colBg : colAccent, bg);
  char label[2] = {BTN_LABELS[i], 0};
  M5.Display.drawString(label, x + btnW / 2, btnY + BTN_H / 2);
  M5.Display.setTextDatum(top_left);
}

static void drawButtons() {
  for (int i = 0; i < BTN_COUNT; i++) drawButton(i);
}

static int buttonAt(int x, int y) {
  if (y < btnY || y >= btnY + BTN_H) return -1;
  for (int i = 0; i < BTN_COUNT; i++) {
    int bx = i * (btnW + PANEL_GAP);
    if (x >= bx && x < bx + btnW) return i;
  }
  return -1;
}

// Shows file `idx` from its top, or (toEnd) scrolled to its last line.
static void selectFile(int idx, bool toEnd = false) {
  selectedIndex = idx;
  selectedLoadFailed = !loadFile(fileNames[idx]);
  wrapText();
  scrollTop = toEnd ? maxScrollTop() : 0;
  drawViewer();
}

// A vertical swipe anywhere on the viewer scrolls it a page, the way a swipe
// on the list moves the selection: swiping up brings the following page into
// view, swiping down the previous one. One line of the old page stays on
// screen for context.
static void handleViewerSwipe() {
  auto t = M5.Touch.getDetail();
  if (!t.wasReleased()) return;
  if (t.base_x < viewX || t.base_x >= viewX + viewW || t.base_y < viewY ||
      t.base_y >= viewY + viewH) {
    return;
  }
  int dy = t.distanceY();
  if (abs(dy) < SWIPE_MIN_PX || abs(dy) <= abs(t.distanceX())) return;
  if (selectedIndex < 0 || selectedLoadFailed) return;
  int page = visibleLines() > 1 ? visibleLines() - 1 : 1;
  int top = scrollTop + (dy < 0 ? page : -page);
  if (top < 0) top = 0;
  if (top > maxScrollTop()) top = maxScrollTop();
  if (top == scrollTop) return;
  scrollTop = top;
  drawViewerContent();
}

// A vertical swipe that starts on the list moves the selection one entry:
// up (finger moving up the screen) selects the next file, down the previous
// one, stopping at either end - the same direction as scrolling the viewer.
// With nothing selected yet, up picks the first file and down the last. Taps and mostly-horizontal drags do nothing.
static void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (t.base_y >= btnY) return;  // the button strip (handleButtons())
  if (!t.wasReleased()) return;
  if (t.base_x < listX || t.base_x >= listX + listW || t.base_y < listY ||
      t.base_y >= listY + listH) {
    return;
  }
  int dy = t.distanceY();
  if (abs(dy) < SWIPE_MIN_PX || abs(dy) <= abs(t.distanceX())) return;
  if (fileCount == 0) return;

  int idx;
  if (selectedIndex < 0) {
    idx = dy < 0 ? 0 : fileCount - 1;
  } else {
    idx = selectedIndex + (dy < 0 ? 1 : -1);
    if (idx < 0) idx = 0;
    if (idx >= fileCount) idx = fileCount - 1;
    if (idx == selectedIndex) return;
  }
  selectFile(idx);
  drawList();  // move the highlight straight away, not at the next refresh
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);

  initTheme();

  screenW = M5.Display.width();
  screenH = M5.Display.height();
  btnY = screenH - BTN_H;
  btnW = (screenW - (BTN_COUNT - 1) * PANEL_GAP) / BTN_COUNT;
  listH = btnY - PANEL_GAP - listY;
  viewX = listX + listW + PANEL_GAP;
  viewY = listY;
  viewW = screenW - viewX;
  viewH = listH;

  M5.Display.fillScreen(colBg);

  drawList();  // title strip first, so the link dot has somewhere to go
  refreshFileList();  // greets the device
  drawList();
  drawViewer();
  drawButtons();
  lastListRefresh = millis();
}

// A tap on A/B/C logs that letter: lit while the finger is down, written when
// it lifts inside the same button.
static void handleButtons() {
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    btnPressed = buttonAt(t.x, t.y);
    if (btnPressed >= 0) drawButton(btnPressed);
    return;
  }
  if (btnPressed < 0 || !t.wasReleased()) return;
  int i = btnPressed;
  btnPressed = -1;
  drawButton(i);
  if (buttonAt(t.x, t.y) != i) return;  // slid off: cancelled
  bool ok = writeLogEntry(BTN_LABELS[i]);
  if (!ok) Serial.println("Failed to write log");
  // If /log.txt is what's currently on screen, show the line that was just
  // added instead of leaving stale content up.
  if (ok && selectedIndex >= 0 && fileNames[selectedIndex] == "/log.txt") {
    selectFile(selectedIndex, true);  // scrolled to the new line
  }
}

void loop() {
  M5.update();
  handleTouch();
  handleViewerSwipe();
  handleButtons();

  uint32_t now = millis();
  if (now - lastListRefresh >= 1000) {
    lastListRefresh = now;
    refreshFileList();
    drawList();
  }

  delay(10);
}
