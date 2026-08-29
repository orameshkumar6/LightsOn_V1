/**
 * License Registry Configuration
 * 
 * This connects to the APP OWNER's Firebase project (not the customer's).
 * Uses Firestore REST API (no SDK needed) to track which license keys
 * are being used against which customer database instances.
 *
 * Same central project as Track_Your_Fitness and other products — each
 * product writes to the same _license_registry collection, distinguished
 * by its appId field.
 */
const LICENSE_REGISTRY_CONFIG = {
  apiKey: 'AIzaSyCr3rjWD-1ulmGFXoI5VW1Z258lh0WSQc4',
  projectId: 'sivaramesalicenseusage',

  // Collection name in Firestore where license usage is tracked
  registryCollection: '_license_registry',

  // Collection name for monthly archives
  historyCollection: '_license_history',

  // App identifier — distinguishes this product from others sharing the same registry
  appId: 'lights_on'
};
