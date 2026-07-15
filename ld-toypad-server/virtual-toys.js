/**
 * virtual-toys.js
 * Virtual toy tag manager - simulates toy placement on the emulated pad
 * 
 * Provides a CLI interface for "placing" and "removing" toys on the pad
 * Each toy is represented by its NFC tag data (UID + character data)
 */

const { TOY_PAD, TAG_UID_SIZE, TAG_DATA_SIZE } = require('./toypad-protocol');
const characterMap = require('./data/charactermap.json');
const tokenMap = require('./data/tokenmap.json');

const CHARACTER_TAG_BASE = 0x2719;
const TOKEN_TAG_BASE = 0x277C;
const LEGACY_ALIASES = new Map([
  ['batman', 'batman'],
  ['gandalf', 'gandalf'],
  ['wyldstyle', 'wyldstyle'],
  ['batmobile', 'batmobile']
]);

const STARTER_CHARACTER_IDS = new Set([1, 2, 3]);
const STARTER_TOKEN_IDS = new Set([
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

function deriveOwnership(entry, type) {
  if (type === 'character' && STARTER_CHARACTER_IDS.has(entry.id)) {
    return 'included';
  }
  if (type === 'token' && STARTER_TOKEN_IDS.has(entry.id)) {
    return 'included';
  }
  return 'dlc';
}

function deriveReleaseYear(entry, type) {
  const numericId = Number(entry.id);
  if (Number.isFinite(numericId)) {
    if (type === 'character') {
      return numericId <= YEAR_ONE_CHARACTER_MAX_ID ? 'year1' : 'year2';
    }
    if (type === 'token') {
      return numericId <= YEAR_ONE_TOKEN_MAX_ID ? 'year1' : 'year2';
    }
  }
  const worldKey = normalizeWorld(entry.world);
  return YEAR_ONE_WORLDS.has(worldKey) ? 'year1' : 'year2';
}

function slugify(value) {
  return String(value || '')
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '');
}

function shouldIncludeEntry(entry) {
  if (!entry || !entry.name || entry.name === 'Unknown') {
    return false;
  }

  const normalizedName = String(entry.name).toLowerCase();
  const normalizedWorld = String(entry.world || '').toLowerCase();

  if (normalizedName.includes('future update') || normalizedName.includes('unreleased')) {
    return false;
  }
  if (/^test\s+\d+/i.test(entry.name)) {
    return false;
  }
  if (normalizedWorld === 'n/a' || /^\d+$/.test(normalizedWorld)) {
    return false;
  }

  return true;
}

function buildUid(id, typeMarker) {
  const uid = Buffer.alloc(TAG_UID_SIZE);
  uid[0] = 0x04;
  uid[1] = 0x5A;
  uid[2] = 0x6B;
  uid[3] = typeMarker;
  uid[4] = (id >> 16) & 0xFF;
  uid[5] = (id >> 8) & 0xFF;
  uid[6] = id & 0xFF;
  return uid;
}

function buildTagData(code) {
  return Buffer.from([
    (code >> 8) & 0xFF,
    code & 0xFF,
    0x00,
    0x00
  ]);
}

function buildToyRecord(entry, type) {
  const code = type === 'character'
    ? CHARACTER_TAG_BASE + entry.id
    : TOKEN_TAG_BASE + entry.id;
  const slug = slugify(entry.name);
  return {
    name: entry.name,
    uid: buildUid(entry.id, type === 'character' ? 0x7C : 0x7D),
    tagData: buildTagData(code),
    gameId: slug,
    type,
    itemId: entry.id,
    world: entry.world || 'Unknown',
    rebuild: Number.isInteger(entry.rebuild) ? entry.rebuild : 0,
    releaseYear: deriveReleaseYear(entry, type),
    ownership: deriveOwnership(entry, type)
  };
}

function buildToyDatabase() {
  const database = new Map();
  const aliases = new Map();

  function addEntry(entry, type) {
    if (!shouldIncludeEntry(entry)) {
      return;
    }

    const toy = buildToyRecord(entry, type);
    let id = toy.gameId;
    if (!id) {
      id = `${type}-${entry.id}`;
    }
    if (database.has(id)) {
      id = `${id}-${entry.id}`;
    }

    database.set(id, toy);
    aliases.set(String(entry.id), id);
    aliases.set(`${type}-${entry.id}`, id);
  }

  characterMap.forEach((entry) => addEntry(entry, 'character'));
  tokenMap.forEach((entry) => addEntry(entry, 'token'));

  return { database, aliases };
}

const { database: TOY_DATABASE, aliases: TOY_ALIASES } = buildToyDatabase();

class VirtualToyManager {
  constructor() {
    this.zoneCapacities = {
      [TOY_PAD.ZONES.LEFT]: 3,
      [TOY_PAD.ZONES.CENTER]: 1,
      [TOY_PAD.ZONES.RIGHT]: 3
    };

    // Each zone stores an array of slots (null or toy object).
    this.zones = {
      [TOY_PAD.ZONES.LEFT]: Array(this.zoneCapacities[TOY_PAD.ZONES.LEFT]).fill(null),
      [TOY_PAD.ZONES.CENTER]: Array(this.zoneCapacities[TOY_PAD.ZONES.CENTER]).fill(null),
      [TOY_PAD.ZONES.RIGHT]: Array(this.zoneCapacities[TOY_PAD.ZONES.RIGHT]).fill(null)
    };

    // Event callbacks
    this._onChange = null;
  }

  /**
   * Register a callback for zone state changes
   * callback(zoneNumber, slotIndex, oldState, newState)
   */
  onChange(callback) {
    this._onChange = callback;
  }

  getZoneCapacity(zone) {
    return this.zoneCapacities[zone] || 0;
  }

  _isValidZone(zone) {
    return Number.isInteger(zone) && zone >= 0 && zone <= 2;
  }

  _resolveSlotIndex(zone, slot) {
    const slots = this.zones[zone];
    if (!slots) {
      return -1;
    }
    if (Number.isInteger(slot)) {
      return slot >= 0 && slot < slots.length ? slot : -1;
    }
    return slots.findIndex((entry) => !entry);
  }

  /**
   * Place a toy on a specific zone
   * @param {number} zone - Zone index (0=LEFT, 1=CENTER, 2=RIGHT)
   * @param {string} toyId - Toy identifier from TOY_DATABASE
   * @param {number|undefined} slot - Optional slot index for multi-slot zones
   * @returns {boolean} - Success
   */
  placeToy(zone, toyId, slot) {
    const resolvedId = TOY_DATABASE.has(toyId) ? toyId : (TOY_ALIASES.get(toyId) || LEGACY_ALIASES.get(toyId));
    const toy = resolvedId ? TOY_DATABASE.get(resolvedId) : null;
    if (!toy) {
      console.error(`[VirtualToy] Unknown toy: ${toyId}`);
      return false;
    }

    if (!this._isValidZone(zone)) {
      console.error(`[VirtualToy] Invalid zone: ${zone}`);
      return false;
    }

    const slotIndex = this._resolveSlotIndex(zone, slot);
    if (slotIndex < 0) {
      console.error(`[VirtualToy] No free/valid slot for zone: ${zone} slot: ${slot}`);
      return false;
    }

    const oldState = this.zones[zone][slotIndex];
    this.zones[zone][slotIndex] = { ...toy };

    console.log(`[VirtualToy] Placed "${toy.name}" on ${TOY_PAD.ZONE_NAMES[zone]} slot ${slotIndex + 1}`);
    
    if (this._onChange) {
      this._onChange(zone, slotIndex, oldState, this.zones[zone][slotIndex]);
    }

    return true;
  }

  /**
   * Remove a toy from a specific zone
   */
  removeToy(zone, slot) {
    if (!this._isValidZone(zone)) {
      console.error(`[VirtualToy] Invalid zone: ${zone}`);
      return false;
    }

    const slots = this.zones[zone];
    let slotIndex = -1;
    if (Number.isInteger(slot)) {
      slotIndex = slot >= 0 && slot < slots.length ? slot : -1;
    } else {
      for (let i = slots.length - 1; i >= 0; i -= 1) {
        if (slots[i]) {
          slotIndex = i;
          break;
        }
      }
    }

    if (slotIndex < 0 || !slots[slotIndex]) {
      console.warn(`[VirtualToy] No toy on ${TOY_PAD.ZONE_NAMES[zone]}${Number.isInteger(slot) ? ` slot ${slot + 1}` : ''} to remove`);
      return false;
    }

    const oldState = slots[slotIndex];
    slots[slotIndex] = null;
    console.log(`[VirtualToy] Removed "${oldState.name}" from ${TOY_PAD.ZONE_NAMES[zone]} slot ${slotIndex + 1}`);

    if (this._onChange) {
      this._onChange(zone, slotIndex, oldState, null);
    }

    return true;
  }

  /**
   * Get the current state of a zone
   * @returns {object|null} Toy info or null if empty
   */
  getZoneState(zone, hint = 0) {
    if (!this._isValidZone(zone)) return null;
    const occupied = this.zones[zone].filter((entry) => entry);
    if (!occupied.length) {
      return null;
    }
    const index = Number.isInteger(hint) ? Math.abs(hint) % occupied.length : 0;
    return occupied[index];
  }

  getZoneSlots(zone) {
    if (!this._isValidZone(zone)) {
      return [];
    }
    return this.zones[zone].map((entry) => (entry ? { ...entry } : null));
  }

  /**
   * Get all zone states
   */
  getAllZones() {
    return {
      [TOY_PAD.ZONES.LEFT]: this.getZoneSlots(TOY_PAD.ZONES.LEFT),
      [TOY_PAD.ZONES.CENTER]: this.getZoneSlots(TOY_PAD.ZONES.CENTER),
      [TOY_PAD.ZONES.RIGHT]: this.getZoneSlots(TOY_PAD.ZONES.RIGHT)
    };
  }

  /**
   * List available toys in the database
   */
  static listAvailableToys() {
    const toys = [];
    for (const [id, info] of TOY_DATABASE) {
      toys.push({ id, ...info });
    }
    return toys.sort((left, right) => {
      if (left.type !== right.type) {
        return left.type.localeCompare(right.type);
      }
      if (left.world !== right.world) {
        return left.world.localeCompare(right.world);
      }
      return left.name.localeCompare(right.name);
    });
  }

  /**
   * Add a custom toy to the database
   * @param {string} id - Unique identifier
   * @param {object} toyData - { name, uid: Buffer(7), tagData: Buffer(4), gameId, type }
   */
  static registerToy(id, toyData) {
    if (toyData.uid.length !== TAG_UID_SIZE) {
      throw new Error(`UID must be ${TAG_UID_SIZE} bytes`);
    }
    if (toyData.tagData.length !== TAG_DATA_SIZE) {
      throw new Error(`Tag data must be ${TAG_DATA_SIZE} bytes`);
    }
    TOY_DATABASE.set(id, toyData);
  }
}

module.exports = { VirtualToyManager, TOY_DATABASE };
