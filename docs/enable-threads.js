/*
 * enable-threads.js — Ensures cross-origin isolation for SharedArrayBuffer.
 *
 * Based on the approach from github.com/nicoleahmed and josephrocca's
 * clip-image-sorter. On GitHub Pages we can't set HTTP headers, so we use
 * a service worker to inject COOP/COEP headers on all responses.
 *
 * This script must be loaded BEFORE any code that needs SharedArrayBuffer.
 * On first load it registers the SW and reloads; on subsequent loads the
 * SW is already active and crossOriginIsolated === true.
 */

// Mini-SW that adds COOP/COEP headers (inlined as a blob for reliability)
const swCode = `
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));
self.addEventListener('fetch', (e) => {
  if (e.request.cache === 'only-if-cached' && e.request.mode !== 'same-origin') return;
  e.respondWith(
    fetch(e.request).then((r) => {
      if (r.status === 0) return r;
      const h = new Headers(r.headers);
      h.set('Cross-Origin-Embedder-Policy', 'require-corp');
      h.set('Cross-Origin-Opener-Policy', 'same-origin');
      return new Response(r.body, { status: r.status, statusText: r.statusText, headers: h });
    }).catch((err) => new Response('SW error: ' + err, { status: 500 }))
  );
});
`;

if (!crossOriginIsolated && window.SharedArrayBuffer === undefined) {
  if ('serviceWorker' in navigator) {
    // Try registering sw.js (file-based SW has broader scope)
    navigator.serviceWorker.register('sw.js', { scope: './' }).then((reg) => {
      if (reg.installing || reg.waiting) {
        // SW is installing — wait for it to activate, then reload
        const sw = reg.installing || reg.waiting;
        sw.addEventListener('statechange', () => {
          if (sw.state === 'activated') {
            console.log('[enable-threads] SW activated, reloading for COOP/COEP...');
            window.location.reload();
          }
        });
      } else if (reg.active && !navigator.serviceWorker.controller) {
        // SW is active but not controlling this page — reload
        console.log('[enable-threads] SW active but not controlling, reloading...');
        window.location.reload();
      }
    }).catch((err) => {
      console.warn('[enable-threads] SW registration failed, trying blob SW:', err);
      // Fallback: blob-based SW (works on some hosts where sw.js path fails)
      const blob = new Blob([swCode], { type: 'application/javascript' });
      const swUrl = URL.createObjectURL(blob);
      navigator.serviceWorker.register(swUrl, { scope: './' }).then((reg) => {
        const sw = reg.installing || reg.waiting;
        if (sw) {
          sw.addEventListener('statechange', () => {
            if (sw.state === 'activated') window.location.reload();
          });
        }
      }).catch((e2) => {
        console.error('[enable-threads] All SW registration methods failed:', e2);
      });
    });
  }
}
