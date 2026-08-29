const CACHE = 'roomctrl-fb-v13';
const STATIC_ASSETS = [
  './manifest.json',
  './icons/icon-192.png',
  './icons/icon-512.png',
  'https://cdnjs.cloudflare.com/ajax/libs/qrcodejs/1.0.0/qrcode.min.js'
];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(STATIC_ASSETS)));
  self.skipWaiting();
});

self.addEventListener('activate', e => {
  e.waitUntil(
    caches.keys().then(keys =>
      Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k)))
    )
  );
  self.clients.claim();
});

self.addEventListener('fetch', e => {
  // Only handle GET requests
  if (e.request.method !== 'GET') return;

  // Never cache Firebase requests — Realtime Database uses two different
  // domain patterns depending on the project's region: the older/default
  // "project.firebaseio.com" and the newer regional
  // "project-default-rtdb.REGION.firebasedatabase.app" (e.g. this project's
  // own asia-southeast1 URL). Missing the second pattern meant every
  // Firebase call — including the EventSource connection — was being
  // routed through this service worker's cache-then-network logic instead
  // of going straight to the network, which is never correct for
  // rapidly-changing real-time data.
  if (e.request.url.includes('firebaseio.com') || e.request.url.includes('firebasedatabase.app') || e.request.url.includes('googleapis.com')) return;

  // NEVER cache activate.html — always fetch fresh from network
  // This ensures slot data is always current
  if (e.request.url.includes('activate.html')) {
    e.respondWith(
      fetch(e.request, { cache: 'no-store' })
        .catch(() => new Response('Activation page unavailable offline. Please connect to internet.', {
          headers: { 'Content-Type': 'text/plain' }
        }))
    );
    return;
  }

  // index.html / product-info.js — network first, cache fallback.
  // product-info.js holds contact details meant to be edited over time
  // (see the file itself) — lumping it into the generic "static assets,
  // cache-first" bucket below meant that once any device cached it once,
  // it would keep serving that same snapshot forever, never re-checking
  // the network even after a real deploy changed the file.
  if (e.request.destination === 'document' ||
      e.request.url.endsWith('index.html') ||
      e.request.url.endsWith('product-info.js') ||
      e.request.url.endsWith('license-registry.js') ||
      e.request.url.endsWith('license-registry-config.js') ||
      e.request.url.endsWith('/')) {
    e.respondWith(
      fetch(e.request, { cache: 'no-store' })
        .then(res => {
          const clone = res.clone();
          // Caching is a best-effort side effect — a failure here (e.g. an
          // aborted/error-type response mid-navigation) must never become an
          // unhandled rejection or block the actual page response
          caches.open(CACHE).then(c => c.put(e.request, clone)).catch(() => {});
          return res;
        })
        // If network AND cache both come up empty (e.g. first-ever offline
        // load), respondWith still needs a real Response, not undefined —
        // that's what causes "Failed to convert value to 'Response'"
        .catch(() =>
          caches.match(e.request).then(cached =>
            cached || new Response(
              'Room Controller is offline and this page was never cached yet. Please connect to the internet and reload.',
              { headers: { 'Content-Type': 'text/plain' } }
            )
          )
        )
    );
    return;
  }

  // Static assets — cache first
  e.respondWith(
    caches.match(e.request).then(cached =>
      cached || fetch(e.request).then(res => {
        const clone = res.clone();
        caches.open(CACHE).then(c => c.put(e.request, clone)).catch(() => {});
        return res;
      }).catch(() => cached || new Response('', { status: 504, statusText: 'Offline and not cached' }))
    )
  );
});

self.addEventListener('message', e => {
  if (e.data === 'skipWaiting') self.skipWaiting();
});
