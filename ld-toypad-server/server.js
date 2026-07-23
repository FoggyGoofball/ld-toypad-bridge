#!/usr/bin/env node

/**
 * server.js
 * LD-ToyPad Network Bridge Server (Phase 2)
 *
 * Main entry point for the LEGO Dimensions Toy Pad emulator server.
 * Listens for UDP packets from the PS3 .sprx plugin and responds
 * with appropriate Toy Pad USB HID data.
 *
 * Protocol: UDP (low latency, connectionless)
 * Default port: 28472
 *
 * Packet Format (PS3 -> PC):
 *   Byte 0:    Packet type (0x01=Poll, 0x02=ReadTag, 0x03=WriteTag)
 *   Byte 1:    Zone (0=LEFT, 1=CENTER, 2=RIGHT)
 *   Byte 2:    Sequence number (for ordering)
 *   Byte 3-7:  Reserved / payload
 *
 * Packet Format (PC -> PS3):
 *   Byte 0:    Status (0x00=OK, 0x01=NoTag, 0xFF=Error)
 *   Byte 1:    Zone
 *   Byte 2:    Sequence number (echoed from request)
 *   Bytes 3-79: Response data (variable, up to 77 bytes)
 */

const dgram = require('dgram');
const http = require('http');
const fs = require('fs');
const path = require('path');
const yargs = require('yargs/yargs');
const { hideBin } = require('yargs/helpers');

const { TOY_PAD, buildTagDataReport, buildEmptyZoneResponse, parsePS3Command, TAG_RESPONSE_SIZE } = require('./toypad-protocol');
const { VirtualToyManager } = require('./virtual-toys');
const { getImagePathForItemId, loadImageManifest } = require('./image-manifest');

// =========================================
// Parse command line arguments
// =========================================
const argv = yargs(hideBin(process.argv))
  .option('port', {
    alias: 'p',
    type: 'number',
    description: 'UDP port to listen on',
    default: 28472
  })
  .option('host', {
    alias: 'H',
    type: 'string',
    description: 'Host address to bind to',
    default: '0.0.0.0'
  })
  .option('ps3-ip', {
    alias: 'P',
    type: 'string',
    description: 'PS3 IP address (for targeted responses)',
    default: null
  })
  .option('verbose', {
    alias: 'v',
    type: 'boolean',
    description: 'Verbose packet logging',
    default: false
  })
  .option('debug-port', {
    type: 'number',
    description: 'UDP port to listen for SPRX debug logs',
    default: 28473
  })
  .option('http-port', {
    type: 'number',
    description: 'HTTP port for browser UI/API',
    default: 8080
  })
  .option('delay', {
    alias: 'd',
    type: 'number',
    description: 'Artificial delay in ms (for testing)',
    default: 0
  })
  .help()
  .alias('help', 'h')
  .argv;

// =========================================
// Configuration
// =========================================
const CONFIG = {
  PORT: argv.port,
  HOST: argv.host,
  PS3_IP: argv['ps3-ip'],
  VERBOSE: argv.verbose,
  DEBUG_PORT: argv['debug-port'],
  HTTP_PORT: argv['http-port'],
  DELAY: argv.delay,

  // Protocol constants
  PACKET_TYPE_POLL: 0x01,
  PACKET_TYPE_READ_TAG: 0x02,
  PACKET_TYPE_WRITE_TAG: 0x03,
  PACKET_TYPE_DATA_OUT: 0x04,
  PACKET_TYPE_DISCOVERY: 0xF0,
  PACKET_TYPE_KEEPALIVE: 0xEE,

  RESPONSE_OK: 0x00,
  RESPONSE_NO_TAG: 0x01,
  RESPONSE_ERROR: 0xFF,

  DISCOVERY_BEACON_INTERVAL_MS: 1000,
};

// =========================================
// State
// =========================================
const toyManager = new VirtualToyManager();
let clientAddress = null; // Address of the PS3 we're talking to
let startTime = Date.now();
const webRoot = path.join(__dirname, 'web');
let discoveryTimer = null;
const portalTelemetry = {
  updatedAt: null,
  lastPacketHex: '',
  inferredLitZones: []
};

/**
 * Infer which zones are lit (LED active) based on the DATA_OUT payload
 * received from the PS3. The game sends 8-byte HID output reports where:
 *   - Byte 0: Report ID (0x01)
 *   - Byte 1: LED mask / zone indicator
 *   - Byte 2: Zone attributes, often a bitmask (bit 0=LEFT, bit 1=CENTER, bit 2=RIGHT)
 *   - Bytes 3-7: Additional LED color/command data
 *
 * During keystone puzzles the game rapidly pulses zones to tell the player
 * where to place matching characters.  We surface any zone that appears
 * active in the payload.
 */
function inferLitZones(payload) {
  const lit = new Set();
  if (!payload || payload.length === 0) {
    return [];
  }

  // --- Heuristic 1: byte[1] as direct zone index ---
  // Some OUT packets carry the target zone directly in byte 1.
  if (payload.length > 1 && payload[1] >= 0 && payload[1] <= 2) {
    lit.add(payload[1]);
  }

  // --- Heuristic 2: byte[2] as a 3-bit zone mask ---
  // Common in HID output reports where bits select which zones light.
  if (payload.length > 2) {
    const mask = payload[2] & 0x07;
    if (mask & 0x01) lit.add(TOY_PAD.ZONES.LEFT);
    if (mask & 0x02) lit.add(TOY_PAD.ZONES.CENTER);
    if (mask & 0x04) lit.add(TOY_PAD.ZONES.RIGHT);
  }

  // --- Heuristic 3: byte[3] as an extended zone mask ---
  // Some firmware revisions / game patches use byte 3 instead.
  if (payload.length > 3) {
    const mask2 = payload[3] & 0x07;
    if (mask2 & 0x01) lit.add(TOY_PAD.ZONES.LEFT);
    if (mask2 & 0x02) lit.add(TOY_PAD.ZONES.CENTER);
    if (mask2 & 0x04) lit.add(TOY_PAD.ZONES.RIGHT);
  }

  // --- Heuristic 4: non-zero bytes 4-7 as active zone indicators ---
  // Some colour/LED commands put per-zone brightness in bytes 4-6
  // (one byte per zone: L, C, R).  A non-zero value means "lit".
  if (payload.length >= 7) {
    if (payload[4] !== 0) lit.add(TOY_PAD.ZONES.LEFT);
    if (payload[5] !== 0) lit.add(TOY_PAD.ZONES.CENTER);
    if (payload[6] !== 0) lit.add(TOY_PAD.ZONES.RIGHT);
  }

  return Array.from(lit).sort((a, b) => a - b);
}

function updatePortalTelemetry(packetBuffer) {
  const payload = packetBuffer.slice(8);
  portalTelemetry.updatedAt = Date.now();
  portalTelemetry.lastPacketHex = payload.toString('hex');
  portalTelemetry.inferredLitZones = inferLitZones(payload);
}

// =========================================
// UDP Server
// =========================================
const server = dgram.createSocket('udp4');
const debugSocket = dgram.createSocket('udp4');
const httpServer = http.createServer(handleHttpRequest);

function getZoneSnapshot() {
  const zones = toyManager.getAllZones();
  return Object.entries(zones).map(([zone, slotToys]) => {
    const zoneNum = Number(zone);
    const toys = slotToys.map((toy, slot) => (toy ? {
      slot,
      name: toy.name,
      gameId: toy.gameId,
      type: toy.type,
      itemId: toy.itemId,
      uidHex: toy.uid.toString('hex'),
      tagHex: toy.tagData.toString('hex')
    } : null));
    return {
      zone: zoneNum,
      zoneName: TOY_PAD.ZONE_NAMES[zoneNum],
      capacity: toyManager.getZoneCapacity(zoneNum),
      toys,
      toy: toys.find((entry) => entry) || null
    };
  });
}

function writeJson(res, status, payload) {
  res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
  res.end(JSON.stringify(payload));
}

function readJsonBody(req) {
  return new Promise((resolve, reject) => {
    let body = '';
    req.on('data', (chunk) => {
      body += chunk;
      if (body.length > 65536) {
        reject(new Error('Request too large'));
      }
    });
    req.on('end', () => {
      try {
        resolve(body ? JSON.parse(body) : {});
      } catch (e) {
        reject(new Error('Invalid JSON body'));
      }
    });
    req.on('error', reject);
  });
}

function serveFile(res, filePath, contentType) {
  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('Not found');
      return;
    }
    res.writeHead(200, { 'Content-Type': contentType });
    res.end(data);
  });
}

async function handleHttpRequest(req, res) {
  const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);

  if (req.method === 'GET' && url.pathname === '/api/status') {
    const uptime = Math.floor((Date.now() - startTime) / 1000);
    writeJson(res, 200, {
      ok: true,
      uptime,
      client: clientAddress,
      udpPort: CONFIG.PORT,
      debugPort: CONFIG.DEBUG_PORT,
      portal: {
        updatedAt: portalTelemetry.updatedAt,
        lastPacketHex: portalTelemetry.lastPacketHex,
        inferredLitZones: portalTelemetry.inferredLitZones,
        inferredLitZoneNames: portalTelemetry.inferredLitZones.map((zone) => TOY_PAD.ZONE_NAMES[zone])
      },
      zones: getZoneSnapshot()
    });
    return;
  }

  if (req.method === 'GET' && url.pathname === '/api/toys') {
    const imageManifest = loadImageManifest();
    const toys = VirtualToyManager.listAvailableToys().map((t) => ({
      id: t.id,
      name: t.name,
      type: t.type,
      gameId: t.gameId,
      itemId: t.itemId,
      world: t.world,
      rebuild: t.rebuild,
      releaseYear: t.releaseYear,
      ownership: t.ownership,
      imagePath: getImagePathForItemId(t.itemId, imageManifest)
    }));
    writeJson(res, 200, { ok: true, toys });
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/place') {
    try {
      const body = await readJsonBody(req);
      const zone = Number(body.zone);
      const slot = body.slot === undefined || body.slot === null || body.slot === ''
        ? undefined
        : Number(body.slot);
      const toyId = String(body.toyId || '').trim();
      if (!Number.isInteger(zone) || zone < 0 || zone > 2 || !toyId) {
        writeJson(res, 400, { ok: false, error: 'Invalid zone or toyId' });
        return;
      }
      if (slot !== undefined && (!Number.isInteger(slot) || slot < 0 || slot >= toyManager.getZoneCapacity(zone))) {
        writeJson(res, 400, { ok: false, error: 'Invalid slot' });
        return;
      }
      const ok = toyManager.placeToy(zone, toyId, slot);
      if (!ok) {
        writeJson(res, 400, { ok: false, error: 'Could not place toy (zone may be full)' });
        return;
      }
      writeJson(res, 200, { ok: true, zones: getZoneSnapshot() });
    } catch (err) {
      writeJson(res, 400, { ok: false, error: err.message });
    }
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/remove') {
    try {
      const body = await readJsonBody(req);
      const zone = Number(body.zone);
      const slot = body.slot === undefined || body.slot === null || body.slot === ''
        ? undefined
        : Number(body.slot);
      if (!Number.isInteger(zone) || zone < 0 || zone > 2) {
        writeJson(res, 400, { ok: false, error: 'Invalid zone' });
        return;
      }
      if (slot !== undefined && (!Number.isInteger(slot) || slot < 0 || slot >= toyManager.getZoneCapacity(zone))) {
        writeJson(res, 400, { ok: false, error: 'Invalid slot' });
        return;
      }
      toyManager.removeToy(zone, slot);
      writeJson(res, 200, { ok: true, zones: getZoneSnapshot() });
    } catch (err) {
      writeJson(res, 400, { ok: false, error: err.message });
    }
    return;
  }

  if (req.method === 'GET' && url.pathname === '/') {
    serveFile(res, path.join(webRoot, 'index.html'), 'text/html; charset=utf-8');
    return;
  }

  if (req.method === 'GET' && url.pathname === '/app.js') {
    serveFile(res, path.join(webRoot, 'app.js'), 'application/javascript; charset=utf-8');
    return;
  }

  if (req.method === 'GET' && url.pathname === '/styles.css') {
    serveFile(res, path.join(webRoot, 'styles.css'), 'text/css; charset=utf-8');
    return;
  }

  if (req.method === 'GET' && url.pathname.startsWith('/images/')) {
    const fileName = path.basename(url.pathname);
    serveFile(res, path.join(__dirname, 'images', fileName), 'image/png');
    return;
  }

  res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
  res.end('Not found');
}

server.on('error', (err) => {
  console.error(`[Server] Error: ${err.message}`);
  server.close();
});

server.on('listening', () => {
  const addr = server.address();
  console.log('='.repeat(60));
  console.log('  LD-ToyPad Network Bridge Server');
  console.log('='.repeat(60));
  console.log(`  Listening:  ${addr.address}:${addr.port}`);
  console.log(`  Port:       ${addr.port}`);
  console.log(`  PS3 IP:     ${CONFIG.PS3_IP || 'ANY (discovery mode)'}`);
  console.log(`  Delay:      ${CONFIG.DELAY}ms`);
  console.log(`  Verbose:    ${CONFIG.VERBOSE}`);
  console.log(`  Debug UDP:  ${CONFIG.DEBUG_PORT}`);
  console.log(`  Web UI:     http://localhost:${CONFIG.HTTP_PORT}`);
  console.log('='.repeat(60));
  console.log('');
  console.log('Available toys:');
  VirtualToyManager.listAvailableToys().forEach(t => {
    console.log(`  ${t.id.padEnd(12)} - ${t.name} (${t.type})`);
  });
  console.log('');
  console.log('Commands via stdin:');
  console.log('  place <zone> <toyId> [slot] - Place toy on zone (optional slot)');
  console.log('  remove <zone> [slot]        - Remove toy from zone (or slot)');
  console.log('  list                    - List available toys');
  console.log('  status                  - Show pad status');
  console.log('  quit/exit               - Shutdown server');
  console.log('='.repeat(60));

  try {
    server.setBroadcast(true);
  } catch (err) {
    console.warn(`[Server] Could not enable UDP broadcast: ${err.message}`);
  }

  discoveryTimer = setInterval(() => {
    if (clientAddress) {
      return;
    }

    const beacon = Buffer.alloc(80, 0x00);
    beacon[0] = CONFIG.PACKET_TYPE_DISCOVERY;
    beacon[1] = TOY_PAD.ZONES.CENTER;

    // Always send broadcast — the PS3 listens for it regardless
    server.send(beacon, CONFIG.PORT, '255.255.255.255', (err) => {
      if (err && CONFIG.VERBOSE) {
        console.warn(`[Server] Broadcast beacon failed: ${err.message}`);
      }
    });

    // Also send directed probe to cached IP if available (faster path)
    if (CONFIG.PS3_IP) {
      server.send(beacon, CONFIG.PORT, CONFIG.PS3_IP, (err) => {
        if (err && CONFIG.VERBOSE) {
          console.warn(`[Server] Directed beacon to ${CONFIG.PS3_IP} failed: ${err.message}`);
        }
      });
    }
  }, CONFIG.DISCOVERY_BEACON_INTERVAL_MS);
  if (typeof discoveryTimer.unref === 'function') {
    discoveryTimer.unref();
  }
});

debugSocket.on('error', (err) => {
  console.error(`[Debug] Error: ${err.message}`);
});

debugSocket.on('listening', () => {
  const addr = debugSocket.address();
  console.log(`[Debug] Listening for SPRX logs on ${addr.address}:${addr.port}`);
});

debugSocket.on('message', (msg, rinfo) => {
  const line = msg.toString('utf8').replace(/\0+$/g, '').trimEnd();
  if (!line) {
    return;
  }
  console.log(`[SPRX ${rinfo.address}:${rinfo.port}] ${line}`);
});

server.on('message', (msg, rinfo) => {
  // Handle incoming packets from PS3
  if (msg.length < 3) {
    console.warn(`[Server] Short packet from ${rinfo.address}:${rinfo.port} (${msg.length} bytes)`);
    return;
  }

  // Track PS3 address (skip our own IPs — server echoes its beacons).
  // Any packet from a non-local IP is accepted for registration.
  // The byte 0x01 is a USB HID Report ID, not a custom protocol type.
  if (!clientAddress) {
    const addr = rinfo.address;
    if (addr === '127.0.0.1' || addr === '::1' || addr === '0.0.0.0' || addr === '192.168.0.17') {
      return;
    }
    clientAddress = { address: addr, port: rinfo.port };
    console.log(`[Server] Client connected from ${addr}:${rinfo.port}`);
  }

  const packetType = msg[0];
  const zone = msg[1];
  const sequence = msg[2];

  // ALWAYS LOG incoming packets regardless of VERBOSE flag
  console.log(`[Server] RX from ${rinfo.address}:${rinfo.port} type=0x${packetType.toString(16)} zone=${zone} seq=${sequence} len=${msg.length}`);

  // Process the packet
  processPacket(packetType, zone, sequence, rinfo, msg);
});

/**
 * Process an incoming packet and send a response
 */
async function processPacket(packetType, zone, sequence, rinfo, packetBuffer) {
  let response;

  switch (packetType) {
    case CONFIG.PACKET_TYPE_POLL:
      response = handlePoll(zone, sequence);
      break;

    case CONFIG.PACKET_TYPE_READ_TAG:
      response = handleReadTag(zone, sequence);
      break;

    case CONFIG.PACKET_TYPE_WRITE_TAG:
      response = handleWriteTag(zone, sequence, rinfo);
      break;

    case CONFIG.PACKET_TYPE_DATA_OUT:
      response = handleDataOut(zone, sequence, packetBuffer);
      break;

    case CONFIG.PACKET_TYPE_DISCOVERY:
      if (CONFIG.VERBOSE) {
        console.log(`[Server] Discovery probe from ${rinfo.address}:${rinfo.port}`);
      }
      // Respond with a discovery ACK — same 0xF0 type, zone echoed, seq echoed
      {
        const ack = Buffer.alloc(8, 0x00);
        ack[0] = CONFIG.PACKET_TYPE_DISCOVERY;
        ack[1] = zone;
        ack[2] = sequence;
        ack[3] = CONFIG.RESPONSE_OK;
        response = ack;
      }
      break;

    case CONFIG.PACKET_TYPE_KEEPALIVE:
      // Keepalive heartbeat from PS3 – just register client and ACK.
      // No zone/data processing needed.  The PS3 sends this every 3 seconds
      // so the server knows it's alive (especially in VSH mode with no USB hooks).
      if (CONFIG.VERBOSE) {
        console.log(`[Server] Keepalive from ${rinfo.address}:${rinfo.port} (seq=${sequence})`);
      }
      {
        const ack = Buffer.alloc(8, 0x00);
        ack[0] = CONFIG.PACKET_TYPE_KEEPALIVE;
        ack[1] = CONFIG.RESPONSE_OK;
        ack[2] = sequence;
        response = ack;
      }
      break;

    default:
      if (CONFIG.VERBOSE) {
        console.warn(`[Server] Unknown packet type: 0x${packetType.toString(16)}`);
      }
      response = buildErrorResponse(CONFIG.RESPONSE_ERROR, zone, sequence);
      break;
  }

  // Apply artificial delay for testing
  if (CONFIG.DELAY > 0) {
    await new Promise(resolve => setTimeout(resolve, CONFIG.DELAY));
  }

  // Send response
  server.send(response, rinfo.port, rinfo.address, (err) => {
    if (err) {
      console.error(`[Server] Send error: ${err.message}`);
    } else if (CONFIG.VERBOSE) {
      console.log(`[Server] TX to ${rinfo.address}:${rinfo.port} (${response.length} bytes)`);
    }
  });
}

function handleDataOut(zone, sequence, packetBuffer) {
  updatePortalTelemetry(packetBuffer);
  return buildPacket(CONFIG.RESPONSE_OK, zone, sequence, Buffer.alloc(77, 0x00));
}

/**
 * Handle a poll request from the PS3
 * The game sends a poll at each USB interrupt interval (~1ms)
 * We respond with current zone state
 */
function handlePoll(zone, sequence) {
  const toy = toyManager.getZoneState(zone, sequence);

  if (toy) {
    // Tag is present on this zone - send tag data report
    const report = buildTagDataReport(zone, TOY_PAD.STATE.TAG_PRESENT, toy.uid, toy.tagData);
    return buildPacket(CONFIG.RESPONSE_OK, zone, sequence, report);
  } else {
    // No tag - send empty response
    return buildPacket(CONFIG.RESPONSE_NO_TAG, zone, sequence, Buffer.alloc(77, 0x00));
  }
}

/**
 * Handle a read tag request
 * The game specifically requests the tag data for a zone
 */
function handleReadTag(zone, sequence) {
  const toy = toyManager.getZoneState(zone, sequence);

  if (toy) {
    // Full tag read response
    const report = buildTagDataReport(zone, TOY_PAD.STATE.TAG_PLACED, toy.uid, toy.tagData);
    return buildPacket(CONFIG.RESPONSE_OK, zone, sequence, report);
  } else {
    return buildPacket(CONFIG.RESPONSE_NO_TAG, zone, sequence, Buffer.alloc(77, 0x00));
  }
}

/**
 * Handle a write tag request
 * The game wants to write data to a tag (e.g., upgrading a vehicle)
 * For now, log the write request
 */
function handleWriteTag(zone, sequence, rinfo) {
  console.log(`[Server] Write request for zone ${zone} from ${rinfo.address}`);
  // TODO: Implement tag writing (requires MIFARE crypto)
  return buildPacket(CONFIG.RESPONSE_OK, zone, sequence, Buffer.alloc(77, 0x00));
}

/**
 * Build a response packet
 * Format:
 *   Byte 0:    Status
 *   Byte 1:    Zone
 *   Byte 2:    Sequence number (echoed)
 *   Bytes 3-79: Response data (77 bytes)
 */
function buildPacket(status, zone, sequence, data) {
  const buf = Buffer.alloc(3 + 77, 0x00); // Total: 80 bytes
  buf[0] = status;
  buf[1] = zone;
  buf[2] = sequence;

  // Copy response data if provided
  if (data && data.length > 0) {
    const copyLen = Math.min(data.length, 77);
    data.copy(buf, 3, 0, copyLen);
  }

  return buf;
}

function buildErrorResponse(status, zone, sequence) {
  const buf = Buffer.alloc(80, 0x00);
  buf[0] = status;
  buf[1] = zone;
  buf[2] = sequence;
  return buf;
}

// =========================================
// CLI stdin interface for virtual toy control
// =========================================
const readline = require('readline');
const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
  prompt: 'toypad> '
});

rl.on('line', (line) => {
  const args = line.trim().split(/\s+/);
  const cmd = args[0]?.toLowerCase();

  switch (cmd) {
    case 'place': {
      const zone = parseInt(args[1]);
      const toyId = args[2];
      const slotArg = args[3] !== undefined ? parseInt(args[3], 10) : undefined;
      if (isNaN(zone) || zone < 0 || zone > 2) {
        console.log('Usage: place <zone (0-2)> <toyId> [slot]');
        break;
      }
      if (!toyId) {
        console.log('Usage: place <zone> <toyId> [slot]');
        console.log('Available toys:', VirtualToyManager.listAvailableToys().map(t => t.id).join(', '));
        break;
      }
      toyManager.placeToy(zone, toyId, Number.isInteger(slotArg) ? slotArg : undefined);
      break;
    }

    case 'remove': {
      const zone = parseInt(args[1]);
      const slotArg = args[2] !== undefined ? parseInt(args[2], 10) : undefined;
      if (isNaN(zone) || zone < 0 || zone > 2) {
        console.log('Usage: remove <zone (0-2)> [slot]');
        break;
      }
      toyManager.removeToy(zone, Number.isInteger(slotArg) ? slotArg : undefined);
      break;
    }

    case 'list': {
      console.log('Available toys:');
      VirtualToyManager.listAvailableToys().forEach(t => {
        console.log(`  ${t.id.padEnd(12)} - ${t.name} (${t.type})`);
      });
      const zones = toyManager.getAllZones();
      console.log('\nCurrent pad state:');
      for (const [zoneNum, slots] of Object.entries(zones)) {
        const zoneName = TOY_PAD.ZONE_NAMES[zoneNum];
        const slotText = slots.map((toy, index) => toy ? `${index + 1}:${toy.name}` : `${index + 1}:(empty)`).join(' | ');
        console.log(`  ${zoneName.padEnd(8)}: ${slotText}`);
      }
      break;
    }

    case 'status': {
      const uptime = Math.floor((Date.now() - startTime) / 1000);
      console.log(`Server status:`);
      console.log(`  Uptime:     ${uptime}s`);
      console.log(`  Client:     ${clientAddress ? `${clientAddress.address}:${clientAddress.port}` : 'None'}`);
      console.log(`  Port:       ${CONFIG.PORT}`);
      console.log(`  Debug UDP:  ${CONFIG.DEBUG_PORT}`);
      const zones = toyManager.getAllZones();
      for (const [zoneNum, slots] of Object.entries(zones)) {
        const zoneName = TOY_PAD.ZONE_NAMES[zoneNum];
        const slotText = slots.map((toy, index) => toy ? `${index + 1}:${toy.name}` : `${index + 1}:(empty)`).join(' | ');
        console.log(`  Zone ${zoneName}: ${slotText}`);
      }
      break;
    }

    case 'quit':
    case 'exit':
      console.log('Shutting down...');
      if (discoveryTimer) {
        clearInterval(discoveryTimer);
        discoveryTimer = null;
      }
      server.close();
      debugSocket.close();
      httpServer.close();
      process.exit(0);
      break;

    case 'help':
      console.log('Commands:');
      console.log('  place <zone> <toyId> [slot] - Place toy on zone (optional slot)');
      console.log('  remove <zone> [slot]        - Remove toy from zone (or slot)');
      console.log('  list                    - List available toys');
      console.log('  status                  - Show pad status');
      console.log('  quit/exit               - Shutdown server');
      break;

    case '':
      break;

    default:
      console.log(`Unknown command: ${cmd}. Type 'help' for usage.`);
  }

  rl.prompt();
});

rl.on('close', () => {
  // stdin closed (e.g., piped input, background mode). Do NOT exit —
  // the server should keep running. The SIGINT handler still works.
  if (process.stdin.isTTY) {
    console.log('Shutting down...');
    if (discoveryTimer) {
      clearInterval(discoveryTimer);
      discoveryTimer = null;
    }
    server.close();
    debugSocket.close();
    httpServer.close();
    process.exit(0);
  }
});

// Handle cleanup
process.on('SIGINT', () => {
  console.log('\nShutting down...');
  if (discoveryTimer) {
    clearInterval(discoveryTimer);
    discoveryTimer = null;
  }
  server.close();
  debugSocket.close();
  httpServer.close();
  process.exit(0);
});

process.on('SIGTERM', () => {
  console.log('\nShutting down...');
  if (discoveryTimer) {
    clearInterval(discoveryTimer);
    discoveryTimer = null;
  }
  server.close();
  debugSocket.close();
  httpServer.close();
  process.exit(0);
});

// =========================================
// Start server
// =========================================
server.bind(CONFIG.PORT, CONFIG.HOST);
debugSocket.bind(CONFIG.DEBUG_PORT, CONFIG.HOST);
httpServer.listen(CONFIG.HTTP_PORT, CONFIG.HOST, () => {
  console.log(`[Web] UI/API listening on http://${CONFIG.HOST}:${CONFIG.HTTP_PORT}`);
});
