#include <Arduino.h>
#include <U8g2lib.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr uint8_t kOledSda = 21;
constexpr uint8_t kOledScl = 22;
constexpr uint8_t kOledAddress = 0x3C;
constexpr uint8_t kJoystickVcc = 25;
constexpr uint8_t kJoystickX = 34;
constexpr uint8_t kJoystickY = 35;
constexpr uint8_t kJoystickButton = 32;
constexpr int kHorizontalSign = 1;  // Set to -1 if left and right are reversed.
constexpr int kVerticalSign = 1;  // Match the installed joystick: larger Y means down.

constexpr int kDisplayWidth = 128;
constexpr int kContentHeight = 56;
constexpr uint32_t kFrameIntervalMs = 33;
constexpr uint32_t kSlideDurationMs = 200;
constexpr uint32_t kMenuSlideDurationMs = 150;
constexpr uint32_t kTimerFrameIntervalMs = 100;
constexpr uint32_t kInputIntervalMs = 8;
constexpr uint32_t kButtonDebounceMs = 25;
constexpr uint32_t kJoystickPowerSettleMs = 20;
constexpr int kTiltThreshold = 600;
constexpr int kReleaseThreshold = 250;

#ifdef OLED_SH1106
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, kOledScl, kOledSda);
#else
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, kOledScl, kOledSda);
#endif

enum class Page : uint8_t { Home, Joystick, Display, Stopwatch, Countdown, Counter };
enum class View : uint8_t { Carousel, Menu, Tool };
constexpr int kCarouselPageCount = 3;

struct MenuItem {
  const char *label;
  Page page;
};

constexpr MenuItem kMenuItems[] = {
    {"Stopwatch", Page::Stopwatch},
    {"Countdown", Page::Countdown},
    {"Counter", Page::Counter},
    {"Brightness", Page::Display},
    {"Joystick", Page::Joystick},
};
constexpr int kMenuItemCount = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
constexpr int kMenuTop = 16;
constexpr int kMenuRowHeight = 13;
constexpr int kMenuVisibleRows = 3;
constexpr int kMenuHeight = kMenuRowHeight * kMenuVisibleRows;

struct MenuPosition {
  int scroll;
  int highlight;
};

struct Timer {
  uint32_t elapsed = 0;
  uint32_t startedAt = 0;
  bool running = false;

  uint32_t elapsedAt(uint32_t now) const {
    return running ? elapsed + (now - startedAt) : elapsed;
  }

  void toggle(uint32_t now, uint32_t limit) {
    if (running) {
      elapsed = elapsedAt(now);
    } else {
      if (elapsed >= limit) {
        elapsed = 0;
      }
      startedAt = now;
    }
    running = !running;
  }

  bool finishIfDue(uint32_t now, uint32_t limit) {
    if (!running || elapsedAt(now) < limit) {
      return false;
    }
    running = false;
    elapsed = limit;
    return true;
  }
};

constexpr uint32_t kStopwatchLimitMs = 5999900;
constexpr uint32_t kCountdownPresetsMs[] = {15000, 30000, 60000, 120000, 300000, 600000, 900000};
constexpr int kCountdownPresetCount = sizeof(kCountdownPresetsMs) / sizeof(kCountdownPresetsMs[0]);

struct BrightnessLevel {
  const char *label;
  uint8_t contrast;
};

constexpr BrightnessLevel kBrightnessLevels[] = {
    {"Low", 32}, {"Medium", 128}, {"High", 255}};
constexpr size_t kBrightnessCount = sizeof(kBrightnessLevels) / sizeof(kBrightnessLevels[0]);

int centerX = 0;
int centerY = 0;
int joystickX = 0;
int joystickY = 0;
bool tiltLatched = false;
bool buttonRaw = false;
bool buttonStable = false;
uint32_t buttonChangedAt = 0;
uint32_t lastInputAt = 0;

Page currentPage = Page::Home;
Page targetPage = Page::Home;
View view = View::Carousel;
int slideDirection = 0;
int pendingDirection = 0;
uint32_t slideStartedAt = 0;
uint32_t lastFrameAt = 0;
size_t brightness = 1;
bool redraw = true;
int menuIndex = 0;
MenuPosition menuFrom{0, 0};
bool menuMoving = false;
uint32_t menuMovedAt = 0;
Timer stopwatch;
Timer countdown;
int countdownPreset = 2;
int counter = 0;
// U8g2 stores each 8-pixel-high band as one byte per column.
uint8_t outgoingContent[kDisplayWidth * (kContentHeight / 8)];

void calibrateJoystick() {
  constexpr int samples = 32;
  for (int i = 0; i < samples; ++i) {
    centerX += analogRead(kJoystickX);
    centerY += analogRead(kJoystickY);
    delay(2);
  }
  centerX /= samples;
  centerY /= samples;
  joystickX = centerX;
  joystickY = centerY;
}

uint32_t countdownLimit() {
  return kCountdownPresetsMs[countdownPreset];
}

Page selectedTool() {
  return kMenuItems[menuIndex].page;
}

void drawTimer(Page page, uint32_t now) {
  const bool isCountdown = page == Page::Countdown;
  const Timer &timer = isCountdown ? countdown : stopwatch;
  const uint32_t elapsed = timer.elapsedAt(now);
  const uint32_t shownMs = isCountdown ? countdownLimit() - elapsed : elapsed;
  const unsigned seconds = isCountdown ? (shownMs + 999) / 1000 : shownMs / 1000;
  char time[12];
  if (isCountdown) {
    snprintf(time, sizeof(time), "%02u:%02u", seconds / 60, seconds % 60);
  } else {
    snprintf(time, sizeof(time), "%02u:%02u.%u", seconds / 60, seconds % 60,
             static_cast<unsigned>(shownMs / 100 % 10));
  }

  oled.drawStr(4, 10, isCountdown ? "Countdown" : "Stopwatch");
  oled.drawStr(4, 51, isCountdown && !timer.running ? "Up/Dn: preset" : "Right: reset");
  if (isCountdown && shownMs == 0) {
    oled.drawStr(99, 35, "DONE");
  }
  oled.setFont(u8g2_font_logisoso22_tn);
  oled.drawStr(4, 38, time);
}

void drawPage(Page page, uint32_t now) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawHLine(4, 13, 120);

  switch (page) {
    case Page::Home:
      oled.drawStr(4, 10, "Home");
      oled.drawStr(4, 49, "Down: Tools");
      oled.setFont(u8g2_font_helvB12_tf);
      oled.drawStr(4, 34, "Slide UI");
      oled.drawRFrame(95, 26, 21, 17, 3);
      oled.drawBox(99, 31, 23, 18);
      oled.setDrawColor(0);
      oled.drawHLine(104, 39, 12);
      oled.drawLine(112, 36, 116, 39);
      oled.drawLine(112, 42, 116, 39);
      oled.setDrawColor(1);
      break;

    case Page::Joystick: {
      oled.drawStr(4, 10, "Joystick");
      oled.setFont(u8g2_font_7x13B_tf);
      char reading[8];
      snprintf(reading, sizeof(reading), "X %4d", joystickX);
      oled.drawStr(4, 31, reading);
      snprintf(reading, sizeof(reading), "Y %4d", joystickY);
      oled.drawStr(4, 48, reading);
      oled.drawRFrame(86, 17, 37, 37, 3);
      oled.drawHLine(90, 35, 29);
      oled.drawVLine(104, 21, 29);
      const int x = 104 + constrain(kHorizontalSign * (joystickX - centerX) * 14 / 1600, -14, 14);
      const int y = 35 + constrain(kVerticalSign * (joystickY - centerY) * 14 / 1600, -14, 14);
      oled.setDrawColor(0);
      oled.drawDisc(x, y, 3);
      oled.setDrawColor(1);
      oled.drawDisc(x, y, 2);
      break;
    }

    case Page::Display:
      oled.drawStr(4, 10, "Display");
      oled.drawStr(4, 49, "Press to change");
      oled.setFont(u8g2_font_helvB12_tf);
      oled.drawStr(4, 34, kBrightnessLevels[brightness].label);
      for (size_t i = 0; i < kBrightnessCount; ++i) {
        const int x = 96 + i * 9;
        const int height = 6 + i * 5;
        oled.drawFrame(x, 48 - height, 6, height);
        if (i <= brightness) {
          oled.drawBox(x, 48 - height, 6, height);
        }
      }
      break;

    case Page::Stopwatch:
    case Page::Countdown:
      drawTimer(page, now);
      break;

    case Page::Counter: {
      oled.drawStr(4, 10, "Counter");
      oled.drawStr(4, 51, "Up +1   Down -1");
      oled.setFont(u8g2_font_logisoso22_tn);
      char value[8];
      snprintf(value, sizeof(value), "%d", counter);
      oled.drawStr(4, 38, value);
      break;
    }
  }
}

void drawFooter(Page activePage) {
  oled.drawLine(8, 57, 5, 60);
  oled.drawLine(5, 60, 8, 63);
  oled.drawLine(119, 57, 122, 60);
  oled.drawLine(122, 60, 119, 63);

  for (int i = 0; i < kCarouselPageCount; ++i) {
    const int x = 54 + i * 10;
    if (i == static_cast<int>(activePage)) {
      oled.drawDisc(x, 60, 2);
    } else {
      oled.drawCircle(x, 60, 2);
    }
  }
}

void startSlide(int direction, uint32_t now) {
  if (slideDirection != 0) {
    pendingDirection = direction;
    return;
  }

  targetPage = static_cast<Page>((static_cast<int>(currentPage) + direction + kCarouselPageCount) %
                                 kCarouselPageCount);
  drawPage(currentPage, now);
  memcpy(outgoingContent, oled.getBufferPtr(), sizeof(outgoingContent));
  slideDirection = direction;
  slideStartedAt = now;
}

void composeSlide(int distance) {
  uint8_t *buffer = oled.getBufferPtr();
  for (int y = 0; y < kContentHeight / 8; ++y) {
    uint8_t *row = buffer + y * kDisplayWidth;
    const uint8_t *oldRow = outgoingContent + y * kDisplayWidth;
    const int remaining = kDisplayWidth - distance;
    if (slideDirection > 0) {
      memmove(row + remaining, row, distance);
      memcpy(row, oldRow + distance, remaining);
    } else {
      memmove(row, row + remaining, distance);
      memcpy(row + distance, oldRow, remaining);
    }
  }
}

int easeOut(int from, int to, uint32_t elapsed, int duration) {
  if (elapsed >= static_cast<uint32_t>(duration)) {
    return to;
  }
  const int left = duration - static_cast<int>(elapsed);
  return to + (from - to) * left * left / (duration * duration);
}

MenuPosition menuPositionAt(uint32_t now) {
  const int firstRow = constrain(menuIndex - 1, 0, kMenuItemCount - kMenuVisibleRows);
  const MenuPosition target{firstRow * kMenuRowHeight, (menuIndex - firstRow) * kMenuRowHeight};
  if (!menuMoving) {
    return target;
  }
  const uint32_t elapsed = now - menuMovedAt;
  return {
      easeOut(menuFrom.scroll, target.scroll, elapsed, kMenuSlideDurationMs),
      easeOut(menuFrom.highlight, target.highlight, elapsed, kMenuSlideDurationMs),
  };
}

void drawMenu(uint32_t now) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(4, 10, "Tools");
  oled.drawStr(82, 10, "Up/Down");
  oled.drawHLine(4, 13, 120);

  const MenuPosition position = menuPositionAt(now);
  oled.setClipWindow(4, kMenuTop, 120, kMenuTop + kMenuHeight);
  oled.drawRBox(4, kMenuTop + position.highlight, 115, kMenuRowHeight, 2);
  oled.setDrawColor(2);
  for (int i = 0; i < kMenuItemCount; ++i) {
    const int top = kMenuTop + i * kMenuRowHeight - position.scroll;
    if (top + kMenuRowHeight > kMenuTop && top < kMenuTop + kMenuHeight) {
      oled.drawStr(10, top + 10, kMenuItems[i].label);
    }
  }
  oled.setDrawColor(1);
  oled.setMaxClipWindow();

  const int thumbHeight = kMenuHeight * kMenuVisibleRows / kMenuItemCount;
  const int scrollRange = (kMenuItemCount - kMenuVisibleRows) * kMenuRowHeight;
  const int thumbTop = kMenuTop + position.scroll * (kMenuHeight - thumbHeight) / scrollRange;
  oled.drawVLine(123, kMenuTop, kMenuHeight);
  oled.drawBox(122, thumbTop, 3, thumbHeight);
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(4, 62, "< Back");
  oled.drawStr(74, 62, "Press Open");
  if (menuMoving && now - menuMovedAt >= kMenuSlideDurationMs) {
    menuMoving = false;
  }
}

void drawTool(uint32_t now) {
  const Page page = selectedTool();
  drawPage(page, now);
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(4, 62, "< Back");
  switch (page) {
    case Page::Stopwatch:
    case Page::Countdown: {
      const Timer &timer = page == Page::Stopwatch ? stopwatch : countdown;
      oled.drawStr(69, 62, timer.running ? "Press Pause" : "Press Start");
      break;
    }
    case Page::Counter:
      oled.drawStr(69, 62, "Press Reset");
      break;
    default:
      break;
  }
}

void drawCarousel(uint32_t now) {
  if (slideDirection == 0) {
    drawPage(currentPage, now);
    drawFooter(currentPage);
  } else {
    drawPage(targetPage, now);
    const uint32_t elapsed = now - slideStartedAt;
    if (elapsed >= kSlideDurationMs) {
      currentPage = targetPage;
      slideDirection = 0;
      drawFooter(currentPage);
    } else {
      const int distance = easeOut(0, kDisplayWidth, elapsed, kSlideDurationMs);
      composeSlide(distance);
      drawFooter(distance >= kDisplayWidth / 2 ? targetPage : currentPage);
    }
  }
}

void renderFrame(uint32_t now) {
  switch (view) {
    case View::Carousel:
      drawCarousel(now);
      break;
    case View::Menu:
      drawMenu(now);
      break;
    case View::Tool:
      drawTool(now);
      break;
  }
  oled.sendBuffer();
  redraw = false;

  if (view == View::Carousel && slideDirection == 0 && pendingDirection != 0) {
    const int direction = pendingDirection;
    pendingDirection = 0;
    startSlide(direction, millis());
  }
}

void changeView(View nextView) {
  view = nextView;
  menuMoving = false;
  redraw = true;
}

void handleTilt(int horizontal, int vertical, uint32_t now) {
  if (view == View::Carousel) {
    if (horizontal != 0) {
      startSlide(horizontal, now);
    } else if (vertical > 0 && slideDirection == 0) {
      changeView(View::Menu);
    }
    return;
  }

  if (horizontal < 0) {
    changeView(view == View::Tool ? View::Menu : View::Carousel);
    return;
  }
  if (view == View::Menu) {
    const int next = constrain(menuIndex + vertical, 0, kMenuItemCount - 1);
    if (next != menuIndex) {
      menuFrom = menuPositionAt(now);
      menuIndex = next;
      menuMovedAt = now;
      menuMoving = true;
    }
    return;
  }

  switch (selectedTool()) {
    case Page::Counter:
      counter = constrain(counter - vertical, -9999, 9999);
      break;
    case Page::Stopwatch:
      if (horizontal > 0) {
        stopwatch = Timer{};
      }
      break;
    case Page::Countdown:
      if (horizontal > 0) {
        countdown = Timer{};
      } else if (!countdown.running && vertical != 0) {
        const int next = constrain(countdownPreset - vertical, 0, kCountdownPresetCount - 1);
        if (next != countdownPreset) {
          countdownPreset = next;
          countdown = Timer{};
        }
      }
      break;
    default:
      break;
  }
  redraw = true;
}

void handlePress(uint32_t now) {
  if (view == View::Menu) {
    changeView(View::Tool);
    return;
  }
  if (view == View::Carousel) {
    if (slideDirection != 0) {
      return;
    }
    if (currentPage == Page::Home) {
      changeView(View::Menu);
      return;
    }
  }

  switch (view == View::Tool ? selectedTool() : currentPage) {
    case Page::Display:
      brightness = (brightness + 1) % kBrightnessCount;
      oled.setContrast(kBrightnessLevels[brightness].contrast);
      break;
    case Page::Stopwatch:
      stopwatch.toggle(now, kStopwatchLimitMs);
      break;
    case Page::Countdown:
      countdown.toggle(now, countdownLimit());
      break;
    case Page::Counter:
      counter = 0;
      break;
    default:
      return;
  }
  redraw = true;
}

void readControls(uint32_t now) {
  joystickX = (3 * joystickX + analogRead(kJoystickX) + 2) / 4;
  joystickY = (3 * joystickY + analogRead(kJoystickY) + 2) / 4;

  const int x = kHorizontalSign * (joystickX - centerX);
  const int y = kVerticalSign * (joystickY - centerY);
  const bool horizontalDominates = abs(x) >= abs(y);
  const int magnitude = horizontalDominates ? abs(x) : abs(y);
  if (magnitude <= kReleaseThreshold) {
    tiltLatched = false;
  } else if (!tiltLatched && magnitude >= kTiltThreshold) {
    tiltLatched = true;
    if (horizontalDominates) {
      handleTilt(x > 0 ? 1 : -1, 0, now);
    } else {
      handleTilt(0, y > 0 ? 1 : -1, now);
    }
  }

  const bool pressed = digitalRead(kJoystickButton) == LOW;
  if (pressed != buttonRaw) {
    buttonRaw = pressed;
    buttonChangedAt = now;
  }
  if (pressed != buttonStable && now - buttonChangedAt >= kButtonDebounceMs) {
    buttonStable = pressed;
    if (pressed) {
      handlePress(now);
    }
  }
}

}  // namespace

void setup() {
  pinMode(kJoystickButton, INPUT_PULLUP);
  analogReadResolution(12);
  pinMode(kJoystickVcc, OUTPUT);
  digitalWrite(kJoystickVcc, HIGH);
  delay(kJoystickPowerSettleMs);
  calibrateJoystick();
  buttonRaw = buttonStable = digitalRead(kJoystickButton) == LOW;

  oled.setI2CAddress(kOledAddress << 1);
  oled.setBusClock(400000);
  oled.begin();
  oled.setFontMode(1);
  oled.setContrast(kBrightnessLevels[brightness].contrast);
}

void loop() {
  const uint32_t now = millis();
  const bool stopwatchFinished = stopwatch.finishIfDue(now, kStopwatchLimitMs);
  const bool countdownFinished = countdown.finishIfDue(now, countdownLimit());
  if (view == View::Tool &&
      ((selectedTool() == Page::Stopwatch && stopwatchFinished) ||
       (selectedTool() == Page::Countdown && countdownFinished))) {
    redraw = true;
  }
  if (now - lastInputAt >= kInputIntervalMs) {
    lastInputAt = now;
    readControls(now);
  }

  const Page visiblePage = view == View::Tool ? selectedTool() : currentPage;
  const bool joystickVisible = view != View::Menu && visiblePage == Page::Joystick;
  const bool timerVisible = view == View::Tool &&
      ((visiblePage == Page::Stopwatch && stopwatch.running) ||
       (visiblePage == Page::Countdown && countdown.running));
  const uint32_t sinceFrame = now - lastFrameAt;
  if (sinceFrame >= kFrameIntervalMs &&
      (redraw || slideDirection != 0 || menuMoving || joystickVisible ||
       (timerVisible && sinceFrame >= kTimerFrameIntervalMs))) {
    lastFrameAt = now;
    renderFrame(now);
  }
  delay(1);
}
