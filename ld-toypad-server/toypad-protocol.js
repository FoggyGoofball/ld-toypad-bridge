/**
 * toypad-protocol.js
 * LEGO Dimensions Toy Pad USB HID Protocol Implementation
 *
 * Based on Berny23/LD-ToyPad-Emulator reverse engineering
 * The Toy Pad appears as a USB HID device with specific report descriptors.
 * 
 * USB Identifiers:
 *   VID: 0x0E6F (Logic3 / PDP)
 *   PID: 0x0241 (LEGO Dimensions Toy Pad)
 *
 * The pad has 3 NFC reader zones: LEFT, CENTER, RIGHT
 * Each zone can read/write MIFARE Ultralight C tags (NTAG213/215/216)
 */

const TOY_PAD = {
  // USB Vendor/Product IDs to spoof
  VID: 0x0E6F,
  PID: 0x0241,

  // Pad zones
  ZONES: {
    LEFT: 0,
    CENTER: 1,
    RIGHT: 2
  },

  // Zone name lookup
  ZONE_NAMES: ['LEFT', 'CENTER', 'RIGHT'],

  // Toy Pad states
  STATE: {
    NO_TAG: 0x00,
    TAG_PLACED: 0x01,
    TAG_REMOVED: 0x02,
    TAG_PRESENT: 0x03
  },

  // HID Report sizes (bytes)
  HID_REPORT_SIZE: 8,

  // Polling interval expectation (the game polls at ~1ms intervals)
  // We need to respond within this window
  POLL_INTERVAL_MS: 4
};

/**
 * MIFARE Ultralight C / NTAG213 NFC tag data structure
 * Each toy figure has:
 *   - 7-byte UID
 *   - 4-byte tag data (character ID + padding)
 *   - 32-byte encrypted payload (MIFARE crypto)
 * 
 * The full tag response sent over USB is 80 bytes
 */
const TAG_RESPONSE_SIZE = 80;
const TAG_UID_SIZE = 7;
const TAG_DATA_SIZE = 4;

// Known toy character IDs for testing (from official toy database)
const KNOWN_CHARACTERS = {
  // Starter Pack characters
  '0x271A': { name: 'Batman', gameId: 'batman' },
  '0x271B': { name: 'Gandalf', gameId: 'gandalf' },
  '0x271C': { name: 'Wyldstyle', gameId: 'wyldstyle' },
  // Example vehicle tags
  '0x2B6A': { name: 'Batmobile', gameId: 'batmobile' },
};

/**
 * Build a raw USB HID report for the Toy Pad
 * Structure (8 bytes):
 *   Byte 0: Report ID (0x01 for pad status)
 *   Byte 1: Zone number (0=LEFT, 1=CENTER, 2=RIGHT)
 *   Byte 2: Tag state (0=empty, 1=placed, 2=removed, 3=present)
 *   Byte 3-7: Reserved / padding
 */
function buildStatusReport(zone, state) {
  const buf = Buffer.alloc(TOY_PAD.HID_REPORT_SIZE, 0x00);
  buf[0] = 0x01;  // Report ID
  buf[1] = zone;   // Zone
  buf[2] = state;  // State
  return buf;
}

/**
 * Build a tag data report for when a tag is placed on a zone
 * This is the full USB HID report that includes tag data
 * 
 * Structure (80 bytes):
 *   Bytes 0-7:   HID header (report ID + zone + state + padding)
 *   Bytes 8-14:  7-byte tag UID
 *   Bytes 15-18: 4-byte character/tag data
 *   Bytes 19-79: Reserved / encrypted payload area
 */
function buildTagDataReport(zone, state, uid, tagData) {
  const buf = Buffer.alloc(80, 0x00);

  // HID header
  buf[0] = 0x01;  // Report ID
  buf[1] = zone;  // Zone number
  buf[2] = state; // Tag state
  buf[3] = 0x00;  // Flags

  // Tag UID (bytes 8-14)
  if (uid && uid.length >= TAG_UID_SIZE) {
    for (let i = 0; i < TAG_UID_SIZE; i++) {
      buf[8 + i] = uid[i];
    }
  }

  // Tag data (bytes 15-18)
  if (tagData && tagData.length >= TAG_DATA_SIZE) {
    for (let i = 0; i < TAG_DATA_SIZE; i++) {
      buf[15 + i] = tagData[i];
    }
  }

  // TODO: Encrypted payload section
  // The real Toy Pad sends MIFARE Ultralight C encrypted pages
  // This requires the crypto keys which need to be extracted
  // For now, send zeroed payload for basic detection

  return buf;
}

/**
 * Parse a USB HID report from the PS3 (inbound poll/command)
 * The PS3 sends 8-byte HID reports to the Toy Pad
 * 
 * Byte 0: Report ID
 * Byte 1: Command (0x01 = poll, 0x02 = read tag, 0x03 = write tag)
 * Byte 2-7: Command-specific parameters
 */
function parsePS3Command(buffer) {
  if (!buffer || buffer.length < TOY_PAD.HID_REPORT_SIZE) {
    return null;
  }

  return {
    reportId: buffer[0],
    command: buffer[1],
    // Interpret remaining bytes based on command
    raw: buffer.slice(2, TOY_PAD.HID_REPORT_SIZE)
  };
}

/**
 * Build a poll response (most frequent - sent every USB interrupt cycle)
 * This tells the game "nothing has changed" for a given zone
 */
function buildPollResponse(zone) {
  return buildStatusReport(zone, TOY_PAD.STATE.TAG_PRESENT);
}

/**
 * Build a "no tag" response (zone is empty)
 */
function buildEmptyZoneResponse(zone) {
  return buildStatusReport(zone, TOY_PAD.STATE.NO_TAG);
}

module.exports = {
  TOY_PAD,
  TAG_RESPONSE_SIZE,
  TAG_UID_SIZE,
  TAG_DATA_SIZE,
  KNOWN_CHARACTERS,
  buildStatusReport,
  buildTagDataReport,
  parsePS3Command,
  buildPollResponse,
  buildEmptyZoneResponse
};
