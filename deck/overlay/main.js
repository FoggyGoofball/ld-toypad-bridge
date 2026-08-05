// LD-ToyPad Bridge — Steam Deck UI v6 (1:1 Berny23 parity, 80px cards, sticky pad, collapsible toybox)
const socket = io();
let chars = [], vehs = [], allToys = [], type = 'character', world = 'All';
let toyBox = {}, padSlots = {1:null,2:null,3:null,4:null,5:null,6:null,7:null};
let pendingToy = null, pendingSlot = null;
const PAD = { left:[1,4,5], center:[2], right:[3,6,7] };
// 🔴 FIXED: zone→position mapping (center=1, left=2, right=3 per Berny23's pad-num)
const ZONE_TO_POSITION = { left: 2, center: 1, right: 3 };
const PLACE_ORDER = { left:[], center:[], right:[] };
const IGNORED_WORLDS = new Set(['15','16','17','18','19','20','N/A','Unknown']);

// ── Berny23-matching filters ──────────────────────────────────
function isValidChar(c) { return c.name && c.name !== 'Unknown'; }
function isValidVeh(v) { return v.name && v.name !== 'Unknown'; }
function isValidWorld(w) { return w && !IGNORED_WORLDS.has(String(w)); }

async function init() {
  const [cm, tm] = await Promise.all([
    fetch('/json/charactermap.json').then(r=>r.json()),
    fetch('/json/tokenmap.json').then(r=>r.json())
  ]);
  chars = cm.filter(isValidChar).map(c=>({...c,type:'character',img:`/images/${c.id}.png`}));
  vehs = tm.filter(isValidVeh).map(v=>({...v,type:'token',img:`/images/${v.id}.png`}));
  allToys = [...chars, ...vehs];
  renderTabs(); applyFilter(); syncToyBox();
  // 🔴 FIXED: emit connectionStatus + syncToyPad on page load (1:1 Berny23)
  socket.emit('connectionStatus');
  socket.emit('syncToyPad');
  // Start keystone glow decay timer
  setInterval(decayGlow, 1500);
}

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
    // 🟡 FIXED: image fallback initials (restored from original)
    const initials = String(toy.name||'?').slice(0,2).toUpperCase();
    c.innerHTML = `<img src="${toy.img}" alt="${toy.name}" loading="lazy" onerror="this.style.display='none';this.nextElementSibling.style.display='flex'">
      <div class="toy-card-fallback" style="display:none">${initials}</div>
      <span>${toy.name}</span>`;
    c.onclick = () => createToy(toy);
    g.appendChild(c);
  });
  // 🟡 FIXED: catalog count (restored from original)
  document.getElementById('toyCount').textContent = `(${toys.length})`;
}
document.getElementById('toyFilter').addEventListener('input', applyFilter);

// ── Create → Toy Box ──────────────────────────────────────────
async function createToy(toy) {
  const ep = toy.type==='character'?'/character':'/vehicle';
  try {
    await fetch(ep,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:toy.id})});
    setStatus(`Created ${toy.name}`, 'ok');
    setTimeout(syncToyBox, 500);
  } catch(e) { setStatus(`Failed to create ${toy.name}`, 'error'); }
}

// ── Sync ──────────────────────────────────────────────────────
async function syncToyBox() {
  try {
    const tags = await fetch('/json/toytags.json').then(r=>r.json());
    toyBox = {}; padSlots = {1:null,2:null,3:null,4:null,5:null,6:null,7:null};
    PLACE_ORDER.left = []; PLACE_ORDER.center = []; PLACE_ORDER.right = [];
    tags.forEach(t=>{
      const info = allToys.find(a=>String(a.id)===String(t.id))||{};
      if (t.index==='-1'||!t.index) toyBox[t.uid]={name:t.name||info.name,id:t.id,uid:t.uid,type:t.type};
      else {
        const idx = parseInt(t.index);
        padSlots[idx] = {name:t.name||info.name,id:t.id,uid:t.uid,type:t.type};
        for (const [zone, slots] of Object.entries(PAD))
          if (slots.includes(idx)) PLACE_ORDER[zone].push(idx);
      }
    });
    renderToyBox(); renderPad();
  } catch(e){ console.error('syncToyBox failed:', e); }
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
    c.innerHTML = `<img src="${info.img||''}" alt="${tb.name}" onerror="this.style.display='none'"><span>${tb.name}</span>`;
    c.onclick = () => openPlaceModal(tb);
    box.appendChild(c);
  });
}

// ── Collapsible Toy Box ───────────────────────────────────────
document.getElementById('toyboxToggle').addEventListener('click', () => {
  const row = document.querySelector('.toybox-row');
  const tog = document.getElementById('toyboxToggle');
  row.classList.toggle('collapsed');
  tog.textContent = row.classList.contains('collapsed')
    ? '▶ Toy Box ' + document.getElementById('toyboxCount').textContent
    : '▼ Toy Box ' + document.getElementById('toyboxCount').textContent;
});

// ── Place Modal (from Toy Box) ────────────────────────────────
function openPlaceModal(tb) {
  pendingToy = tb;
  document.getElementById('placeModalName').textContent = `Place ${tb.name} on:`;
  document.getElementById('placeModal').hidden = false;
}
function closePlaceModal() { pendingToy = null; document.getElementById('placeModal').hidden = true; }

async function placeOnZone(zone) {
  const tb = pendingToy; if (!tb) return;
  closePlaceModal();
  const slots = PAD[zone];
  let target = slots.find(s => !padSlots[s]);

  if (target === undefined) {
    // Zone is full — FIFO evict oldest toy to Toy Box
    target = PLACE_ORDER[zone].shift();
    if (target !== undefined && padSlots[target]) {
      const evicted = padSlots[target];
      await fetch('/remove',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:target,uid:evicted.uid})});
      // Mark slot empty locally so find() picks it up below
      padSlots[target] = null;
      setStatus(`↻ ${evicted.name} moved to Toy Box (zone full)`, 'ok');
    }
    target = slots.find(s => !padSlots[s]) || slots[0];
  }

  try {
    await fetch('/place',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({uid:tb.uid,id:tb.id,position:ZONE_TO_POSITION[zone],index:target})});
    setStatus(`Placed ${tb.name} on ${zone.toUpperCase()}`, 'ok');
  } catch(e) { setStatus(`Place failed: ${tb.name}`, 'error'); }
  setTimeout(syncToyBox, 300);
}

document.querySelectorAll('#placeModal .place-btn').forEach(b => {
  b.onclick = () => placeOnZone(b.dataset.zone);
});

// ── Pad Modal (move/remove) ───────────────────────────────────
function openPadModal(slot) {
  const toy = padSlots[slot]; if (!toy) return;
  pendingSlot = slot;
  document.getElementById('padModalName').textContent = `${toy.name} (slot ${slot})`;
  document.getElementById('padModal').hidden = false;
}
function closePadModal() { pendingSlot = null; document.getElementById('padModal').hidden = true; }

async function moveFromPad(zone) {
  const slot = pendingSlot; if (!slot || !padSlots[slot]) return;
  const toy = padSlots[slot]; closePadModal();
  try {
    await fetch('/remove',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:slot,uid:toy.uid})});
  } catch(e) { setStatus(`Remove failed: ${toy.name}`, 'error'); return; }
  await sleep(500); // 🟡 FIXED: 500ms delay matches upstream (was 200ms)
  pendingToy = toy;
  await placeOnZone(zone);
}
async function removeFromPad() {
  const slot = pendingSlot; if (!slot || !padSlots[slot]) return;
  const name = padSlots[slot].name;
  try {
    await fetch('/remove',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:slot,uid:padSlots[slot].uid})});
    setStatus(`Removed ${name}`, 'ok');
  } catch(e) { setStatus(`Remove failed: ${name}`, 'error'); }
  closePadModal(); setTimeout(syncToyBox, 300);
}

document.querySelectorAll('#padModal .place-btn').forEach(b => {
  b.onclick = () => moveFromPad(b.dataset.zone);
});
document.querySelector('#padModal .remove-btn').onclick = removeFromPad;

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
        d.innerHTML = `<img src="${info.img||''}" alt="${toy.name}" onerror="this.style.display='none';this.nextElementSibling.style.display='flex'">
          <div class="pad-slot-fallback" style="display:none">${initials}</div>
          <span>${toy.name}</span>`;
        d.style.cursor = 'pointer';
        d.onclick = () => openPadModal(s);
      } else {
        d.innerHTML = `<span class="empty-slot">Slot ${s}</span>`;
      }
      el.appendChild(d);
    });
  });
}

// ── Status feedback (restored from original) ──────────────────
function setStatus(msg, cls) {
  const sl = document.getElementById('statusLine');
  sl.textContent = msg; sl.className = cls || '';
  if (cls === 'ok') setTimeout(() => { if (sl.textContent === msg) sl.textContent = ''; }, 3000);
}

// ── Keystone Glow (restored from original) ────────────────────
const litTimestamps = { left:0, center:0, right:0 };

function applyZoneGlow(zoneName) {
  const now = Date.now();
  litTimestamps[zoneName] = now;
  const el = document.querySelector(`.pad-zone[data-zone="${zoneName}"]`);
  if (!el) return;
  el.classList.add('zone-lit');
  el.classList.remove('zone-lit-sustain');
}

function decayGlow() {
  const now = Date.now();
  for (const [zone, ts] of Object.entries(litTimestamps)) {
    if (!ts) continue;
    const el = document.querySelector(`.pad-zone[data-zone="${zone}"]`);
    if (!el) continue;
    const age = now - ts;
    if (age > 3000) {
      el.classList.remove('zone-lit', 'zone-lit-sustain');
      litTimestamps[zone] = 0;
    } else if (age > 2000) {
      el.classList.remove('zone-lit');
      el.classList.add('zone-lit-sustain');
    }
  }
}

// LED socket handlers (1:1 Berny23 pattern — single array argument indexed like upstream)
// Upstream emits: io.emit("event", [array]) → client receives single arg e; access via e[0], e[1], ...
socket.on('Color One', (e) => {
  // e = [pad, color]
  const pad = e[0];
  const zoneNames = ['','center','left','right'];
  if (pad >= 1 && pad <= 3) applyZoneGlow(zoneNames[pad]);
  updatePortalTelemetry();
});
socket.on('Color All', (e) => {
  // e = [centerColor, leftColor, rightColor]  (CMD_COLALL)
  // or e = [sameColor, sameColor, sameColor] (CMD_COL pad=0)
  if (e[0]) applyZoneGlow('center');
  if (e[1]) applyZoneGlow('left');
  if (e[2]) applyZoneGlow('right');
  updatePortalTelemetry();
});
socket.on('Fade One', (e) => {
  // e = [pad, speed, cycles, color]
  const pad = e[0];
  const zoneNames = ['','center','left','right'];
  if (pad >= 1 && pad <= 3) applyZoneGlow(zoneNames[pad]);
  updatePortalTelemetry();
});
socket.on('Fade All', (e) => {
  // e = [topSpeed, topCycles, topColor, leftSpeed, leftCycles, leftColor, rightSpeed, rightCycles, rightColor]
  // per Berny23 original: center=e[2], left=e[5], right=e[8]
  if (e[2]) applyZoneGlow('center');
  if (e[5]) applyZoneGlow('left');
  if (e[8]) applyZoneGlow('right');
  updatePortalTelemetry();
});

function updatePortalTelemetry() {
  const active = [];
  const now = Date.now();
  for (const [zone, ts] of Object.entries(litTimestamps))
    if (ts && (now - ts) < 3000) active.push(zone.toUpperCase());
  const el = document.getElementById('portalTelemetry');
  el.textContent = active.length
    ? `Portal lights inferred: ${active.join(', ')}`
    : 'Portal lights: waiting for packet data...';
}

// ── Socket ────────────────────────────────────────────────────
socket.on('Connection True', ()=>{
  document.getElementById('meta').textContent = 'Connected to PS3 ✓';
  document.getElementById('meta').style.color = '#3cc47c';
});
socket.on('refreshTokens', () => setTimeout(syncToyBox, 1000));

// ── Image Sync (on-demand from UI) ────────────────────────────
document.getElementById('syncImagesBtn').addEventListener('click', async () => {
  const modal = document.getElementById('syncModal');
  const progress = document.getElementById('syncProgress');
  const bar = document.getElementById('syncBarFill');
  modal.hidden = false; progress.textContent = 'Starting...'; bar.style.width = '0%';
  try {
    const res = await fetch('/api/sync-images', { method: 'POST' });
    const reader = res.body.getReader();
    const decoder = new TextDecoder();
    let buf = '';
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      buf += decoder.decode(value, { stream: true });
      const lines = buf.split('\n');
      buf = lines.pop();
      for (const line of lines) {
        if (!line.trim()) continue;
        try {
          const d = JSON.parse(line);
          progress.textContent = d.msg || d.status || line;
          if (d.pct !== undefined) bar.style.width = d.pct + '%';
        } catch { progress.textContent = line; }
      }
    }
    progress.textContent = 'Sync complete.';
    bar.style.width = '100%';
  } catch(e) {
    progress.textContent = 'Sync failed: ' + e.message;
  }
});

// ── Helpers ───────────────────────────────────────────────────
function el(tag,cls,text){const e=document.createElement(tag);if(cls)e.className=cls;if(text)e.textContent=text;return e;}
function sleep(ms){return new Promise(r=>setTimeout(r,ms));}
init(); syncToyBox();
