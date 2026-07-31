// LD-ToyPad Bridge — Steam Deck UI v4 (pad move/remove modal)
const socket = io();
let chars = [], vehs = [], allToys = [], type = 'character', world = 'All';
let toyBox = {}, padSlots = {1:null,2:null,3:null,4:null,5:null,6:null,7:null};
let pendingToy = null, pendingSlot = null;
const PAD = { left:[1,4,5], center:[2], right:[3,6,7] };
const PLACE_ORDER = { left:[], center:[], right:[] };

async function init() {
  const [cm, tm] = await Promise.all([
    fetch('/json/charactermap.json').then(r=>r.json()),
    fetch('/json/tokenmap.json').then(r=>r.json())
  ]);
  chars = cm.filter(c=>c.name&&c.name!=='Unknown').map(c=>({...c,type:'character',img:`/images/${c.id}.png`}));
  vehs = tm.filter(v=>v.name&&v.name!=='Unknown').map(v=>({...v,type:'token',img:`/images/${v.id}.png`}));
  allToys = [...chars, ...vehs];
  renderTabs(); applyFilter(); syncToyBox();
}

function renderTabs() {
  document.getElementById('typeTabs').innerHTML = '';
  [{k:'character',l:'Chars'},{k:'token',l:'Vehicles'}].forEach(t=>{
    const b = el('button','tab-button'+(type===t.k?' active':''),t.l);
    b.onclick=()=>{type=t.k;world='All';renderTabs();applyFilter();};
    document.getElementById('typeTabs').appendChild(b);
  });
  document.getElementById('worldTabs').innerHTML = '';
  ['All',...new Set(allToys.filter(t=>t.type===type).map(t=>t.world))].sort().forEach(w=>{
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
    c.innerHTML = `<img src="${toy.img}" alt="${toy.name}" loading="lazy" onerror="this.style.display='none'"><span>${toy.name}</span>`;
    c.onclick = () => createToy(toy);
    g.appendChild(c);
  });
}
document.getElementById('toyFilter').addEventListener('input', applyFilter);

// ── Create → Toy Box ──────────────────────────────────────────
async function createToy(toy) {
  const ep = toy.type==='character'?'/character':'/vehicle';
  await fetch(ep,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:toy.id})});
  setTimeout(syncToyBox, 500);
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
  } catch(e){}
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
    target = PLACE_ORDER[zone].shift();
    if (target !== undefined && padSlots[target])
      await fetch('/remove',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:target,uid:padSlots[target].uid})});
    target = slots.find(s => !padSlots[s]) || slots[0];
  }
  await fetch('/place',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({uid:tb.uid,id:tb.id,position:1,index:target})});
  setTimeout(syncToyBox, 300);
}

document.querySelectorAll('#placeModal .place-btn').forEach(b => {
  b.onclick = () => placeOnZone(b.dataset.zone);
});

// ── Pad Modal (move/remove from pad) ──────────────────────────
function openPadModal(slot) {
  const toy = padSlots[slot];
  if (!toy) return;
  pendingSlot = slot;
  document.getElementById('padModalName').textContent = `${toy.name} (slot ${slot})`;
  document.getElementById('padModal').hidden = false;
}
function closePadModal() { pendingSlot = null; document.getElementById('padModal').hidden = true; }

async function moveFromPad(zone) {
  const slot = pendingSlot; if (!slot || !padSlots[slot]) return;
  const toy = padSlots[slot];
  closePadModal();
  // Remove from current slot
  await fetch('/remove',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:slot,uid:toy.uid})});
  await sleep(200);
  // Place in new zone (same as placeOnZone but with the existing toy)
  pendingToy = toy;
  await placeOnZone(zone);
}

async function removeFromPad() {
  const slot = pendingSlot; if (!slot || !padSlots[slot]) return;
  await fetch('/remove',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:slot,uid:padSlots[slot].uid})});
  closePadModal();
  setTimeout(syncToyBox, 300);
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
        d.innerHTML = `<img src="${info.img||''}" alt="${toy.name}" onerror="this.style.display='none'"><span>${toy.name}</span>`;
        d.style.cursor = 'pointer';
        d.onclick = () => openPadModal(s);
      } else {
        d.innerHTML = `<span class="empty-slot">Slot ${s}</span>`;
      }
      el.appendChild(d);
    });
  });
}

// ── Socket ────────────────────────────────────────────────────
socket.on('Connection True', ()=>{
  document.getElementById('meta').textContent = 'Connected to PS3 ✓';
  document.getElementById('meta').style.color = '#3cc47c';
});
socket.on('refreshTokens', syncToyBox);
socket.on('syncToyPad', syncToyBox);

function el(tag,cls,text){const e=document.createElement(tag);if(cls)e.className=cls;if(text)e.textContent=text;return e;}
function sleep(ms){return new Promise(r=>setTimeout(r,ms));}
init(); syncToyBox();
