// LD-ToyPad Bridge — Steam Deck UI
// Talks to Berny23's Express/Socket.io backend

const socket = io();
let characters = [];
let vehicles = [];
let allToys = [];
let selectedType = 'character';
let selectedWorld = 'All';
let selectedRelease = 'All';
let padState = { 1: null, 2: null, 3: null, 4: null, 5: null, 6: null, 7: null };
let gameConnected = false;

const RELEASE_LABELS = { All: 'All', year1: 'Year 1', year2: 'Year 2' };
const YEAR_ONE_WORLDS = new Set(['dc comics','doctor who','ghostbusters','jurrasic park','legends of chima','lord of the rings','midway arcade','ninjago','portal 2','scooby-doo','the lego movie','the simpsons','wizard of oz','back to the future']);
const YEAR_ONE_CHAR_MAX = 46, YEAR_ONE_TOKEN_MAX = 1172;

// ── Load catalog ──────────────────────────────────────────────
async function loadCatalog() {
  const [cm, tm] = await Promise.all([
    fetch('/json/charactermap.json').then(r => r.json()),
    fetch('/json/tokenmap.json').then(r => r.json())
  ]);
  characters = cm.filter(c => c.name && c.name !== 'Unknown');
  vehicles = tm.filter(v => v.name && v.name !== 'Unknown');
  buildToyList();
  renderTypeTabs();
  renderWorldTabs();
  renderReleaseTabs();
  applyFilter();
  renderZones();
  document.getElementById('meta').textContent = gameConnected ? 'Connected to PS3' : 'Waiting for PS3...';
}

function buildToyList() {
  allToys = [
    ...characters.map(c => ({ ...c, type: 'character', releaseYear: inferRelease(c, 'character'), image: `/images/${c.id}.png` })),
    ...vehicles.map(v => ({ ...v, type: 'token', releaseYear: inferRelease(v, 'token'), image: `/images/${v.id}.png` }))
  ];
}

function inferRelease(toy, type) {
  const n = Number(toy.id);
  if (Number.isFinite(n)) return type === 'character' ? (n <= YEAR_ONE_CHAR_MAX ? 'year1' : 'year2') : (n <= YEAR_ONE_TOKEN_MAX ? 'year1' : 'year2');
  return YEAR_ONE_WORLDS.has(String(toy.world||'').toLowerCase()) ? 'year1' : 'year2';
}

// ── Tabs ──────────────────────────────────────────────────────
function renderTypeTabs() {
  const root = document.getElementById('typeTabs');
  root.innerHTML = '';
  ['character','token'].forEach(type => {
    const b = document.createElement('button');
    b.className = 'tab-button' + (type === selectedType ? ' active' : '');
    b.textContent = type === 'character' ? 'Characters' : 'Vehicles/Items';
    b.onclick = () => { selectedType = type; selectedWorld = 'All'; renderWorldTabs(); applyFilter(); };
    root.appendChild(b);
  });
}

function renderWorldTabs() {
  const root = document.getElementById('worldTabs');
  const worlds = ['All', ...new Set(allToys.filter(t => t.type === selectedType).map(t => t.world))].sort();
  root.innerHTML = '';
  worlds.forEach(w => {
    const b = document.createElement('button');
    b.className = 'tab-button' + (w === selectedWorld ? ' active' : '');
    b.textContent = w;
    b.onclick = () => { selectedWorld = w; applyFilter(); };
    root.appendChild(b);
  });
}

function renderReleaseTabs() {
  const root = document.getElementById('releaseTabs');
  root.innerHTML = '';
  ['All','year1','year2'].forEach(r => {
    const b = document.createElement('button');
    b.className = 'tab-button' + (r === selectedRelease ? ' active' : '');
    b.textContent = RELEASE_LABELS[r];
    b.onclick = () => { selectedRelease = r; applyFilter(); };
    root.appendChild(b);
  });
}

// ── Filter ────────────────────────────────────────────────────
function applyFilter() {
  const q = (document.getElementById('toyFilter').value || '').toLowerCase();
  let toys = allToys.filter(t => t.type === selectedType);
  if (selectedWorld !== 'All') toys = toys.filter(t => t.world === selectedWorld);
  if (selectedRelease !== 'All') toys = toys.filter(t => t.releaseYear === selectedRelease);
  if (q) toys = toys.filter(t => (t.name||'').toLowerCase().includes(q) || String(t.id).includes(q) || (t.world||'').toLowerCase().includes(q));
  renderCatalog(toys);
}

document.getElementById('toyFilter').addEventListener('input', applyFilter);

// ── Catalog grid ──────────────────────────────────────────────
function renderCatalog(toys) {
  const root = document.getElementById('catalog');
  root.innerHTML = '';
  toys.forEach(toy => {
    const card = document.createElement('button');
    card.className = 'toy-card';
    card.innerHTML = `<img src="${toy.image}" alt="${toy.name}" loading="lazy" onerror="this.style.display='none'"><span>${toy.name}</span><small>${toy.world||''} #${toy.id}</small>`;
    card.onclick = () => placeToy(toy);
    root.appendChild(card);
  });
}

// ── Place / Remove ────────────────────────────────────────────
async function placeToy(toy) {
  const pad = prompt('Pad slot (1-7)?\n1-3=Center  4-5=Left  6-7=Right', '1');
  if (!pad) return;
  const index = parseInt(pad);
  if (index < 1 || index > 7) return alert('Slot must be 1-7');

  // Create the toy first
  const endpoint = toy.type === 'character' ? '/character' : '/vehicle';
  try {
    await fetch(endpoint, { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({ id: toy.id }) });
    // Wait briefly then place
    setTimeout(async () => {
      await fetch('/place', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({ uid: '', id: toy.id, position: 1, index }) });
    }, 300);
  } catch(e) {
    console.error(e);
  }
}

async function removeToy(index) {
  await fetch('/remove', { method: 'DELETE', headers: {'Content-Type':'application/json'}, body: JSON.stringify({ index, uid: '' }) });
}

// ── Zones display ─────────────────────────────────────────────
function renderZones() {
  const root = document.getElementById('zones');
  root.innerHTML = '';
  const groups = [
    { name: 'Center', slots: [2] },
    { name: 'Left', slots: [1,4,5] },
    { name: 'Right', slots: [3,6,7] }
  ];
  groups.forEach(g => {
    const card = document.createElement('article');
    card.className = 'zone';
    card.innerHTML = `<div class="zone-top"><h3>${g.name}</h3></div>`;
    const grid = document.createElement('div');
    grid.className = 'zone-slot-grid';
    g.slots.forEach(slot => {
      const s = document.createElement('div');
      s.className = 'zone-slot';
      const toy = padState[slot];
      s.innerHTML = toy
        ? `<img src="/images/${toy}.png" alt="${toy}" onerror="this.style.display='none'"><span>${toy}</span><button class="zone-remove" onclick="removeToy(${slot})">✕</button>`
        : `<span class="zone-empty">Slot ${slot} — empty</span>`;
      grid.appendChild(s);
    });
    card.appendChild(grid);
    root.appendChild(card);
  });
}

// ── Socket.io events ──────────────────────────────────────────
socket.on('Connection True', () => {
  gameConnected = true;
  document.getElementById('meta').textContent = 'Connected to PS3 ✓';
});

socket.on('refreshTokens', () => {
  fetch('/json/toytags.json').then(r => r.json()).then(tags => {
    tags.forEach(t => { if (t.index !== '-1') padState[parseInt(t.index)] = t.name || t.id; });
    renderZones();
  }).catch(() => {});
});

socket.on('syncToyPad', () => {
  fetch('/json/toytags.json').then(r => r.json()).then(tags => {
    tags.forEach(t => { if (t.index !== '-1') padState[parseInt(t.index)] = t.name || t.id; });
    renderZones();
  }).catch(() => {});
});

// Start
loadCatalog();
