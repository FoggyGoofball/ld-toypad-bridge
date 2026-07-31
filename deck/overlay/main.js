// LD-ToyPad Bridge — Steam Deck UI v2
const socket = io();
let chars = [], vehs = [], allToys = [], type = 'character', world = 'All';
let toyBox = {}, padSlots = {1:null,2:null,3:null,4:null,5:null,6:null,7:null};
const PAD_MAP = { left:[1,4,5], center:[2], right:[3,6,7] };

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

async function syncToyBox() {
  try {
    const tags = await fetch('/json/toytags.json').then(r=>r.json());
    toyBox = {}; padSlots = {1:null,2:null,3:null,4:null,5:null,6:null,7:null};
    tags.forEach(t=>{
      const info = allToys.find(a=>String(a.id)===String(t.id))||{};
      if (t.index==='-1'||!t.index) toyBox[t.uid]={name:t.name||info.name,id:t.id,uid:t.uid,type:t.type};
      else padSlots[parseInt(t.index)]={name:t.name||info.name,id:t.id,uid:t.uid,type:t.type};
    });
    renderToyBox(); renderPad();
  } catch(e){}
}

// ── Toy Box ───────────────────────────────────────────────────
function renderToyBox() {
  const box = document.getElementById('toybox'); box.innerHTML = '';
  const entries = Object.values(toyBox);
  document.getElementById('toyboxCount').textContent = entries.length?`(${entries.length})`:'';
  if (!entries.length) { box.innerHTML = '<p class="muted">Click a character or vehicle below to add it here, then click it to place on the pad.</p>'; return; }
  entries.forEach(tb=>{
    const info = allToys.find(a=>String(a.id)===String(tb.id))||{};
    const c = el('button','toybox-item');
    c.innerHTML = `<img src="${info.img||''}" alt="${tb.name}" onerror="this.style.display='none'"><span>${tb.name}</span>`;
    c.onclick = ()=>{
      const slot = prompt('Place on slot?\nLeft: 1,4,5 | Center: 2 | Right: 3,6,7','2');
      if (slot) placeToy(tb, parseInt(slot));
    };
    box.appendChild(c);
  });
}

async function placeToy(tb, index) {
  await fetch('/place',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({uid:tb.uid,id:tb.id,position:1,index})});
  setTimeout(syncToyBox, 300);
}

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
        const rm = el('button','pad-remove','✕');
        rm.onclick = async e=>{e.stopPropagation();await removeToy(s);};
        d.appendChild(rm);
      } else {
        d.innerHTML = `<span class="empty-slot">Slot ${s}</span>`;
      }
      el.appendChild(d);
    });
  });
}

async function removeToy(index) {
  const toy = padSlots[index];
  if (!toy) return;
  await fetch('/remove',{method:'DELETE',headers:{'Content-Type':'application/json'},body:JSON.stringify({index,uid:toy.uid})});
  setTimeout(syncToyBox, 300);
}

// ── Socket ────────────────────────────────────────────────────
socket.on('Connection True', ()=>{
  document.getElementById('meta').textContent = 'Connected to PS3 ✓';
  document.getElementById('meta').style.color = '#3cc47c';
});
socket.on('refreshTokens', syncToyBox);
socket.on('syncToyPad', syncToyBox);

function el(tag,cls,text){const e=document.createElement(tag);if(cls)e.className=cls;if(text)e.textContent=text;return e;}
init(); syncToyBox();
