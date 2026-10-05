// ============================================================================
//  STICK WATCH  |  a simple watch for the M5StickS3
//  Board (Arduino IDE): M5Stack > M5StickS3     Library: M5Unified (pulls in M5GFX)
//  Files: StickWatch.ino (this file) + wifi_secrets.h (your WiFi, optional)
// ----------------------------------------------------------------------------
//  HOW IT KEEPS THE RIGHT TIME
//   The StickS3 has a small clock chip (RTC) that keeps ticking on the battery
//   while the stick is switched off. The watch reads it every time it starts,
//   and writes to it whenever it learns the exact time:
//    1. From the internet (best): if wifi_secrets.h has your WiFi name and
//       password, the watch checks an internet time server when it starts and
//       then once a day, and turns WiFi off again straight after.
//    2. From the upload: the first time it runs after a new upload, if the
//       clock looks wrong, it sets it to the moment the sketch was compiled.
//       That is close (a few seconds to a couple of minutes behind), not exact.
//   The clock chip is stored in UTC. TIMEZONE below turns it into your local
//   time and handles summer/winter time by itself.
// ----------------------------------------------------------------------------
//  BUTTONS
//   Front (M5) : wake the screen.  Hold 2 s = check the time online now
//   Side       : brightness, 3 levels (remembered)
//   Power      : press once = on.  Double-press = off (built into the stick)
// ============================================================================

#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <time.h>
#include <sys/time.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""
#endif

// ============================================================================
//  SETTINGS (safe to tweak)
// ============================================================================

// Your time zone. Copy the line for your city, or look yours up in
// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
//   London / Dublin / Lisbon        "GMT0BST,M3.5.0/1,M10.5.0"
//   Amsterdam / Berlin / Paris      "CET-1CEST,M3.5.0,M10.5.0/3"
//   Athens / Helsinki / Kyiv        "EET-2EEST,M3.5.0/3,M10.5.0/4"
//   Moscow                          "MSK-3"
//   Istanbul                        "<+03>-3"
//   Dubai                           "<+04>-4"
//   New York                        "EST5EDT,M3.2.0,M11.1.0"
//   Los Angeles                     "PST8PDT,M3.2.0,M11.1.0"
//   Tokyo                           "JST-9"
#define TIMEZONE  "GMT0BST,M3.5.0/1,M10.5.0"

#define USE_24_HOUR          1    // 0 = 12-hour clock with AM/PM
#define SHOW_SECONDS         1    // 0 = hours and minutes only
#define SCREEN_ON_SECONDS    15   // screen turns off after this long; 0 = never
#define STAY_ON_WHEN_PLUGGED 1    // 1 = keep the screen on while on USB power
#define SCREEN_ROTATION      1    // 1 or 3. Flip this if the screen is upside down
#define SYNC_EVERY_HOURS     24   // how often to check the time online
#define UPLOAD_DELAY_S       20   // rough gap between compiling and the watch starting

const uint8_t  BRIGHTNESS[]      = { 30, 90, 200 };   // side button cycles these
const uint32_t HOLD_TO_SYNC_MS   = 2000;
const uint32_t WIFI_TIMEOUT_MS   = 20000;  // give up joining WiFi after this
const uint32_t NTP_TIMEOUT_MS    = 15000;  // give up waiting for a time server after this
const uint32_t RETRY_AFTER_MS    = 60UL * 60 * 1000;  // after a failed check, try again in 1 h

#define SCREEN_W 240
#define SCREEN_H 135
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// ============================================================================
//  COLOURS
// ============================================================================
const uint16_t C_BG     = 0x0000;
const uint16_t C_TIME   = RGB565(255, 255, 255);
const uint16_t C_TEXT   = RGB565(200, 200, 215);
const uint16_t C_DIM    = RGB565(110, 110, 130);
const uint16_t C_TRACK  = RGB565(40, 40, 55);
const uint16_t C_ACCENT = RGB565(90, 215, 255);
const uint16_t C_GOOD   = RGB565(100, 225, 160);
const uint16_t C_WARN   = RGB565(255, 200, 80);
const uint16_t C_BAD    = RGB565(255, 110, 100);

// ============================================================================
//  STATE
// ============================================================================
enum SyncState : uint8_t { SYNC_IDLE, SYNC_JOINING, SYNC_WAITING_NTP };

// Anything before 1 Jan 2025 means the clock chip was never set.
const time_t MIN_VALID_TIME = 1735689600;

M5Canvas    canvas(&M5.Display);
Preferences prefs;

bool      clockKnown   = false;   // false = clock lost power and nothing has set it since
bool      fromUpload   = false;   // clock was last set from the compile time
time_t    lastSync     = 0;       // last good internet check (UTC), 0 = never
bool      lastSyncFailed = false;
SyncState syncState    = SYNC_IDLE;
uint32_t  syncStepMs   = 0;
uint32_t  nextSyncMs   = 0;
bool      syncPlanned  = false;
volatile bool ntpArrived = false;

bool      screenOn     = true;
uint32_t  lastPressMs  = 0;
uint8_t   brightIdx    = 1;
bool      pluggedIn    = false;
uint32_t  lastPowerMs  = 0;
int32_t   batteryPct   = -1;
bool      holdHandled  = false;

char      toastText[48] = "";
uint32_t  toastUntilMs  = 0;
time_t    lastDrawn     = 0;

static const char* const WEEKDAYS[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char* const MONTHS[]   = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// ============================================================================
//  CLOCK
// ============================================================================
bool wifiConfigured() { return WIFI_SSID[0] != '\0'; }

// Compile time ("Oct  5 2026" + "14:07:32", in the computer's local time) as UTC.
time_t buildTimeUtc() {
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = "";
  struct tm t = {};
  sscanf(__DATE__, "%3s %d %d", mon, &t.tm_mday, &t.tm_year);
  sscanf(__TIME__, "%d:%d:%d", &t.tm_hour, &t.tm_min, &t.tm_sec);
  const char* found = strstr(months, mon);
  t.tm_mon   = found ? (found - months) / 3 : 0;
  t.tm_year -= 1900;
  t.tm_isdst = -1;
  return mktime(&t);   // TZ is already set, so this does local -> UTC
}

// Set both the ESP32's own clock and the battery-backed clock chip.
void setClock(time_t utc) {
  timeval tv = { utc, 0 };
  settimeofday(&tv, nullptr);
  M5.Rtc.setDateTime(gmtime(&utc));
  clockKnown = true;
}

void saveLastSync(time_t t) {
  lastSync = t;
  prefs.putLong64("lastSync", (int64_t)t);
}

void setupClock() {
  setenv("TZ", TIMEZONE, 1);
  tzset();
  sntp_set_time_sync_notification_cb([](struct timeval*) { ntpArrived = true; });

  lastSync = (time_t)prefs.getLong64("lastSync", 0);

  // M5.begin() has already copied the clock chip into the ESP32's clock.
  time_t now = time(nullptr);
  bool rtcLooksRight = M5.Rtc.isEnabled() && !M5.Rtc.getVoltLow() && now >= MIN_VALID_TIME;
  clockKnown = rtcLooksRight;

  // First start after a new upload: if the clock is clearly wrong, use the
  // compile time. A right clock reads a little after the compile time.
  String stamp = __DATE__ " " __TIME__;
  if (prefs.getString("build", "") != stamp) {
    prefs.putString("build", stamp);
    time_t built = buildTimeUtc();
    if (!rtcLooksRight || now < built || now > built + 3600) {
      setClock(built + UPLOAD_DELAY_S);
      fromUpload = true;
      saveLastSync(0);   // the old internet check no longer describes this clock
      Serial.println("[clock] set from compile time");
    }
  }

  if (!M5.Rtc.isEnabled()) Serial.println("[clock] clock chip not found!");
  Serial.printf("[clock] known=%d fromUpload=%d lastSync=%ld\n", clockKnown, fromUpload, (long)lastSync);

  // Check online now if it's been a while (or never), otherwise when due.
  if (wifiConfigured()) {
    time_t age = time(nullptr) - lastSync;
    time_t every = (time_t)SYNC_EVERY_HOURS * 3600;
    if (!clockKnown || lastSync == 0 || age < 0 || age >= every) {
      startSync();
    } else {
      nextSyncMs  = millis() + (uint32_t)(every - age) * 1000UL;
      syncPlanned = true;
    }
  }
}

// ============================================================================
//  INTERNET TIME (WiFi is only on while checking)
// ============================================================================
void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void startSync() {
  if (!wifiConfigured() || syncState != SYNC_IDLE) return;
  Serial.printf("[sync] joining %s\n", WIFI_SSID);
  ntpArrived = false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  syncState   = SYNC_JOINING;
  syncStepMs  = millis();
  syncPlanned = false;
}

void finishSync(bool ok) {
  wifiOff();
  syncState      = SYNC_IDLE;
  lastSyncFailed = !ok;
  nextSyncMs     = millis() + (ok ? (uint32_t)SYNC_EVERY_HOURS * 3600UL * 1000UL : RETRY_AFTER_MS);
  syncPlanned    = true;
  Serial.printf("[sync] %s\n", ok ? "done" : "failed");
}

void updateSync(uint32_t nowMs) {
  if (syncState == SYNC_IDLE) {
    if (syncPlanned && (int32_t)(nowMs - nextSyncMs) >= 0) startSync();
    return;
  }

  if (syncState == SYNC_JOINING) {
    if (WiFi.status() == WL_CONNECTED) {
      configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
      syncState  = SYNC_WAITING_NTP;
      syncStepMs = nowMs;
    } else if (nowMs - syncStepMs > WIFI_TIMEOUT_MS) {
      finishSync(false);
    }
    return;
  }

  // SYNC_WAITING_NTP
  if (ntpArrived) {
    // The ESP32 clock is now exact. Copy it to the clock chip right as a new
    // second starts, because the chip only stores whole seconds.
    time_t t = time(nullptr) + 1;
    while (time(nullptr) < t) delay(1);
    M5.Rtc.setDateTime(gmtime(&t));
    clockKnown = true;
    fromUpload = false;
    saveLastSync(t);
    finishSync(true);
  } else if (nowMs - syncStepMs > NTP_TIMEOUT_MS) {
    finishSync(false);
  }
}

// ============================================================================
//  SCREEN
// ============================================================================
void toast(const char* text) {
  strlcpy(toastText, text, sizeof(toastText));
  toastUntilMs = millis() + 2500;
  lastDrawn = 0;   // redraw now
}

void wakeScreen() {
  if (!screenOn) {
    M5.Display.wakeup();
    screenOn  = true;
    lastDrawn = 0;
  }
}

void sleepScreen() {
  M5.Display.sleep();
  screenOn = false;
}

void formatAgo(time_t secs, char* out, size_t len) {
  if (secs < 120)              snprintf(out, len, "just now");
  else if (secs < 2 * 3600)    snprintf(out, len, "%ld min ago", (long)(secs / 60));
  else if (secs < 2 * 86400)   snprintf(out, len, "%ld h ago", (long)(secs / 3600));
  else                         snprintf(out, len, "%ld days ago", (long)(secs / 86400));
}

void drawBattery(int right, int top) {
  const int w = 24, h = 12;
  int x = right - w - 3;
  uint16_t col = pluggedIn ? C_ACCENT : batteryPct > 30 ? C_GOOD : batteryPct > 15 ? C_WARN : C_BAD;
  canvas.drawRoundRect(x, top, w, h, 2, C_TEXT);
  canvas.fillRect(x + w, top + 3, 3, h - 6, C_TEXT);
  if (batteryPct >= 0) {
    int fill = (w - 4) * batteryPct / 100;
    if (fill < 1) fill = 1;
    canvas.fillRect(x + 2, top + 2, fill, h - 4, col);
  }
  if (pluggedIn) {   // little lightning bolt
    int cx = x + w / 2, cy = top + h / 2;
    canvas.fillTriangle(cx + 1, cy - 5, cx - 3, cy + 1, cx + 1, cy + 1, C_BG);
    canvas.fillTriangle(cx - 1, cy + 5, cx + 3, cy - 1, cx - 1, cy - 1, C_BG);
    canvas.fillTriangle(cx + 1, cy - 4, cx - 2, cy + 1, cx + 1, cy + 1, C_TIME);
    canvas.fillTriangle(cx - 1, cy + 4, cx + 2, cy - 1, cx - 1, cy - 1, C_TIME);
  }

  char pct[12];
  if (batteryPct >= 0) {
    snprintf(pct, sizeof(pct), "%ld%%", (long)batteryPct);
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(C_TEXT);
    canvas.setTextDatum(textdatum_t::baseline_right);
    canvas.drawString(pct, x - 5, top + h);
  }
}

void drawStatus(time_t now, int baseline) {
  char text[48];
  uint16_t dot;

  if ((int32_t)(toastUntilMs - millis()) > 0) {
    strlcpy(text, toastText, sizeof(text));
    dot = C_ACCENT;
  } else if (syncState != SYNC_IDLE) {
    strlcpy(text, "checking time online...", sizeof(text));
    dot = C_ACCENT;
  } else if (!clockKnown) {
    strlcpy(text, wifiConfigured() ? "clock reset, no WiFi yet" : "clock reset: add WiFi", sizeof(text));
    dot = C_BAD;
  } else if (lastSync > 0) {
    char ago[24];
    formatAgo(now - lastSync, ago, sizeof(ago));
    snprintf(text, sizeof(text), "synced %s", ago);
    // amber when the last check failed or it's been a while
    dot = (!lastSyncFailed && now - lastSync < 3 * 86400) ? C_GOOD : C_WARN;
  } else if (fromUpload) {
    strlcpy(text, wifiConfigured() ? "set at upload, no WiFi yet" : "set at upload, add WiFi", sizeof(text));
    dot = C_WARN;
  } else {
    strlcpy(text, wifiConfigured() ? "not synced yet" : "add WiFi to sync", sizeof(text));
    dot = C_WARN;
  }

  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextColor(C_DIM);
  canvas.setTextDatum(textdatum_t::baseline_left);
  canvas.fillCircle(12, baseline - 5, 4, dot);
  canvas.drawString(text, 22, baseline);
}

void drawWatch() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);

  canvas.fillSprite(C_BG);

  // --- top row: date and battery
  char date[24];
  snprintf(date, sizeof(date), "%s %d %s", WEEKDAYS[lt.tm_wday], lt.tm_mday, MONTHS[lt.tm_mon]);
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextColor(clockKnown ? C_ACCENT : C_DIM);
  canvas.setTextDatum(textdatum_t::baseline_left);
  canvas.drawString(clockKnown ? date : "time not set", 8, 20);
  drawBattery(SCREEN_W - 6, 8);

  // --- the time
  char hhmm[8];
  if (!clockKnown) {
    strlcpy(hhmm, "--:--", sizeof(hhmm));
  } else {
    int h = lt.tm_hour;
#if !USE_24_HOUR
    h %= 12;
    if (h == 0) h = 12;
#endif
    snprintf(hhmm, sizeof(hhmm), USE_24_HOUR ? "%02d:%02d" : "%d:%02d", h, lt.tm_min);
  }

  const int timeBase = 86;   // baseline of the big digits
  canvas.setFont(&fonts::DejaVu72);
  canvas.setTextColor(clockKnown ? C_TIME : C_DIM);
  canvas.setTextDatum(textdatum_t::baseline_center);
  canvas.drawString(hhmm, SCREEN_W / 2, timeBase);

  // --- under the time: AM/PM on the left, seconds on the right, and
  //     a thin bar between them that fills up across the minute
  if (clockKnown) {
    const int rowY = timeBase + 12;   // centre line of this row
    [[maybe_unused]] int barL = 14, barR = SCREEN_W - 14;
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    canvas.setTextColor(C_DIM);
#if !USE_24_HOUR
    const char* ampm = lt.tm_hour < 12 ? "AM" : "PM";
    canvas.setTextDatum(textdatum_t::baseline_left);
    canvas.drawString(ampm, barL, rowY + 6);
    barL += canvas.textWidth(ampm) + 8;
#endif
#if SHOW_SECONDS
    char ss[4];
    snprintf(ss, sizeof(ss), "%02d", lt.tm_sec);
    canvas.setTextDatum(textdatum_t::baseline_right);
    canvas.drawString(ss, barR, rowY + 6);
    barR -= canvas.textWidth(ss) + 8;
    int barW = barR - barL;
    canvas.fillRoundRect(barL, rowY - 1, barW, 3, 1, C_TRACK);
    canvas.fillRoundRect(barL, rowY - 1, barW * (lt.tm_sec + 1) / 60, 3, 1, C_ACCENT);
#endif
  }

  // --- bottom row: where the time came from
  drawStatus(now, SCREEN_H - 8);

  canvas.pushSprite(0, 0);
}

// ============================================================================
//  BUTTONS AND POWER
// ============================================================================
void handleButtons(uint32_t nowMs) {
  bool anyPress = M5.BtnA.wasPressed() || M5.BtnB.wasPressed();

  if (!screenOn) {
    if (anyPress) {   // a press on a dark screen only wakes it
      wakeScreen();
      lastPressMs = nowMs;
      holdHandled = true;   // ignore the rest of this press
    }
    return;
  }
  if (anyPress) {
    lastPressMs = nowMs;
    holdHandled = false;
  }

  // front button held: check the time online now
  if (M5.BtnA.pressedFor(HOLD_TO_SYNC_MS) && !holdHandled) {
    holdHandled = true;
    if (!wifiConfigured())          toast("add WiFi in wifi_secrets.h");
    else if (syncState != SYNC_IDLE) toast("already checking...");
    else                             startSync();
  }

  // side button: brightness
  if (M5.BtnB.wasClicked() && !holdHandled) {
    brightIdx = (brightIdx + 1) % (sizeof(BRIGHTNESS) / sizeof(BRIGHTNESS[0]));
    M5.Display.setBrightness(BRIGHTNESS[brightIdx]);
    prefs.putUChar("bright", brightIdx);
    char msg[24];
    snprintf(msg, sizeof(msg), "brightness %d of %d", brightIdx + 1,
             (int)(sizeof(BRIGHTNESS) / sizeof(BRIGHTNESS[0])));
    toast(msg);
  }

  if (M5.BtnA.wasReleased() || M5.BtnB.wasReleased()) lastPressMs = nowMs;
}

void updatePower(uint32_t nowMs) {
  if (lastPowerMs != 0 && nowMs - lastPowerMs < 2000) return;
  lastPowerMs = nowMs;
  batteryPct  = M5.Power.getBatteryLevel();
  // USB gives about 5 V; the battery never goes above 4.2 V
  bool wasPlugged = pluggedIn;
  pluggedIn   = M5.Power.isCharging() == m5::Power_Class::is_charging
             || M5.Power.getVBUSVoltage() > 4300;
  if (pluggedIn && !wasPlugged) {   // just plugged in: show the time
    wakeScreen();
    lastPressMs = nowMs;
  }
}

void updateScreenTimeout(uint32_t nowMs) {
  if (!screenOn || SCREEN_ON_SECONDS == 0) return;
  if (STAY_ON_WHEN_PLUGGED && pluggedIn) {
    lastPressMs = nowMs;
    return;
  }
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) return;
  if (nowMs - lastPressMs > (uint32_t)SCREEN_ON_SECONDS * 1000UL) sleepScreen();
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================
void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);   // also loads the time from the clock chip
  setCpuFrequencyMhz(80);   // plenty for a watch, and kinder to the battery

  prefs.begin("watch", false);
  brightIdx = prefs.getUChar("bright", 1) % (sizeof(BRIGHTNESS) / sizeof(BRIGHTNESS[0]));

  M5.Display.setRotation(SCREEN_ROTATION);
  M5.Display.setBrightness(BRIGHTNESS[brightIdx]);
  canvas.setColorDepth(16);
  canvas.createSprite(SCREEN_W, SCREEN_H);

  setupClock();
  updatePower(millis());
  lastPressMs = millis();
  drawWatch();
}

void loop() {
  M5.update();
  uint32_t nowMs = millis();

  handleButtons(nowMs);
  updatePower(nowMs);
  updateSync(nowMs);
  updateScreenTimeout(nowMs);

  if (screenOn) {
    time_t now = time(nullptr);
    bool toastEnded = toastUntilMs != 0 && (int32_t)(nowMs - toastUntilMs) >= 0;
    if (now != lastDrawn || toastEnded) {
      if (toastEnded) toastUntilMs = 0;
      lastDrawn = now;
      drawWatch();
    }
  }

  delay(screenOn ? 10 : 30);
}
