// Adds the cross-origin isolation headers to every response, so hosts without them (e.g. GitHub Pages) can run threaded wasm.
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));
self.addEventListener('fetch', (e) => {
  if (e.request.cache === 'only-if-cached' && e.request.mode !== 'same-origin') return;
  e.respondWith(fetch(e.request).then((r) => {
    if (r.status === 0) return r;
    const h = new Headers(r.headers);
    h.set('Cross-Origin-Embedder-Policy', 'require-corp');
    h.set('Cross-Origin-Opener-Policy', 'same-origin');
    h.set('Cross-Origin-Resource-Policy', 'cross-origin');
    return new Response(r.body, { status: r.status, statusText: r.statusText, headers: h });
  }));
});
