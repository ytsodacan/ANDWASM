// ANDWASM web loader: boots a game built by ANDWASM from the folder this script sits in.
// Reads manifest.json, downloads each file (checked against its sha256), unpacks the asset pack,
// then starts the emscripten glue. Needs cross-origin isolation for threads (see coi-sw.js / serve.py).
(function () {
  const base = new URL('./', document.currentScript.src).href;
  const status = document.getElementById('status'), bar = document.getElementById('bar');
  const say = (t, pct) => { status.textContent = t; if (pct != null) bar.style.width = pct + '%'; };
  const gunzip = (ab) => new Response(new Blob([ab]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer();
  const hex = async (ab) => Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', ab)), (b) => b.toString(16).padStart(2, '0')).join('');

  async function fetchChecked(f) {
    const r = await fetch(base + (f.path || f.name) + '?h=' + f.sha256.slice(0, 16));
    if (!r.ok) throw new Error(f.name + ' ' + r.status);
    const ab = await r.arrayBuffer();
    if ((await hex(ab)) !== f.sha256) throw new Error(f.name + ' failed its checksum');
    return ab;
  }

  async function boot() {
    const man = await (await fetch(base + 'manifest.json', { cache: 'no-store' })).json();
    const files = man.files.filter((f) => f.kind !== 'glue');
    const blobs = {}, assetParts = [];
    let done = 0;
    for (const f of files) {
      say('Downloading ' + f.name + ' (' + (done + 1) + '/' + files.length + ')', (100 * done) / files.length);
      const raw = await gunzip(await fetchChecked(f));
      if (f.kind === 'wasm' || f.kind === 'data') {
        blobs[f.name.replace(/\.gz$/, '')] = URL.createObjectURL(new Blob([raw], { type: f.kind === 'wasm' ? 'application/wasm' : 'application/octet-stream' }));
      } else if (f.kind === 'assets') {
        assetParts.push(new Uint8Array(raw));
      }
      done++;
    }
    // The asset pack is a stream of [u32 path length][path][u32 data length][data] (big-endian), split over parts in order.
    const total = assetParts.reduce((a, p) => a + p.length, 0), assets = new Uint8Array(total);
    let at = 0; for (const p of assetParts) { assets.set(p, at); at += p.length; }

    const glue = man.files.find((f) => f.kind === 'glue');
    if (!glue) throw new Error('manifest has no glue script');
    await fetchChecked(glue);   // verify it before running it

    say('Starting…', 100);
    window.Module = {
      canvas: document.getElementById('canvas'),
      locateFile: (n) => blobs[n] || n,
      print: (t) => console.log(t),
      printErr: (t) => console.warn(t),
      setStatus: (t) => { if (t) status.textContent = t; else document.getElementById('toast').classList.add('done'); },
      preRun: [() => {
        const dv = new DataView(assets.buffer, assets.byteOffset, assets.byteLength), dec = new TextDecoder(), made = new Set();
        for (let o = 0; o < assets.length;) {
          const pl = dv.getUint32(o); o += 4; const path = dec.decode(assets.subarray(o, o + pl)); o += pl;
          const dl = dv.getUint32(o); o += 4;
          const full = '/assets/' + path, dir = full.slice(0, full.lastIndexOf('/'));
          if (!made.has(dir)) { FS.mkdirTree(dir); made.add(dir); }
          FS.writeFile(full, assets.subarray(o, o + dl)); o += dl;
        }
      }],
    };
    const sc = document.createElement('script');
    sc.src = base + glue.name + '?h=' + glue.sha256.slice(0, 16);
    document.body.appendChild(sc);
  }

  // Threads need cross-origin isolation. Hosts that can't send the headers get coi-sw.js, then one reload.
  if (!window.crossOriginIsolated && 'serviceWorker' in navigator && window.isSecureContext && !sessionStorage.getItem('andwasm-coi')) {
    sessionStorage.setItem('andwasm-coi', '1');
    navigator.serviceWorker.register(base + 'coi-sw.js').then(() => navigator.serviceWorker.ready).then(() => location.reload());
    say('Preparing the browser…');
  } else {
    boot().catch((e) => { console.error(e); say('Could not start: ' + e.message); });
  }
})();
