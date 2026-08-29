/**
 * License Registry Module — Lights On
 *
 * Reports license usage to the app owner's central Firestore project
 * using the REST API (no Firebase SDK required — keeps LightsOn SDK-free).
 *
 * Behavior:
 * - Every app open (when licensed): writes heartbeat to
 *   _license_registry/{docId}
 * - Checks _license_registry/_meta for the current tracking month
 * - If the month has changed (new month started), the FIRST device to
 *   detect it:
 *   1. Reads all docs from _license_registry
 *   2. Copies them to _license_history (keyed by month + docId)
 *   3. Deletes originals from _license_registry
 *   4. Updates _meta.month to the current month
 *   5. Writes its own heartbeat as the first entry of the new month
 *
 * Entirely non-critical — all errors are silently swallowed so this
 * never interferes with the app's normal operation.
 */
const LicenseRegistry = (function () {
  'use strict';

  var BASE_URL = 'https://firestore.googleapis.com/v1/projects/';

  function getMonthKey() {
    var now = new Date();
    return now.getFullYear() + '-' + String(now.getMonth() + 1).padStart(2, '0');
  }

  function firestoreUrl(collection, docId) {
    var projectId = LICENSE_REGISTRY_CONFIG.projectId;
    var url = BASE_URL + projectId + '/databases/(default)/documents/' + collection;
    if (docId) url += '/' + docId;
    return url;
  }

  // Convert a plain JS object to Firestore REST "fields" format
  function toFirestoreFields(obj) {
    var fields = {};
    for (var key in obj) {
      if (!obj.hasOwnProperty(key)) continue;
      var val = obj[key];
      if (val === null || val === undefined) {
        fields[key] = { nullValue: null };
      } else if (typeof val === 'number') {
        fields[key] = Number.isInteger(val) ? { integerValue: String(val) } : { doubleValue: val };
      } else if (typeof val === 'boolean') {
        fields[key] = { booleanValue: val };
      } else {
        fields[key] = { stringValue: String(val) };
      }
    }
    return fields;
  }

  // Read a Firestore document — returns parsed fields object or null
  async function fsGet(collection, docId) {
    try {
      var url = firestoreUrl(collection, docId);
      var res = await fetch(url, { cache: 'no-store' });
      if (!res.ok) return null;
      var doc = await res.json();
      return parseFields(doc.fields || {});
    } catch (e) { return null; }
  }

  // Write/overwrite a Firestore document
  async function fsSet(collection, docId, data) {
    try {
      var url = firestoreUrl(collection, docId);
      var body = { fields: toFirestoreFields(data) };
      var res = await fetch(url + '?currentDocument.exists=true', {
        method: 'PATCH',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body)
      });
      // If doc doesn't exist yet, create it
      if (!res.ok) {
        res = await fetch(firestoreUrl(collection) + '?documentId=' + encodeURIComponent(docId), {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(body)
        });
      }
      return res.ok;
    } catch (e) { return false; }
  }

  // Delete a Firestore document
  async function fsDelete(collection, docId) {
    try {
      var url = firestoreUrl(collection, docId);
      await fetch(url, { method: 'DELETE' });
    } catch (e) {}
  }

  // List all documents in a collection
  async function fsList(collection) {
    try {
      var url = firestoreUrl(collection) + '?pageSize=500';
      var res = await fetch(url, { cache: 'no-store' });
      if (!res.ok) return [];
      var data = await res.json();
      var docs = data.documents || [];
      return docs.map(function (doc) {
        var parts = doc.name.split('/');
        return { id: parts[parts.length - 1], data: parseFields(doc.fields || {}) };
      });
    } catch (e) { return []; }
  }

  // Parse Firestore field values back to plain JS
  function parseFields(fields) {
    var obj = {};
    for (var key in fields) {
      if (!fields.hasOwnProperty(key)) continue;
      var f = fields[key];
      if ('stringValue' in f) obj[key] = f.stringValue;
      else if ('integerValue' in f) obj[key] = parseInt(f.integerValue, 10);
      else if ('doubleValue' in f) obj[key] = f.doubleValue;
      else if ('booleanValue' in f) obj[key] = f.booleanValue;
      else if ('nullValue' in f) obj[key] = null;
      else obj[key] = null;
    }
    return obj;
  }

  // Generate a stable device ID for this browser/device
  function getDeviceId() {
    var key = 'lo_device_id';
    var id = localStorage.getItem(key);
    if (!id) {
      id = Date.now().toString(36) + Math.random().toString(36).substring(2, 10);
      localStorage.setItem(key, id);
    }
    return id;
  }

  /**
   * Main entry point — call once on app boot (after license is validated).
   * Only reports if the app is licensed (licenseValid === true).
   */
  async function report() {
    try {
      // Guard: only report if licensed
      if (typeof licenseValid === 'undefined' || !licenseValid) return;
      if (typeof LICENSE_REGISTRY_CONFIG === 'undefined') return;
      if (!LICENSE_REGISTRY_CONFIG.projectId) return;

      var regCol = LICENSE_REGISTRY_CONFIG.registryCollection || '_license_registry';
      var currentMonth = getMonthKey();

      // --- Check if month has rolled over ---
      var meta = await fsGet(regCol, '_meta');
      var storedMonth = meta ? (meta.month || '') : '';

      if (storedMonth && storedMonth !== currentMonth) {
        await archiveAndReset(regCol, storedMonth, currentMonth);
      } else if (!storedMonth) {
        // First time ever — just set the meta month
        await fsSet(regCol, '_meta', { month: currentMonth });
      }

      // --- Write heartbeat ---
      var licenseKey = '';
      try { licenseKey = (state.licenseKey || '').trim(); } catch (e) {}

      var licenseData = {};
      try { licenseData = JSON.parse(atob(licenseKey)); } catch (e) {}

      var deviceId = getDeviceId();
      var licenseName = licenseData.n || 'unknown';
      var licenseHash = (licenseData.h || 'unknown').substring(0, 16);

      // Customer's Firebase DB URL (the one they entered in Settings)
      var customerDbUrl = '';
      try {
        var p = getActiveProfile();
        if (p && p.dbUrl) customerDbUrl = p.dbUrl;
      } catch (e) {}

      var profileName = '';
      try {
        var p2 = getActiveProfile();
        if (p2 && p2.name) profileName = p2.name;
      } catch (e) {}

      // Doc ID: hash prefix + appId + deviceId prefix — stable per device+license
      var docId = licenseHash + '_' + LICENSE_REGISTRY_CONFIG.appId + '_' + deviceId.substring(0, 8);

      await fsSet(regCol, docId, {
        licenseName: licenseName,
        licenseHash: licenseData.h || 'unknown',
        customerDbUrl: customerDbUrl,
        profileName: profileName,
        deviceId: deviceId,
        lastSeen: new Date().toISOString(),
        appId: LICENSE_REGISTRY_CONFIG.appId,
        appVersion: 'v4.0'
      });

    } catch (e) {
      // Non-critical — never surface errors to the user
      console.debug('LicenseRegistry: report failed (non-critical)', e);
    }
  }

  /**
   * Archive all current registry docs to _license_history,
   * then delete them from _license_registry, and update _meta.month.
   */
  async function archiveAndReset(regCol, oldMonth, newMonth) {
    try {
      var historyCol = LICENSE_REGISTRY_CONFIG.historyCollection || '_license_history';

      // Read all docs from registry (except _meta)
      var docs = await fsList(regCol);

      for (var i = 0; i < docs.length; i++) {
        if (docs[i].id === '_meta') continue;

        // Copy to history: _license_history/{oldMonth}_{docId}
        var historyDocId = oldMonth + '_' + docs[i].id;
        var data = docs[i].data;
        data.month = oldMonth;
        await fsSet(historyCol, historyDocId, data);

        // Delete from registry
        await fsDelete(regCol, docs[i].id);
      }

      // Update meta to current month
      await fsSet(regCol, '_meta', { month: newMonth });

    } catch (e) {
      console.debug('LicenseRegistry: archiveAndReset failed (non-critical)', e);
      // Still update meta so we don't retry the archive endlessly
      try { await fsSet(regCol, '_meta', { month: newMonth }); } catch (e2) {}
    }
  }

  return { report: report };
})();
