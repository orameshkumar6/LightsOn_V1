/*
 * ============================================================
 *  Room Controller — ESP32 + Firebase Realtime Database
 *  Board  : ESP32 Dev Module
 *  Libraries: WiFiManager (by tzapu — install via Library Manager),
 *             LittleFS (bundled with modern ESP32 board packages),
 *             WiFi, HTTPClient, WiFiClientSecure (all built-in)
 *
 *  v6.10 — Rollover self-heal (marker-vs-data desync recovery):
 *  - If the rollover marker says "done today" but a room's today bucket still
 *    holds a one-time slot stamped with a PAST date (the admin PWA or a partial
 *    run advanced the marker without this board rebuilding the slots), the
 *    firmware now detects it and forces a real midnightRollover() to drop the
 *    stale slots. Detection is ZERO extra Firebase reads — it compares today's
 *    date against a per-room date stamped for free by parseSlots() from slots
 *    already read on the normal sync cycle. Throttled to <=1 forced heal / 5 min.
 *  - (The v6.9 emergency-light timeout change was reverted; emergency logic is
 *    unchanged from v6.8.)
 *
 *  v6.8 — HTTPS connection reuse (persistent TLS client):
 *  - fbGet/fbPut/fbPatch share one long-lived WiFiClientSecure + HTTPClient
 *    with setReuse(true), so the TLS handshake (and its ~3-5 KB cert-chain
 *    download) runs ONCE instead of on every poll. Cuts RTDB "downloaded"
 *    volume dramatically at the SAME poll rate — the handshake, not the tiny
 *    JSON body, was the real cost. A transport failure (stale/dropped socket)
 *    triggers a single clean-reconnect retry, so robustness is unchanged.
 *  - No change to the Sync V2 data contract or poll intervals.
 *
 *  v6.7 — 40 slots/room + validated missing-version recovery:
 *  - Beeper is active-low and driven only by its timed state machine.
 *  - GPIO configuration/read checks never call digitalWrite().
 *  - recurringDef CREATE/UPDATE/DELETE and all Sync V2 changes retained.
 *
 *  v5.0 — ID-keyed Firebase Sync V2:
 *  - Daily generation + revision delta sync under /sync.
 *  - Stable slot IDs under /slotRecords/roomN/today/{slotId}.
 *  - Settings use independent /configSync/version and are not reset daily.
 *  - Sync cursors persist in LittleFS and generation mismatch forces full refresh.
 *  - Legacy /rooms/roomN/slots remains supported during migration.
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
#include <new>        // std::nothrow for the TLS client fallback allocation


// Forward declarations used by Sync V2.
void applyRelayWiringConfig();
void applyEmergencyPinConfig();
void applyEmergencyTimeoutConfig();
void selfHealRollover();
void applyBeeperPinConfig();
void applyWarnMinutesConfig();
void applyBeepConfig();
void setBeeperPhysicalState(bool on);
void warningAwareDelay(unsigned long ms);
void saveConfig();
void updateEmergencyLight();
void refreshSlotsOnly();
bool loadSyncMeta(long &remoteGeneration, long &remoteRevision);
bool fullSyncV2Refresh();
bool processSyncChangePayload(long revisionNo, const String &change);
bool processSyncChange(long revisionNo);
bool validateSlotsJsonForRefresh(const String &json);
bool refreshOneRoomFromLegacy(int roomIdx);
bool refreshAllSlotsFromRooms();
bool recoverMissingSyncRevision(long missingRevision);
void syncSlotsV2();
void syncConfigV2();
int daysMaskFromJson(const String &slotObj);
String extractStringField(const String &json, const String &field);
String extractRawField(const String &json, const String &field);
void catchUpMissedRollover();
bool midnightRollover();

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
#define MAX_SLOTS_PER_ROOM 40
const int PIN_NONE = -1;  // sentinel: not configured

// ── Emergency/standby light — global, not per-room ────────────
// ON whenever every room is OFF, OFF the moment any room turns on. Read
// from /config/emergencyPin (set per-profile from the PWA Settings page),
// PIN_NONE if left blank. Driven with the same RELAY_ON/RELAY_OFF the
// room relays use, so it follows the same NC/NO fail-safe convention —
// NC wiring means it's ON by default even if the ESP32 itself loses power.
int emergencyPin = PIN_NONE;

// Optional auto-off timeout for the emergency light (minutes). Read from
// /config/emergencyTimeout, 0 (or unset) = disabled = the original always-on
// behaviour. When >0, the emergency light turns OFF after it has been ON
// continuously for this many minutes, and re-arms the next time a room turns
// on (which ends the all-off standby period). emergencyOnSince is the millis()
// timestamp when the current continuous-ON period began (0 = not currently on);
// emergencyLatchedOff is set once the timeout fires so we hold it off without
// re-toggling every tick, and cleared when standby ends (a room comes on).
int  emergencyTimeoutMin  = 0;      // 0 = disabled
unsigned long emergencyOnSince = 0; // millis() when the light last turned ON
// Last level actually written to emergencyPin. -1 = never written yet, so
// the first real update always goes through. updateEmergencyLight() re-runs
// its validation (all-rooms-off check, timeout comparison) far more often
// than the pin's desired state actually changes — every room settling,
// every loop tick — so writing digitalWrite() unconditionally on every one
// of those re-checks touches the pin redundantly the whole time it's
// sitting still validated-but-unchanged. See setEmergencyLightState() below.
bool emergencyLatchedOff  = false;  // timed out, held off until room activity rearms it
bool emergencyOutputOn    = false;  // software ownership state; only command function changes GPIO  // true = timed out, held off until re-arm

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
// Beeper is powered through a relay contact. Use the SAME physical load-state
// mapping as room/emergency relays. NC default: ON=HIGH releases the relay and
// closes NC; OFF=LOW energizes the relay and opens NC. NO swaps automatically.
// Beeper relay is physically wired using COM + NO on an active-low relay input.
// Keep this polarity independent from /config/relayWiring used by room and
// emergency relays. Normal beeper state = HIGH (relay released, NO open).
// Warning state = LOW (relay energized, NO closed).
#define BEEPER_ON  LOW
#define BEEPER_OFF HIGH

String firebaseUrl;  // e.g. https://your-project-default-rtdb.asia-southeast1.firebasedatabase.app

// ── Persistent HTTPS transport (connection reuse) ─────────────
// Every fbGet/fbPut/fbPatch used to create a stack-local WiFiClientSecure and
// call http.begin()/http.end() per request. Because setInsecure() skips cert
// validation but the TLS HANDSHAKE still happens, each poll performed a full
// TLS 1.2 handshake (TCP connect + ServerHello + ~3-5 KB certificate chain +
// key exchange) just to transfer a ~15-byte body — a ~200x overhead that
// Firebase meters as "downloaded". At a 3 s override poll that handshake, not
// the data, was the dominant source of daily RTDB download volume.
//
// The transport is now long-lived and shared. With http.setReuse(true) the
// underlying TLS socket stays open across requests, so the handshake runs ONCE
// and subsequent polls ride the already-open connection (just HTTP headers +
// the tiny JSON on the wire). fbRequest() below centralises reuse + a single
// recreate-and-retry fallback so one dropped/wedged connection still just costs
// one reconnect (today's robustness) instead of wedging sync.
// The TLS client is a heap pointer (not a plain global) specifically so the
// fallback can DESTROY a client that has gone bad and build a genuinely fresh
// one — not merely stop()/begin() the same object. If a reused socket drop were
// the only failure mode, reopening the same client would be enough; but a
// WiFiClientSecure can also wedge its internal TLS/mbedTLS state (OOM mid-
// handshake, a half-closed session stop() didn't fully clear). Recreating the
// object guarantees a clean slate. fbEnsureClient() lazily (re)allocates it.
WiFiClientSecure* fbClient = nullptr;
HTTPClient        fbHttp;

// Profiles sharing one Firebase project are namespaced under /profiles/{n} —
// set once during setup (matches whatever number the PWA's Settings page
// shows for this site's profile). Empty means no namespacing — bare /rooms
// at the database root, same as pre-v4 deployments.
String profileNum;

// ── Room state ────────────────────────────────────────────────
// daysMask: 7-bit weekday set for recurring slots — bit 0=Sun .. bit 6=Sat.
// 0 = no restriction (runs every day), which keeps pre-V1 recurring slots
// (and all non-recurring slots) behaving exactly as before.
// slotTs mirrors the PWA's per-slot modified-at timestamp (written by
// pushRoom() on every create/edit). Kept as a string purely for equality
// comparison in refreshSlotsOnly() — it's a JS millisecond timestamp
// (~13 digits), too large for a 32-bit int, and nothing here ever needs
// to do arithmetic on it, only detect "did this slot's content change."
struct Slot {
  int sh, sm, eh, em;
  bool recurring;
  bool activated;
  bool expired;
  int daysMask;
  char slotTs[16];
  char id[40];          // stable Firebase record identity
  long version;         // per-record version from Sync V2
};

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
  char defId[24];    // matches the PWA's makeRecurDefId() — "" if this def
                      // predates the id field. Stamped onto every day-bucket
                      // instance materialized from it (see buildDefSlotJson)
                      // so rollover can recognize "the same occurrence" across
                      // the tomorrow→today relabeling and carry its activation
                      // forward instead of resetting it.
};

struct Room {
  bool lightOn   = false;
  int  ovr       = -1;       // -1=auto  0=force OFF  1=force ON
  int  relayPin  = PIN_NONE; // set from Firebase at boot
  int  ledPin    = PIN_NONE; // PIN_NONE = no LED configured for this room
  Slot slots[MAX_SLOTS_PER_ROOM];
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

// Per-room cache of the last-seen /rooms/roomN/slotsUpdatedAt value —
// refreshSlotsOnly() checks this cheap marker before paying for the full
// /slots download. Every writer that touches a room's /slots bumps this
// same field (the PWA's writeRoomSlots() helper; this firmware's own
// midnightRollover()/markSlotExpired()) — see refreshSlotsOnly() for why an
// empty/unset marker must never be treated as "unchanged".
String lastSlotsMarker[MAX_ROOMS];

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

// ── Rollover self-heal ────────────────────────────────────────
// Ground-truth date of what is ACTUALLY in each room's today (/slots) bucket,
// captured for free while parseSlots() parses the slots the firmware already
// reads on its normal sync cycle (NO extra Firebase reads). It holds the "date"
// field of the one-time slots in today's bucket; "" = unknown or only recurring
// slots (which carry today's date when freshly materialized).
//
// Why: lastRolloverDay/the Firebase marker can say "rolled over today" while the
// slot DATA was never actually transformed (a marker-desync — the admin PWA or a
// partial run advanced the marker, this board adopted it without rebuilding).
// Comparing this stamped date against today detects that desync from RAM alone,
// so a device that is alive + time-synced self-heals by forcing a real rollover
// instead of showing yesterday's stale slots all day.
char roomSlotsDate[MAX_ROOMS][11] = {{0}};   // "YYYY-MM-DD" per room, or ""
unsigned long lastSelfHealAttempt = 0;       // throttle: millis() of last forced heal
const unsigned long SELF_HEAL_MIN_INTERVAL = 300000UL; // >= 5 min between forced heals

// ── Firebase Sync V2 cursor (persisted in CONFIG_PATH) ─────────
long syncGeneration = 0;     // YYYYMMDD daily generation
long syncRevision = 0;       // last fully-applied daily revision
long configVersion = 0;      // persistent settings version, independent of daily sync
bool syncV2Available = false;


// Intervals
const unsigned long POLL_INTERVAL     = 3000;   // override poll every 3 sec
const unsigned long SCHEDULE_INTERVAL = 10000;  // schedule check every 10 sec
const unsigned long SLOT_REFRESH      = 10000;  // slot refresh every 10 sec — picks up activation fast
const unsigned long HEARTBEAT         = 300000; // heartbeat every 5 min
const unsigned long HEARTBEAT_RETRY   = 30000;  // retry a failed/skipped heartbeat after 30 s (PWA flags stale at 11 min)

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
// One controller-wide configured burst per aggregate warning episode.
// A dedicated task guarantees pulse timing even while HTTPS calls block loop().
TaskHandle_t beeperTaskHandle = nullptr;
bool warningEpisodeActive = false;
bool beeperHardwareInitialized = false;
bool gpioAssignmentsLocked = false; // true after setup pinMode initialization; runtime config cannot change GPIO numbers
const int MAX_WARNED_SLOT_IDS = MAX_ROOMS * MAX_SLOTS_PER_ROOM;
char warnedSlotIds[MAX_WARNED_SLOT_IDS][40] = {{0}};
int warnedSlotIdCount = 0;

// ── Time helpers ──────────────────────────────────────────────
int nowH()    { struct tm t; getLocalTime(&t); return t.tm_hour; }
int nowMn()   { struct tm t; getLocalTime(&t); return t.tm_min;  }
int nowSec()  { struct tm t; getLocalTime(&t); return t.tm_sec;  }
// Days since the Unix epoch — unlike day-of-month (tm_mday), this never
// wraps at month/year boundaries, so a straight != comparison across a
// reboot is always correct regardless of how much time actually passed.
//
// time(nullptr) is UTC seconds even after configTime(GMT_OFFSET_SEC, ...) —
// the offset only affects localtime(). Dividing it directly made the "day"
// change at 00:00 UTC (05:30 IST) instead of local midnight, so rollover ran
// 5.5 h late and yesterday's slots stayed live until then. Add the local
// offset first so this is the LOCAL calendar day number, which is also what
// the PWA's localEpochDay() writes to /config/lastRolloverEpochDay.
int currentEpochDay() { return (int)((time(nullptr) + GMT_OFFSET_SEC + DST_OFFSET_SEC) / 86400L); }
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
  if (rooms[idx].relayPin >= 0 && rooms[idx].relayPin != beeperPin) {
    digitalWrite(rooms[idx].relayPin, on ? RELAY_ON : RELAY_OFF);
  }
  // While a room is in its end-of-slot warning window, the blink logic in
  // loop() owns its LED — don't fight it here. The relay still switches
  // normally; only the LED is deferred. When the warning ends,
  // checkEndOfSlotWarnings() restores the LED to the current relay state.
  if (rooms[idx].ledPin >= 0 && rooms[idx].ledPin != beeperPin && !rooms[idx].warning) {
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
    for (int i = 0; i < roomCount; i++) if (rooms[i].ledPin >= 0 && rooms[i].ledPin != beeperPin) digitalWrite(rooms[i].ledPin, LED_ON);
    delay(150);
    for (int i = 0; i < roomCount; i++) if (rooms[i].ledPin >= 0 && rooms[i].ledPin != beeperPin) digitalWrite(rooms[i].ledPin, LED_OFF);
    delay(100);
  }
}

// ── HTTP helpers — returns "error" on failure ─────────────────
String profilePrefix() {
  return profileNum.length() > 0 ? "/profiles/" + profileNum : "";
}

// Tear the shared transport down completely: finish any in-flight HTTPClient
// request AND DESTROY the TLS client object. The next fbEnsureClient() builds a
// brand-new one. This is the real fallback — not a stop()/reopen of the same
// client, but a full recreate — so a WiFiClientSecure whose internal TLS state
// has wedged (not just a dropped socket) can never be reused in that bad state.
void fbResetConnection() {
  fbHttp.end();                 // release HTTPClient's hold on the stream first
  if (fbClient) {
    fbClient->stop();           // close the socket if still open
    delete fbClient;            // free the TLS context/buffers
    fbClient = nullptr;         // force a fresh allocation on next use
  }
}

// Lazily (re)allocate the shared TLS client. Returns false only if the ESP32 is
// out of heap for a new TLS context — in which case the caller treats it as a
// transport failure (the usual "return error, change no state" path).
bool fbEnsureClient() {
  if (!fbClient) {
    fbClient = new (std::nothrow) WiFiClientSecure();
    if (!fbClient) { Serial.println("fbEnsureClient: out of heap for TLS client"); return false; }
    fbClient->setInsecure();    // same trust model as before (no cert pinning)
  }
  return true;
}

// One shared request path for GET/PUT/PATCH so connection reuse + the
// recreate-on-failure fallback live in exactly one place.
//
// method : "GET" | "PUT" | "PATCH"
// body   : payload for PUT/PATCH (ignored for GET)
// out    : receives the response body on a 200 GET (nullptr for writes)
// returns: HTTP status code (>=100), or a negative transport error.
//
// Fallback: a reused idle socket the server/NAT silently dropped surfaces as a
// NEGATIVE code (HTTPC_ERROR_*), not an HTTP status. On that first failure we
// DESTROY the client and retry ONCE on a brand-new one — so if the existing
// connection isn't working we create a new one and use it, exactly as intended.
// A dropped keep-alive therefore costs at most one reconnect, never a wedged
// poll. A real HTTP error (404, 401, ...) is returned as-is and NOT retried.
int fbRequest(const char* method, const String& path, const String& body, String* out) {
  const String url = firebaseUrl + profilePrefix() + path + ".json";
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!fbEnsureClient()) return -1;   // no heap for a TLS client — treat as transport failure

    fbHttp.setReuse(true);      // keep the TLS socket open across requests
    fbHttp.setTimeout(5000);
    if (!fbHttp.begin(*fbClient, url)) {
      // begin() couldn't even set up — recreate the client and retry once.
      fbResetConnection();
      continue;
    }

    int code;
    if (strcmp(method, "GET") == 0) {
      code = fbHttp.GET();
    } else {
      fbHttp.addHeader("Content-Type", "application/json");
      code = (strcmp(method, "PUT") == 0) ? fbHttp.PUT(body)
                                          : fbHttp.sendRequest("PATCH", body);
    }

    if (code > 0) {
      // Request completed at the HTTP layer (may still be 4xx/5xx).
      if (out && code == 200) { *out = fbHttp.getString(); out->trim(); }
      fbHttp.end();             // reuse=true -> finishes request, keeps socket
      return code;
    }

    // Negative code = transport failure. Destroy this client and, if this was
    // the first attempt, loop to build a fresh one and try again.
    Serial.printf("fbRequest %s %s transport failed code=%d%s\n",
      method, path.c_str(), code, attempt == 0 ? " — recreating client, retrying" : " — giving up");
    fbResetConnection();
  }
  return -1;   // both attempts failed at the transport level
}

String fbGet(String path) {
  String result = "error";
  int code = fbRequest("GET", path, String(), &result);
  if (code != 200) {
    Serial.printf("fbGet failed %s code=%d\n", path.c_str(), code);
    return "error";
  }
  return result;
}

bool fbPut(String path, String jsonValue) {
  return fbRequest("PUT", path, jsonValue, nullptr) == 200;
}

// PATCH selected children without replacing siblings.
bool fbPatch(String path, String jsonValue) {
  return fbRequest("PATCH", path, jsonValue, nullptr) == 200;
}

// ── Push status back to Firebase ─────────────────────────────
// One char per room ('1' = light on), room 1 first — e.g. "1010". Lets the PWA
// read every room's light state with ONE small GET of /status instead of one
// GET per room every poll.
String lightsString() {
  String s;
  for (int i = 0; i < roomCount; i++) s += (rooms[i].lightOn ? '1' : '0');
  return s;
}

// Called when a room's state changes (and at boot). Keeps the per-room
// /rooms/roomN/lightOn for older PWAs and adds the compact /status/lights.
// The per-room "lastSeen" write that used to live here is gone: nothing reads
// it (the board heartbeat is /status), and it doubled the writes.
void pushStatus(int idx) {
  String base = "/rooms/room" + String(idx + 1);
  fbPut(base + "/lightOn",  rooms[idx].lightOn ? "true" : "false");
  fbPut("/status/lights", "\"" + lightsString() + "\"");
}

// Controller-level heartbeat — ONE PATCH for the whole board (all rooms share
// one ESP32, so health is board-wide). Previously this was 2 PUTs per room plus
// the timestamp every 5 minutes.
//   lastSeenEpoch — UTC seconds. Timezone-proof: the PWA compares it with its
//                   own clock no matter where the viewer is.
//   lastSeen      — "YYYY-MM-DD HH:MM:SS" board-local text, kept for older PWAs.
//   lights        — see lightsString().
// Returns false if the write failed or the clock isn't synced (an unsynced
// clock would publish a 1970 timestamp and make a healthy board look dead).
bool pushHeartbeat() {
  if (!timeSynced) return false;
  String body = "{\"lastSeen\":\"" + getDateStr() + " " + getTime() + "\"" +
                ",\"lastSeenEpoch\":" + String((unsigned long)time(nullptr)) +
                ",\"lights\":\"" + lightsString() + "\"}";
  return fbPatch("/status", body);
}

// Boot: write every room's lightOn once so Firebase can't keep a stale value
// from before a reboot, then the heartbeat.
void pushAllStatus() {
  for (int i = 0; i < roomCount; i++) {
    fbPut("/rooms/room" + String(i + 1) + "/lightOn", rooms[i].lightOn ? "true" : "false");
    warningAwareDelay(50);
  }
  // Start the heartbeat timer from boot: on success wait the full interval, on
  // failure (e.g. NTP not up yet) loop() retries in HEARTBEAT_RETRY ms.
  lastStatusPush = pushHeartbeat() ? millis() : millis() - (HEARTBEAT - HEARTBEAT_RETRY);
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
      String di = extractStringField(obj, "id");       di.toCharArray(d.defId, sizeof(d.defId));
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

  Slot tempSlots[MAX_SLOTS_PER_ROOM];
  int  tempCount = 0;
  int  pos = 0;
  String oneTimeDate = "";    // self-heal: date of a one-time slot in this bucket
  bool sawSlotObject = false; // true if we parsed ANY real slot object, deleted
                              // or not — distinguishes "every slot got soft-
                              // deleted, count really is 0" from "couldn't
                              // parse anything", which must NOT stomp slotCount

  while (pos < (int)json.length() && tempCount < MAX_SLOTS_PER_ROOM) {
    int si = json.indexOf("\"s\":\"", pos);
    int ei = json.indexOf("\"e\":\"", pos);
    if (si < 0 || ei < 0) break;
    String startStr = json.substring(si + 5, si + 10);
    String endStr   = json.substring(ei + 5, ei + 10);
    int sh, sm, eh, em;
    if (parseTime(startStr, sh, sm) && parseTime(endStr, eh, em)) {
      sawSlotObject = true; // saw a real slot object, even if it turns out deleted below
      int objStart = json.lastIndexOf('{', si);
      int objEnd   = json.indexOf('}', ei);
      bool isRecurring = false;
      bool isActivated = false;
      bool isExpired   = false;
      bool isDeleted   = false;
      int  slotDaysMask = 0; // 0 = every day (see Slot.daysMask)
      String slotTsStr = ""; // "" if absent — compares unequal to any real timestamp, which is fine
      String slotIdStr = "";
      long slotVersion = 0;
      if (objStart >= 0 && objEnd >= 0) {
        String slotObj = json.substring(objStart, objEnd + 1);
        slotTsStr = extractRawField(slotObj, "slotTs");
        slotIdStr = extractStringField(slotObj, "id");
        slotVersion = extractRawField(slotObj, "version").toInt();
        // Soft-deleted (see the PWA's deleteSlot()) — skip entirely, never
        // scheduled/activated. It stays in Firebase, still carrying its id,
        // until the next midnightRollover() permanently drops it.
        isDeleted = slotObj.indexOf("\"deleted\":true") >= 0;
        // Recurring flag
        isRecurring = slotObj.indexOf("\"recurring\":true") >= 0;
        // Self-heal: remember the date of a live one-time slot (zero extra read —
        // slotObj is already in hand). A one-time slot carrying a PAST date while
        // the rollover marker says "today" is the desync signal we heal on.
        if (!isDeleted && !isRecurring && oneTimeDate.length() == 0) {
          String d = extractStringField(slotObj, "date");
          if (d.length() > 0) oneTimeDate = d;
        }
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
      if (!isDeleted) {
        // char[] can't be filled via the brace initializer above, so set the
        // scalar fields there and copy slotTs in as a separate step.
        tempSlots[tempCount] = {sh, sm, eh, em, isRecurring, isActivated, isExpired, slotDaysMask};
        slotTsStr.toCharArray(tempSlots[tempCount].slotTs, sizeof(tempSlots[tempCount].slotTs));
        slotIdStr.toCharArray(tempSlots[tempCount].id, sizeof(tempSlots[tempCount].id));
        tempSlots[tempCount].version = slotVersion;
        tempCount++;
      }
    }
    pos = max(si, ei) + 10;
  }

  if (tempCount > 0 || json == "[]" || sawSlotObject) {
    rooms[idx].slotCount = tempCount;
    for (int i = 0; i < tempCount; i++) rooms[idx].slots[i] = tempSlots[i];
    mergeSlots(idx);
    // Self-heal stamp: a one-time slot's own date is the ground truth for "what
    // day is this bucket". With no one-time slots (only recurring, or empty),
    // there is nothing that can be STALE — recurring are regenerated with today's
    // date — so record today's date, which never trips the heal.
    if (idx >= 0 && idx < MAX_ROOMS) {
      String stamp = (oneTimeDate.length() > 0) ? oneTimeDate : getDateStr();
      stamp.toCharArray(roomSlotsDate[idx], sizeof(roomSlotsDate[idx]));
    }
  }
}

// ── Firebase Sync V2 ─────────────────────────────────────────
String syncRawField(const String &json, const String &field) {
  String key = "\"" + field + "\":";
  int p = json.indexOf(key);
  if (p < 0) return "";
  p += key.length();
  while (p < (int)json.length() && isspace(json[p])) p++;
  if (p < (int)json.length() && json[p] == '"') {
    int e = json.indexOf('"', p + 1);
    return e < 0 ? "" : json.substring(p + 1, e);
  }
  int comma = json.indexOf(',', p);
  int brace = json.indexOf('}', p);
  int e = comma < 0 ? brace : (brace < 0 ? comma : min(comma, brace));
  if (e < 0) e = json.length();
  String out = json.substring(p, e); out.trim(); return out;
}

bool loadSyncMeta(long &remoteGeneration, long &remoteRevision) {
  String meta = fbGet("/sync/meta");
  if (meta == "error" || meta == "null" || meta.length() < 4) return false;
  remoteGeneration = syncRawField(meta, "generation").toInt();
  remoteRevision = syncRawField(meta, "revision").toInt();
  return remoteGeneration > 0 && remoteRevision >= 0;
}

int roomIndexFromId(const String &roomId) {
  String digits = roomId;
  if (digits.startsWith("room")) digits.remove(0, 4);
  int n = digits.toInt();
  return (n >= 1 && n <= roomCount) ? n - 1 : -1;
}

bool parseOneCanonicalSlot(const String &json, Slot &out) {
  if (json == "error" || json == "null" || json.length() < 5) return false;
  String ss = extractStringField(json, "s");
  String ee = extractStringField(json, "e");
  if (!parseTime(ss, out.sh, out.sm) || !parseTime(ee, out.eh, out.em)) return false;
  out.recurring = json.indexOf("\"recurring\":true") >= 0;
  out.activated = json.indexOf("\"activatedAt\":") >= 0 && json.indexOf("\"activatedAt\":null") < 0;
  out.expired = json.indexOf("\"expired\":true") >= 0;
  out.daysMask = daysMaskFromJson(json);
  extractRawField(json, "slotTs").toCharArray(out.slotTs, sizeof(out.slotTs));
  extractStringField(json, "id").toCharArray(out.id, sizeof(out.id));
  out.version = extractRawField(json, "version").toInt();
  return json.indexOf("\"deleted\":true") < 0;
}

int findLocalSlotById(int roomIdx, const String &slotId) {
  for (int i = 0; i < rooms[roomIdx].slotCount; i++) {
    if (String(rooms[roomIdx].slots[i].id) == slotId) return i;
  }
  return -1;
}

bool applyCanonicalSlotDelta(const String &roomId, const String &bucket,
                             const String &recordId, const String &operation) {
  // ESP32 drives today's schedule only. Tomorrow changes are consumed by the
  // next daily full refresh after rollover.
  if (bucket != "today") return true;
  int ri = roomIndexFromId(roomId);
  if (ri < 0) return false;
  int existing = findLocalSlotById(ri, recordId);
  if (operation == "delete") {
    if (existing >= 0) {
      for (int i = existing; i < rooms[ri].slotCount - 1; i++) rooms[ri].slots[i] = rooms[ri].slots[i + 1];
      rooms[ri].slotCount--;
      applyState(ri);
    }
    return true;
  }
  String raw = fbGet("/slotRecords/room" + String(ri + 1) + "/today/" + recordId);

  // A slot we already hold: take only its STATE from the canonical record
  // (activation / expiry / version). The record can be partial (activate.html
  // writes just the activation fields when no full record exists) or stale
  // (pushRoom edits times in /rooms without touching /slotRecords), and the
  // old whole-slot replace then overwrote correct start/end/days with it.
  if (existing >= 0 && raw != "error" && raw != "null" && raw.length() >= 5) {
    if (raw.indexOf("\"deleted\":true") >= 0) {
      for (int i = existing; i < rooms[ri].slotCount - 1; i++) rooms[ri].slots[i] = rooms[ri].slots[i + 1];
      rooms[ri].slotCount--;
    } else {
      Slot &cur = rooms[ri].slots[existing];
      cur.activated = raw.indexOf("\"activatedAt\":") >= 0 && raw.indexOf("\"activatedAt\":null") < 0;
      cur.expired = raw.indexOf("\"expired\":true") >= 0;
      extractRawField(raw, "slotTs").toCharArray(cur.slotTs, sizeof(cur.slotTs));
      cur.version = extractRawField(raw, "version").toInt();
    }
    applyState(ri);
    return true;
  }

  Slot incoming = {};
  if (!parseOneCanonicalSlot(raw, incoming)) {
    Serial.printf("Sync V2: canonical slot unavailable room=%d id=%s; trying /rooms fallback\n",
      ri + 1, recordId.c_str());
    return refreshOneRoomFromLegacy(ri);
  }
  if (existing >= 0) rooms[ri].slots[existing] = incoming;
  else {
    if (rooms[ri].slotCount >= MAX_SLOTS_PER_ROOM) {
      Serial.printf("Sync V2: slot capacity reached for room %d (%d slots)\n", ri + 1, MAX_SLOTS_PER_ROOM);
      return false;
    }
    rooms[ri].slots[rooms[ri].slotCount++] = incoming;
  }
  mergeSlots(ri);
  applyState(ri);
  return true;
}

void refreshPersistentConfig() {
  // Settings are independent of daily slot sync. Pin changes still require a
  // reboot because pinMode is established during setup.
  applyRelayWiringConfig();
  // GPIO-number assignments are boot-locked. Runtime refresh changes behavior
  // settings only; pin changes in Firebase take effect after reboot.
  applyEmergencyTimeoutConfig();
  applyWarnMinutesConfig();
  applyBeepConfig();
  int requestedEmergencyPin = readConfiguredGpio("/config/emergencyPin");
  int requestedBeeperPin = readConfiguredGpio("/config/beeperPin");
  if (requestedEmergencyPin != emergencyPin)
    Serial.printf("Emergency GPIO change pending reboot: %d -> %d\n", emergencyPin, requestedEmergencyPin);
  if (requestedBeeperPin != beeperPin)
    Serial.printf("Beeper GPIO change pending reboot: %d -> %d\n", beeperPin, requestedBeeperPin);
  Serial.println("Sync V2: runtime settings refreshed; GPIO assignments remain locked until reboot");
}

// A full refresh reads today's slots from /rooms/roomN/slots — the projection
// the PWA (pushRoom/writeRoomSlots), activate.html and this firmware's own
// rollover all keep current. It used to read /slotRecords/.../today, but that
// store is only written for individual slot mutations (never by pushRoom),
// never cleaned, and may hold partial records: an absent node was treated as
// "no slots" (relays all OFF) and leftovers from earlier days were reloaded.
// /slotRecords is now used ONLY for incremental activation deltas.
bool fullSyncV2Refresh() {
  if (!refreshAllSlotsFromRooms()) return false;
  long g, r;
  if (!loadSyncMeta(g, r)) return false;
  syncGeneration = g;
  syncRevision = r;
  syncV2Available = true;
  saveConfig();
  return true;
}

// Apply one recurring-definition delta by stable defId. Definitions are
// persistent schedule rules; concrete today/tomorrow slot occurrences arrive
// through slot or roomSnapshot events. This function keeps the ESP32's local
// definition cache current even when the rule does not apply today/tomorrow.
int findRecurringDefById(int roomIdx, const String &defId) {
  for (int i = 0; i < rooms[roomIdx].recurDefCount; i++) {
    if (String(rooms[roomIdx].recurDefs[i].defId) == defId) return i;
  }
  return -1;
}

bool parseOneCanonicalRecurringDef(const String &json, RecurDef &out) {
  if (json == "error" || json == "null" || json.length() < 5) return false;
  if (json.indexOf("\"deleted\":true") >= 0) return false;
  String start = extractStringField(json, "s");
  String end   = extractStringField(json, "e");
  if (!parseTime(start, out.sh, out.sm) || !parseTime(end, out.eh, out.em)) return false;
  out.daysMask = daysMaskFromJson(json);
  extractStringField(json, "code").toCharArray(out.code, sizeof(out.code));
  extractStringField(json, "bookedBy").toCharArray(out.bookedBy, sizeof(out.bookedBy));
  extractStringField(json, "phone").toCharArray(out.phone, sizeof(out.phone));
  extractStringField(json, "id").toCharArray(out.defId, sizeof(out.defId));
  return strlen(out.defId) > 0;
}

bool applyRecurringDefDelta(const String &roomIdRaw, const String &defId,
                            const String &operation) {
  int roomIdx = roomIndexFromId(roomIdRaw);
  if (roomIdx < 0 || defId.length() == 0) return false;
  int existing = findRecurringDefById(roomIdx, defId);

  if (operation == "delete") {
    if (existing >= 0) {
      for (int i = existing; i < rooms[roomIdx].recurDefCount - 1; i++)
        rooms[roomIdx].recurDefs[i] = rooms[roomIdx].recurDefs[i + 1];
      rooms[roomIdx].recurDefCount--;
    }
    Serial.printf("Sync V2: recurringDef DELETE room=%d id=%s\n",
      roomIdx + 1, defId.c_str());
    return true;
  }

  String raw = fbGet("/recurringDefs/room" + String(roomIdx + 1) + "/" + defId);
  RecurDef incoming = {};
  if (!parseOneCanonicalRecurringDef(raw, incoming)) return false;

  if (existing >= 0) rooms[roomIdx].recurDefs[existing] = incoming;
  else {
    if (rooms[roomIdx].recurDefCount >= 10) {
      Serial.printf("Sync V2: recurring definition capacity reached for room %d\n", roomIdx + 1);
      return false;
    }
    rooms[roomIdx].recurDefs[rooms[roomIdx].recurDefCount++] = incoming;
  }
  Serial.printf("Sync V2: recurringDef %s room=%d id=%s\n",
    operation.c_str(), roomIdx + 1, defId.c_str());
  return true;
}

// Validate a full slots payload before allowing it to replace ESP RAM or
// advance the Sync V2 cursor. Valid empty payloads are accepted. A non-empty
// payload is accepted only when it contains at least one real slot object with
// parseable s/e values; soft-deleted slot objects still count as structurally
// valid because parseSlots() intentionally produces an empty active schedule.
bool validateSlotsJsonForRefresh(const String &json) {
  String trimmed = json;
  trimmed.trim();
  if (trimmed == "null" || trimmed == "[]") return true;
  if (trimmed == "" || trimmed == "error" || trimmed.length() < 5) return false;

  int pos = 0;
  bool sawValidSlotObject = false;
  while (pos < (int)trimmed.length()) {
    int si = trimmed.indexOf("\"s\":\"", pos);
    int ei = trimmed.indexOf("\"e\":\"", pos);
    if (si < 0 || ei < 0) break;
    int objStart = trimmed.lastIndexOf('{', min(si, ei));
    int objEnd = trimmed.indexOf('}', max(si, ei));
    if (objStart < 0 || objEnd < 0) return false;
    String startStr = trimmed.substring(si + 5, si + 10);
    String endStr = trimmed.substring(ei + 5, ei + 10);
    int sh, sm, eh, em;
    if (!parseTime(startStr, sh, sm) || !parseTime(endStr, eh, em)) return false;
    sawValidSlotObject = true;
    pos = objEnd + 1;
  }
  return sawValidSlotObject;
}

// Room-level fallback for create/copy snapshots and missing canonical records.
bool refreshOneRoomFromLegacy(int roomIdx) {
  if (roomIdx < 0 || roomIdx >= roomCount) return false;
  String raw = fbGet("/rooms/room" + String(roomIdx + 1) + "/slots");
  if (!validateSlotsJsonForRefresh(raw)) {
    Serial.printf("Sync V2: invalid /rooms slot payload for room %d; preserving previous RAM state\n",
      roomIdx + 1);
    return false;
  }
  if (raw == "null") raw = "[]";
  parseSlots(roomIdx, raw);
  applyState(roomIdx);
  Serial.printf("Sync V2: room %d refreshed from /rooms fallback (%d slots)\n",
    roomIdx + 1, rooms[roomIdx].slotCount);
  return true;
}


// Authoritative current-state reconciliation. This reads the same materialized
// Today slot projection used by the PWA. It is used when the incremental Sync V2
// history has a hole, because X+2/X+4 must never be replayed while X+1 is absent.
bool refreshAllSlotsFromRooms() {
  bool allOk = true;
  Serial.println("Sync V2: authoritative /rooms full refresh started");
  for (int i = 0; i < roomCount; i++) {
    if (!refreshOneRoomFromLegacy(i)) {
      allOk = false;
      Serial.printf("Sync V2: authoritative refresh failed for room %d\n", i + 1);
    }
    warningAwareDelay(100);
  }
  return allOk;
}

// Retry an expected revision briefly in case its Firebase write is still
// becoming visible. If it remains absent, reconcile the entire current Today
// schedule from /rooms, reread the Sync V2 head, and only then persist cursor.
bool recoverMissingSyncRevision(long missingRevision) {
  Serial.printf("Sync V2: expected revision %ld missing; retrying\n", missingRevision);

  for (int retry = 1; retry <= 2; retry++) {
    warningAwareDelay(250);
    String change = fbGet("/sync/changes/" + String(missingRevision));
    if (change != "error" && change != "null" && change.length() >= 4) {
      Serial.printf("Sync V2: revision %ld appeared on retry %d\n",
        missingRevision, retry);
      long cursorBefore = syncRevision;
      if (processSyncChangePayload(missingRevision, change)) {
        // FULL_SYNC may have advanced the cursor directly to the current head.
        // Never overwrite that newer value with the lower missing revision.
        if (syncRevision < missingRevision) syncRevision = missingRevision;
        if (syncRevision != cursorBefore) saveConfig();
        return true;
      }
      Serial.printf("Sync V2: revision %ld appeared but processing failed; full refresh required\n",
        missingRevision);
      break;
    }
  }

  Serial.printf("Sync V2: confirmed history gap at revision %ld; forcing authoritative full refresh\n",
    missingRevision);

  if (!refreshAllSlotsFromRooms()) {
    Serial.printf("Sync V2: missing-version full refresh failed; cursor remains %ld/%ld\n",
      syncGeneration, syncRevision);
    return false;
  }

  long recoveredGeneration, recoveredRevision;
  if (!loadSyncMeta(recoveredGeneration, recoveredRevision)) {
    Serial.println("Sync V2: full refresh succeeded but meta reread failed; cursor unchanged");
    return false;
  }
  if (recoveredGeneration <= 0 || recoveredRevision < missingRevision) {
    Serial.printf("Sync V2: invalid recovery head %ld/%ld for missing revision %ld; cursor unchanged\n",
      recoveredGeneration, recoveredRevision, missingRevision);
    return false;
  }

  syncGeneration = recoveredGeneration;
  syncRevision = recoveredRevision;
  syncV2Available = true;
  saveConfig();
  Serial.printf("Sync V2: missing-version full refresh complete; cursor=%ld/%ld\n",
    syncGeneration, syncRevision);
  return true;
}

bool processSyncChangePayload(long revisionNo, const String &change) {
  if (change == "error" || change == "null" || change.length() < 4) return false;
  if (syncRawField(change, "type") == "FULL_SYNC") return fullSyncV2Refresh();
  String entity = syncRawField(change, "entity");
  if (entity == "slot") {
    return applyCanonicalSlotDelta(syncRawField(change, "roomId"),
      syncRawField(change, "bucket"), syncRawField(change, "recordId"),
      syncRawField(change, "operation"));
  }
  if (entity == "recurringDef") {
    return applyRecurringDefDelta(syncRawField(change, "roomId"),
      syncRawField(change, "recordId"), syncRawField(change, "operation"));
  }
  if (entity == "roomSnapshot") {
    int roomIdx = roomIndexFromId(syncRawField(change, "roomId"));
    return refreshOneRoomFromLegacy(roomIdx);
  }
  Serial.printf("Sync V2: revision %ld has unsupported/empty entity; forcing recovery\n", revisionNo);
  return false;
}

bool processSyncChange(long revisionNo) {
  String change = fbGet("/sync/changes/" + String(revisionNo));
  return processSyncChangePayload(revisionNo, change);
}

void syncSlotsV2() {
  long remoteGeneration, remoteRevision;
  if (!loadSyncMeta(remoteGeneration, remoteRevision)) {
    syncV2Available = false;
    refreshSlotsOnly();
    return;
  }
  syncV2Available = true;

  if (syncGeneration != remoteGeneration || syncRevision > remoteRevision) {
    Serial.printf("Sync V2: generation/cursor mismatch; local=%ld/%ld remote=%ld/%ld; forcing full sync\n",
      syncGeneration, syncRevision, remoteGeneration, remoteRevision);
    if (!fullSyncV2Refresh()) {
      Serial.println("Sync V2: canonical generation recovery failed; trying authoritative /rooms refresh");
      if (refreshAllSlotsFromRooms()) {
        long g, r;
        if (loadSyncMeta(g, r)) {
          syncGeneration = g;
          syncRevision = r;
          syncV2Available = true;
          saveConfig();
        }
      }
    }
    return;
  }

  for (long revision = syncRevision + 1; revision <= remoteRevision; revision++) {
    // Check the exact expected version before processing. Later revisions are
    // never treated as substitutes for a missing X+1.
    String expectedChange = fbGet("/sync/changes/" + String(revision));
    if (expectedChange == "error" || expectedChange == "null" ||
        expectedChange.length() < 4) {
      recoverMissingSyncRevision(revision);
      return;
    }

    if (!processSyncChangePayload(revision, expectedChange)) {
      Serial.printf("Sync V2: revision %ld processing failed; forcing full canonical sync\n", revision);
      if (!fullSyncV2Refresh()) {
        Serial.printf("Sync V2: canonical recovery failed at revision %ld; trying authoritative /rooms refresh\n",
          revision);
        if (refreshAllSlotsFromRooms()) {
          long recoveredGeneration, recoveredRevision;
          if (loadSyncMeta(recoveredGeneration, recoveredRevision)) {
            syncGeneration = recoveredGeneration;
            syncRevision = recoveredRevision;
            syncV2Available = true;
            saveConfig();
            Serial.printf("Sync V2: /rooms recovery complete; generation=%ld revision=%ld\n",
              syncGeneration, syncRevision);
          } else {
            Serial.println("Sync V2: /rooms recovery succeeded but meta reread failed; cursor unchanged");
          }
        } else {
          Serial.printf("Sync V2: all recovery paths failed at revision %ld; cursor remains %ld/%ld\n",
            revision, syncGeneration, syncRevision);
        }
      } else {
        Serial.printf("Sync V2: canonical recovery complete; generation=%ld revision=%ld\n",
          syncGeneration, syncRevision);
      }
      return;
    }

    // Advance exactly one revision only after its state was applied.
    syncRevision = revision;
    saveConfig();
  }
}

void syncConfigV2() {
  String raw = fbGet("/configSync/version");
  if (raw == "error" || raw == "null" || raw.length() == 0) return;
  long remote = raw.toInt();
  if (remote == configVersion) return;
  refreshPersistentConfig();
  configVersion = remote;
  saveConfig();
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

int readConfiguredGpio(const String &path) {
  String val = fbGet(path);
  if (val == "" || val == "null" || val == "error") return PIN_NONE;
  return val.toInt();
}

// ── Emergency/standby light pin — read once at boot ───────────
// /config/emergencyPin is a bare scalar (not nested in an object), so this
// parses it directly rather than via parseIntField(), which expects a
// "field": prefix inside a larger JSON blob.
void applyEmergencyPinConfig() {
  int requestedPin = readConfiguredGpio("/config/emergencyPin");
  if (!gpioAssignmentsLocked) {
    emergencyPin = requestedPin;
    if (emergencyPin < 0) Serial.println("Emergency light: not configured");
    else Serial.printf("Emergency light: GPIO %d (locked after boot)\n", emergencyPin);
    return;
  }
  if (requestedPin != emergencyPin) {
    Serial.printf("Emergency GPIO config changed %d -> %d; ignored until reboot\n", emergencyPin, requestedPin);
  }
}

// ── Emergency light auto-off timeout — read once at boot ──────────────
// Same bare-scalar /config read. Missing/null/error/<=0 → 0 (disabled), so the
// emergency light stays always-on for setups that haven't opted in.
void applyEmergencyTimeoutConfig() {
  String val = fbGet("/config/emergencyTimeout");
  int m = val.toInt();
  if (val == "" || val == "null" || val == "error" || m <= 0) {
    emergencyTimeoutMin = 0;
    Serial.println("Emergency light auto-off: disabled");
  } else {
    emergencyTimeoutMin = m;
    Serial.printf("Emergency light auto-off: %d min\n", emergencyTimeoutMin);
  }
}

// ── Shared end-of-slot warning beeper pin — read once at boot ─────────
// Same bare-scalar /config read as applyEmergencyPinConfig(). Missing/null/
// error → PIN_NONE, so no beeper is ever driven (backward-compatible off).
void applyBeeperPinConfig() {
  int requestedPin = readConfiguredGpio("/config/beeperPin");
  if (!gpioAssignmentsLocked) {
    beeperPin = requestedPin;
    if (beeperPin < 0) Serial.println("End-of-slot beeper: not configured");
    else Serial.printf("End-of-slot beeper: GPIO %d (locked after boot)\n", beeperPin);
    return;
  }
  if (requestedPin != beeperPin) {
    Serial.printf("Beeper GPIO config changed %d -> %d; ignored until reboot\n", beeperPin, requestedPin);
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
  if (msVal == "" || msVal == "null" || msVal == "error" || ms <= 0) beepOnMs = 250;
  else beepOnMs = (unsigned long)constrain(ms, 20L, 5000L);

  // Missing/null/error → default 1 beep. An explicit 0 means "no beep" (mute;
  // the LED warning still blinks). Negatives are clamped to 0.
  String cntVal = fbGet("/config/beepCount");
  if (cntVal == "" || cntVal == "null" || cntVal == "error") {
    beepBurstCount = 1;
  } else {
    int cnt = cntVal.toInt();
    beepBurstCount = constrain(cnt, 0, 10);
  }

  Serial.printf("Beep pattern: %lu ms x %d%s\n", beepOnMs, beepBurstCount, beepBurstCount == 0 ? " (muted)" : "");
}

// Only actually touches the pin when the desired level differs from what
// was last written — updateEmergencyLight() re-validates (all-rooms-off
// check, timeout comparison) far more often than the answer actually
// changes, so calling digitalWrite() unconditionally on every one of those
// re-checks was toggling the pin the whole time it sat there
// validated-but-unchanged.
void setEmergencyLightState(bool on) {
  if (emergencyPin < 0 || emergencyPin == beeperPin) return;
  if (emergencyOutputOn == on) return; // verification only, no redundant GPIO assignment
  digitalWrite(emergencyPin, on ? RELAY_ON : RELAY_OFF);
  emergencyOutputOn = on;
  Serial.printf("[%s] Emergency light -> %s\n", getTime().c_str(), on ? "ON" : "OFF");
}

// Recomputed after every room state change (called from setRelay(), the
// single funnel every schedule/override/activation change already goes
// through) — ON only when every known room is currently OFF.
void updateEmergencyLight() {
  if (emergencyPin < 0 || emergencyPin == beeperPin) return;
  bool allOff = true;
  for (int i = 0; i < roomCount; i++) {
    if (rooms[i].lightOn) { allOff = false; break; }
  }

  if (!allOff) {
    // A room is on → emergency light off, and reset the standby timer + latch
    // so the timeout re-arms for the NEXT all-off period (re-arm on activity).
    setEmergencyLightState(false);
    emergencyOnSince = 0;
    emergencyLatchedOff = false;
    return;
  }

  // All rooms are off → the emergency light wants to be ON.
  if (emergencyTimeoutMin <= 0) {
    // No timeout configured — original always-on-while-standby behaviour.
    setEmergencyLightState(true);
    return;
  }

  // Timeout is configured. Start the standby timer on the rising edge (the
  // moment we enter the all-off period).
  if (emergencyOnSince == 0 && !emergencyLatchedOff) {
    emergencyOnSince = millis();
  }
  // If we've already timed out this standby period, keep it off.
  if (emergencyLatchedOff) {
    setEmergencyLightState(false);
    return;
  }
  // Still within the allowed window → on; past it → latch off.
  if (millis() - emergencyOnSince >= (unsigned long)emergencyTimeoutMin * 60000UL) {
    emergencyLatchedOff = true;
    setEmergencyLightState(false);
    Serial.printf("[%s] Emergency light auto-off after %d min standby\n", getTime().c_str(), emergencyTimeoutMin);
  } else {
    setEmergencyLightState(true);
  }
}

// ── Bounded JSON helpers ─────────────────────────────────────
// Index of the bracket that closes the '{' or '[' at openIdx, skipping quoted
// strings. -1 if the payload is truncated/unbalanced. Used so a per-room or
// per-array parse can never read past its own object into the next room's data
// (Firebase drops null fields, so "field missing" and "field of the NEXT room"
// looked identical to a plain indexOf() on the rest of the string).
int findMatchingClose(const String &json, int openIdx) {
  if (openIdx < 0 || openIdx >= (int)json.length()) return -1;
  char open = json[openIdx];
  if (open != '{' && open != '[') return -1;
  char close = (open == '{') ? '}' : ']';
  int depth = 0;
  bool inStr = false;
  for (int i = openIdx; i < (int)json.length(); i++) {
    char c = json[i];
    if (inStr) {
      if (c == '\\') i++;
      else if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') inStr = true;
    else if (c == open) depth++;
    else if (c == close) { if (--depth == 0) return i; }
  }
  return -1;
}

// Firebase child key (array index or object key) of the slot whose "id" equals
// slotId inside a /rooms/roomN/slots payload, or "" if absent. Firebase returns
// a dense list as a JSON array and a sparse one as an object, so handle both.
// The firmware's own slot array is compacted/merged in RAM, so its index is NOT
// the Firebase key — writing by RAM index could flag the wrong slot.
String findSlotFirebaseKey(const String &slotsJson, const char *slotId) {
  if (!slotId || !slotId[0]) return "";
  String needle = "\"id\":\"" + String(slotId) + "\"";
  int n = slotsJson.length();
  int i = 0;
  while (i < n && isspace(slotsJson[i])) i++;
  if (i >= n) return "";
  bool isArray = (slotsJson[i] == '[');
  if (!isArray && slotsJson[i] != '{') return "";
  int pos = i + 1;
  int arrayIdx = 0;
  while (pos < n) {
    while (pos < n && (isspace(slotsJson[pos]) || slotsJson[pos] == ',')) pos++;
    if (pos >= n || slotsJson[pos] == ']' || slotsJson[pos] == '}') break;
    String key;
    if (isArray) {
      key = String(arrayIdx);
    } else {
      if (slotsJson[pos] != '"') break;
      int ke = slotsJson.indexOf('"', pos + 1);
      if (ke < 0) break;
      key = slotsJson.substring(pos + 1, ke);
      pos = ke + 1;
      while (pos < n && (isspace(slotsJson[pos]) || slotsJson[pos] == ':')) pos++;
    }
    arrayIdx++;
    if (pos >= n) break;
    char c = slotsJson[pos];
    int end;
    if (c == '{' || c == '[') {
      end = findMatchingClose(slotsJson, pos);
      if (end < 0) break;
      if (c == '{' && slotsJson.substring(pos, end + 1).indexOf(needle) >= 0) return key;
    } else {
      // scalar or null — skip to its last character
      end = pos;
      while (end + 1 < n && slotsJson[end + 1] != ',' && slotsJson[end + 1] != '}' && slotsJson[end + 1] != ']') end++;
    }
    pos = end + 1;
  }
  return "";
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
    // Bound the parse to THIS room's own object. It used to be substring(start)
    // — everything to the end of /rooms — so a field absent from this room
    // (Firebase drops nulls: no ledPin, no override) was silently read from the
    // next room instead.
    int openBrace = start + key.length() - 1;
    int closeBrace = findMatchingClose(json, openBrace);
    if (closeBrace < 0) {
      Serial.printf("readAllRooms: room %d payload truncated — keeping existing state\n", i + 1);
      continue;
    }
    String roomJson = json.substring(openBrace, closeBrace + 1);

    // ── Pins — only overwrite relayPin if Firebase actually has a value;
    // never blank out an already-working pin because of one bad/short read.
    // ledPin's PIN_NONE is itself a valid, meaningful state, so always apply it.
    // GPIO numbers are boot-locked (pinMode was already run for the old ones),
    // so this function — which also re-runs after every rollover — only assigns
    // pins before setup() locks them.
    if (!gpioAssignmentsLocked) {
      int rp = parseIntField(roomJson, "relayPin");
      if (rp >= 0) rooms[i].relayPin = rp;
      rooms[i].ledPin = parseIntField(roomJson, "ledPin");
    }

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
    // The closing ']' must be matched by nesting: the first ']' is usually the
    // end of a slot's own "days":[..] list, which truncated the slot array.
    int slotsOpen = (slotIdx >= 0) ? slotIdx + 8 : -1;
    int slotsClose = (slotsOpen >= 0) ? findMatchingClose(roomJson, slotsOpen) : -1;
    if (slotsClose > slotsOpen) {
      parseSlots(i, roomJson.substring(slotsOpen, slotsClose + 1));
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
    // Cheap check first: only pay for the full /slots download when this
    // room's marker actually moved since we last looked. An empty/"null"
    // marker (a room that predates this feature, or hasn't had its first
    // post-deploy write yet) never counts as "unchanged" — that would skip
    // the very first real read.
    String marker = fbGet("/rooms/room" + String(i+1) + "/slotsUpdatedAt");
    if (marker != "error" && marker != "" && marker != "null" && marker == lastSlotsMarker[i]) {
      warningAwareDelay(150);
      continue;
    }
    if (marker != "error") lastSlotsMarker[i] = marker;

    String slotJson = fbGet("/rooms/room" + String(i+1) + "/slots");
    if (slotJson != "error") {
      // Save previous state for comparison
      int  prevCount = rooms[i].slotCount;
      bool prevActivated[MAX_SLOTS_PER_ROOM] = {};
      char prevSlotTs[MAX_SLOTS_PER_ROOM][16] = {{0}};
      for (int j = 0; j < rooms[i].slotCount && j < MAX_SLOTS_PER_ROOM; j++) {
        prevActivated[j] = rooms[i].slots[j].activated;
        strncpy(prevSlotTs[j], rooms[i].slots[j].slotTs, sizeof(prevSlotTs[j]));
      }

      parseSlots(i, slotJson);

      // Re-apply if slot count changed, any activation flag changed, or any
      // slot's own slotTs changed. slotTs is the PWA's per-slot "this
      // content was touched" marker (written on every create/edit, e.g. a
      // time-range or code change) -- comparing it catches edits the
      // activation-only check above would otherwise miss, since editing a
      // slot's time doesn't necessarily also change whether it's activated.
      bool needsReapply = false;
      if (rooms[i].slotCount != prevCount) {
        needsReapply = true;
      } else {
        for (int j = 0; j < rooms[i].slotCount; j++) {
          if (rooms[i].slots[j].activated != prevActivated[j] ||
              strcmp(rooms[i].slots[j].slotTs, prevSlotTs[j]) != 0) {
            needsReapply = true;
            break;
          }
        }
      }

      if (needsReapply) {
        applyState(i);
      }
    }
    warningAwareDelay(150);
  }
}

// ── Poll overrides ───────────────────────────────────────────
// Reading every room's /override each POLL_INTERVAL cost one HTTPS request per
// room every 3 s. The PWA now stamps a single /status/overrideSeq (server
// timestamp; the heartbeat PATCH of /status leaves it alone) in the same write
// as any override change, so the steady state is ONE tiny
// read; the per-room scan only runs when that marker moves. A full scan still
// runs every OVERRIDE_FULL_SCAN ms as a safety net (an older PWA build that
// doesn't stamp the marker, or a missed read), and on every poll while the
// marker has never been written at all — i.e. exactly the old behaviour for
// deployments that haven't got the new PWA yet.
String lastOverrideSeq = "";
unsigned long lastOverrideFullScan = 0;
const unsigned long OVERRIDE_FULL_SCAN = 30000;

void pollOverrides() {
  String seq = fbGet("/status/overrideSeq");
  if (seq == "error" || seq == "") return;  // bad read — never change state on error
  bool seqPresent = (seq != "null");
  if (seqPresent && seq == lastOverrideSeq && millis() - lastOverrideFullScan < OVERRIDE_FULL_SCAN) {
    return;  // nothing changed since the last complete scan
  }
  bool scanOk = true;
  for (int i = 0; i < roomCount; i++) {
    String val = fbGet("/rooms/room" + String(i + 1) + "/override");

    // Skip bad reads — NEVER change state on error
    if (val == "error" || val == "") { scanOk = false; warningAwareDelay(100); continue; }

    int newOvr;
    if      (val == "true"  || val == "1")  newOvr = 1;
    else if (val == "false" || val == "0")  newOvr = 0;
    else if (val == "null"  || val == "-1") newOvr = -1;
    else {
      // Unknown value — skip, never change active override
      scanOk = false; warningAwareDelay(100); continue;
    }

    // Extra guard: if manual override is active and new value is auto (-1)
    // only accept if Firebase returned full "null" string — not a short bad read
    if (rooms[i].ovr != -1 && newOvr == -1 && val.length() < 4) {
      scanOk = false; warningAwareDelay(100); continue;
    }

    if (newOvr != rooms[i].ovr) {
      Serial.printf("Room %d override: %d → %d\n", i+1, rooms[i].ovr, newOvr);
      rooms[i].ovr = newOvr;
      applyState(i);
      pushStatus(i);
    }
    warningAwareDelay(100);
  }
  // Remember the marker only after a COMPLETE, clean scan — otherwise the next
  // poll re-scans instead of trusting a marker whose change we never fully read.
  lastOverrideFullScan = millis();
  if (scanOk && seqPresent) lastOverrideSeq = seq;
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

// Extracts a field's raw (unquoted) value — a number, null, true/false — from
// a JSON object substring, e.g. extractRawField(obj, "activatedAt") on
// {"activatedAt":1690000000123,...} returns "1690000000123". Returns "" if
// the field is missing. Unlike extractStringField, the value isn't
// necessarily a plain string, so this is used only where the raw JSON value
// is meant to be re-embedded verbatim (see findActivatedRecurringInstance).
String extractRawField(const String &json, const String &field) {
  String key = "\"" + field + "\":";
  int idx = json.indexOf(key);
  if (idx < 0) return "";
  int start = idx + key.length();
  int endComma = json.indexOf(',', start);
  int endBrace = json.indexOf('}', start);
  int end = (endComma < 0) ? endBrace : (endBrace < 0 ? endComma : min(endComma, endBrace));
  if (end < 0) return "";
  return json.substring(start, end);
}

// Scans a bucket's raw slots JSON (e.g. the pre-rollover /slotsT, fetched
// BEFORE it gets overwritten) for a recurring instance matching the given
// def — by defId when both sides have one, else falling back to matching by
// start+end time (same fallback the PWA's deleteRecurringDef() already uses,
// since an instance materialized before the defId field existed carries
// none). Returns true and fills outActivatedAtRaw/outActivatedBy only when a
// match is found AND it's actually activated (activatedAt present and not
// null) — a rollover relabels an existing occurrence from "tomorrow" to
// "today", it isn't a new booking, so an activation already granted for it
// shouldn't be undone just because the calendar date changed under it.
bool findActivatedRecurringInstance(const String &bucketJson, const String &defId,
                                     const String &sStr, const String &eStr,
                                     String &outActivatedAtRaw, String &outActivatedBy) {
  if (bucketJson == "null" || bucketJson == "" || bucketJson == "error" || bucketJson.length() < 5) return false;
  int pos = 0;
  while (pos < (int)bucketJson.length()) {
    int si = bucketJson.indexOf("\"s\":\"", pos);
    int ei = bucketJson.indexOf("\"e\":\"", pos);
    if (si < 0 || ei < 0) break;
    int objStart = bucketJson.lastIndexOf('{', si);
    int objEnd   = bucketJson.indexOf('}', ei);
    if (objStart < 0 || objEnd < 0) { pos = max(si, ei) + 10; continue; }
    String obj = bucketJson.substring(objStart, objEnd + 1);
    pos = objEnd + 1;

    if (obj.indexOf("\"recurring\":true") < 0) continue; // one-time slot — not what we're matching

    String objDefId = extractStringField(obj, "defId");
    bool matched = (defId.length() > 0 && objDefId == defId) ||
                   (objDefId.length() == 0 &&
                    bucketJson.substring(si + 5, si + 10) == sStr &&
                    bucketJson.substring(ei + 5, ei + 10) == eStr);
    if (!matched) continue;

    bool hasActivatedField = obj.indexOf("\"activatedAt\":") >= 0;
    bool activatedIsNull   = obj.indexOf("\"activatedAt\":null") >= 0;
    if (!hasActivatedField || activatedIsNull) return false; // matched, but not activated — nothing to carry
    outActivatedAtRaw = extractRawField(obj, "activatedAt");
    outActivatedBy    = extractStringField(obj, "activatedBy");
    return true;
  }
  return false;
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
// phone + days + defId, activation reset by default. This is the def-driven
// replacement for buildRecurringSlotJson during rollover.
//
// overrideActivatedAtRaw/overrideActivatedBy let a caller carry an existing
// activation forward instead of resetting it — used when rollover finds this
// same occurrence was already activated in the bucket it's being promoted
// FROM (see findActivatedRecurringInstance in midnightRollover). Leave both
// empty for the normal reset-on-materialize behavior.
String buildDefSlotJson(int roomIdx, int k, const String &dateStr,
                         const String &overrideActivatedAtRaw, const String &overrideActivatedBy) {
  RecurDef &d = rooms[roomIdx].recurDefs[k];
  char s[6], e[6];
  snprintf(s, 6, "%02d:%02d", d.sh, d.sm);
  snprintf(e, 6, "%02d:%02d", d.eh, d.em);
  bool hasCode = strlen(d.code) > 0;
  // Per-INSTANCE id — distinct from defId (the link back to the definition).
  // A new one every materialization, same as the PWA's materializeRecurringSlot
  // (makeSlotId()) — identity resets daily by design, only defId is stable.
  // dateStr+k is unique within this one rollover write (each def index k
  // appears once per day), which is all the PWA's merge-by-id needs — it
  // only ever compares ids within the same room's CURRENT /slots snapshot.
  String idField    = ",\"id\":\"sl_" + dateStr + "_" + String(k) + "\"";
  String codeField  = hasCode ? (",\"code\":\"" + String(d.code) + "\"") : "";
  String bbField    = (strlen(d.bookedBy) > 0) ? (",\"bookedBy\":\"" + String(d.bookedBy) + "\"") : "";
  String phField    = (strlen(d.phone) > 0)    ? (",\"phone\":\"" + String(d.phone) + "\"") : "";
  String defIdField = (strlen(d.defId) > 0)    ? (",\"defId\":\"" + String(d.defId) + "\"") : "";
  String daysField = daysFieldFromMask(d.daysMask);
  String activatedField, activatedByField;
  if (overrideActivatedAtRaw.length() > 0) {
    activatedField   = ",\"activatedAt\":" + overrideActivatedAtRaw;
    activatedByField = (overrideActivatedBy.length() > 0) ? (",\"activatedBy\":\"" + overrideActivatedBy + "\"") : "";
  } else {
    // No-code ("Auto") slots are active by default: seed a non-null activatedAt
    // so the relay comes on for the window (parseSlots now keys purely on
    // activatedAt, no longer force-activating on code:null). Coded slots reset to
    // null so they wait for the QR PIN / admin Activate. Sentinel 1 = "activated,
    // exact time unknown" — parseSlots only checks non-null.
    activatedField = hasCode ? ",\"activatedAt\":null" : ",\"activatedAt\":1";
  }
  return "{\"s\":\"" + String(s) + "\",\"e\":\"" + String(e) + "\",\"recurring\":true" + idField + defIdField +
    codeField + bbField + phField + daysField + ",\"date\":\"" + dateStr + "\"" + activatedField + activatedByField + "}";
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

  // Global marker (not per-room, unlike slotsUpdatedAt) — request.html's
  // loadAvailability() checks this once before fetching every room, so one
  // write covering the whole rollover is enough. Written before any of the
  // per-room work below, same fail-safe reasoning as slotsUpdatedAt: if
  // rollover fails partway through, a visitor doing one wasted-but-safe
  // extra fetch is fine, missing a real change is not.
  fbPut("/config/roomsUpdatedAt", String((unsigned long)time(nullptr)));

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
    // 0 = every day. Stable code + booker/phone carried; activation resets
    // UNLESS this same occurrence was already activated in tomorrowJson (the
    // bucket it's being promoted FROM) — e.g. someone used the activation
    // page's lookahead window to activate it before midnight. That activation
    // is carried forward instead of being silently undone by the relabel.
    int todayWd = nowWeekday();
    for (int k = 0; k < rooms[i].recurDefCount; k++) {
      RecurDef &def = rooms[i].recurDefs[k];
      if (!maskRunsOnDay(def.daysMask, todayWd)) continue;
      if (!first) newTodayJson += ",";
      char sBuf[6], eBuf[6];
      snprintf(sBuf, 6, "%02d:%02d", def.sh, def.sm);
      snprintf(eBuf, 6, "%02d:%02d", def.eh, def.em);
      String overrideAt = "", overrideBy = "";
      findActivatedRecurringInstance(tomorrowJson, String(def.defId), String(sBuf), String(eBuf), overrideAt, overrideBy);
      newTodayJson += buildDefSlotJson(i, k, getDateStr(), overrideAt, overrideBy);
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
      // Soft-deleted (PWA's deleteSlot()) — this is the permanent purge the
      // tombstone was waiting for: just don't carry it into the new today.
      if (existingSlot.indexOf("\"deleted\":true") >= 0) continue;
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
          // Only move one-time, non-deleted slots (skip recurring, and skip a
          // soft-deleted tomorrow slot — that's this tombstone's permanent
          // purge, same as the "keep today's slots" loop above).
          if (obj.indexOf("\"recurring\":true") < 0 && obj.indexOf("\"deleted\":true") < 0) {
            String startStr = tomorrowJson.substring(si + 5, si + 10);
            String endStr   = tomorrowJson.substring(ei + 5, ei + 10);
            if (!first) newTodayJson += ",";
            // Preserve id + code + the booker's name/phone (see the recurring
            // loop above for why) — omit only activatedAt, and re-stamp
            // date to today since this slot is leaving "tomorrow" now. This
            // is the SAME slot (just relabeled to today), so its id must
            // survive the move — dropping it would make the PWA's merge-by-id
            // treat it as a brand-new slot instead of recognizing it, on the
            // next push. Only a slot that somehow has no id yet (pre-dates
            // the id field) gets a fresh one here.
            String id2 = extractStringField(obj, "id");
            String idField2 = (id2.length() > 0) ? (",\"id\":\"" + id2 + "\"") : (",\"id\":\"sl_" + getDateStr() + "_" + String(si) + "\"");
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
            newTodayJson += "{\"s\":\"" + startStr + "\",\"e\":\"" + endStr + "\"" + idField2 +
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
    // Tomorrow's recurring slots, also generated FROM the definitions. These
    // are brand-new future occurrences (not yet materialized anywhere), so
    // there's nothing to carry forward — always the normal reset.
    for (int k = 0; k < rooms[i].recurDefCount; k++) {
      if (!maskRunsOnDay(rooms[i].recurDefs[k].daysMask, tomorrowWd)) continue;
      if (!tfirst) newTomorrowJson += ",";
      newTomorrowJson += buildDefSlotJson(i, k, tomorrowDate, "", "");
      tfirst = false;
    }
    newTomorrowJson += "]";

    // Marker written BEFORE the data — fail-safe ordering: if the /slots
    // write below fails, "marker says changed but data didn't move" just
    // costs refreshSlotsOnly() one wasted full re-fetch next cycle, never a
    // missed one. Not load-bearing for THIS firmware's own correctness
    // (readAllRooms() right after this function returns already refreshes
    // everything unconditionally) — this is for any other reader/hygiene.
    String newMarker = String((unsigned long)time(nullptr));
    fbPut(base + "/slotsUpdatedAt", newMarker);
    lastSlotsMarker[i] = newMarker;

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
  warningEpisodeActive = false;
  applyAllStates();

  // Record that today's transition is now handled — this is what lets
  // setup() detect a *missed* rollover after a reboot, so update it however
  // this function was reached (normal per-minute checkMidnight(), or the
  // boot-time catch-up call). Only reached when every room's write above
  // actually succeeded — see the function comment for why that matters.
  lastRolloverDay = currentEpochDay();
  warnedSlotIdCount = 0;
  memset(warnedSlotIds, 0, sizeof(warnedSlotIds));
  saveConfig();

  // Also expose it in Firebase — the PWA has no other way to tell whether
  // today's rollover has actually happened, since it only ever reads the
  // rooms/slots data itself, not this board's local state.
  fbPut("/config/lastRolloverEpochDay", String(lastRolloverDay));
  // PWA establishes the daily Sync V2 baseline; invalidate local cursor so
  // the next sync observes the new generation and performs a full refresh.
  syncGeneration = 0;
  syncRevision = 0;
  saveConfig();

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

// ── Rollover self-heal ────────────────────────────────────────
// Handles the DESYNC case checkMidnight() can't: the rollover marker says
// "done today" (lastRolloverDay == today) yet a room's today bucket still holds
// a one-time slot stamped with a PAST date — i.e. the slots were never actually
// transformed (admin PWA / partial run advanced the marker without rebuilding,
// so this board adopted "done" via syncRolloverMarkerFromFirebase and never
// rebuilt). Left alone this shows yesterday's stale slots all day and, since
// they're expired/unactivated, no room turns on.
//
// Detection is ZERO extra Firebase reads: it compares today's date against
// roomSlotsDate[], which parseSlots() already stamped from slots the firmware
// reads on its normal sync cycle. A heal forces one real midnightRollover(),
// throttled to at most once per SELF_HEAL_MIN_INTERVAL so a persistently-failing
// heal can't storm. Guarded so it ONLY runs in the genuine desync state:
//   - time is synced (otherwise "today" is meaningless),
//   - the marker already claims today (the pure "marker behind" case is
//     checkMidnight()'s job, not this),
//   - at least one room's today bucket carries a strictly-past date.
void selfHealRollover() {
  if (!timeSynced) return;
  int todayEpochDay = currentEpochDay();
  if (lastRolloverDay != todayEpochDay) return;   // not a desync — checkMidnight handles "behind"
  if (millis() - lastSelfHealAttempt < SELF_HEAL_MIN_INTERVAL && lastSelfHealAttempt != 0) return;

  String todayStr = getDateStr();
  bool stale = false;
  for (int i = 0; i < roomCount && i < MAX_ROOMS; i++) {
    const char *d = roomSlotsDate[i];
    // A real, strictly-past date is the only trigger. "" (unknown / recurring-
    // only / empty bucket) and today's date never heal — avoids false positives.
    if (d[0] != '\0' && strcmp(d, todayStr.c_str()) < 0) { stale = true; break; }
  }
  if (!stale) return;

  lastSelfHealAttempt = millis();
  Serial.printf("[%s] Rollover self-heal: today bucket holds a stale-dated slot but marker=today — forcing rollover\n",
    getTime().c_str());
  // Force the real transform. midnightRollover() rebuilds today from recurring
  // defs + keeps only today-dated one-time slots (so the stale ones are dropped)
  // + promotes tomorrow (a no-op if already promoted), then re-reads rooms and
  // re-drives relays. On success it advances the marker and re-stamps
  // roomSlotsDate[] via the readAllRooms() it calls, clearing the stale state.
  midnightRollover();
}

// ── Mark slot as expired in Firebase ─────────────────────────
// Writes ONLY the single field /rooms/roomN/slots/{firebaseKey}/expired = true —
// a targeted per-field write, NOT a full-array rewrite. {firebaseKey} is
// resolved from the slot's id (see the body); it is not the RAM index.
//
// The previous version fetched the whole /slots array, located the slot by a
// start-time STRING match (first "s":"HH:MM" occurrence — no unique id), spliced
// expired:true in, and PUT the entire array back. That had two problems with
// adjacent/coded slots: (1) it matched only by start time with no per-slot id,
// and (2) the read-modify-write of the whole array raced with the PWA's own
// per-slot writes — either side's full-array PUT could clobber the other's
// changes, which could drop/merge slots. Writing just the one field by the
// index we already hold removes both the string-match fragility and the race:
// it touches nothing else in the array, so it can never merge or delete slots.
void markSlotExpired(int roomIdx, int slotIdx) {
  if (slotIdx < 0 || slotIdx >= rooms[roomIdx].slotCount) return;
  // Never expire a slot that's currently activated — the firmware already
  // tracks this per slot, so we don't need to fetch/parse the array to know it.
  if (rooms[roomIdx].slots[slotIdx].activated) return;
  String base = "/rooms/room" + String(roomIdx + 1);
  // slotIdx is this firmware's RAM index. That array is compacted/merged and has
  // deleted slots removed, so it is NOT the Firebase child key — writing to
  // /slots/{slotIdx} could flag a different slot or create a stray child. Look
  // the real key up by the slot's stable id (one small read, once per expiry).
  // No id (very old data) or not found → skip the remote write; the caller has
  // already latched expired locally, and the next full refresh reconciles.
  const char *slotId = rooms[roomIdx].slots[slotIdx].id;
  String slotsJson = (slotId[0] != '\0') ? fbGet(base + "/slots") : String("error");
  String fbKey = (slotsJson == "error" || slotsJson == "null") ? String("") : findSlotFirebaseKey(slotsJson, slotId);
  if (fbKey.length() == 0) {
    Serial.printf("Room %d slot expiry not written: no matching Firebase slot id\n", roomIdx + 1);
    return;
  }
  // Marker before data — same fail-safe ordering as midnightRollover(), and
  // likewise not load-bearing for this firmware's own correctness (the
  // caller already latched rooms[roomIdx].slots[slotIdx].expired locally
  // before calling this) — just keeps slotsUpdatedAt honest for any other
  // reader.
  String newMarker = String((unsigned long)time(nullptr));
  fbPut(base + "/slotsUpdatedAt", newMarker);
  lastSlotsMarker[roomIdx] = newMarker;
  fbPut(base + "/slots/" + fbKey + "/expired", "true");
  fbPut("/slotRecords/room" + String(roomIdx + 1) + "/today/" + String(slotId) + "/expired", "true");
  char startBuf[6], endBuf[6];
  snprintf(startBuf, 6, "%02d:%02d", rooms[roomIdx].slots[slotIdx].sh, rooms[roomIdx].slots[slotIdx].sm);
  snprintf(endBuf,   6, "%02d:%02d", rooms[roomIdx].slots[slotIdx].eh, rooms[roomIdx].slots[slotIdx].em);
  Serial.printf("Room %d slot %s-%s marked expired\n", roomIdx+1, startBuf, endBuf);
}

// ── End-of-slot warning ──────────────────────────────────────
// True if the room is currently inside an ACTIVATED slot's final
// warnMinutes, i.e. now ∈ [slotEnd - warnMinutes, slotEnd). Only activated
// slots count — there's no point warning about a slot nobody turned on.
// Always false when the feature is disabled (warnMinutes <= 0), so callers
// need no extra guard.
bool wasSlotWarningTriggered(const char *slotId) {
  if (!slotId || !slotId[0]) return false;
  for (int i = 0; i < warnedSlotIdCount; i++) {
    if (strcmp(warnedSlotIds[i], slotId) == 0) return true;
  }
  return false;
}
void markSlotWarningTriggered(const char *slotId) {
  if (!slotId || !slotId[0] || wasSlotWarningTriggered(slotId)) return;
  if (warnedSlotIdCount >= MAX_WARNED_SLOT_IDS) return;
  strncpy(warnedSlotIds[warnedSlotIdCount], slotId, 39);
  warnedSlotIds[warnedSlotIdCount][39] = '\0';
  warnedSlotIdCount++;
}
bool slotIsInWarningWindow(const Slot &sl) {
  if (warnMinutes <= 0 || !sl.activated) return false;
  if (sl.recurring && !slotRunsToday(sl)) return false;
  int nowSeconds = nowH() * 3600 + nowMn() * 60 + nowSec();
  int startSeconds = sl.sh * 3600 + sl.sm * 60;
  int endSeconds = sl.eh * 3600 + sl.em * 60;
  if (nowSeconds < startSeconds || nowSeconds >= endSeconds) return false;
  int remainingSeconds = endSeconds - nowSeconds;
  return remainingSeconds > 0 && remainingSeconds <= warnMinutes * 60;
}

bool inWarningWindow(int idx) {
  for (int j = 0; j < rooms[idx].slotCount; j++) {
    if (slotIsInWarningWindow(rooms[idx].slots[j])) return true;
  }
  return false;
}

// Kick off the one-shot attention burst on the shared beeper. Non-blocking:
// the actual on/off toggling happens in serviceBeeper() from loop(). No-op
// if no beeper pin is configured, OR if a burst is already in progress —
// so when several rooms enter their warning window together (e.g. multiple
// slots ending at the same time), the shared bell rings exactly ONCE rather
// than being re-triggered/extended per room.
void setBeeperPhysicalState(bool on) {
  if (!beeperHardwareInitialized || beeperPin < 0) return;
  // No cached pin state. The requested physical state is written directly.
  // Normal state is OFF; ON is used only by the short warning burst task.
  digitalWrite(beeperPin, on ? BEEPER_ON : BEEPER_OFF);
}

void beeperBurstTask(void *parameter) {
  const int count = beepBurstCount;
  const TickType_t onTicks = pdMS_TO_TICKS(max(20UL, beepOnMs));
  const TickType_t gapTicks = pdMS_TO_TICKS(BEEP_GAP_MS);
  Serial.printf("BEEPER burst start: %d pulse(s), %lums each, pin=%d, ON=%d, OFF=%d\n",
    count, beepOnMs, beeperPin, BEEPER_ON, BEEPER_OFF);
  for (int i = 0; i < count; i++) {
    setBeeperPhysicalState(true);
    vTaskDelay(onTicks);
    setBeeperPhysicalState(false);
    if (i + 1 < count) vTaskDelay(gapTicks);
  }
  setBeeperPhysicalState(false);
  Serial.println("BEEPER burst complete; output OFF");
  beeperTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void startBeepBurst() {
  if (!beeperHardwareInitialized || beeperPin < 0) { Serial.println("BEEPER skipped: hardware not initialized"); return; }
  if (warnMinutes <= 0 || beepOnMs == 0 || beepBurstCount <= 0) { Serial.println("BEEPER skipped: warning/beep config disabled"); return; }
  if (beeperTaskHandle != nullptr) { Serial.println("BEEPER skipped: burst already active"); return; }
  BaseType_t ok = xTaskCreatePinnedToCore(beeperBurstTask, "beeperBurst", 2048, nullptr, 1, &beeperTaskHandle, 0);
  if (ok != pdPASS) {
    beeperTaskHandle = nullptr;
    setBeeperPhysicalState(false);
    Serial.println("BEEPER ERROR: task creation failed");
  }
}

// Advances the non-blocking beep burst one phase at a time. Called every
// loop() iteration; cheap no-op when nothing is beeping or no pin is set.
void serviceBeeper() {
  // Dedicated beeperBurstTask owns precise timing. No GPIO writes from loop.
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
    if (!rooms[i].warning || rooms[i].ledPin < 0 || rooms[i].ledPin == beeperPin) continue;
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
  bool anyWarningNow = false;
  bool anyNewSlotWarning = false;
  for (int i = 0; i < roomCount; i++) {
    bool roomWarningNow = false;
    for (int j = 0; j < rooms[i].slotCount; j++) {
      Slot &sl = rooms[i].slots[j];
      if (!slotIsInWarningWindow(sl)) continue;
      roomWarningNow = true;
      anyWarningNow = true;
      if (!wasSlotWarningTriggered(sl.id)) {
        markSlotWarningTriggered(sl.id);
        anyNewSlotWarning = true;
        Serial.printf("[%s] Beeper marked triggered: room=%d slot=%s\n", getTime().c_str(), i + 1, sl.id);
      }
    }
    if (roomWarningNow && !rooms[i].warning) {
      rooms[i].warning = true;
      rooms[i].ledBlinkOn = false;
    } else if (!roomWarningNow && rooms[i].warning) {
      rooms[i].warning = false;
      if (rooms[i].ledPin >= 0 && rooms[i].ledPin != beeperPin)
        digitalWrite(rooms[i].ledPin, rooms[i].lightOn ? LED_ON : LED_OFF);
    }
  }
  // Mark every qualifying slot first, then create one shared burst.
  if (anyNewSlotWarning && beeperTaskHandle == nullptr) startBeepBurst();
  warningEpisodeActive = anyWarningNow;
  // Do not touch the beeper GPIO during ordinary warning scans. The burst task
  // always returns it to OFF immediately after its final pulse.
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
  // Re-evaluate the emergency light every tick too — its auto-off timeout must
  // fire during a long standby when no room state changes (setRelay() only
  // calls this on a change). No-op when the pin is unconfigured or the timeout
  // is disabled and the light is already in the right state.
  updateEmergencyLight();
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
  syncGeneration = syncRawField(json, "syncGeneration").toInt();
  syncRevision = syncRawField(json, "syncRevision").toInt();
  configVersion = syncRawField(json, "configVersion").toInt();
  return firebaseUrl.length() > 0;
}

void saveConfig() {
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) { Serial.println("Failed to save config to LittleFS"); return; }
  String json = "{\"fbUrl\":\"" + firebaseUrl + "\",\"profileNum\":\"" + profileNum +
    "\",\"lastRolloverDay\":" + String(lastRolloverDay) +
    ",\"syncGeneration\":" + String(syncGeneration) +
    ",\"syncRevision\":" + String(syncRevision) +
    ",\"configVersion\":" + String(configVersion) + "}";
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
  applyEmergencyTimeoutConfig(); // emergency light auto-off timeout (0 = disabled)
  applyBeeperPinConfig();    // shared end-of-slot beeper pin (off/PIN_NONE if unconfigured)
  applyWarnMinutesConfig();  // warn window in minutes (0 = feature disabled)
  applyBeepConfig();         // per-beep duration + count (defaults 250ms, 1)
  if (beeperPin == STATUS_LED_PIN || beeperPin == CONFIG_BUTTON_PIN) {
    Serial.printf("BEEPER GPIO %d conflicts with reserved controller GPIO; disabling beeper until config is corrected and rebooted\n", beeperPin);
    beeperPin = PIN_NONE;
  }
  Serial.printf("BEEPER config summary: pin=%d warn=%dmin count=%d ms=%lu wiring=NO ON=%d OFF=%d\n",
    beeperPin, warnMinutes, beepBurstCount, beepOnMs, BEEPER_ON, BEEPER_OFF);

  // Configure pins now that we know which GPIOs each room actually uses
  for (int i = 0; i < roomCount; i++) {
    if (rooms[i].relayPin >= 0 && rooms[i].relayPin != beeperPin) {
      pinMode(rooms[i].relayPin, OUTPUT);
      digitalWrite(rooms[i].relayPin, RELAY_OFF); // start OFF — correct state restored below
    } else {
      Serial.printf("Room %d has no relay GPIO configured — set one in the PWA Settings and reboot\n", i + 1);
    }
    if (rooms[i].ledPin >= 0 && rooms[i].ledPin != beeperPin) {
      pinMode(rooms[i].ledPin, OUTPUT);
      digitalWrite(rooms[i].ledPin, LED_OFF);
    } else {
      Serial.printf("Room %d has no LED configured — skipping its LED indicator\n", i + 1);
    }
  }
  if (emergencyPin >= 0 && emergencyPin != beeperPin) {
    digitalWrite(emergencyPin, RELAY_OFF); // preload physical OFF before OUTPUT
    pinMode(emergencyPin, OUTPUT);
    digitalWrite(emergencyPin, RELAY_OFF);
    emergencyOutputOn = false;
  }
  // Shared end-of-slot beeper — only touch the pin if it's actually
  // configured, so unconfigured deployments never drive a random GPIO.
  if (beeperPin >= 0) {
    // One-time hardware initialization. Preload NC relay OFF before OUTPUT.
    digitalWrite(beeperPin, BEEPER_OFF);
    pinMode(beeperPin, OUTPUT);
    digitalWrite(beeperPin, BEEPER_OFF);
    beeperTaskHandle = nullptr;
    warningEpisodeActive = false;
    beeperHardwareInitialized = true;
    Serial.printf("BEEPER initialized OFF on GPIO %d, level %d\n", beeperPin, BEEPER_OFF);
  }

  gpioAssignmentsLocked = true;
  Serial.println("GPIO assignments locked for this boot");

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
  // active slots. If NTP hasn't synced yet, this same check runs again
  // from loop()'s NTP retry the moment it eventually does — see there.
  if (timeSynced) {
    catchUpMissedRollover();
  } else {
    Serial.println("NTP never synced at boot — will keep retrying in the background; rollover catch-up runs once it succeeds");
  }

  // Initialize independent settings and daily slot synchronization cursors.
  syncConfigV2();
  syncSlotsV2();

  Serial.println("=== Ready — state restored from Firebase ===");
}

// Runs the exact "did we miss a rollover" check setup() runs at boot, but
// callable again later once a delayed NTP sync finally succeeds (see the
// retry in loop()) -- factored out so both call sites share one path
// rather than two copies that could drift apart. Only ever called with
// timeSynced already true -- an unsynced clock's epoch day is meaningless
// and would look like a huge missed gap, wrongly wiping active slots.
// Safe to call even when nothing was actually missed: syncRolloverMarkerFromFirebase()
// and midnightRollover()'s own date-based keep-filter mean this can never
// clobber a slot legitimately created for today while this board's clock
// was still unsynced.
void catchUpMissedRollover() {
  int todayEpochDay = currentEpochDay();
  if (lastRolloverDay == -1) {
    // No baseline yet (first boot on this firmware, or ever) — nothing to
    // catch up on, just record today so future checks have something to
    // compare against.
    lastRolloverDay = todayEpochDay;
    saveConfig();
    return;
  }
  if (todayEpochDay != lastRolloverDay) {
    if (syncRolloverMarkerFromFirebase(todayEpochDay)) {
      Serial.println("Rollover already done today via PWA — syncing marker");
    } else {
      Serial.println("Missed rollover while offline — catching up now");
      midnightRollover(); // updates lastRolloverDay + saveConfig() itself
    }
  }
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

    // NTP is a one-shot attempt at boot (20 tries over ~10s) — if the
    // network wasn't fully up yet (e.g. a power outage where the local
    // WiFi reconnects before the router's own internet uplink does),
    // timeSynced stays false for the rest of this boot with nothing ever
    // retrying it, and checkMidnight() below silently never runs at all.
    // Retry here on the same 10s cadence, WiFi permitting; the instant it
    // succeeds, run the same missed-rollover catch-up setup() runs at
    // boot, so a slow network recovery doesn't cost a whole reboot cycle.
    if (!timeSynced && WiFi.status() == WL_CONNECTED) {
      struct tm t;
      timeSynced = getLocalTime(&t);
      if (timeSynced) {
        Serial.println("NTP synced (delayed) — Time: " + getTime());
        catchUpMissedRollover();
      }
    }

    checkSchedules();
    checkEndOfSlotWarnings(); // update per-room warning flags, fire beeper burst on entry
    checkMidnight();  // detect date change → rollover slots
    selfHealRollover(); // detect a marker-vs-data desync and force a rollover
  }

  // Non-blocking end-of-slot warning outputs — run every iteration so the
  // beep burst and LED blink are smooth. Both no-op when disabled/unconfigured.
  serviceBeeper();
  serviceWarningLeds();

  // Refresh slots only — no relay state change unless slots count changes
  if (millis() - lastSlotRefresh > SLOT_REFRESH) {
    lastSlotRefresh = millis();
    if (WiFi.status() == WL_CONNECTED) {
      syncConfigV2();
      syncSlotsV2();
    }
  }

  // Heartbeat every 5 minutes
  if (millis() - lastStatusPush > HEARTBEAT) {
    lastStatusPush = millis();
    // Only when WiFi is up (otherwise it just burns a 5 s timeout). A failed or
    // skipped heartbeat (write error, NTP not synced yet) is retried in 30 s
    // instead of waiting a full 5 min — the PWA marks the board stale at 11 min,
    // so two silent misses used to be enough to trigger a false alarm.
    bool hbOk = (WiFi.status() == WL_CONNECTED) && pushHeartbeat();
    if (!hbOk) lastStatusPush = millis() - (HEARTBEAT - HEARTBEAT_RETRY);
  }

  // WiFi watchdog — reboots if disconnected too long
  wifiWatchdog();

  delay(10);
}
