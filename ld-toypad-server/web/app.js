async function api(path, options) {
  const res = await fetch(path, options);
  const body = await res.json();
  if (!res.ok || !body.ok) {
    throw new Error(body.error || `HTTP ${res.status}`);
  }
  return body;
}

const state = {
  toys: [],
  filteredToys: [],
  selectedType: 'character',
  selectedWorld: 'All',
  selectedRelease: 'All',
  selectedOwnership: 'All',
  selectedToyId: null,
  modalToyId: null,
  litZones: [],              // zone numbers currently lit per telemetry
  litZoneTimestamps: [0, 0, 0], // when each zone was last seen lit (ms)
};

const TYPE_LABELS = {
  character: 'Characters',
  token: 'Vehicles / Items'
};

const RELEASE_LABELS = {
  All: 'All',
  year1: 'Year 1',
  year2: 'Year 2'
};

const OWNERSHIP_LABELS = {
  All: 'All',
  included: 'Included',
  dlc: 'DLC'
};

const STARTER_ITEM_IDS = new Set([
  1, 2, 3,
  1000, 1001, 1002,
  1006, 1007, 1008,
  1009, 1010, 1011
]);

const YEAR_ONE_WORLDS = new Set([
  'dc comics',
  'doctor who',
  'ghostbusters',
  'jurrasic park',
  'legends of chima',
  'lord of the rings',
  'midway arcade',
  'ninjago',
  'portal 2',
  'scooby-doo',
  'the lego movie',
  'the simpsons',
  'wizard of oz',
  'back to the future'
]);

const YEAR_ONE_CHARACTER_MAX_ID = 46;
const YEAR_ONE_TOKEN_MAX_ID = 1172;

function normalizeWorld(world) {
  return String(world || '').trim().toLowerCase();
}

function inferReleaseYear(toy) {
  const numericId = Number(toy.itemId);
  if (Number.isFinite(numericId)) {
    if (toy.type === 'character') {
      return numericId <= YEAR_ONE_CHARACTER_MAX_ID ? 'year1' : 'year2';
    }
    if (toy.type === 'token') {
      return numericId <= YEAR_ONE_TOKEN_MAX_ID ? 'year1' : 'year2';
    }
  }
  return YEAR_ONE_WORLDS.has(normalizeWorld(toy.world)) ? 'year1' : 'year2';
}

function inferOwnership(toy) {
  return STARTER_ITEM_IDS.has(Number(toy.itemId)) ? 'included' : 'dlc';
}

function enrichToyMetadata(toy) {
  return {
    ...toy,
    releaseYear: toy.releaseYear || inferReleaseYear(toy),
    ownership: toy.ownership || inferOwnership(toy)
  };
}

function setStatus(msg, isError = false) {
  const el = document.getElementById('statusLine');
  el.textContent = msg;
  el.className = isError ? 'error' : 'ok';
}

function getZoneName(zone) {
  if (zone === 0) return 'LEFT';
  if (zone === 1) return 'CENTER';
  if (zone === 2) return 'RIGHT';
  return String(zone);
}

function renderPortalTelemetry(portal) {
  const root = document.getElementById('portalTelemetry');
  if (!portal || !portal.updatedAt) {
    root.textContent = 'Portal lights: waiting for packet data...';
    return;
  }
  const zoneNames = Array.isArray(portal.inferredLitZoneNames) && portal.inferredLitZoneNames.length
    ? portal.inferredLitZoneNames.join(', ')
    : 'none inferred';
  root.textContent = `Portal lights inferred: ${zoneNames}`;
}

function resolveKnownToy(zoneToy) {
  return state.toys.find((entry) => entry.id === zoneToy.gameId)
    || state.toys.find((entry) => entry.id === zoneToy.id)
    || state.toys.find((entry) => entry.gameId === zoneToy.gameId)
    || state.toys.find((entry) => entry.itemId === zoneToy.itemId && entry.type === zoneToy.type)
    || null;
}

function renderZones(zones) {
  const root = document.getElementById('zones');
  root.innerHTML = '';
  zones.forEach((z) => {
    const card = document.createElement('article');
    card.className = 'zone';

    const top = document.createElement('div');
    top.className = 'zone-top';

    const title = document.createElement('h3');
    title.textContent = `${z.zoneName} (${z.zone})`;

    top.appendChild(title);
    const slotHint = document.createElement('span');
    slotHint.className = 'zone-copy';
    slotHint.textContent = `${z.capacity || 1} slot${(z.capacity || 1) > 1 ? 's' : ''}`;
    top.appendChild(slotHint);

    const body = document.createElement('div');
    body.className = 'zone-slot-grid';

    const toys = Array.isArray(z.toys) ? z.toys : [];
    const capacity = Number.isInteger(z.capacity) ? z.capacity : Math.max(1, toys.length);

    for (let slot = 0; slot < capacity; slot += 1) {
      const zoneSlot = document.createElement('div');
      zoneSlot.className = 'zone-slot';

      const thumbWrap = document.createElement('div');
      thumbWrap.className = 'zone-thumb-wrap';

      const thumb = document.createElement('img');
      thumb.className = 'zone-thumb';
      thumb.alt = `${z.zoneName} slot ${slot + 1} toy image`;
      thumb.hidden = true;

      const fallback = document.createElement('div');
      fallback.className = 'zone-fallback';

      const copy = document.createElement('div');
      copy.className = 'zone-copy';
      const text = document.createElement('p');

      const removeButton = document.createElement('button');
      removeButton.type = 'button';
      removeButton.className = 'zone-remove';
      removeButton.textContent = `Remove ${slot + 1}`;

      const slotToy = toys[slot] || null;
      if (slotToy) {
        fallback.textContent = String(slotToy.name || '?').slice(0, 2).toUpperCase();
        text.textContent = `${slot + 1}: ${slotToy.name}`;
        const knownToy = resolveKnownToy(slotToy);
        const imageUrl = knownToy ? getImageUrl(knownToy) : null;

        thumb.addEventListener('load', () => {
          thumb.hidden = false;
          fallback.hidden = true;
        }, { once: true });
        thumb.addEventListener('error', () => {
          thumb.hidden = true;
          fallback.hidden = false;
        }, { once: true });
        if (imageUrl) {
          thumb.src = imageUrl;
        }

        removeButton.disabled = false;
        removeButton.addEventListener('click', async () => {
          try {
            await api('/api/remove', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ zone: z.zone, slot })
            });
            setStatus(`Removed toy from ${getZoneName(z.zone)} slot ${slot + 1}`);
            await refresh();
          } catch (err) {
            setStatus(`Remove failed: ${err.message}`, true);
          }
        });
      } else {
        fallback.textContent = '--';
        text.textContent = `${slot + 1}: Empty`;
        removeButton.disabled = true;
      }

      thumbWrap.appendChild(thumb);
      thumbWrap.appendChild(fallback);
      copy.appendChild(text);
      zoneSlot.appendChild(thumbWrap);
      zoneSlot.appendChild(copy);
      zoneSlot.appendChild(removeButton);
      body.appendChild(zoneSlot);
    }

    card.appendChild(top);
    card.appendChild(body);
    root.appendChild(card);
  });
}

function getTypeLabel(type) {
  return TYPE_LABELS[type] || type;
}

function getImageUrl(toy) {
  return toy.imagePath || `/images/${toy.itemId}.png`;
}

function getVisibleWorlds() {
  const worlds = new Set();
  state.toys
    .filter((toy) => toy.type === state.selectedType)
    .forEach((toy) => worlds.add(toy.world));
  return ['All', ...Array.from(worlds).sort((left, right) => left.localeCompare(right))];
}

function getToyById(toyId) {
  return state.toys.find((entry) => entry.id === toyId) || null;
}

function openPlaceModal(toyId) {
  const toy = getToyById(toyId);
  if (!toy) {
    return;
  }
  state.modalToyId = toyId;
  const modal = document.getElementById('placeModal');
  const meta = document.getElementById('placeModalMeta');
  meta.textContent = `Place ${toy.name} on:`;
  modal.hidden = false;
}

function closePlaceModal() {
  state.modalToyId = null;
  const modal = document.getElementById('placeModal');
  modal.hidden = true;
}

function getZoneCapacity(zone) {
  if (zone === 1) {
    return 1;
  }
  return 3;
}

function updateSlotSelectOptions() {
  const zone = Number(document.getElementById('zoneSelect').value);
  const slotSelect = document.getElementById('slotSelect');
  const capacity = getZoneCapacity(zone);
  slotSelect.innerHTML = '';

  const autoOption = document.createElement('option');
  autoOption.value = '';
  autoOption.textContent = 'Auto';
  slotSelect.appendChild(autoOption);

  for (let slot = 0; slot < capacity; slot += 1) {
    const option = document.createElement('option');
    option.value = String(slot);
    option.textContent = String(slot + 1);
    slotSelect.appendChild(option);
  }
}

async function placeToyOnZone(zone, toyId, slot) {
  const body = { zone, toyId };
  if (Number.isInteger(slot)) {
    body.slot = slot;
  }
  await api('/api/place', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body)
  });
}

function renderTypeTabs() {
  const root = document.getElementById('typeTabs');
  const types = Array.from(new Set(state.toys.map((toy) => toy.type))).sort();
  root.innerHTML = '';

  types.forEach((type) => {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'tab-button';
    if (type === state.selectedType) {
      button.classList.add('active');
    }
    button.textContent = getTypeLabel(type);
    button.addEventListener('click', () => {
      state.selectedType = type;
      state.selectedWorld = 'All';
      applyToyFilter(document.getElementById('toyFilter').value);
    });
    root.appendChild(button);
  });
}

function renderWorldTabs() {
  const root = document.getElementById('worldTabs');
  const worlds = getVisibleWorlds();
  if (!worlds.includes(state.selectedWorld)) {
    state.selectedWorld = 'All';
  }
  root.innerHTML = '';

  worlds.forEach((world) => {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'tab-button';
    if (world === state.selectedWorld) {
      button.classList.add('active');
    }
    button.textContent = world;
    button.addEventListener('click', () => {
      state.selectedWorld = world;
      applyToyFilter(document.getElementById('toyFilter').value);
    });
    root.appendChild(button);
  });
}

function renderReleaseTabs() {
  const root = document.getElementById('releaseTabs');
  const releases = ['All', 'year1', 'year2'];
  root.innerHTML = '';

  releases.forEach((release) => {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'tab-button';
    if (release === state.selectedRelease) {
      button.classList.add('active');
    }
    button.textContent = RELEASE_LABELS[release] || release;
    button.addEventListener('click', () => {
      state.selectedRelease = release;
      applyToyFilter(document.getElementById('toyFilter').value);
    });
    root.appendChild(button);
  });
}

function renderOwnershipTabs() {
  const root = document.getElementById('ownershipTabs');
  const ownerships = ['All', 'included', 'dlc'];
  root.innerHTML = '';

  ownerships.forEach((ownership) => {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'tab-button';
    if (ownership === state.selectedOwnership) {
      button.classList.add('active');
    }
    button.textContent = OWNERSHIP_LABELS[ownership] || ownership;
    button.addEventListener('click', () => {
      state.selectedOwnership = ownership;
      applyToyFilter(document.getElementById('toyFilter').value);
    });
    root.appendChild(button);
  });
}

function buildCatalogCard(toy) {
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'toy-card';
  if (toy.id === state.selectedToyId) {
    button.classList.add('selected');
  }

  const thumbWrap = document.createElement('div');
  thumbWrap.className = 'toy-card-thumb-wrap';

  const img = document.createElement('img');
  img.className = 'toy-card-thumb';
  img.alt = `${toy.name} thumbnail`;
  img.src = getImageUrl(toy);
  img.loading = 'lazy';

  const fallback = document.createElement('div');
  fallback.className = 'toy-card-fallback';
  fallback.textContent = toy.name.slice(0, 2).toUpperCase();
  fallback.hidden = false;

  img.addEventListener('load', () => {
    img.hidden = false;
    fallback.hidden = true;
  }, { once: true });
  img.addEventListener('error', () => {
    img.hidden = true;
    fallback.hidden = false;
  }, { once: true });

  thumbWrap.appendChild(img);
  thumbWrap.appendChild(fallback);

  const name = document.createElement('strong');
  name.textContent = toy.name;

  const world = document.createElement('span');
  world.className = 'toy-card-world';
  world.textContent = toy.world;

  const meta = document.createElement('span');
  meta.className = 'toy-card-meta';
  meta.textContent = toy.type === 'character'
    ? `Character #${toy.itemId}`
    : `Token #${toy.itemId}${toy.rebuild ? ` • Rebuild ${toy.rebuild}` : ''}`;

  button.appendChild(thumbWrap);
  button.appendChild(name);
  button.appendChild(world);
  button.appendChild(meta);

  button.addEventListener('click', () => {
    state.selectedToyId = toy.id;
    renderCatalog();
    openPlaceModal(toy.id);
  });

  return button;
}

function renderCatalog() {
  renderTypeTabs();
  renderWorldTabs();
  renderReleaseTabs();
  renderOwnershipTabs();

  const root = document.getElementById('toyGrid');
  root.innerHTML = '';
  state.filteredToys.forEach((toy) => {
    root.appendChild(buildCatalogCard(toy));
  });

  if (!state.filteredToys.some((toy) => toy.id === state.selectedToyId)) {
    state.selectedToyId = state.filteredToys[0]?.id || null;
  }

  const count = document.getElementById('toyCount');
  count.textContent = `Showing ${state.filteredToys.length} entries in ${getTypeLabel(state.selectedType)} / ${state.selectedWorld} / ${RELEASE_LABELS[state.selectedRelease]} / ${OWNERSHIP_LABELS[state.selectedOwnership]}`;
}

function applyToyFilter(value) {
  const search = value.trim().toLowerCase();
  state.filteredToys = state.toys.filter((toy) => {
    if (toy.type !== state.selectedType) {
      return false;
    }
    if (state.selectedWorld !== 'All' && toy.world !== state.selectedWorld) {
      return false;
    }
    if (state.selectedRelease !== 'All' && toy.releaseYear !== state.selectedRelease) {
      return false;
    }
    if (state.selectedOwnership !== 'All' && toy.ownership !== state.selectedOwnership) {
      return false;
    }
    if (!search) {
      return true;
    }
    return [toy.name, toy.world, toy.id, String(toy.itemId), toy.releaseYear, toy.ownership]
      .some((part) => String(part || '').toLowerCase().includes(search));
  });
  renderCatalog();
}

function applyZoneGlow() {
  const now = Date.now();
  const ZONE_GLOW_SUSTAIN_MS = 1500; // Keep glow for 1.5s after last telemetry

  // Build the set of zones that should still be glowing
  const activeLit = new Set();
  for (let z = 0; z < 3; z += 1) {
    const elapsed = now - state.litZoneTimestamps[z];
    if (elapsed < ZONE_GLOW_SUSTAIN_MS) {
      activeLit.add(z);
    }
  }

  const zoneCards = document.querySelectorAll('#zones .zone');
  zoneCards.forEach((card) => {
    // Zone cards have heading text like "LEFT (0)" — extract the zone number
    const heading = card.querySelector('h3');
    if (!heading) return;
    const match = heading.textContent.match(/\((\d+)\)/);
    if (!match) return;
    const zoneNum = parseInt(match[1], 10);

    card.classList.remove('zone-lit', 'zone-lit-sustain');
    if (state.litZones.includes(zoneNum)) {
      // Currently reported as lit — full glow
      card.classList.add('zone-lit');
    } else if (activeLit.has(zoneNum)) {
      // Recently was lit — sustain a dimmer glow
      card.classList.add('zone-lit-sustain');
    }
  });
}

async function refresh() {
  try {
    const status = await api('/api/status');

    // Update lit-zone tracking with sustain timestamps
    const lit = Array.isArray(status.portal?.inferredLitZones)
      ? status.portal.inferredLitZones
      : [];
    state.litZones = lit;
    const now = Date.now();
    for (let z = 0; z < 3; z += 1) {
      if (lit.includes(z)) {
        state.litZoneTimestamps[z] = now;
      }
    }

    renderZones(status.zones);
    applyZoneGlow();
    renderPortalTelemetry(status.portal);

    const meta = document.getElementById('meta');
    const client = status.client ? `${status.client.address}:${status.client.port}` : 'none';
    meta.textContent = `UDP ${status.udpPort} | Debug ${status.debugPort} | Client ${client} | Uptime ${status.uptime}s`;
  } catch (err) {
    setStatus(`Refresh failed: ${err.message}`, true);
  }
}

async function loadToys() {
  const toys = await api('/api/toys');
  state.toys = toys.toys.map((toy) => enrichToyMetadata(toy));
  if (!state.toys.some((toy) => toy.type === state.selectedType)) {
    state.selectedType = state.toys[0]?.type || 'character';
  }
  applyToyFilter(document.getElementById('toyFilter').value || '');
}

document.getElementById('placeForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const zone = Number(document.getElementById('zoneSelect').value);
  const slotRaw = document.getElementById('slotSelect').value;
  const slot = slotRaw === '' ? undefined : Number(slotRaw);
  const toyId = state.selectedToyId;
  if (!toyId) {
    setStatus('Select a toy before placing it.', true);
    return;
  }
  try {
    await placeToyOnZone(zone, toyId, slot);
    setStatus(`Placed ${toyId} on ${getZoneName(zone)}${Number.isInteger(slot) ? ` slot ${slot + 1}` : ''}`);
    await refresh();
  } catch (err) {
    setStatus(`Place failed: ${err.message}`, true);
  }
});

document.querySelectorAll('[data-modal-place-zone]').forEach((button) => {
  button.addEventListener('click', async () => {
    const zone = Number(button.getAttribute('data-modal-place-zone'));
    const toyId = state.modalToyId;
    if (!toyId) {
      return;
    }
    try {
      await placeToyOnZone(zone, toyId, undefined);
      setStatus(`Placed ${toyId} on ${getZoneName(zone)}`);
      closePlaceModal();
      await refresh();
    } catch (err) {
      setStatus(`Place failed: ${err.message}`, true);
    }
  });
});

document.getElementById('placeModalCancel').addEventListener('click', () => {
  closePlaceModal();
});

document.getElementById('placeModal').addEventListener('click', (event) => {
  if (event.target.id === 'placeModal') {
    closePlaceModal();
  }
});

document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') {
    closePlaceModal();
  }
});

document.getElementById('toyFilter').addEventListener('input', (event) => {
  applyToyFilter(event.target.value);
});

document.getElementById('zoneSelect').addEventListener('change', () => {
  updateSlotSelectOptions();
});

(async function init() {
  await loadToys();
  updateSlotSelectOptions();
  await refresh();
  setInterval(refresh, 1000);
})();
