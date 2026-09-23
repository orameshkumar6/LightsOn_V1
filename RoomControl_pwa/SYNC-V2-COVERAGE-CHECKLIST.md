# Lights On Sync V2 mutation coverage

## Rule
Every mutation that changes an effective slot or schedule writes the legacy projection during migration, the ID-keyed canonical record, a `/sync/changes/{revision}` entry, and `/sync/meta/revision`. Settings use `/configSync/version`, not the daily slot revision.

## Main admin PWA (`index.html`)

- [x] Existing slot edit: `updateSlotById` publishes canonical UPDATE.
- [x] Soft delete: `deleteSlotById` uses `updateSlotById` and publishes DELETE.
- [x] Paid/unpaid change: uses `updateSlotById` and publishes UPDATE.
- [x] Activation-code change: uses `updateSlotById` and publishes UPDATE.
- [x] Admin activation/deactivation: uses `updateSlotById` and publishes UPDATE.
- [x] New slot from modal: goes through `pushRoom`; publishes canonical room snapshot and delta.
- [x] Imported booking: goes through `pushRoom`; publishes canonical room snapshot and delta.
- [x] Confirmed booking request: goes through `pushRoom`; publishes canonical room snapshot and delta.
- [x] Copy slots: goes through room synchronization; publishes canonical room snapshot and delta.
- [x] Recurring CREATE: writes `/recurringDefs/roomN/{defId}` and explicit `recurringDef/create` delta.
- [x] Recurring UPDATE: updates the definition and writes explicit `recurringDef/update` delta.
- [x] Recurring DELETE: writes a definition tombstone and explicit `recurringDef/delete` delta.
- [x] Recurring materialization: `pushRoom` also publishes current today/tomorrow slot snapshots.
- [x] Forced/midnight rollover: establishes authoritative buckets and resets daily Sync V2 to generation + revision 1 with `FULL_SYNC`.
- [x] Settings save: increments `/configSync/version`; it does not reset daily slot sync.

## Customer activation (`activate.html`)

- [x] Successful PIN/admin activation: canonical record + slot UPDATE delta.
- [x] Attempts reset and lock cleared on activation: included in same slot update.
- [x] Wrong PIN attempt count: canonical record + slot UPDATE delta.
- [x] Lockout timestamp: canonical record + slot UPDATE delta.

## Public request page (`request.html`)

- [x] Ordinary request submission: no slot delta because no slot exists yet.
- [x] Decline/dismiss request: no slot delta because schedule is unchanged.
- [x] Auto-approved request: canonical slot CREATE + revision/change entry before request confirmation.

## ESP32 (`RoomController_Firebase_SyncV2.ino`)

- [x] Reads daily generation/revision cursor.
- [x] Applies slot CREATE/UPDATE/DELETE deltas by stable slot ID.
- [x] Applies recurringDef CREATE/UPDATE/DELETE deltas by stable defId; room snapshots refresh materialized occurrences.
- [x] Performs full canonical refresh after generation mismatch or room/recurring snapshot event.
- [x] Checks independent config version.
- [x] Persists sync cursor in LittleFS.
- [x] Writes expired state to canonical and legacy projection during migration.
- [x] Keeps heartbeat/light status outside slot sync.

## Migration compatibility

- [x] PWA still writes legacy `/rooms/roomN/slots` and `/slotsT` while controllers migrate.
- [x] Canonical records use `/slotRecords/roomN/today|tomorrow/{slotId}`.
- [x] Recurring definitions use `/recurringDefs/roomN/{defId}`.
- [x] Legacy projection should be removed only after every controller runs Sync V2 firmware.
