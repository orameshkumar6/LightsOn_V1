# Room Controller — Complete Setup Guide
## Firebase + ESP32 + PWA — v4.0

This guide assumes you have **not** worked with an ESP32 board before. Every
step is spelled out — if something feels too basic, skip ahead.

---

## What this system does

Controls room lights (or anything wired through a relay) from any phone,
tablet, or laptop — using:
- An **ESP32** board with a 6-channel relay module (expandable) + optional
  LED indicators
- **Firebase Realtime Database** as the communication bridge between your
  phone and the ESP32
- A **PWA** (installable web app) hosted on GitHub Pages

**What's new in v4.0**: multiple sites (profiles) can now share a single
Firebase project — each one gets its own automatically-assigned number,
and all its data lives under that number instead of the database root. See
Part 7 and the Firebase data structure reference below.

**What's new in v3.0**: nothing is hardcoded anymore. The ESP32 sketch has
no WiFi password or Firebase URL baked into it — you configure both
directly on the board the first time you power it on, through its own
built-in setup page. Each room's relay/LED pins, room count, and even
which Firebase project a device talks to are all configured from the app
itself, not from editing code.

---

## Glossary (skip if you already know these)

| Term | Plain-English meaning |
|---|---|
| **ESP32** | A small WiFi-connected microcontroller board — like a tiny computer that can turn physical wires on/off and connect to the internet. |
| **GPIO pin** | "General Purpose Input/Output" — one of the numbered metal pins on the board you can wire something to (a relay, an LED) and control from code. "GPIO 26" just means the pin labeled 26 on the board. |
| **Relay** | An electrically-controlled switch. The ESP32 can't directly switch mains-voltage devices, so it flips a low-voltage signal to a relay, and the relay does the actual switching. |
| **Firmware / sketch** | The program that runs on the ESP32 — the `.ino` file in this package. |
| **Flashing / uploading** | Copying the sketch onto the ESP32's memory so it runs. |
| **Captive portal** | The temporary WiFi hotspot + setup webpage the board creates so you can type in your home WiFi details without ever needing a keyboard/screen attached to the board itself. Same idea as setting up a smart plug or a Chromecast. |
| **BOOT button** | A small physical button on the ESP32 board, usually labeled "BOOT". Used here to re-open the setup page later if you need to change WiFi. |
| **Realtime Database (RTDB)** | The specific type of Firebase database this project uses — it's built to push live updates instantly to every connected device, which is what makes the dashboard update in real time. |
| **PWA** | "Progressive Web App" — a website that can be installed like a regular app icon on your phone's home screen. |

---

## System architecture

```
PWA App (any device)
      ↓ writes rooms, pins, schedule, override
Firebase Realtime Database
      ↓ ESP32 polls every 3-10 seconds
ESP32 reads config → drives relay → writes status back
      ↓
Relay switches the light/device ON/OFF
      ↓
LED on board glows to confirm state (if you wired one for that room)
```

Each ESP32 board talks to **one** Firebase project. If you're managing
multiple independent sites (a home + an office, say), you'd set up a
separate Firebase project and a separate ESP32 for each — the PWA's
**Profiles** feature (see Part 7) lets one app installation switch between
managing several of these independently.

---

## What you'll need

| Item | Qty | Notes |
|---|---|---|
| ESP32 Dev Module | 1 | Any 38-pin variant |
| Relay module (5V) | 1 | 6-channel shown here; any channel count works, matched to how many rooms you want |
| LEDs (green recommended) | 0–6 | Optional per room — skip entirely if you don't want indicator lights |
| 330Ω resistors | 1 per LED | Only needed if using LEDs |
| Jumper wires | ~20 | Male-female |
| USB OTG adapter | 1 | For flashing from an Android tablet |
| Power supply 5V 2A | 1 | For relay + ESP32 |
| A Google account | — | For Firebase |
| A GitHub account | — | For hosting the PWA |
| An Android phone/tablet | — | For flashing via ArduinoDroid, and for the WiFi setup step |

---

## Part 1 — Create your Firebase project

1. Go to https://console.firebase.google.com and create a new project (free
   tier is enough for this).
2. In the project, go to **Build → Realtime Database → Create Database**.
   Pick a region close to you.
3. Go to the **Rules** tab and set:
   ```json
   {
     "rules": {
       ".read": true,
       ".write": true
     }
   }
   ```
   **Why open rules?** The activation page (`activate.html`) is meant to be
   reachable by anyone who scans its QR code — that only works if it can
   read/write without first logging in. The trade-off is that anyone who
   has your Database URL can also read/write your data directly. If that's
   a concern for your use case, this is the one place to revisit — adding
   Firebase Authentication or App Check would tighten it, at the cost of
   the QR-scan-and-go flow. Most home/small-office setups accept this
   trade-off; it's your call.
4. Go to the **Data** tab — it should be empty at first. That's fine; the
   PWA creates the room structure the first time you save a profile
   (Part 3).
5. Copy your **Database URL** from the top of the Data tab (looks like
   `https://your-project-default-rtdb.REGION.firebasedatabase.app`) — you'll
   need it in Part 3. You do **not** need an API key for anything in this
   project.

---

## Part 2 — Deploy the PWA to GitHub Pages

1. Go to https://github.com → **New repository**. Name it whatever you like
   (e.g. `room-controller`). Public is fine — there are no secrets in these
   files.
2. In the repo, **Add file → Upload files**, and upload everything inside
   the `pwa/` folder from this package:
   - `index.html`
   - `activate.html`
   - `manifest.json`
   - `sw.js`
   - `icons/icon-192.png`
   - `icons/icon-512.png`

   Nothing needs editing before upload — no Firebase URL, no API key.
3. Commit the upload.
4. Go to **Settings → Pages**. Source: "Deploy from a branch". Branch:
   `main` → `/ (root)` → Save. Wait about a minute.
5. Your app is now live at:
   ```
   https://YOUR-USERNAME.github.io/room-controller/index.html
   ```

---

## Part 3 — Configure your first profile and rooms

1. Open your GitHub Pages URL in Chrome (phone, tablet, or laptop — any
   device works, but you'll use this same device again in Part 6).
2. Tap the **Settings** tab.
3. Under **Profiles**, you'll see a "Default" profile already created.
   Tap into the **Database URL** field and paste the URL you copied in
   Part 1. Tap **Save changes to this profile**.
4. Tap **Test Firebase connection** — you should see "✓ Firebase
   connected!". If not, double check the URL was pasted correctly (no
   trailing slash, starts with `https://`).
   - The first time you save a profile with a Database URL, it's
     automatically assigned a **profile number** (shown next to its name
     in the Profiles list, e.g. "Main Office **#1**"). Write this number
     down — you'll type it into the ESP32's setup portal in Part 6, so
     the board knows which profile's rooms to read/write in that database.
     If you're pointing several profiles at the *same* Database URL (one
     Firebase project shared by multiple sites), each one gets its own
     number automatically — you don't need to manage this yourself.
5. Scroll to **Room names & GPIO pins**. For each room you plan to wire:
   - Give it a name (e.g. "Living Room", or leave the default "Room 1").
   - Set its **Relay GPIO** — the pin number you'll wire that room's
     relay channel to. See the pin cheat-sheet in Part 4 before choosing.
   - Set its **LED GPIO**, or tick **"No LED for this room"** if you don't
     want an indicator light for it.
   - Use **+ Add room** if you need more than the default 6, or the 🗑
     button to remove one you don't need.
6. Tap **Save changes to this profile** again once all rooms are set.

You can come back and edit any of this later — but remember: **the ESP32
only reads room/pin configuration once, at boot**. If you change pins or
room count after the board is already running, you'll need to reboot it
for the change to take effect (see Part 8).

---

## Part 4 — Choosing safe GPIO pins

Not every pin on an ESP32 is safe to use as a relay/LED output. Some are
reserved for the board's internal flash memory, some affect how the board
boots if pulled to the wrong voltage, and a few can only be used as
*inputs*, never outputs.

| Category | GPIOs | Notes |
|---|---|---|
| **Good to use** | 4, 5, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33 | Safe general-purpose outputs — pick from this list first |
| **Reserved by this project** | 0, 2 | GPIO 0 is the BOOT button (used for WiFi reconfiguration, Part 8). GPIO 2 is the built-in system status LED (blinks during setup/WiFi issues) — don't assign either to a room |
| **Avoid** | 1, 3, 6, 7, 8, 9, 10, 11 | 1/3 are the USB serial connection (Serial Monitor). 6-11 connect to the board's internal flash chip — using them can prevent the board from booting at all |
| **Use with care** | 12 | Affects boot voltage mode on some boards — the original v2.0 wiring used this for Room 4 and it worked fine in practice, but if you have boot problems, move off it first |
| **Cannot be used as output** | 34, 35, 36, 39 | Input-only pins — don't assign these to a relay or LED |

If you're continuing from the original v2.0 wiring (Relay: 26, 27, 14, 12,
13, 15 / LED: 2, 4, 5, 18, 19, 21), you don't need to rewire anything —
just enter those exact same numbers in Part 3's Settings screen. GPIO 2
being both "Room 1's LED" and "the system status LED" is fine — it's the
same physical LED serving double duty, exactly as it did in v2.0.

---

## Part 5 — Wiring

### Relay module → ESP32
```
Relay VCC  → ESP32 VIN (5V)
Relay GND  → ESP32 GND
Relay IN1  → whichever GPIO you set as Room 1's Relay GPIO
Relay IN2  → Room 2's Relay GPIO
...and so on for however many rooms/relay channels you have
```

### LED indicators → ESP32 (only for rooms where you didn't tick "No LED")
```
GPIO → 330Ω resistor → LED+ (long leg) → LED- (short leg) → GND
```
Long leg (+) = positive = connects to the resistor.
Short leg (-) = negative = connects to GND.
Flat edge on the LED's base = negative side, if the legs have been trimmed.

### Load wiring (per relay)
```
COM terminal → Power supply +
NO terminal  → Load + (your light/device)
Load -       → Power supply -
```
⚠️ For mains voltage (220V/110V), have an electrician handle the load
side, and make sure your relay module and wiring are rated for the actual
current your load draws.

---

## Part 6 — Upload the firmware and complete first-boot setup

### Step 1 — Install apps on your Android tablet/phone
1. Install **ArduinoDroid** from the Play Store.
2. Install **Serial USB Terminal** from the Play Store (for the Serial
   Monitor — this is how you'll watch what the board is doing).
3. Open ArduinoDroid with WiFi on — it downloads the ESP32 SDK the first
   time (~500MB, 5-10 minutes).

### Step 2 — Install the WiFiManager library
1. In ArduinoDroid: ≡ menu → **Library Manager**.
2. Search `WiFiManager`, install the one **by tzapu**.
3. This is the only extra library needed beyond what ArduinoDroid ships
   with — WiFi, HTTPClient, WiFiClientSecure, and LittleFS are all
   already built in.

### Step 3 — Open the sketch
1. Copy `RoomController_Firebase.ino` to your tablet (Google Drive
   download, or USB transfer).
2. ⚠️ The `.ino` file must be inside a folder with the exact same name:
   ```
   RoomController_Firebase/
     RoomController_Firebase.ino   ← correct
   ```
3. In ArduinoDroid: ≡ menu → Open → navigate to the file.

### Step 4 — Select board
1. ≡ menu → Select Board → ESP32 Arduino → **ESP32 Dev Module**.

### Step 5 — Connect and upload
1. Plug the OTG adapter into the tablet, then the ESP32's USB cable into
   the OTG adapter.
2. Allow ArduinoDroid to access the USB device when prompted.
3. ≡ menu → Select Port → tap the detected device.
4. ≡ menu → Upload.
5. If you see "Failed to connect": hold the **BOOT** button on the ESP32,
   tap Upload again, release BOOT once you see "Connecting......".
6. Wait for "Done uploading" — **nothing needed to be edited before this
   step**, unlike v2.0.

### Step 6 — Watch the Serial Monitor
1. ≡ menu → Serial Monitor → 115200 baud.
2. Press the **RST** (reset) button on the ESP32, or unplug/replug power.
3. You should see:
   ```
   === Room Controller Booting ===
   === Setup mode ===
   Connect to WiFi "RoomController-Setup" (password: setup1234)
   Then browse to 192.168.4.1 if it doesn't open automatically
   ```
   This is expected on a brand-new board — it has no WiFi saved yet.

### Step 7 — Connect your phone to the setup hotspot
1. On your phone, open WiFi settings, connect to **RoomController-Setup**,
   password **setup1234**.
2. Your phone will likely show a "Sign in to network" notification within
   a few seconds — tap it, and a small setup page should open
   automatically.
3. If nothing opens automatically after ~15 seconds: open any browser and
   go to `http://192.168.4.1` (must be `http://`, not `https://`).
4. Your phone will probably warn "No internet connection" for this WiFi —
   that's expected, the board's hotspot doesn't have internet access
   itself. Tap "stay connected" / "use anyway" if asked, and if you have
   mobile data on, consider toggling it off briefly so your phone doesn't
   auto-switch away from the setup network mid-way through.

### Step 8 — Fill in the setup page
1. Tap **Configure WiFi**.
2. Pick your home WiFi network from the scanned list (or type it manually
   if hidden), enter its password.
3. Scroll down to the custom field **"Firebase Database URL"** — paste
   the exact same URL you entered in the PWA's Settings in Part 3.
4. Fill in **"Profile number"** with the number shown next to this site's
   profile in the PWA's Settings (Part 3) — e.g. if Settings shows "Main
   Office #1", type `1`. Leave it blank only if you're intentionally using
   the database root directly (no profile namespacing).
5. Tap **Save**.

### Step 9 — Confirm it connected
Back in the Serial Monitor, you should see something like:
```
WiFi connected — IP: 192.168.x.x
Firebase URL: https://your-project-default-rtdb...
Profile: /profiles/1
NTP.....
Time: 14:30:05
Found 6 room(s) in Firebase
Room 4 has no relay GPIO configured — set one in the PWA Settings and reboot   ← only if you left one blank
=== Ready — state restored from Firebase ===
```
If you left "Profile number" blank, that line reads `Profile: (none — using
database root)` instead — also fine, matches pre-v4 behavior.

Your phone will show "disconnected" from RoomController-Setup around this
point — that's expected, reconnect it to your normal home WiFi.

### Step 10 — Test a relay
1. In Firebase console → Realtime Database → `profiles` → `1` → `rooms` →
   `room1` → set `override` to `true` (adjust the `1` to whatever profile
   number you configured).
2. Serial Monitor should show:
   ```
   Room 1 override: -1 → 1
   [14:30:12] Room 1 → ON
   ```
3. The relay should click, and its LED (if wired) should light up.
4. Set `override` back to `null` to return to Auto mode. Or just do this
   from the PWA dashboard instead — that's exactly what the "Force
   ON/OFF/Auto" buttons on the Rooms tab do.

---

## Part 7 — Profiles (managing more than one site)

Use the **Profiles** section in Settings to manage more than one site —
whether each one has its own separate Firebase project, or several share
one project (since v4.0, each profile gets its own numbered namespace
inside the database, so they don't collide — see the data structure
reference above).

- **+ New profile** clears the form so you can enter a new site's Database
  URL and give it a name. Saving it assigns a profile number automatically.
- Tapping any profile in the list switches the dashboard to that site.
- Each profile generates its own QR code (Settings → "Show QR code") — the
  Firebase URL *and* profile number both travel inside that specific QR
  link, so scanning one profile's code never points at another site's data,
  even when several profiles share one database.
- Room names, GPIO pins, schedules — all of it is independent per profile.
- Renaming a profile is purely cosmetic and safe at any time — the name is
  never part of how its data is stored or located, only the number is.

This only affects the PWA. Each physical ESP32 board still only ever talks
to the one Firebase URL *and* one profile number you gave it during its
own setup (Part 6) — a board doesn't "have" multiple profiles, it has
exactly one, even if that one happens to share a database with others.

---

## Part 8 — Reconfiguring later

### Changing WiFi (new router, new password, moved the board)
Hold the **BOOT** button for 3 seconds while powering the board on. It
reopens the `RoomController-Setup` hotspot — your Firebase URL is
remembered, so you'll only need to re-enter WiFi details (though the
Firebase field will be pre-filled if you want to change that too).

### Adding, removing, or repinning rooms
1. Make the change in the PWA (Settings → Room names & GPIO pins).
2. Reboot the ESP32 (press RST, or power-cycle it).

The board only reads room count and pin assignments once, at boot — this
is deliberate, so a mid-operation pin change can't be applied to a GPIO
that was never safely configured as an output. A simple reboot picks up
whatever's currently in Firebase.

### If you're not sure what changed
Watch the Serial Monitor right after a reboot — it prints exactly how many
rooms it found and flags any room with a missing relay pin.

---

## Daily operation

### Adding a schedule
1. Open the app → Rooms tab → tap a room to expand it → Slots tab.
2. Choose Today or Tomorrow, set a start/end time, tap **+ Add slot**.
3. The ESP32 picks up new slots within about 10 seconds.

### Manual override
1. Tap a room → Override tab.
2. **Force ON** → relay stays on until cleared. **Force OFF** → stays off.
   **Auto** → hands control back to the schedule.

### Activation codes (for slots requiring a PIN)
- Each coded slot gets a 4-digit code, shown in the room's slot list.
- Anyone with the QR code (Settings → Show QR code) can open the
  activation page and enter that PIN during the slot's time window.
- A separate **admin code** (shown in Settings) works across every slot,
  for staff/admin use.
- 3 wrong attempts locks that slot for 5 minutes.

### Timeline view
12-hour / Full day / Tomorrow views, with a red line marking the current
time. Amber bars = manual ON, red = manual OFF, blue = scheduled.

---

## Troubleshooting

### Board won't leave "Setup mode" / keeps rebooting into the portal
- The setup portal times out after 5 minutes with no input — power-cycle
  and try again, this time completing the form within that window.
- Make sure you tapped **Save** on the Firebase URL field, not just
  "Configure WiFi" alone.

### "No rooms found under /rooms in Firebase" then it reboots
- You configured a Database URL the board can't find any rooms under.
  Open the PWA, confirm the Database URL in Settings matches exactly, and
  that at least one room exists (Settings → Room names & GPIO pins should
  show at least Room 1).

### "Room N has no relay GPIO configured"
- That room's Relay GPIO field is empty in the PWA. Set one and reboot the
  board — this room simply won't control anything physical until then.

### Lights always ON after boot
- The relay module is active-LOW by default — GPIO pins float LOW before
  the sketch runs, which briefly energizes the relay. This clears within
  a couple of seconds once the board finishes booting. If it persists,
  swap `RELAY_ON`/`RELAY_OFF` near the top of the sketch.

### Lights flickering every few minutes
- This was fixed in v2.0 (temp-buffer slot parsing) and is unrelated to
  the v3.0 changes — if you still see it, check for a weak/unstable WiFi
  signal causing repeated bad HTTP reads.

### Schedule not triggering
- Check Serial Monitor for schedule-check output.
- Confirm the slot appears in Firebase console under
  `rooms/room1/slots`.
- Slots refresh from Firebase roughly every 10 seconds — give it a moment
  after adding one.

### GPIO conflict warning in the PWA
- Means two rooms (or a room and a relay/LED) are pointed at the same
  GPIO number. Non-blocking — it still saves — but you'll need to fix the
  actual wiring/pin assignment before both rooms will work correctly.

### PWA install option not showing
- Must be Chrome on Android or Safari on iPhone, served over HTTPS
  (GitHub Pages provides this automatically).

---

## Firebase data structure reference

Since v4.0, everything lives under a numbered profile node instead of the
database root — this is what lets multiple sites/profiles share one Firebase
project without colliding. The number is assigned automatically the first
time a profile is saved with a Database URL (see Part 3), and is what you
type into the ESP32's setup portal (Part 6) — **not** the profile's name,
which is just a display label and never appears in the path at all.

```
/profiles/
  1/
    name       : "Main Office"           ← display label only, not part of any path
    config/
      adminSeed    : "1234"              ← used by the PWA/activation page only;
                                            the ESP32 firmware never reads this
      relayWiring  : "NC" | "NO"         ← read by the ESP32 at boot
    rooms/
      room1/
        name       : "Room 1"
        override   : null | true | false    ← PWA writes this
        lightOn    : true | false            ← ESP32 writes this
        relayPin   : 26 | null               ← PWA writes this; ESP32 reads it at boot
        ledPin     : 2 | null                ← null/absent = no LED for this room
        slots      : [{s:"09:00", e:"11:00"}, ...]  ← today
        slotsT     : [{s:"09:00", e:"11:00"}, ...]  ← tomorrow
        lastSeen   : "14:32:05"              ← ESP32 heartbeat
        updatedAt  : 1234567890              ← timestamp
  2/
    name       : "Warehouse"
    config/ ...
    rooms/ ...
```

A profile created without ever being saved to a Database URL has no number
yet and never gets a QR code — see the "Save this profile first" message
in Settings if that happens.

---

## Polling intervals (unchanged from v2.0)

| Task | Interval | Purpose |
|---|---|---|
| Override poll | 5 seconds | Detect manual override change |
| Schedule check | 10 seconds | Slot start/end relay control |
| Slot refresh | 10 seconds | Pick up new slots from PWA |
| Heartbeat push | 5 minutes | Keep Firebase status current |

---

## File structure

```
room-controller/
├── pwa/
│   ├── index.html        ← Dashboard — deploy this folder to GitHub Pages
│   ├── activate.html     ← QR-code activation page
│   ├── manifest.json     ← PWA install config
│   ├── sw.js              ← Service worker (offline + caching)
│   └── icons/
│       ├── icon-192.png
│       └── icon-512.png
├── esp32/
│   └── RoomController_Firebase.ino  ← Upload as-is, no editing needed
└── docs/
    ├── DOCUMENTATION.md  ← This file
    └── wiring-diagram.html
```

---

## Version history

### v4.0 (current)
- Multiple profiles (sites) can now share one Firebase database. Each
  profile is automatically assigned an incremental numeric key the first
  time it's saved with a Database URL — all its rooms/config live under
  `/profiles/{n}/...` instead of the database root. The profile's *name*
  stays a purely cosmetic display property, never part of the storage
  path, so renaming is always safe.
- The ESP32 setup portal has a new "Profile number" field alongside the
  Firebase Database URL — matches the number shown in the PWA's Settings.
  Leave blank to use the database root directly, same as v3.0.
- QR/activation links now carry the profile number alongside the Database
  URL, so scanning one profile's code can never read/write another's data
  even when they share a database.
- Fixed a stale-cache bug in the PWA's `fbGet()` — a repeated GET on the
  exact same Firebase path could return a browser-cached response instead
  of the current server value, which broke anything that reads a value to
  decide what to write next (the profile-number allocation being the
  first case that actually exercised this pattern).

### v3.0
- Removed all hardcoded WiFi credentials and Firebase URL from the sketch
  — configured instead through a WiFiManager captive portal on first boot
  (hold BOOT 3s to reopen it later).
- Per-room relay/LED GPIO pins now configured from the PWA's Settings
  page and read from Firebase at boot, instead of a fixed array in code.
- LED indicator is now optional per room ("No LED for this room").
- Room count is dynamic — add or remove rooms from the PWA (Settings),
  reboot the ESP32 to apply.
- Added multi-profile support in the PWA for managing more than one
  independent Firebase project/site from the same app installation.
- Each profile's QR/activation link now carries that profile's own
  Database URL — no config file needed on the hosting side.
- Added a dedicated system-status LED (GPIO 2) independent of any room's
  LED, so setup/WiFi/error states can always signal even before any
  room's pins are known.

### v2.0
- Fixed relay flickering — slot refresh no longer clears relay state.
- Fixed manual override lost on bad network read.
- Added overlap detection and slot merging (PWA + ESP32).
- Added LED indicators for all 6 rooms with status patterns.
- Added PWA install icon.
- Increased schedule check to every 10 seconds.

### v1.0
- Initial release. Firebase Realtime Database integration. 6 relay
  control via HTTP polling. Schedule slots + recurring + manual override.
  Timeline view with 12h/day/tomorrow modes.

---

*Room Controller v4.0 — ESP32 + Firebase + PWA*
