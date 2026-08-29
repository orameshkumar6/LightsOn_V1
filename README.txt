╔══════════════════════════════════════════════════════╗
║         ROOM CONTROLLER v4.0 — QUICK START           ║
║         Firebase + ESP32 + PWA                       ║
╚══════════════════════════════════════════════════════╝

NOTHING IS HARDCODED ANYMORE
─────────────────────────────
There is no single set of credentials baked into this package. Every
deployment (your own ESP32 + your own Firebase project) configures itself
at first boot / first use — nothing to edit in the source files.

FILES IN THIS PACKAGE
─────────────────────
pwa/
  index.html      ← The dashboard app — deploy to GitHub Pages
  activate.html   ← The QR-code activation page — deploy alongside index.html
  manifest.json   ← PWA install config
  sw.js           ← Service worker (offline support)
  icons/
    icon-192.png  ← App icon
    icon-512.png  ← App icon large

esp32/
  RoomController_Firebase.ino  ← Upload to ESP32 — no editing required

docs/
  DOCUMENTATION.md     ← Full step-by-step guide — start here if you're
                          new to ESP32; it assumes no prior experience
  wiring-diagram.html  ← Open in browser for wiring

4 THINGS TO DO
───────────────
1. CREATE A FIREBASE PROJECT + REALTIME DATABASE
   Free tier is enough. See docs/DOCUMENTATION.md Part 1.

2. DEPLOY THE PWA TO GITHUB PAGES (no config needed)
   Upload the pwa/ folder contents as-is — nothing to fill in before
   deploying. See docs/DOCUMENTATION.md Part 2.

3. IN THE DEPLOYED APP: create a profile + assign room GPIO pins
   Open your deployed URL → Settings → enter your Firebase Database URL
   as a new profile → set each room's Relay GPIO (and LED GPIO, optional)
   → Save. Note the profile number shown next to its name (e.g. "#1") —
   you'll need it in step 4. Multiple profiles can safely share the same
   Database URL; each gets its own number automatically.
   See docs/DOCUMENTATION.md Part 3.

4. UPLOAD THE SKETCH, THEN CONFIGURE WIFI ON THE BOARD ITSELF
   - Install ArduinoDroid + the WiFiManager library (Library Manager)
   - Upload the .ino as-is — no lines to edit
   - On first power-up, the board creates its own WiFi hotspot
     "RoomController-Setup" (password: setup1234) — connect to it with
     your phone, fill in your home WiFi + the Firebase Database URL from
     step 3, plus the profile number from step 3, tap Save. The board
     reboots and starts running.
   See docs/DOCUMENTATION.md Part 6 for the full walkthrough with pictures
   of what each screen should look like.

TO RECONFIGURE WIFI LATER
──────────────────────────
Hold the BOOT button on the ESP32 for 3 seconds while powering it on —
this reopens the setup hotspot without erasing your Firebase URL.

WIRING
──────
GPIO pin numbers are no longer fixed — you choose them in the PWA's
Settings page (Part 3 above), then wire accordingly. See
docs/DOCUMENTATION.md Part 4 for a safe-pin cheat sheet and Part 5 for
the physical wiring steps (unchanged relay/LED wiring pattern from v2.0).

Relay module: VCC → ESP32 VIN, GND → ESP32 GND, INx → whichever GPIO you
assigned that room's relay to.

LED indicators (optional per room): GPIO → 330Ω → LED+ → LED- → GND.
LED long leg (+) = positive = connects to resistor.
LED short leg (-) = negative = connects to GND.

PRIORITY LOGIC (unchanged)
───────────────────────────
  Manual Override ON  → relay always ON  (schedule ignored)
  Manual Override OFF → relay always OFF (schedule ignored)
  Auto mode (null)    → schedule controls relay

See docs/DOCUMENTATION.md for full details, troubleshooting, and a plain-
English glossary of ESP32 terms if you're new to this board.

v4.0 — Multiple profiles can now share one Firebase database, each under
       its own automatically-assigned number. See DOCUMENTATION.md Part 7.
v3.0 — Captive-portal WiFi/Firebase setup, per-room GPIO pins configured
       from the PWA, optional per-room LED, add/remove rooms from the app.
