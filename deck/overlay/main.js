// LD-ToyPad Bridge — Steam Deck UI v7 (hardened: error handling, race fixes, socket reconnect)
const socket = io();
let chars = [], vehs = [], allToys = [], type = 'character', world = 'All';
let toyBox = {}, padSlots = {1:null,2:null,3:null,4:null,5:null,6:null,7:null};
let pendingToy = null, pendingSlot = null, syncInProgress = false;
const PAD = { left:[1,4,5], center:[2], right:[3,6,7] };
const ZONE_TO_POSITION = { left: 2, center: 1, right: 3 };
const PLACE_ORDER = { left:[], center:[], right:[] };
const IGNORED_WORLDS = new Set(['15','16','17','18','19','20','N/A','Unknown']);
let _insertCounter = 0; // stable FIFO order independent of server tag order

// ── Helpers ───────────────────────────────────────────────────
function el(tag,cls,text){const e=document.createElement(tag);if(cls)e.className=cls;if(text)e.textContent=text;return e;}
function sleep(ms){return new Promise(r=>setTimeout(r,ms));}
function safeName(toy){return (toy&&toy.name)||'???';}

async function api(method, url, body) {
  const opts = { method, headers: { 'Content-Type': 'application/json' } };
  if (body) opts.body = JSON.stringify(body);
  const res = await fetch(url, opts);
  if (!res.ok) throw new Error(`${method} ${url} → ${res.status}`);
  return res;
}

// ── Init ──────────────────────────────────────────────────────
async function init() {
  showMeta('Loading toy data...', '');
  try {
    const [cmRes, tmRes] = await Promise.all([fetch('/json/charactormap.json'), fetch('/json/tokenmap.json')]);
    if (!cmRes.ok || !tmRes.ok) { showError('Failed to load toy catalog. Is the server running?'); return; }
    const [cm, tm] = await Promise.all([cmRes.json(), tmRes.json()]);
    chars = cm.filter(isValidChar).map(c=>({...c,type:'character',img:`/images/${c.id}.png`}));
    vehs = tm.filter(isValidVeh).map(v=>({...v,type:'token',img:`/images/${v.id}.png`}));
    allToys = [...chars, ...vehs];
    renderTabs(); applyFilter();
    await syncToyBox();
    socket.emit('connectionStatus');
    socket.emit('syncToyPad');
    setInterval(decayGlow, 1500);
  } catch(e) { showError(`Init failed: ${e.message}`); console.error(e); }
}

function showError(msg) {
  document.body.innerHTML = `<div style="padding:40px;text-align:center;color:#ff6b6b;font-family:sans-serif"><h2>⚠ Error</h2><p>${msg}</p><p style="color:#95a8a1;font-size:0.8rem">Check that the server is running and try refreshing.</p></div>`;
}

function showMeta(text, color) {
  const m = document.getElementById('meta');
  m.textContent = text;
  if (color !== undefined) m.style.color = color;
}

function isValidChar(c) { return c.name && c.name !== 'Unknown'; }
function isValidVeh(v) { return v.name && v.name !== 'Unknown'; }
function isValidWorld(w) { return w && !IGNORED_WORLDS.has(String(w)); }

// ── Tabs & Filter ─────────────────────────────────────────────
function renderTabs() {
  document.getElementById('typeTabs').innerHTML = '';
  [{k:'character',l:'Chars'},{k:'token',l:'Vehicles'}].forEach(t=>{
    const b = el('button','tab-button'+(type===t.k?' active':''),t.l);
    b.onclick=()=>{type=t.k;world='All';renderTabs();applyFilter();};
    document.getElementById('typeTabs').appendChild(b);
  });
  document.getElementById('worldTabs').innerHTML = '';
  ['All',...new Set(allToys.filter(t=>t.type===type).map(t=>t.world).filter(isValidWorld))].sort().forEach(w=>{
    const b = el('button','tab-button'+(world===w?' active':''),w);
    b.onclick=()=>{world=w;applyFilter();};
    document.getElementById('worldTabs').appendChild(b);
  });
}

function applyFilter() {
  const q = (document.getElementById('toyFilter').value||'').toLowerCase();
  let toys = allToys.filter(t=>t.type===type);
  if (world!=='All') toys = toys.filter(t=>t.world===world);
  if (q) toys = toys.filter(t=>(t.name||'').toLowerCase().includes(q)||String(t.id).includes(q)||(t.world||'').toLowerCase().includes(q));
  const g = document.getElementById('catalog'); g.innerHTML = '';
  toys.forEach(toy=>{
    const c = el('button','toy-card');
    const initials = String(toy.name||'?').slice(0,2).toUpperCase();
    c.innerHTML = `<img src="${toy.img}" alt="${toy.name}" loading="lazy" onerror="this.style.display='none';this.nextElementSibling.style.display='flex'">
      <div class="toy-card-fallback" style="display:none">${initials}</div>
      <span>${toy.name||'???'}</span>`;
    c.onclick = () => createToy(toy);
    g.appendChild(c);
  });
  document.getElementById('toyCount').textContent = `(${toys.length})`;
}
document.getElementById('toyFilter').addEventListener('input', applyFilter);

// ── Create → Toy Box ──────────────────────────────────────────
async function createToy(toy) {
  const ep = toy.type==='character'?'/character':'/vehicle';
  try {
    await api('POST', ep, { id: toy.id });
    setStatus(`Created ${safeName(toy)}`, 'ok');
    socket.emit('syncToyPad'); // Berny23 parity — server re-syncs after create
    setTimeout(syncToyBox, 500);
  } catch(e) { setStatus(`Create failed: ${e.message}`, 'error'); }
}

// ── Sync (state reset AFTER successful fetch) ─────────────────
async function syncToyBox() {
  if (syncInProgress) return; // guard against concurrent syncs
  syncInProgress = true;
  try {
    const tags = await api('GET', '/json/toytags.json').then(r=>r.json());
    // Reset state ONLY after successful fetch
    toyBox = {}; padSlots = {1:null,2:null,3:null,4:null,5:null,6:null,7:null};
    PLACE_ORDER.left = []; PLACE_ORDER.center = []; PLACE_ORDER.right = [];
    _insertCounter = 0;
    tags.forEach(t=>{
      const info = allToys.find(a=>String(a.id)===String(t.id))||{};
      if (t.index==='-1'||!t.index) {
        toyBox[t.uid] = { name: t.name||info.name||'???', id: t.id, uid: t.uid, type: t.type, order: _insertCounter++ };
      } else {
        const idx = parseInt(t.index);
        if (isNaN(idx)) return; // skip invalid indices
        padSlots[idx] = { name: t.name||info.name||'???', id: t.id, uid: t.uid, type: t.type, order: _insertCounter++ };
        for (const [zone, slots] of Object.entries(PAD))
          if (slots.includes(idx)) PLACE_ORDER[zone].push(idx);
      }
    });
    renderToyBox(); renderPad();
  } catch(e) {
    console.error('syncToyBox failed:', e);
    if (!Object.keys(toyBox).length && !Object.values(padSlots).some(Boolean)) {
      setStatus('⚠ Could not load pad state. Retrying...', 'error');
    }
  } finally { syncInProgress = false; }
}

// ── Toy Box ───────────────────────────────────────────────────
function renderToyBox() {
  const box = document.getElementById('toybox'); box.innerHTML = '';
  const entries = Object.values(toyBox);
  document.getElementById('toyboxCount').textContent = entries.length?`(${entries.length})`:'';
  if (!entries.length) { box.innerHTML = '<p class="muted">Click a character or vehicle below → it appears here → click it → pick a pad zone.</p>'; return; }
  entries.forEach(tb=>{
    const info = allToys.find(a=>String(a.id)===String(tb.id))||{};
    const c = el('button','toybox-item');
    c.innerHTML = `<img src="${info.img||''}" alt="${safeName(tb)}" onerror="this.style.display='none'"><span>${safeName(tb)}</span>`;
    c.onclick = () => openPlaceModal(tb);
    box.appendChild(c);
  });
}

// ── Collapsible Toy Box ───────────────────────────────────────
document.getElementById('toyboxToggle').addEventListener('click', () => {
  const row = document.querySelector('.toybox-row');
  const tog = document.getElementById('toyboxToggle');
  row.classList.toggle('collapsed');
  tog.textContent = (row.classList.contains('collapsed') ? '▶' : '▼') + ' Toy Box ' + document.getElementById('toyboxCount').textContent;
});

// ── Place Modal ───────────────────────────────────────────────
function openPlaceModal(tb) {
  pendingToy = tb;
  document.getElementById('placeModalName').textContent = `Place ${safeName(tb)} on:`;
  document.getElementById('placeModal').hidden = false;
}
function closePlaceModal() { pendingToy = null; document.getElementById('placeModal').hidden = true; }

async function placeOnZone(zone) {
  const tb = pendingToy; if (!tb) return;
  closePlaceModal();
  const slots = PAD[zone]; if (!slots) return;
  let target = slots.find(s => !padSlots[s]);

  if (target === undefined) {
    // Zone full — FIFO evict oldest to Toy Box
    target = PLACE_ORDER[zone].shift();
    if (target !== undefined && padSlots[target]) {
      const evicted = padSlots[target];
      try { await api('DELETE', '/remove', { index: target, uid: evicted.uid }); }
      catch(e) { setStatus(`Eviction failed: ${e.message}`, 'error'); return; }
      padSlots[target] = null;
      setStatus(`↻ ${safeName(evicted)} moved to Toy Box (zone full)`, 'ok');
    }
    target = slots.find(s => !padSlots[s]) || slots[0];
  }

  try {
    await api('POST', '/place', { uid: tb.uid, id: tb.id, position: ZONE_TO_POSITION[zone], index: target });
    setStatus(`Placed ${safeName(tb)} on ${zone.toUpperCase()}`, 'ok');
  } catch(e) { setStatus(`Place failed: ${e.message}`, 'error'); }
  setTimeout(syncToyBox, 300);
}

document.querySelectorAll('#placeModal .place-btn').forEach(b => {
  b.onclick = () => placeOnZone(b.dataset.zone);
});

// ── Pad Modal ─────────────────────────────────────────────────
function openPadModal(slot) {
  const toy = padSlots[slot]; if (!toy) return;
  pendingSlot = slot;
  document.getElementById('padModalName').textContent = `${safeName(toy)} (slot ${slot})`;
  document.getElementById('padModal').hidden = false;
}
function closePadModal() { pendingSlot = null; document.getElementById('padModal').hidden = true; }

async function moveFromPad(zone) {
  const slot = pendingSlot; if (!slot || !padSlots[slot]) return;
  const toy = padSlots[slot]; closePadModal();
  try { await api('DELETE', '/remove', { index: slot, uid: toy.uid }); }
  catch(e) { setStatus(`Move failed (remove): ${e.message}`, 'error'); return; }
  await sleep(500);
  pendingToy = toy;
  await placeOnZone(zone);
}
async function removeFromPad() {
  const slot = pendingSlot; if (!slot || !padSlots[slot]) return;
  const name = safeName(padSlots[slot]);
  try {
    await api('DELETE', '/remove', { index: slot, uid: padSlots[slot].uid });
    setStatus(`Removed ${name}`, 'ok');
  } catch(e) { setStatus(`Remove failed: ${e.message}`, 'error'); }
  closePadModal(); setTimeout(syncToyBox, 300);
}

async function deleteToken() {
  const slot = pendingSlot; if (!slot || !padSlots[slot]) return;
  const name = safeName(padSlots[slot]);
  const uid = padSlots[slot].uid;
  if (!confirm(`Permanently delete ${name}? This cannot be undone.`)) return;
  try {
    await api('DELETE', '/remove', { index: slot, uid });
    socket.emit('deleteToken', uid); // Berny23 parity — permanently remove from toytags.json
    setStatus(`Deleted ${name}`, 'ok');
  } catch(e) { setStatus(`Delete failed: ${e.message}`, 'error'); }
  closePadModal(); setTimeout(syncToyBox, 500);
}

document.querySelectorAll('#padModal .place-btn').forEach(b => {
  b.onclick = () => moveFromPad(b.dataset.zone);
});
document.querySelector('#padModal .remove-btn').onclick = removeFromPad;
document.querySelector('#padModal .delete-btn').onclick = deleteToken;

// ── ToyPad ────────────────────────────────────────────────────
function renderPad() {
  document.querySelectorAll('.pad-slots').forEach(el=>{
    const slots = el.dataset.slots.split(',').map(Number);
    el.innerHTML = '';
    slots.forEach(s=>{
      const d = el('div','pad-slot');
      const toy = padSlots[s];
      if (toy) {
        const info = allToys.find(a=>String(a.id)===String(toy.id))||{};
        const initials = String(toy.name||'?').slice(0,2).toUpperCase();
        d.innerHTML = `<img src="${info.img||''}" alt="${safeName(toy)}" onerror="this.style.display='none';this.nextElementSibling.style.display='flex'">
          <div class="pad-slot-fallback" style="display:none">${initials}</div>
          <span>${safeName(toy)}</span>`;
        d.style.cursor = 'pointer';
        d.onclick = () => openPadModal(s);
      } else {
        d.innerHTML = `<span class="empty-slot">Slot ${s}</span>`;
      }
      el.appendChild(d);
    });
  });
}

// ── Status ────────────────────────────────────────────────────
function setStatus(msg, cls) {
  const sl = document.getElementById('statusLine');
  sl.textContent = msg; sl.className = cls || '';
  if (cls === 'ok') setTimeout(() => { if (sl.textContent === msg) sl.textContent = ''; }, 3000);
}

// ── Keystone Glow ─────────────────────────────────────────────
const litTimestamps = { left:0, center:0, right:0 };
function applyZoneGlow(zoneName) {
  litTimestamps[zoneName] = Date.now();
  const el = document.querySelector(`.pad-zone[data-zone="${zoneName}"]`);
  if (!el) return;
  el.classList.add('zone-lit'); el.classList.remove('zone-lit-sustain');
}
function decayGlow() {
  const now = Date.now();
  for (const [zone, ts] of Object.entries(litTimestamps)) {
    if (!ts) continue;
    const el = document.querySelector(`.pad-zone[data-zone="${zone}"]`);
    if (!el) continue;
    const age = now - ts;
    if (age > 3000) { el.classList.remove('zone-lit', 'zone-lit-sustain'); litTimestamps[zone] = 0; }
    else if (age > 2000) { el.classList.remove('zone-lit'); el.classList.add('zone-lit-sustain'); }
  }
}

// ── LED Socket Handlers (1:1 Berny23 parity — actual hex colors on pad zones) ──
const zoneColorCache = { center: '#ffffff', left: '#ffffff', right: '#ffffff' };
function setZoneColor(zone, hex) {
  if (!hex) return;
  zoneColorCache[zone] = hex;
  const el = document.querySelector(`.pad-zone[data-zone="${zone}"]`);
  if (!el) return;
  el.style.backgroundColor = hex + '40';
  el.style.borderColor = hex;
  el.setAttribute('color', hex);
  applyZoneGlow(zone);
}
function fadeZoneColor(zone, hex, speedMs) {
  if (!hex) return;
  const el = document.querySelector(`.pad-zone[data-zone="${zone}"]`);
  if (!el) return;
  const prevColor = el.getAttribute('color') || zoneColorCache[zone] || '#ffffff';
  el.style.backgroundColor = hex + '40';
  el.style.borderColor = hex;
  el.setAttribute('color', hex);
  setTimeout(() => {
    el.style.backgroundColor = prevColor + '40';
    el.style.borderColor = prevColor;
    el.setAttribute('color', prevColor);
  }, speedMs * 100);
}
function handleLED(e, handler) {
  try { if (Array.isArray(e)) handler(e); } catch(ex) { /* silently ignore malformed LED events */ }
}
// Color One: [padNumber, colorHex] — pad 1=center, 2=left, 3=right (Berny23 convention)
socket.on('Color One', (e) => handleLED(e, (e)=>{ if(e[0]>=1&&e[0]<=3) setZoneColor(['','center','left','right'][e[0]], e[1]); updatePortalTelemetry(); }));
// Color All: [centerColor, leftColor, rightColor]
socket.on('Color All', (e) => handleLED(e, (e)=>{ if(e[0])setZoneColor('center',e[0]); if(e[1])setZoneColor('left',e[1]); if(e[2])setZoneColor('right',e[2]); updatePortalTelemetry(); }));
// Fade One: [padNumber, speed, cycles, colorHex]
socket.on('Fade One', (e) => handleLED(e, (e)=>{ if(e[0]>=1&&e[0]<=3) fadeZoneColor(['','center','left','right'][e[0]], e[3], e[1]||1); updatePortalTelemetry(); }));
// Fade All: [topSpeed, topCycles, topColor, leftSpeed, leftCycles, leftColor, rightSpeed, rightCycles, rightColor]
socket.on('Fade All', (e) => handleLED(e, (e)=>{ if(e[2])fadeZoneColor('center',e[2],e[0]||1); if(e[5])fadeZoneColor('left',e[5],e[3]||1); if(e[8])fadeZoneColor('right',e[8],e[6]||1); updatePortalTelemetry(); }));

function updatePortalTelemetry() {
  const active = [], now = Date.now();
  for (const [zone, ts] of Object.entries(litTimestamps))
    if (ts && (now-ts)<3000) active.push(zone.toUpperCase());
  document.getElementById('portalTelemetry').textContent = active.length
    ? `Portal lights inferred: ${active.join(', ')}` : 'Portal lights: waiting for packet data...';
}

// ── Socket Lifecycle ──────────────────────────────────────────
socket.on('connect', () => {
  socket.emit('connectionStatus');
  socket.emit('syncToyPad');
});
socket.on('disconnect', () => {
  showMeta('Disconnected — reconnecting...', '#ff6b6b');
});
socket.on('Connection True', () => {
  showMeta('Connected to PS3 ✓', '#3cc47c');
});
socket.on('refreshTokens', () => setTimeout(syncToyBox, 1000));

// ── Image Sync ────────────────────────────────────────────────
let syncRunning = false;
document.getElementById('syncImagesBtn').addEventListener('click', async () => {
  if (syncRunning) return;
  syncRunning = true;
  const btn = document.getElementById('syncImagesBtn');
  const modal = document.getElementById('syncModal');
  const progress = document.getElementById('syncProgress');
  const bar = document.getElementById('syncBarFill');
  btn.disabled = true; btn.textContent = '⏳ Syncing...';
  modal.hidden = false; progress.textContent = 'Starting...'; bar.style.width = '0%';
  try {
    const res = await fetch('/api/sync-images', { method: 'POST' });
    if (!res.ok) throw new Error(`Server returned ${res.status}`);
    const reader = res.body.getReader(); const decoder = new TextDecoder(); let buf = '';
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      buf += decoder.decode(value, { stream: true });
      const lines = buf.split('\n'); buf = lines.pop();
      for (const line of lines) {
        if (!line.trim()) continue;
        try { const d = JSON.parse(line); progress.textContent = d.msg||line; if (d.pct!==undefined) bar.style.width = d.pct+'%'; }
        catch { progress.textContent = line; }
      }
    }
    progress.textContent = 'Sync complete.'; bar.style.width = '100%';
  } catch(e) { progress.textContent = `Sync failed: ${e.message}`; }
  finally { syncRunning = false; btn.disabled = false; btn.textContent = '⬇ Sync Missing Images'; }
});

// ── Sync Button ──────────────────────────────────────────────────
document.getElementById('syncBtn').addEventListener('click', () => {
  socket.emit('syncToyPad');
  setStatus('Syncing...', 'ok');
  setTimeout(syncToyBox, 500);
});

// ── Start ─────────────────────────────────────────────────────
init();
