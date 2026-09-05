/*
 * ============================================================
 *  Room Controller — ESP32 + Firebase Realtime Database
 *  Board  : ESP32 Dev Module
 *  Libraries: WiFiManager (by tzapu — install via Library Manager),
 *             LittleFS (bundled with modern ESP32 board packages),
 *             WiFi, HTTPClient, WiFiClientSecure (all built-in)
 *
 *  v4.0 — Multiple profiles (sites) can share one Firebase database:
 *  - New "Profile number" field in the setup portal, alongside the
 *    Firebase Database URL — matches the number shown next to this site's
 *    profile in the PWA's Settings page. All reads/writes then go under
 *    /profiles/{n}/... instead of the database root. Leave blank to use
 *    the root directly, same as v3.0 deployments.
 *
 *  v3.0 — Configurable via captive portal, no more hardcoded secrets:
 *  - First boot (or hold BOOT/GPIO0 for 3s at power-up): the board opens
 *    its own WiFi hotspot "RoomController-Setup". Connect a phone to it,
 *    a setup page should open automatically (or browse to 192.168.4.1),
 *    fill in your home WiFi + the Firebase Database URL, tap Save.
 *  - Each room's relay/LED GPIO pins now come from Firebase
 *    (/rooms/roomN/relayPin, /rooms/roomN/ledPin) instead of a fixed
 *    array — set them from the PWA's Settings page. ledPin may be left
 *    unset ("No LED for this room") to skip the LED entirely.
 *  - Room count is however many roomN nodes exist in Firebase (up to
 *    MAX_ROOMS), instead of a fixed 6. Add/remove rooms from the PWA,
 *    then reboot this board to pick up the change.
 *  - Relay contact wiring (NC/NO) is read from /config/relayWiring, set
 *    per-profile from the PWA Settings page — see applyRelayWiringConfig().
 *
 *  Flicker fix (unchanged from v2.0):
 *  - Slot refresh does NOT call applyState during active slot
 *  - Slots parsed into temp buffer first, only copied if valid
 *  - Bad HTTP responses always skipped — state never changes
 *  - Schedule check compares seconds not just minutes
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <FS.h>
#include <LittleFS.h>

// ── Setup portal ────────────────────────────────────────────────
#define CONFIG_PATH             "/config.json"
#define CONFIG_PORTAL_AP        "RoomController-Setup"
#define CONFIG_PORTAL_PASSWORD  "setup1234"   // shown to installer — change here if desired
#define CONFIG_PORTAL_TIMEOUT_S 300           // give up and reboot after 5 min with no input
#define CONFIG_BUTTON_PIN       0             // BOOT button on most ESP32 Dev Modules

// GPIO reserved for system status (boot/WiFi/error blinks) — independent of
// any room's LED, so we always have a way to signal status even before a
// single room's pins are known. Matches the original wiring's Room-1 LED
// pin, so existing boards need no rewiring.
#define STATUS_LED_PIN 2

// Timezone: IST India = 19800 | GMT = 0 | EST = -18000
#define GMT_OFFSET_SEC  19800
#define DST_OFFSET_SEC  0

// ACTIVE LOW = most relay modules (LOW = relay ON)
// ── Relay contact wiring — NC or NO ──────────────────────────
// NC (Normally Closed): Light ON = relay de-energised = NC contact closed = GPIO HIGH
//                        When ESP32 is OFF → relay de-energised → lights ON (fail-safe ON)
// NO (Normally Open):   Light ON = relay energised = NO contact closed = GPIO LOW
//                        When ESP32 is OFF → relay de-energised → lights OFF (fail-safe OFF)
// Defaults to NC below; applyRelayWiringConfig() reads /config/relayWiring
// ("NC" or "NO", set per-profile from the PWA Settings page) at boot and
// swaps these two if the profile is configured for NO instead.
int RELAY_ON  = HIGH;   // NC default: de-energise coil → NC closed → light ON
int RELAY_OFF = LOW;    // NC default: energise coil    → NC open   → light OFF
#define LED_ON    HIGH
#define LED_OFF   LOW

// ── Room limits ──────────────────────────────────────────────────
// An ESP32 only has so many GPIOs safe to use as outputs once flash/
// strapping/UART pins are excluded. 10 rooms (relay + optional LED each)
// comfortably fits that budget — raise only if you've checked your board's
// actual free-pin count first.
#define MAX_ROOMS 10
const int PIN_NONE = -1;  // sentinel: not configured

// ── Emergency/standby light — global, not per-room ────────────
// ON whenever every room is OFF, OFF the moment any room turns on. Read
// from /config/emergencyPin (set per-profile from the PWA Settings page),
// PIN_NONE if left blank. Driven with the same RELAY_ON/RELAY_OFF the
// room relays use, so it follows the same NC/NO fail-safe convention —
// NC wiring means it's ON by default even if the ESP32 itself loses power.
int emergencyPin = PIN_NONE;

// ── End-of-slot warning — shared beeper + per-room LED blink ──
// A single controller-wide beeper (not per-room) sounds a short burst when
// any room's ACTIVE slot enters its final warnMinutes, and that room's own
// LED (the existing ledPin — no new per-room pin) blinks for the rest of the
// window. Both settings are read from /config at boot and default to OFF, so
// existing deployments that never set them behave exactly as before:
//   /config/beeperPin  → PIN_NONE if unset  → beeper never driven
//   /config/warnMinutes → <=0 if unset      → whole feature disabled
// The beeper is driven with plain digitalWrite (active buzzer), same as LEDs.
int beeperPin   = PIN_NONE;
int warnMinutes = 0;   // 0 or negative = feature disabled
#define BEEPER_ON  HIGH
#define BEEPER_OFF LOW

String firebaseUrl;  // e.g. https://your-project-default-rtdb.asia-southeast1.firebasedatabase.app

// Profiles sharing one Firebase project are namespaced under /profiles/{n} —
// set once during setup (matches whatever number the PWA's Settings page
// shows for this site's profile). Empty means no namespacing — bare /rooms
// at the database root, same as pre-v4 deployments.
String profileNum;

// ── Room state ────────────────────────────────────────────────
// daysMask: 7-bit weekday set for recurring slots — bit 0=Sun .. bit 6=Sat.
// 0 = no restriction (runs every day), which keeps pre-V1 recurring slots
// (and all non-recurring slots) behaving exactly as before.
struct Slot { int sh, sm, eh, em; bool recurring; bool activated; bool expired; int daysMask; };

// A recurring DEFINITION read from /rooms/roomN/recurring — the authoritative
// source the day buckets are generated FROM during rollover. daysMask uses the
// same bit convention as Slot (0 = every day). code/bookedBy/phone are carried
// through into the materialized day slot so the PWA/activate page keep them.
struct RecurDef {
  int  sh, sm, eh, em;
  int  daysMask;
  char code[6];      // "" = auto-approved (no code)
  char bookedBy[24];
  char phone[20];
};

struct Room {
  bool lightOn   = false;
  int  ovr       = -1;       // -1=auto  0=force OFF  1=force ON
  int  relayPin  = PIN_NONE; // set from Firebase at boot
  int  ledPin    = PIN_NONE; // PIN_NONE = no LED configured for this room
  Slot slots[10];
  int  slotCount = 0;
  RecurDef recurDefs[10];    // recurring definitions for this room
  int  recurDefCount = 0;
  char name[24];
  // End-of-slot warning state (see beeperPin/warnMinutes above). warning is
  // true only while this room is inside an active slot's final warnMinutes;
  // while true, the blink logic in loop() owns this room's LED instead of
  // setRelay(). ledBlinkOn tracks the current blink phase so the toggle is
  // non-blocking. Both stay false/unused when the feature is disabled.
  bool warning    = false;
  bool ledBlinkOn = false;
};

Room rooms[MAX_ROOMS];
int  roomCount = 0;  // how many rooms were actually found in Firebase at boot (<= MAX_ROOMS)

unsigned long lastPollTime      = 0;
unsigned long lastScheduleCheck = 0;
unsigned long lastSlotRefresh   = 0;
unsigned long lastStatusPush    = 0;
bool          timeSynced        = false; // set once in setup() from getLocalTime()'s result — an
                                          // unsynced clock's epoch day is garbage, so checkMidnight()
                                          // must not trust it to decide whether a day has passed

// Persisted across reboots (LittleFS) — lets setup() detect a day change
// that happened while this board was off/rebooting/disconnected. Also the
// single source of truth checkMidnight() advances only after a fully
// successful rollover, so a dropped connection retries instead of being
// silently skipped. -1 = no rollover recorded yet (first boot on this
// firmware, or ever) — see loadConfig()/saveConfig().
int lastRolloverDay = -1;

// Intervals
const unsigned long POLL_INTERVAL     = 3000;   // override poll every 3 sec
const unsigned long SCHEDULE_INTERVAL = 10000;  // schedule check every 10 sec
const unsigned long SLOT_REFRESH      = 10000;  // slot refresh every 10 sec — picks up activation fast
const unsigned long HEARTBEAT         = 300000; // heartbeat every 5 min

// ── End-of-slot warning timing (non-blocking) ────────────────
const unsigned long LED_BLINK_INTERVAL = 100;  // LED toggle period during a warning window (faster blink)
const unsigned long BEEP_GAP_MS        = 150;  // silence between beeps in a multi-beep burst (fixed)

// Beep pattern — configurable from the PWA Settings page, read from
// /config/beepMs and /config/beepCount at boot (reboot to apply). Defaults:
// beepOnMs = on-time of EACH beep (250ms = 0.25s); beepBurstCount = number of
// beeps (1 = single beep). Multiple beeps are separated by BEEP_GAP_MS.
unsigned long beepOnMs       = 250;
int           beepBurstCount = 1;
unsigned long lastLedBlinkToggle = 0;
// One-shot beeper burst, driven non-blocking from loop() so it never stalls
// polling. beepsRemaining>0 means a burst is in progress; beepPhaseUntil is
// the millis() deadline for the current on/off phase.
int           beepsRemaining = 0;
bool          beepOnPhase    = false;
unsigned long beepPhaseUntil = 0;

// ── Time helpers ──────────────────────────────────────────────
int nowH()    { struct tm t; getLocalTime(&t); return t.tm_hour; }
int nowMn()   { struct tm t; getLocalTime(&t); return t.tm_min;  }
int nowSec()  { struct tm t; getLocalTime(&t); return t.tm_sec;  }
// Days since the Unix epoch — unlike day-of-month (tm_mday), this never
// wraps at month/year boundaries, so a straight != comparison across a
// reboot is always correct regardless of how much time actually passed.
int currentEpochDay() { return (int)(time(nullptr) / 86400L); }
int nowMins() { return nowH() * 60 + nowMn(); }
int nowWeekday() { struct tm t; getLocalTime(&t); return t.tm_wday; } // 0=Sun..6=Sat

// True if a recurring slot runs on the current weekday. daysMask 0 = every
// day (back-compat). Non-recurring slots aren't day-restricted, so callers
// only apply this to recurring ones.
bool slotRunsToday(const Slot &sl) {
  if (sl.daysMask == 0) return true;
  return (sl.daysMask & (1 << nowWeekday())) != 0;
}

String getTime() {
  struct tm t; getLocalTime(&t);
  char buf[9]; snprintf(buf, 9, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

// "YYYY-MM-DD" for today, local time — matches the format the PWA stores
// in each slot's "date" field (todayStr() there).
String getDateStr() {
  struct tm t; getLocalTime(&t);
  char buf[11]; snprintf(buf, 11, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return String(buf);
}

// "YYYY-MM-DD" for tomorrow, local time — used to stamp recurring slots
// seeded into the slotsT (tomorrow) bucket during rollover.
String getTomorrowDateStr() {
  time_t tt = time(nullptr) + 86400L;
  struct tm t; localtime_r(&tt, &t);
  char buf[11]; snprintf(buf, 11, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return String(buf);
}
// Tomorrow's weekday (0=Sun..6=Sat).
int tomorrowWeekday() { return (nowWeekday() + 1) % 7; }

bool parseTime(String s, int &h, int &m) {
  s.trim();
  int c = s.indexOf(':');
  if (c < 0) return false;
  h = s.substring(0, c).toInt();
  m = s.substring(c + 1).toInt();
  return (h >= 0 && h < 24 && m >= 0 && m < 60);
}

// ── Status LED — system-level signalling, independent of any room ────
void flashStatusLed(int times, int ms) {
  for (int i = 0; i < times; i++) {
    digitalWrite(STATUS_LED_PIN, HIGH); delay(ms);
    digitalWrite(STATUS_LED_PIN, LOW);  delay(ms);
  }
}

// ── Relay + LED — only fires GPIO if state truly changes ──────
void setRelay(int idx, bool on) {
  // Always write to hardware — don't trust software state matches physical state
  // NC wiring: RELAY_ON=HIGH (de-energised=NC closed=light ON)
  //            RELAY_OFF=LOW (energised=NC open=light OFF)
  if (rooms[idx].relayPin >= 0) {
    digitalWrite(rooms[idx].relayPin, on ? RELAY_ON : RELAY_OFF);
  }
  // While a room is in its end-of-slot warning window, the blink logic in
  // loop() owns its LED — don't fight it here. The relay still switches
  // normally; only the LED is deferred. When the warning ends,
  // checkEndOfSlotWarnings() restores the LED to the current relay state.
  if (rooms[idx].ledPin >= 0 && !rooms[idx].warning) {
    digitalWrite(rooms[idx].ledPin, on ? LED_ON : LED_OFF);
  }
  if (rooms[idx].lightOn != on) {
    rooms[idx].lightOn = on;
    Serial.printf("[%s] Room %d → %s\n", getTime().c_str(), idx+1, on?"ON":"OFF");
  }
  updateEmergencyLight(); // re-check "are all rooms off" every time any one room's state settles
}

// ── mergeSlots — merge overlapping/adjacent slots into clean ranges ──
// Example: [9-11, 10-12] → [9-12]  [9-10, 11-12] → [9-10, 11-12]
// ── mergeSlots — only merge AUTO slots (no code required) ────
// Slots with activation codes are kept SEPARATE — each has its own
// code and activation state. Merging them would destroy that.
// Only auto-approved (no-code) slots get merged when overlapping.
void mergeSlots(int idx) {
  if (rooms[idx].slotCount < 2) return;

  // Sort slots by start time
  for (int i = 0; i < rooms[idx].slotCount - 1; i++) {
    for (int j = i + 1; j < rooms[idx].slotCount; j++) {
      int si = rooms[idx].slots[i].sh * 60 + rooms[idx].slots[i].sm;
      int sj = rooms[idx].slots[j].sh * 60 + rooms[idx].slots[j].sm;
      if (sj < si) {
        Slot tmp = rooms[idx].slots[i];
        rooms[idx].slots[i] = rooms[idx].slots[j];
        rooms[idx].slots[j] = tmp;
      }
    }
  }
  // Note: we intentionally do NOT merge slots with codes
  // Each coded slot is independent with its own activation state
  // isInSlot() checks each slot individually
}

// ── isInSlot — true if current time is in an ACTIVATED slot ──
// Slot must be both: within time range AND activated by user code
// Non-activated slots do NOT turn on the relay
bool isInSlot(int idx) {
  int nm = nowMins();
  for (int i = 0; i < rooms[idx].slotCount; i++) {
    // A recurring slot that isn't scheduled for today's weekday is ignored —
    // this is the day-of-week gate. Non-recurring slots (and recurring slots
    // with daysMask 0) are unaffected.
    if (rooms[idx].slots[i].recurring && !slotRunsToday(rooms[idx].slots[i])) continue;
    int s = rooms[idx].slots[i].sh * 60 + rooms[idx].slots[i].sm;
    int e = rooms[idx].slots[i].eh * 60 + rooms[idx].slots[i].em;
    if (nm >= s && nm < e) {
      if (rooms[idx].slots[i].activated) return true;
    }
  }
  return false;
}

// ── applyState — uses override first, then schedule ──────────
void applyState(int idx) {
  if      (rooms[idx].ovr == 1)  setRelay(idx, true);
  else if (rooms[idx].ovr == 0)  setRelay(idx, false);
  else                           setRelay(idx, isInSlot(idx));
}

void applyAllStates() {
  for (int i = 0; i < roomCount; i++) applyState(i);
}

// ── LED startup test — quick blink on every configured room LED ──
// Runs AFTER pins are known (Firebase read + pinMode), so it doubles as a
// visual confirmation that each room's LED pin is wired correctly.
void ledStartupTest() {
  for (int b = 0; b < 3; b++) {
    for (int i = 0; i < roomCount; i++) if (rooms[i].ledPin >= 0) digitalWrite(rooms[i].ledPin, LED_ON);
    delay(150);
    for (int i = 0; i < roomCount; i++) if (rooms[i].ledPin >= 0) digitalWrite(rooms[i].ledPin, LED_OFF);
    delay(100);
  }
}

// ── HTTP helpers — returns "error" on failure ─────────────────
String profilePrefix() {
  return profileNum.length() > 0 ? "/profiles/" + profileNum : "";
}

String fbGet(String path) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, firebaseUrl + profilePrefix() + path + ".json");
  http.setTimeout(5000);
  int code = http.GET();
  String result = "error";
  if (code == 200) {
    result = http.getString();
    result.trim();
  } else {
    Serial.printf("fbGet failed %s code=%d\n", path.c_str(), code);
  }
  http.end();
  return result;
}

bool fbPut(String path, String jsonValue) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, firebaseUrl + profilePrefix() + path + ".json");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);
  int code = http.PUT(jsonValue);
  http.end();
  return (code == 200);
}

// ── Push status back to Firebase ─────────────────────────────
void pushStatus(int idx) {
  String base = "/rooms/room" + String(idx + 1);
  fbPut(base + "/lightOn",  rooms[idx].lightOn ? "true" : "false");
  fbPut(base + "/lastSeen", "\"" + getTime() + "\"");
}

void pushAllStatus() {
  for (int i = 0; i < roomCount; i++) { pushStatus(i); warningAwareDelay(150); }
  // Controller-level heartbeat — a single "board last seen" timestamp for the
  // whole profile (all rooms share one ESP32, so health is board-wide, not
  // per-room). Full "YYYY-MM-DD HH:MM:SS" local time so the PWA can compute
  // how long ago it was and flag the controller as not responding. Written to
  // /status/lastSeen under this profile's namespace (via fbPut's prefix).
  fbPut("/status/lastSeen", "\"" + getDateStr() + " " + getTime() + "\"");
}

// ── Parse an integer field like "relayPin":26 — returns PIN_NONE if the
// field is missing, explicitly null, or not a number. Handles variable-
// width values (1 or 2 digit GPIO numbers), unlike fixed-width substring
// slicing used for the "HH:MM" time fields elsewhere in this file.
int parseIntField(const String &json, const String &field) {
  String key = "\"" + field + "\":";
  int idx = json.indexOf(key);
  if (idx < 0) return PIN_NONE;
  int start = idx + key.length();
  if (json.substring(start, start + 4) == "null") return PIN_NONE;
  int end = start;
  bool neg = false;
  if (end < (int)json.length() && json[end] == '-') { neg = true; end++; }
  int digitsStart = end;
  while (end < (int)json.length() && isDigit(json[end])) end++;
  if (end == digitsStart) return PIN_NONE; // no digits found — malformed, treat as unset
  int val = json.substring(digitsStart, end).toInt();
  return neg ? -val : val;
}

// ── Count contiguous roomN nodes (room1, room2, ...) up to MAX_ROOMS ──
// The PWA always keeps room numbering contiguous (renumbering on delete),
// so stopping at the first gap is a safe, simple way to size the array.
int countRooms(const String &json) {
  int n = 0;
  while (n < MAX_ROOMS) {
    String key = "\"room" + String(n + 1) + "\":{";
    if (json.indexOf(key) < 0) break;
    n++;
  }
  return n;
}

// ── Parse slots — into TEMP buffer, only copy if fully valid ──
// ── Parse recurring DEFINITIONS from /rooms/roomN/recurring ──
// Each def object: {"id","s","e","days":[..],"code","bookedBy","phone"}.
// Stored into rooms[idx].recurDefs; these are the source of truth the
// rollover generates day buckets from.
void parseRecurDefs(int idx, String json) {
  rooms[idx].recurDefCount = 0;
  if (json == "null" || json == "" || json == "error" || json.length() < 5) return;
  int pos = 0;
  while (pos < (int)json.length() && rooms[idx].recurDefCount < 10) {
    int si = json.indexOf("\"s\":\"", pos);
    int ei = json.indexOf("\"e\":\"", pos);
    if (si < 0 || ei < 0) break;
    int objStart = json.lastIndexOf('{', si);
    int objEnd   = json.indexOf('}', ei);
    String startStr = json.substring(si + 5, si + 10);
    String endStr   = json.substring(ei + 5, ei + 10);
    int sh, sm, eh, em;
    if (parseTime(startStr, sh, sm) && parseTime(endStr, eh, em) && objStart >= 0 && objEnd >= 0) {
      String obj = json.substring(objStart, objEnd + 1);
      RecurDef &d = rooms[idx].recurDefs[rooms[idx].recurDefCount];
      d.sh = sh; d.sm = sm; d.eh = eh; d.em = em;
      d.daysMask = daysMaskFromJson(obj);
      String c  = extractStringField(obj, "code");  c.toCharArray(d.code, sizeof(d.code));
      String bb = extractStringField(obj, "bookedBy"); bb.toCharArray(d.bookedBy, sizeof(d.bookedBy));
      String ph = extractStringField(obj, "phone");    ph.toCharArray(d.phone, sizeof(d.phone));
      rooms[idx].recurDefCount++;
    }
    pos = max(si, ei) + 10;
  }
}

void parseSlots(int idx, String json) {
  if (json == "null" || json == "" || json == "error" || json.length() < 5) {
    rooms[idx].slotCount = 0;
    return;
  }

  Slot tempSlots[10];
  int  tempCount = 0;
  int  pos = 0;

  while (pos < (int)json.length() && tempCount < 10) {
    int si = json.indexOf("\"s\":\"", pos);
    int ei = json.indexOf("\"e\":\"", pos);
    if (si < 0 || ei < 0) break;
    String startStr = json.substring(si + 5, si + 10);
    String endStr   = json.substring(ei + 5, ei + 10);
    int sh, sm, eh, em;
    if (parseTime(startStr, sh, sm) && parseTime(endStr, eh, em)) {
      int objStart = json.lastIndexOf('{', si);
      int objEnd   = json.indexOf('}', ei);
      bool isRecurring = false;
      bool isActivated = false;
      bool isExpired   = false;
      int  slotDaysMask = 0; // 0 = every day (see Slot.daysMask)
      if (objStart >= 0 && objEnd >= 0) {
        String slotObj = json.substring(objStart, objEnd + 1);
        // Recurring flag
        isRecurring = slotObj.indexOf("\"recurring\":true") >= 0;
        // Activated: activatedAt exists and is NOT null. This is now the SINGLE
        // source of truth for whether a slot drives the relay — for BOTH coded
        // and code-less ("Auto") slots.
        //
        // Previously a slot with "code":null was force-activated here regardless
        // of activatedAt. That made an "Auto" slot impossible to turn OFF from
        // the app: the admin's Deactivate nulled activatedAt but the firmware
        // re-activated it anyway. Now the PWA SEEDS activatedAt for a no-code
        // slot at creation (so it's on by default) and CLEARS it on Deactivate,
        // and the firmware simply honours that flag — so Deactivate actually
        // holds the relay off.
        //
        // Upgrade note: an Auto slot written by an OLDER PWA (has "code":null
        // but no activatedAt) will read as NOT activated until it's re-saved /
        // rolled over by the new PWA, which seeds activatedAt. This is the
        // intended co-upgrade behaviour (flash firmware + update PWA together).
        bool hasActivatedField = slotObj.indexOf("\"activatedAt\":") >= 0;
        bool activatedIsNull   = slotObj.indexOf("\"activatedAt\":null") >= 0;
        isActivated = (hasActivatedField && !activatedIsNull);
        // Expired flag — MUST be read back, otherwise every slot refresh
        // resets it to false and checkSchedules() re-marks it expired on the
        // next tick, spamming the log and re-writing Firebase every 10s.
        isExpired = slotObj.indexOf("\"expired\":true") >= 0;
        // Recurring weekday set — the PWA writes "days":[0..6] (0=Sun). Parse
        // the digits between [ and ] into a 7-bit mask. Absent/null/empty →
        // mask 0 = "every day" (back-compat for pre-V1 recurring slots).
        int daysKey = slotObj.indexOf("\"days\":[");
        if (daysKey >= 0) {
          int p = daysKey + 8; // just past "days":[
          while (p < (int)slotObj.length() && slotObj[p] != ']') {
            if (isDigit(slotObj[p])) {
              int d = slotObj[p] - '0';         // single-digit 0..6
              if (d >= 0 && d <= 6) slotDaysMask |= (1 << d);
            }
            p++;
          }
        }
      }
      tempSlots[tempCount++] = {sh, sm, eh, em, isRecurring, isActivated, isExpired, slotDaysMask};
    }
    pos = max(si, ei) + 10;
  }

  if (tempCount > 0 || json == "[]") {
    rooms[idx].slotCount = tempCount;
    for (int i = 0; i < tempCount; i++) rooms[idx].slots[i] = tempSlots[i];
    mergeSlots(idx);
  }
}

// ── Relay wiring (NC/NO) — set per-profile from the PWA Settings page ──
// Defaults to NC (this project's original assumption) for any value other
// than exactly "NO" — including a missing field, so existing deployments
// that never set this are unaffected.
void applyRelayWiringConfig() {
  String val = fbGet("/config/relayWiring");
  if (val == "\"NO\"") {
    RELAY_ON  = LOW;
    RELAY_OFF = HIGH;
    Serial.println("Relay wiring: Normally Open (fail-safe OFF)");
  } else {
    RELAY_ON  = HIGH;
    RELAY_OFF = LOW;
    Serial.println("Relay wiring: Normally Closed (fail-safe ON) — default");
  }
}

// ── Emergency/standby light pin — read once at boot ───────────
// /config/emergencyPin is a bare scalar (not nested in an object), so this
// parses it directly rather than via parseIntField(), which expects a
// "field": prefix inside a larger JSON blob.
void applyEmergencyPinConfig() {
  String val = fbGet("/config/emergencyPin");
  if (val == "" || val == "null" || val == "error") {
    emergencyPin = PIN_NONE;
    Serial.println("Emergency light: not configured");
  } else {
    emergencyPin = val.toInt();
    Serial.printf("Emergency light: GPIO %d\n", emergencyPin);
  }
}

// ── Shared end-of-slot warning beeper pin — read once at boot ─────────
// Same bare-scalar /config read as applyEmergencyPinConfig(). Missing/null/
// error → PIN_NONE, so no beeper is ever driven (backward-compatible off).
void applyBeeperPinConfig() {
  String val = fbGet("/config/beeperPin");
  if (val == "" || val == "null" || val == "error") {
    beeperPin = PIN_NONE;
    Serial.println("End-of-slot beeper: not configured");
  } else {
    beeperPin = val.toInt();
    Serial.printf("End-of-slot beeper: GPIO %d\n", beeperPin);
  }
}

// ── Warn-minutes-before-slot-end — read once at boot ─────────────────
// Missing/null/error → toInt() gives 0 → treated as disabled. A negative or
// zero value also disables, so the feature stays fully off for any
// deployment that hasn't explicitly opted in with a positive number.
void applyWarnMinutesConfig() {
  String val = fbGet("/config/warnMinutes");
  int m = val.toInt();
  if (val == "" || val == "null" || val == "error" || m <= 0) {
    warnMinutes = 0;
    Serial.println("End-of-slot warning: disabled");
  } else {
    warnMinutes = m;
    Serial.printf("End-of-slot warning: %d min before end\n", warnMinutes);
  }
}

// ── Beep pattern (per-beep duration + count) — read once at boot ─────
// /config/beepMs = on-time of each beep (ms), /config/beepCount = number of
// beeps. Missing/invalid → sensible defaults (250ms, 1 beep). Values are
// clamped so a bad config can't produce a 0ms beep or a negative count.
void applyBeepConfig() {
  String msVal = fbGet("/config/beepMs");
  long ms = msVal.toInt();
  beepOnMs = (msVal == "" || msVal == "null" || msVal == "error" || ms <= 0) ? 250 : (unsigned long)ms;

  // Missing/null/error → default 1 beep. An explicit 0 means "no beep" (mute;
  // the LED warning still blinks). Negatives are clamped to 0.
  String cntVal = fbGet("/config/beepCount");
  if (cntVal == "" || cntVal == "null" || cntVal == "error") {
    beepBurstCount = 1;
  } else {
    int cnt = cntVal.toInt();
    beepBurstCount = (cnt < 0) ? 0 : cnt;
  }

  Serial.printf("Beep pattern: %lu ms x %d%s\n", beepOnMs, beepBurstCount, beepBurstCount == 0 ? " (muted)" : "");
}

// Recomputed after every room state change (called from setRelay(), the
// single funnel every schedule/override/activation change already goes
// through) — ON only when every known room is currently OFF.
void updateEmergencyLight() {
  if (emergencyPin < 0) return;
  bool allOff = true;
  for (int i = 0; i < roomCount; i++) {
    if (rooms[i].lightOn) { allOff = false; break; }
  }
  digitalWrite(emergencyPin, allOff ? RELAY_ON : RELAY_OFF);
}

// ── Read all rooms from Firebase — pins, overrides, names, slots ──
// Only touches rooms[0..roomCount-1] — roomCount itself is fixed at boot
// (see setup()) so a room added in the PWA mid-day won't suddenly get a
// pin here without pinMode() ever having been called for it; that's why
// new rooms/pin changes need a reboot to take effect.
void readAllRooms() {
  String json = fbGet("/rooms");
  if (json == "" || json == "null" || json == "error") {
    Serial.println("readAllRooms: could not reach Firebase — keeping existing state");
    return;
  }

  for (int i = 0; i < roomCount; i++) {
    String key = "\"room" + String(i + 1) + "\":{";
    int start = json.indexOf(key);
    if (start < 0) continue;
    String roomJson = json.substring(start);

    // ── Pins — only overwrite relayPin if Firebase actually has a value;
    // never blank out an already-working pin because of one bad/short read.
    // ledPin's PIN_NONE is itself a valid, meaningful state, so always apply it.
    int rp = parseIntField(roomJson, "relayPin");
    if (rp >= 0) rooms[i].relayPin = rp;
    rooms[i].ledPin = parseIntField(roomJson, "ledPin");

    // ── Override — strict parsing, never reset on bad value ──
    int ovIdx = roomJson.indexOf("\"override\":");
    if (ovIdx >= 0) {
      String ovVal = roomJson.substring(ovIdx + 11, ovIdx + 16);
      ovVal.trim();
      if      (ovVal.startsWith("true"))  rooms[i].ovr = 1;
      else if (ovVal.startsWith("false")) rooms[i].ovr = 0;
      else if (ovVal.startsWith("null") || ovVal.startsWith("-1"))
                                          rooms[i].ovr = -1;
      // else: unknown/malformed — KEEP existing ovr, do not reset
    }
    // If override field missing entirely — keep existing ovr too
    // (do NOT reset to -1 just because field is absent)

    // Name
    int nameIdx = roomJson.indexOf("\"name\":\"");
    if (nameIdx >= 0) {
      int ns = nameIdx + 8;
      int ne = roomJson.indexOf("\"", ns);
      if (ne > ns) roomJson.substring(ns, ne).toCharArray(rooms[i].name, sizeof(rooms[i].name));
    }

    // Slots — Firebase stores as nested object {"slots":{"0":{...},"1":{...}}}
    // Try array format first, then object format
    int slotIdx = roomJson.indexOf("\"slots\":[");
    if (slotIdx >= 0) {
      int as = slotIdx + 8;
      int ae = roomJson.indexOf("]", as) + 1;
      parseSlots(i, roomJson.substring(as, ae));
    } else {
      // Firebase nested format — fetch directly for this room
      String slotJson = fbGet("/rooms/room" + String(i + 1) + "/slots");
      if (slotJson != "error" && slotJson != "null" && slotJson.length() > 2) {
        parseSlots(i, slotJson);
      } else {
        rooms[i].slotCount = 0;
      }
    }

    // Recurring definitions — the authoritative source rollover generates
    // day buckets from. Fetched directly (not embedded in the /rooms blob).
    String recJson = fbGet("/rooms/room" + String(i + 1) + "/recurring");
    parseRecurDefs(i, recJson);
  }
}

// ── Refresh slots only — does NOT touch relay state ──────────
// KEY FIX: separate from applyState so slot update never causes flicker
void refreshSlotsOnly() {
  for (int i = 0; i < roomCount; i++) {
    String slotJson = fbGet("/rooms/room" + String(i+1) + "/slots");
    if (slotJson != "error") {
      // Save previous state for comparison
      int  prevCount = rooms[i].slotCount;
      bool prevActivated[10] = {};
      for (int j = 0; j < rooms[i].slotCount && j < 10; j++)
        prevActivated[j] = rooms[i].slots[j].activated;

      parseSlots(i, slotJson);

      // Re-apply if slot count changed OR any activation flag changed
      bool activationChanged = false;
      if (rooms[i].slotCount != prevCount) {
        activationChanged = true;
      } else {
        for (int j = 0; j < rooms[i].slotCount; j++) {
          if (rooms[i].slots[j].activated != prevActivated[j]) {
            activationChanged = true;
            break;
          }
        }
      }

      if (activationChanged) {
        applyState(i);
      }
    }
    warningAwareDelay(150);
  }
}

// ── Poll overrides every 5 seconds ───────────────────────────
void pollOverrides() {
  for (int i = 0; i < roomCount; i++) {
    String val = fbGet("/rooms/room" + String(i + 1) + "/override");

    // Skip bad reads — NEVER change state on error
    if (val == "error" || val == "") { warningAwareDelay(100); continue; }

    int newOvr;
    if      (val == "true"  || val == "1")  newOvr = 1;
    else if (val == "false" || val == "0")  newOvr = 0;
    else if (val == "null"  || val == "-1") newOvr = -1;
    else {
      // Unknown value — skip, never change active override
      warningAwareDelay(100); continue;
    }

    // Extra guard: if manual override is active and new value is auto (-1)
    // only accept if Firebase returned full "null" string — not a short bad read
    if (rooms[i].ovr != -1 && newOvr == -1 && val.length() < 4) {
      warningAwareDelay(100); continue;
    }

    if (newOvr != rooms[i].ovr) {
      Serial.printf("Room %d override: %d → %d\n", i+1, rooms[i].ovr, newOvr);
      rooms[i].ovr = newOvr;
      applyState(i);
      pushStatus(i);
    }
    warningAwareDelay(100);
  }
}

// Extracts a quoted string field's value from a raw JSON object substring
// — e.g. extractStringField(obj, "phone") for {"phone":"9198...",...}
// returns "9198...". Returns "" if the field is missing or not a plain
// string (explicitly null, a number, etc) — callers only append it to the
// rebuilt JSON when non-empty, so a missing field is simply omitted.
String extractStringField(const String &json, const String &field) {
  String key = "\"" + field + "\":\"";
  int idx = json.indexOf(key);
  if (idx < 0) return "";
  int start = idx + key.length();
  int end = json.indexOf("\"", start);
  if (end < 0) return "";
  return json.substring(start, end);
}

// ── Midnight rollover — runs once when date changes ───────────
// Rule:
//   Recurring slots → today only, stay forever, never in tomorrow
//   One-time today, dated today     → kept as-is (see below)
//   One-time today, dated otherwise → deleted (genuinely stale)
//   One-time tomorrow → moves to today, tomorrow cleared after
// The "dated today" check matters when this runs more than once on the
// same calendar day (e.g. the PWA's force-rollover already ran earlier
// ── Rollover helpers for the day-of-week recurring model ──────
// A daysMask (bit d = weekday d) with value 0 means "every day".
bool maskRunsOnDay(int daysMask, int weekday) {
  if (daysMask == 0) return true;
  return (daysMask & (1 << weekday)) != 0;
}

// Builds the ",\"days\":[..]" JSON fragment from a daysMask (empty string when
// mask 0 = every day, matching how the PWA writes it).
String daysFieldFromMask(int daysMask) {
  if (daysMask == 0) return "";
  String out = ",\"days\":[";
  bool dfirst = true;
  for (int dd = 0; dd < 7; dd++) {
    if (daysMask & (1 << dd)) { if (!dfirst) out += ","; out += String(dd); dfirst = false; }
  }
  out += "]";
  return out;
}

// Parses a "days":[..] array out of a raw slot JSON object into a mask.
int daysMaskFromJson(const String &slotObj) {
  int mask = 0;
  int k = slotObj.indexOf("\"days\":[");
  if (k < 0) return 0;
  int p = k + 8;
  while (p < (int)slotObj.length() && slotObj[p] != ']') {
    if (isDigit(slotObj[p])) { int d = slotObj[p] - '0'; if (d >= 0 && d <= 6) mask |= (1 << d); }
    p++;
  }
  return mask;
}

// Scans a slots JSON array string and, for each RECURRING slot that runs on
// targetWd, appends a rebuilt recurring-slot JSON (dated dateStr) to `out`,
// skipping any whose s|e|daysMask key is already in `seen` (dedup). Updates
// `first` for comma handling and adds emitted keys to `seen`. Used to fold in
// recurring defs that live only in slotsT (e.g. a weekday slot created on an
// off day, seeded by the PWA into tomorrow only) so the ESP32's own rollover
// doesn't drop them.
// `seenKeys` is a running string of "|s|e|mask|" tokens already emitted, used
// for dedup via substring search (avoids pulling in STL containers).
void appendRecurringFromJson(const String &slotsJson, int targetWd, const String &dateStr,
                             String &out, bool &first, String &seenKeys) {
  if (slotsJson == "null" || slotsJson == "" || slotsJson == "error" || slotsJson.length() < 5) return;
  int pos = 0;
  while (pos < (int)slotsJson.length()) {
    int si = slotsJson.indexOf("\"s\":\"", pos);
    int ei = slotsJson.indexOf("\"e\":\"", pos);
    if (si < 0 || ei < 0) break;
    int objStart = slotsJson.lastIndexOf('{', si);
    int objEnd   = slotsJson.indexOf('}', ei);
    if (objStart >= 0 && objEnd >= 0) {
      String obj = slotsJson.substring(objStart, objEnd + 1);
      if (obj.indexOf("\"recurring\":true") >= 0) {
        String sStr = slotsJson.substring(si + 5, si + 10);
        String eStr = slotsJson.substring(ei + 5, ei + 10);
        int mask = daysMaskFromJson(obj);
        String key = "|" + sStr + "|" + eStr + "|" + String(mask) + "|";
        bool dup = seenKeys.indexOf(key) >= 0;
        if (!dup && maskRunsOnDay(mask, targetWd)) {
          seenKeys += key;
          String codeField = "", bookedByField = "", phoneField = "";
          bool hasCode = false;
          String c = extractStringField(obj, "code");
          if (c.length() > 0) { codeField = ",\"code\":\"" + c + "\""; hasCode = true; }
          String bb = extractStringField(obj, "bookedBy");
          if (bb.length() > 0) bookedByField = ",\"bookedBy\":\"" + bb + "\"";
          String ph = extractStringField(obj, "phone");
          if (ph.length() > 0) phoneField = ",\"phone\":\"" + ph + "\"";
          if (!first) out += ",";
          // No-code slot → seed activatedAt (auto-active); coded → null.
          String activatedField = hasCode ? ",\"activatedAt\":null" : ",\"activatedAt\":1";
          out += "{\"s\":\"" + sStr + "\",\"e\":\"" + eStr + "\",\"recurring\":true" +
            codeField + bookedByField + phoneField + daysFieldFromMask(mask) +
            ",\"date\":\"" + dateStr + "\"" + activatedField + "}";
          first = false;
        }
      }
    }
    pos = max(si, ei) + 10;
  }
}

// Builds a full recurring-slot JSON object for a given room slot, stamped to
// dateStr, activation reset. Preserves code/bookedBy/phone read from Firebase.
String buildRecurringSlotJson(int roomIdx, int j, const String &base, const String &dateStr) {
  char s[6], e[6];
  snprintf(s, 6, "%02d:%02d", rooms[roomIdx].slots[j].sh, rooms[roomIdx].slots[j].sm);
  snprintf(e, 6, "%02d:%02d", rooms[roomIdx].slots[j].eh, rooms[roomIdx].slots[j].em);
  String existingSlot = fbGet(base + "/slots/" + String(j));
  String codeField = "", bookedByField = "", phoneField = "";
  bool hasCode = false;
  if (existingSlot != "error" && existingSlot != "null") {
    String c = extractStringField(existingSlot, "code");
    if (c.length() > 0) { codeField = ",\"code\":\"" + c + "\""; hasCode = true; }
    String bb = extractStringField(existingSlot, "bookedBy");
    if (bb.length() > 0) bookedByField = ",\"bookedBy\":\"" + bb + "\"";
    String ph = extractStringField(existingSlot, "phone");
    if (ph.length() > 0) phoneField = ",\"phone\":\"" + ph + "\"";
  }
  String daysField = daysFieldFromMask(rooms[roomIdx].slots[j].daysMask);
  // No-code slot → seed activatedAt (auto-active); coded slot → null (waits for
  // activation). See buildDefSlotJson for the rationale.
  String activatedField = hasCode ? ",\"activatedAt\":null" : ",\"activatedAt\":1";
  return "{\"s\":\"" + String(s) + "\",\"e\":\"" + String(e) + "\",\"recurring\":true" +
    codeField + bookedByField + phoneField + daysField + ",\"date\":\"" + dateStr + "\"" + activatedField + "}";
}

// Materialize a recurring DEFINITION (rooms[roomIdx].recurDefs[k]) into a
// day-slot JSON object dated dateStr. Carries the def's stable code + booker/
// phone + days, activation reset. This is the def-driven replacement for
// buildRecurringSlotJson during rollover.
String buildDefSlotJson(int roomIdx, int k, const String &dateStr) {
  RecurDef &d = rooms[roomIdx].recurDefs[k];
  char s[6], e[6];
  snprintf(s, 6, "%02d:%02d", d.sh, d.sm);
  snprintf(e, 6, "%02d:%02d", d.eh, d.em);
  bool hasCode = strlen(d.code) > 0;
  String codeField = hasCode ? (",\"code\":\"" + String(d.code) + "\"") : "";
  String bbField   = (strlen(d.bookedBy) > 0) ? (",\"bookedBy\":\"" + String(d.bookedBy) + "\"") : "";
  String phField   = (strlen(d.phone) > 0)    ? (",\"phone\":\"" + String(d.phone) + "\"") : "";
  String daysField = daysFieldFromMask(d.daysMask);
  // No-code ("Auto") slots are active by default: seed a non-null activatedAt
  // so the relay comes on for the window (parseSlots now keys purely on
  // activatedAt, no longer force-activating on code:null). Coded slots reset to
  // null so they wait for the QR PIN / admin Activate. Sentinel 1 = "activated,
  // exact time unknown" — parseSlots only checks non-null.
  String activatedField = hasCode ? ",\"activatedAt\":null" : ",\"activatedAt\":1";
  return "{\"s\":\"" + String(s) + "\",\"e\":\"" + String(e) + "\",\"recurring\":true" +
    codeField + bbField + phField + daysField + ",\"date\":\"" + dateStr + "\"" + activatedField + "}";
}

// today) — a slot created for today AFTER that first run must not be
// treated the same as leftover junk from a previous day just because
// both happen to sit in the same "today" bucket.
// Returns false if any room's Firebase write failed (e.g. WiFi down right
// at midnight) — callers must NOT treat that as done: lastRolloverDay is
// only advanced on a fully-successful run, specifically so a dropped
// connection doesn't silently skip a day's rollover forever.
bool midnightRollover() {
  Serial.println("=== Midnight rollover ===");
  bool allOk = true;

  for (int i = 0; i < roomCount; i++) {
    String base = "/rooms/room" + String(i + 1);

    // Read tomorrow's one-time slots from Firebase
    String tomorrowJson = fbGet(base + "/slotsT");

    // ── Build new TODAY ───────────────────────────────────────
    // Keep today's recurring slots + add tomorrow's one-time slots
    String newTodayJson = "[";
    bool first = true;

    // Regenerate today's recurring slots FROM the recurring DEFINITIONS
    // (/rooms/roomN/recurring), not from whatever is in the buckets. A def
    // whose daysMask excludes today is simply not emitted into today. daysMask
    // 0 = every day. Stable code + booker/phone carried, activation reset.
    int todayWd = nowWeekday();
    for (int k = 0; k < rooms[i].recurDefCount; k++) {
      if (!maskRunsOnDay(rooms[i].recurDefs[k].daysMask, todayWd)) continue;
      if (!first) newTodayJson += ",";
      newTodayJson += buildDefSlotJson(i, k, getDateStr());
      first = false;
    }

    // Keep today's one-time slots whose OWN date is still actually
    // today — created after an earlier rollover already ran today (or
    // just now via the PWA's force-rollover), not a leftover from a
    // previous day. Copied verbatim rather than rebuilt field-by-field:
    // nothing about it needs to change since it isn't transitioning from
    // anywhere, so activatedAt/attempts/lockedUntil/expired must all
    // survive exactly as they are — a slot someone just activated must
    // not have that reset just because rollover ran again today.
    String todayDateStr = getDateStr();
    for (int j = 0; j < rooms[i].slotCount; j++) {
      if (rooms[i].slots[j].recurring) continue; // already handled above
      String existingSlot = fbGet(base + "/slots/" + String(j));
      if (existingSlot == "error" || existingSlot == "null" || existingSlot.length() < 5) continue;
      if (extractStringField(existingSlot, "date") != todayDateStr) continue; // not today — genuinely stale, drop it
      if (!first) newTodayJson += ",";
      newTodayJson += existingSlot;
      first = false;
    }

    // Move tomorrow's one-time slots into today
    // (skip any recurring ones — recurring should only be in today)
    if (tomorrowJson != "null" && tomorrowJson != "" &&
        tomorrowJson != "error" && tomorrowJson.length() > 2) {
      int pos = 0;
      while (pos < (int)tomorrowJson.length()) {
        int si = tomorrowJson.indexOf("\"s\":\"", pos);
        int ei = tomorrowJson.indexOf("\"e\":\"", pos);
        if (si < 0 || ei < 0) break;
        int objStart = tomorrowJson.lastIndexOf('{', si);
        int objEnd   = tomorrowJson.indexOf('}', ei);
        if (objStart >= 0 && objEnd >= 0) {
          String obj = tomorrowJson.substring(objStart, objEnd + 1);
          // Only move one-time slots (skip recurring)
          if (obj.indexOf("\"recurring\":true") < 0) {
            String startStr = tomorrowJson.substring(si + 5, si + 10);
            String endStr   = tomorrowJson.substring(ei + 5, ei + 10);
            if (!first) newTodayJson += ",";
            // Preserve code + the booker's name/phone (see the recurring
            // loop above for why) — omit only activatedAt, and re-stamp
            // date to today since this slot is leaving "tomorrow" now.
            String codeField2 = "", bookedByField2 = "", phoneField2 = "";
            String c2 = extractStringField(obj, "code");
            if (c2.length() > 0) codeField2 = ",\"code\":\"" + c2 + "\"";
            String bb2 = extractStringField(obj, "bookedBy");
            if (bb2.length() > 0) bookedByField2 = ",\"bookedBy\":\"" + bb2 + "\"";
            String ph2 = extractStringField(obj, "phone");
            if (ph2.length() > 0) phoneField2 = ",\"phone\":\"" + ph2 + "\"";
            // No-code slot → seed activatedAt so it's auto-active after
            // promotion; coded slot → null (re-activation required). Matches
            // the new activatedAt-only rule in parseSlots.
            String activatedField2 = (c2.length() > 0) ? ",\"activatedAt\":null" : ",\"activatedAt\":1";
            newTodayJson += "{\"s\":\"" + startStr + "\",\"e\":\"" + endStr + "\"" +
              codeField2 + bookedByField2 + phoneField2 + ",\"date\":\"" + getDateStr() + "\"" + activatedField2 + "}";
            first = false;
          }
        }
        pos = max(si, ei) + 10;
      }
    }
    newTodayJson += "]";

    // Build new TOMORROW: only the recurring slots that run on tomorrow's
    // weekday (fresh, activation reset, dated tomorrow). One-time tomorrow
    // slots were promoted into today above, so they are not carried here.
    // This is what makes the Tomorrow view show the right recurring slots
    // straight after rollover, matching the PWA's rolloverRoomLocally().
    int tomorrowWd = tomorrowWeekday();
    String tomorrowDate = getTomorrowDateStr();
    String newTomorrowJson = "[";
    bool tfirst = true;
    // Tomorrow's recurring slots, also generated FROM the definitions.
    for (int k = 0; k < rooms[i].recurDefCount; k++) {
      if (!maskRunsOnDay(rooms[i].recurDefs[k].daysMask, tomorrowWd)) continue;
      if (!tfirst) newTomorrowJson += ",";
      newTomorrowJson += buildDefSlotJson(i, k, tomorrowDate);
      tfirst = false;
    }
    newTomorrowJson += "]";

    bool ok1 = fbPut(base + "/slots",  newTodayJson);
    bool ok2 = fbPut(base + "/slotsT", newTomorrowJson);
    allOk = allOk && ok1 && ok2;
    delay(200);
  }

  if (!allOk) {
    Serial.println("=== Rollover incomplete — a Firebase write failed, will retry ===");
    return false;
  }

  readAllRooms();
  // Clear any stale end-of-slot warning state carried across the rollover —
  // yesterday's slots are gone, so no LED should still be blinking. Silence
  // the shared beeper burst too. applyAllStates() below then re-drives every
  // LED from the fresh relay state. All no-ops when the feature is disabled.
  for (int i = 0; i < roomCount; i++) { rooms[i].warning = false; rooms[i].ledBlinkOn = false; }
  beepsRemaining = 0;
  if (beeperPin >= 0) digitalWrite(beeperPin, BEEPER_OFF);
  applyAllStates();

  // Record that today's transition is now handled — this is what lets
  // setup() detect a *missed* rollover after a reboot, so update it however
  // this function was reached (normal per-minute checkMidnight(), or the
  // boot-time catch-up call). Only reached when every room's write above
  // actually succeeded — see the function comment for why that matters.
  lastRolloverDay = currentEpochDay();
  saveConfig();

  // Also expose it in Firebase — the PWA has no other way to tell whether
  // today's rollover has actually happened, since it only ever reads the
  // rooms/slots data itself, not this board's local state.
  fbPut("/config/lastRolloverEpochDay", String(lastRolloverDay));

  Serial.println("=== Rollover complete ===");
  return true;
}

// ── Adopt a rollover the PWA already forced today ─────────────
// The PWA's Settings → "Force slot rollover now" button applies the same
// rule as midnightRollover() and writes this same Firebase marker
// afterward. Without checking it here, this board would have no way to
// know that happened and would redundantly re-run its own rollover at
// the next check — resetting activation state on any slot someone
// activates between the PWA's manual run and this board's own midnight.
// Returns true if today is already covered (nothing more to do here);
// 0 from a failed/empty fbGet never satisfies >= a real epoch day, so a
// network hiccup just falls through to running midnightRollover() as usual.
bool syncRolloverMarkerFromFirebase(int todayEpochDay) {
  int remoteDay = fbGet("/config/lastRolloverEpochDay").toInt();
  if (remoteDay >= todayEpochDay) {
    lastRolloverDay = remoteDay;
    saveConfig();
    return true;
  }
  return false;
}

// ── Check if date changed — called every minute ───────────────
// Uses the same persisted lastRolloverDay/currentEpochDay() marker as the
// boot-time catch-up in setup() — one shared, retry-safe source of truth.
// If midnightRollover() fails (e.g. WiFi down right at midnight), the
// marker is deliberately left unadvanced, so this keeps retrying every
// minute until it actually succeeds instead of silently skipping that
// day's rollover until the next reboot.
void checkMidnight() {
  if (!timeSynced) return; // an unsynced clock's epoch day is meaningless
  int todayEpochDay = currentEpochDay();
  if (lastRolloverDay == -1) {
    lastRolloverDay = todayEpochDay; // first run ever — nothing to catch up on
    saveConfig();
    return;
  }
  if (todayEpochDay != lastRolloverDay) {
    if (syncRolloverMarkerFromFirebase(todayEpochDay)) {
      Serial.println("Rollover already done today via PWA — syncing marker");
      return;
    }
    midnightRollover();
  }
}

// ── Mark slot as expired in Firebase ─────────────────────────
void markSlotExpired(int roomIdx, int slotIdx) {
  String path = "/rooms/room" + String(roomIdx + 1) + "/slots";
  String slotsJson = fbGet(path);
  if (slotsJson == "error" || slotsJson == "null" || slotsJson.length() < 5) return;

  // Find and update the matching slot — add expired:true
  // Simple approach: find the slot by time and inject expired field
  char startBuf[6], endBuf[6];
  snprintf(startBuf, 6, "%02d:%02d", rooms[roomIdx].slots[slotIdx].sh, rooms[roomIdx].slots[slotIdx].sm);
  snprintf(endBuf,   6, "%02d:%02d", rooms[roomIdx].slots[slotIdx].eh, rooms[roomIdx].slots[slotIdx].em);

  String searchKey = String("\"s\":\"") + startBuf + "\"";
  int pos = slotsJson.indexOf(searchKey);
  if (pos < 0) return;

  // Find the slot object boundaries
  int objStart = slotsJson.lastIndexOf('{', pos);
  int objEnd   = slotsJson.indexOf('}', pos);
  if (objStart < 0 || objEnd < 0) return;

  String slotObj = slotsJson.substring(objStart, objEnd + 1);
  // Only mark expired if not already activated
  if (slotObj.indexOf("\"activatedAt\":null") >= 0 ||
      slotObj.indexOf("\"activatedAt\":") < 0) {
    // Add expired:true to the slot object
    String newSlotObj = slotObj.substring(0, slotObj.length() - 1);
    newSlotObj += ",\"expired\":true}";
    String newSlotsJson = slotsJson.substring(0, objStart) +
                         newSlotObj +
                         slotsJson.substring(objEnd + 1);
    fbPut(path, newSlotsJson);
    Serial.printf("Room %d slot %s-%s marked expired\n",
      roomIdx+1, startBuf, endBuf);
  }
}

// ── End-of-slot warning ──────────────────────────────────────
// True if the room is currently inside an ACTIVATED slot's final
// warnMinutes, i.e. now ∈ [slotEnd - warnMinutes, slotEnd). Only activated
// slots count — there's no point warning about a slot nobody turned on.
// Always false when the feature is disabled (warnMinutes <= 0), so callers
// need no extra guard.
bool inWarningWindow(int idx) {
  if (warnMinutes <= 0) return false;
  int nm = nowMins();
  for (int j = 0; j < rooms[idx].slotCount; j++) {
    if (!rooms[idx].slots[j].activated) continue;
    // Don't warn for a recurring slot that isn't scheduled today — same
    // day-of-week gate isInSlot() applies.
    if (rooms[idx].slots[j].recurring && !slotRunsToday(rooms[idx].slots[j])) continue;
    int e = rooms[idx].slots[j].eh * 60 + rooms[idx].slots[j].em;
    if (nm >= e - warnMinutes && nm < e) return true;
  }
  return false;
}

// Kick off the one-shot attention burst on the shared beeper. Non-blocking:
// the actual on/off toggling happens in serviceBeeper() from loop(). No-op
// if no beeper pin is configured, OR if a burst is already in progress —
// so when several rooms enter their warning window together (e.g. multiple
// slots ending at the same time), the shared bell rings exactly ONCE rather
// than being re-triggered/extended per room.
void startBeepBurst() {
  if (beeperPin < 0) return;
  if (beepBurstCount <= 0) return; // count 0 = muted: no beep (LED still blinks)
  if (beepsRemaining > 0) return;  // a ring is already sounding — don't re-arm it
  beepsRemaining = beepBurstCount;
  beepOnPhase    = true;
  beepPhaseUntil = millis() + beepOnMs;
  digitalWrite(beeperPin, BEEPER_ON);
}

// Advances the non-blocking beep burst one phase at a time. Called every
// loop() iteration; cheap no-op when nothing is beeping or no pin is set.
void serviceBeeper() {
  if (beeperPin < 0 || beepsRemaining <= 0) return;
  if (millis() < beepPhaseUntil) return;
  if (beepOnPhase) {
    // End this beep's on-phase → go silent for the gap
    digitalWrite(beeperPin, BEEPER_OFF);
    beepsRemaining--;
    beepOnPhase    = false;
    beepPhaseUntil = millis() + BEEP_GAP_MS;
  } else if (beepsRemaining > 0) {
    // Gap over → start the next beep
    digitalWrite(beeperPin, BEEPER_ON);
    beepOnPhase    = true;
    beepPhaseUntil = millis() + beepOnMs;
  }
}

// Toggles the LED of every room currently in its warning window, so it
// blinks for the duration. Rooms not warning are left untouched — their LED
// stays owned by setRelay(). Non-blocking: one shared toggle timer for all
// warning LEDs. No-op when the feature is disabled.
void serviceWarningLeds() {
  if (warnMinutes <= 0) return;
  if (millis() - lastLedBlinkToggle < LED_BLINK_INTERVAL) return;
  lastLedBlinkToggle = millis();
  bool anyWarning = false;
  for (int i = 0; i < roomCount; i++) {
    if (!rooms[i].warning || rooms[i].ledPin < 0) continue;
    anyWarning = true;
    rooms[i].ledBlinkOn = !rooms[i].ledBlinkOn;
    digitalWrite(rooms[i].ledPin, rooms[i].ledBlinkOn ? LED_ON : LED_OFF);
  }
  (void)anyWarning;
}

// A delay() replacement that keeps the warning LED blink + beeper serviced
// while it waits. The Firebase poll/push loops (pollOverrides, refreshSlotsOnly,
// pushAllStatus) each block for hundreds of ms per room plus up to a 5s HTTP
// timeout; during a plain delay()/blocking fetch the loop() can't run, so the
// blink froze mid-cycle for seconds (the "off for a few sec, then blinks"
// symptom). Slicing the wait and pumping the outputs keeps the blink rhythmic.
void warningAwareDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    serviceWarningLeds();
    serviceBeeper();
    delay(5);
  }
}

// Recomputes each room's warning flag from the clock + activated slots, and
// fires the shared beeper burst once on the rising edge (window just
// entered). On the falling edge (window ended / slot over) it restores the
// LED to its relay-driven steady state so setRelay() owns it again. Entirely
// skipped when the feature is disabled.
void checkEndOfSlotWarnings() {
  if (warnMinutes <= 0) return;
  for (int i = 0; i < roomCount; i++) {
    bool nowWarning = (rooms[i].ovr == -1) && inWarningWindow(i);
    if (nowWarning && !rooms[i].warning) {
      // Rising edge — enter warning: one attention burst, LED starts blinking.
      rooms[i].warning    = true;
      rooms[i].ledBlinkOn = false;
      startBeepBurst();
      Serial.printf("[%s] Room %d entering end-of-slot warning\n", getTime().c_str(), i + 1);
    } else if (!nowWarning && rooms[i].warning) {
      // Falling edge — leave warning: hand the LED back to relay state.
      rooms[i].warning = false;
      if (rooms[i].ledPin >= 0)
        digitalWrite(rooms[i].ledPin, rooms[i].lightOn ? LED_ON : LED_OFF);
    }
  }
}

// ── Schedule check every 10 seconds ──────────────────────────
void checkSchedules() {
  int nm = nowMins();
  for (int i = 0; i < roomCount; i++) {
    if (rooms[i].ovr != -1) continue; // manual override — skip

    // Check each slot individually for expiry detection
    for (int j = 0; j < rooms[i].slotCount; j++) {
      int slotStart = rooms[i].slots[j].sh * 60 + rooms[i].slots[j].sm;
      int slotEnd   = rooms[i].slots[j].eh * 60 + rooms[i].slots[j].em;
      bool hasCode  = true; // assume code required (safe default)

      // Slot just ended — check if it was never activated. Guard on the
      // local `expired` flag and latch it here so this fires exactly ONCE
      // per slot: markSlotExpired() only writes to Firebase, so without
      // this the check kept re-matching every 10s tick for the whole
      // minute after slotEnd, spamming the log and re-writing Firebase.
      if (nm >= slotEnd && nm <= slotEnd + 1) {
        if (!rooms[i].slots[j].activated && hasCode && !rooms[i].slots[j].expired) {
          // Slot ended without activation — mark expired (once)
          rooms[i].slots[j].expired = true; // latch locally so we don't re-fire
          markSlotExpired(i, j);
        }
      }
    }

    bool shouldOn = isInSlot(i);
    if (shouldOn != rooms[i].lightOn) {
      Serial.printf("[%s] Room %d schedule: %s → %s\n",
        getTime().c_str(), i+1,
        rooms[i].lightOn ? "ON" : "OFF",
        shouldOn ? "ON" : "OFF");
      setRelay(i, shouldOn);
      pushStatus(i);
    }
  }
}

// ── WiFi watchdog — relies on WiFi.setAutoReconnect(); this just
// detects a prolonged outage and reboots as a last-resort safety net ──
void wifiWatchdog() {
  static unsigned long disconnectedSince = 0;

  if (WiFi.status() == WL_CONNECTED) { disconnectedSince = 0; return; }

  if (disconnectedSince == 0) {
    disconnectedSince = millis();
    Serial.println("WiFi lost — waiting for auto-reconnect");
    flashStatusLed(3, 200);
  }

  if (millis() - disconnectedSince > 120000) {
    Serial.println("WiFi did not recover within 2 minutes — rebooting");
    delay(300);
    ESP.restart();
  }
}

// ── LittleFS config (Firebase URL + profile number) — set via setup portal ──
// Extracts a "field":"value" string from our own small flat JSON config —
// not for Firebase's richer JSON (see parseIntField() for that).
String readJsonStringField(const String &json, const String &field) {
  String key = "\"" + field + "\":\"";
  int idx = json.indexOf(key);
  if (idx < 0) return "";
  int start = idx + key.length();
  int end = json.indexOf("\"", start);
  if (end < 0) return "";
  return json.substring(start, end);
}

bool loadConfig() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return false;
  }
  if (!LittleFS.exists(CONFIG_PATH)) return false;
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return false;
  String json = f.readString();
  f.close();

  firebaseUrl = readJsonStringField(json, "fbUrl");
  profileNum  = readJsonStringField(json, "profileNum");
  lastRolloverDay = parseIntField(json, "lastRolloverDay"); // -1 if missing — see declaration
  return firebaseUrl.length() > 0;
}

void saveConfig() {
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) { Serial.println("Failed to save config to LittleFS"); return; }
  String json = "{\"fbUrl\":\"" + firebaseUrl + "\",\"profileNum\":\"" + profileNum +
    "\",\"lastRolloverDay\":" + String(lastRolloverDay) + "}";
  f.print(json);
  f.close();
  Serial.println("Config saved to LittleFS");
}

// ── Setup portal (WiFiManager) ────────────────────────────────
bool shouldSaveConfig = false;
void saveConfigCallback() { shouldSaveConfig = true; }

void configModeCallback(WiFiManager *wm) {
  Serial.println("=== Setup mode ===");
  Serial.printf("Connect to WiFi \"%s\" (password: %s)\n", CONFIG_PORTAL_AP, CONFIG_PORTAL_PASSWORD);
  Serial.printf("Then browse to %s if it doesn't open automatically\n", WiFi.softAPIP().toString().c_str());
  flashStatusLed(3, 150);
}

// Returns true if BOOT (GPIO0) was held low for 3s right at power-up
bool shouldForceConfigPortal() {
  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(CONFIG_BUTTON_PIN) != LOW) return false;
  Serial.println("BOOT held at power-up — checking for 3s hold to force setup mode...");
  unsigned long start = millis();
  while (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    if (millis() - start > 3000) return true;
    delay(50);
  }
  return false;
}

void runConfigPortal(bool forceReset) {
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_S);

  WiFiManagerParameter customFbUrl("fburl", "Firebase Database URL", firebaseUrl.c_str(), 200);
  WiFiManagerParameter customProfileNum("profilenum", "Profile number (from the PWA Settings page)", profileNum.c_str(), 10);
  wm.addParameter(&customFbUrl);
  wm.addParameter(&customProfileNum);

  if (forceReset) wm.resetSettings();

  bool connected = wm.autoConnect(CONFIG_PORTAL_AP, CONFIG_PORTAL_PASSWORD);

  if (shouldSaveConfig) {
    firebaseUrl = String(customFbUrl.getValue());
    firebaseUrl.trim();
    if (firebaseUrl.endsWith("/")) firebaseUrl.remove(firebaseUrl.length() - 1);
    profileNum = String(customProfileNum.getValue());
    profileNum.trim();
    saveConfig();
  }

  if (!connected) {
    Serial.println("Setup portal timed out without connecting — rebooting to try again");
    flashStatusLed(10, 150);
    delay(2000);
    ESP.restart();
  }
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n\n=== Room Controller Booting ===");

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  bool forceSetup = shouldForceConfigPortal();
  if (forceSetup) Serial.println("Forcing setup portal (BOOT held 3s)");

  loadConfig(); // pre-fills firebaseUrl from a previous setup, if any

  runConfigPortal(forceSetup);

  if (firebaseUrl.length() == 0) {
    Serial.println("No Firebase URL configured — hold BOOT for 3s at power-up to open setup. Rebooting in 5s.");
    flashStatusLed(10, 150);
    delay(5000);
    ESP.restart();
  }

  WiFi.setAutoReconnect(true);
  Serial.printf("WiFi connected — IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("Firebase URL: %s\n", firebaseUrl.c_str());
  Serial.printf("Profile: %s\n", profileNum.length() > 0 ? ("/profiles/" + profileNum).c_str() : "(none — using database root)");

  // Sync time
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.google.com");
  Serial.print("NTP");
  struct tm t; int n = 0;
  timeSynced = getLocalTime(&t);
  while (!timeSynced && n++ < 20) { delay(500); Serial.print("."); timeSynced = getLocalTime(&t); }
  Serial.println("\nTime: " + getTime());

  // Discover rooms from Firebase — this only happens here at boot. Adding a
  // room or changing a pin in the PWA needs a reboot of this board to take
  // effect, since pins must be pinMode()'d before we can safely use them.
  String roomsJson = fbGet("/rooms");
  if (roomsJson != "" && roomsJson != "null" && roomsJson != "error") {
    int found = countRooms(roomsJson);
    if (found > MAX_ROOMS) {
      Serial.printf("WARNING: Firebase has %d rooms — only the first %d fit MAX_ROOMS, the rest are ignored\n", found, MAX_ROOMS);
    }
    roomCount = min(found, MAX_ROOMS);
  }

  if (roomCount == 0) {
    Serial.println("No rooms found under /rooms in Firebase — check the PWA Settings page. Rebooting in 10s.");
    flashStatusLed(6, 300);
    delay(10000);
    ESP.restart();
  }
  Serial.printf("Found %d room(s) in Firebase\n", roomCount);

  for (int i = 0; i < roomCount; i++) snprintf(rooms[i].name, sizeof(rooms[i].name), "Room %d", i + 1);

  readAllRooms(); // fills pins/names/overrides/slots for rooms[0..roomCount-1]
  applyRelayWiringConfig();  // must run before pin init below, so RELAY_OFF is already correct
  applyEmergencyPinConfig(); // same — emergencyPin must be known before pinMode() below
  applyBeeperPinConfig();    // shared end-of-slot beeper pin (off/PIN_NONE if unconfigured)
  applyWarnMinutesConfig();  // warn window in minutes (0 = feature disabled)
  applyBeepConfig();         // per-beep duration + count (defaults 250ms, 1)

  // Configure pins now that we know which GPIOs each room actually uses
  for (int i = 0; i < roomCount; i++) {
    if (rooms[i].relayPin >= 0) {
      pinMode(rooms[i].relayPin, OUTPUT);
      digitalWrite(rooms[i].relayPin, RELAY_OFF); // start OFF — correct state restored below
    } else {
      Serial.printf("Room %d has no relay GPIO configured — set one in the PWA Settings and reboot\n", i + 1);
    }
    if (rooms[i].ledPin >= 0) {
      pinMode(rooms[i].ledPin, OUTPUT);
      digitalWrite(rooms[i].ledPin, LED_OFF);
    } else {
      Serial.printf("Room %d has no LED configured — skipping its LED indicator\n", i + 1);
    }
  }
  if (emergencyPin >= 0) {
    pinMode(emergencyPin, OUTPUT);
    digitalWrite(emergencyPin, RELAY_OFF); // start OFF — corrected below once real room states are known
  }
  // Shared end-of-slot beeper — only touch the pin if it's actually
  // configured, so unconfigured deployments never drive a random GPIO.
  if (beeperPin >= 0) {
    pinMode(beeperPin, OUTPUT);
    digitalWrite(beeperPin, BEEPER_OFF); // silent at boot
  }

  ledStartupTest();
  applyAllStates(); // also brings the emergency light to its correct state via setRelay() -> updateEmergencyLight()
  pushAllStatus();

  // Catch up on a missed rollover — a reboot, power loss, or WiFi drop
  // spanning midnight would otherwise skip that day's rollover, since
  // checkMidnight() in loop() only detects a date change while it's
  // continuously running. lastRolloverDay is persisted in LittleFS
  // specifically to survive that gap. Only run this when NTP actually
  // synced — comparing against an unsynced clock (epoch 0) would
  // otherwise look like a huge missed gap on every boot and wrongly wipe
  // active slots. If this fails too (e.g. WiFi still isn't up yet),
  // checkMidnight() will keep retrying every minute once loop() starts —
  // see its comment.
  if (timeSynced) {
    int todayEpochDay = currentEpochDay();
    if (lastRolloverDay == -1) {
      // No baseline yet (first boot on this firmware, or ever) — nothing to
      // catch up on, just record today so future boots have something to
      // compare against.
      lastRolloverDay = todayEpochDay;
      saveConfig();
    } else if (todayEpochDay != lastRolloverDay) {
      if (syncRolloverMarkerFromFirebase(todayEpochDay)) {
        Serial.println("Rollover already done today via PWA — syncing marker");
      } else {
        Serial.println("Missed rollover while offline — catching up now");
        midnightRollover(); // updates lastRolloverDay + saveConfig() itself
      }
    }
  } else {
    Serial.println("NTP never synced — skipping missed-rollover check this boot");
  }

  Serial.println("=== Ready — state restored from Firebase ===");
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {

  // Poll override every 5 seconds
  if (millis() - lastPollTime > POLL_INTERVAL) {
    lastPollTime = millis();
    if (WiFi.status() == WL_CONNECTED) pollOverrides();
  }

  // Check schedule every 10 seconds
  if (millis() - lastScheduleCheck > SCHEDULE_INTERVAL) {
    lastScheduleCheck = millis();
    checkSchedules();
    checkEndOfSlotWarnings(); // update per-room warning flags, fire beeper burst on entry
    checkMidnight();  // detect date change → rollover slots
  }

  // Non-blocking end-of-slot warning outputs — run every iteration so the
  // beep burst and LED blink are smooth. Both no-op when disabled/unconfigured.
  serviceBeeper();
  serviceWarningLeds();

  // Refresh slots only — no relay state change unless slots count changes
  if (millis() - lastSlotRefresh > SLOT_REFRESH) {
    lastSlotRefresh = millis();
    refreshSlotsOnly();
  }

  // Heartbeat every 5 minutes
  if (millis() - lastStatusPush > HEARTBEAT) {
    lastStatusPush = millis();
    pushAllStatus();
  }

  // WiFi watchdog — reboots if disconnected too long
  wifiWatchdog();

  delay(10);
}
