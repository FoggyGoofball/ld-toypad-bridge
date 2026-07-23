#!/usr/bin/env node
/**
 * simulate-ps3.js — Fake PS3 Discovery/Keepalive Tester
 *
 * Sends synthetic keepalive, discovery, or poll packets to the
 * LD-ToyPad server to verify the server registers the client.
 *
 * Use this to test the PC server IN ISOLATION from the actual PS3:
 *   node simulate-ps3.js                   # discovery beacon (0xF0)
 *   node simulate-ps3.js --keepalive       # keepalive (0xEE)
 *   node simulate-ps3.js --poll            # poll (0x01)
 *   node simulate-ps3.js --ps3-ip 192.168.0.10   # spoof source IP
 *
 * The server should log something like:
 *   [Server] Client connected from 192.168.0.10:54321
 *   [Server] RX type=0xF0 zone=1 seq=0 len=8
 *
 * If it doesn't, the server isn't receiving the packet at all
 * (firewall / wrong port / wrong host bind).
 *
 * (c) 2026 LD-ToyPad Bridge Team
 */

const dgram = require('dgram');
const yargs = require('yargs/yargs');
const { hideBin } = require('yargs/helpers');

const argv = yargs(hideBin(process.argv))
  .option('server', {
    alias: 's',
    type: 'string',
    description: 'Server host',
    default: '127.0.0.1'
  })
  .option('port', {
    alias: 'p',
    type: 'number',
    description: 'Server UDP port',
    default: 28472
  })
  .option('type', {
    alias: 't',
    type: 'string',
    description: 'Packet type to send (discovery|keepalive|poll)',
    default: 'discovery'
  })
  .option('ps3-ip', {
    type: 'string',
    description: 'Fake PS3 IP address (default: OS-chosen)',
    default: null
  })
  .option('wait', {
    alias: 'w',
    type: 'boolean',
    description: 'Keep running and listen for responses',
    default: false
  })
  .option('verbose', {
    alias: 'v',
    type: 'boolean',
    description: 'Verbose output',
    default: false
  })
  .help()
  .alias('help', 'h')
  .argv;

// Packet type constants (mirrors network.h / server.js)
const PACKET_TYPES = {
  discovery: 0xF0,
  keepalive: 0xEE,
  poll: 0x01,
};

const sock = dgram.createSocket('udp4');

if (argv.wait) {
  sock.on('message', (msg, rinfo) => {
    console.log(`[SIM-PS3] RX from ${rinfo.address}:${rinfo.port} [${msg.length}B]`);
    if (argv.verbose) {
      console.log(`[SIM-PS3]  Hex: ${msg.toString('hex')}`);
    }
  });
}

sock.on('listening', () => {
  const addr = sock.address();
  console.log(`[SIM-PS3] Local socket: ${addr.address}:${addr.port}`);

  const packetType = PACKET_TYPES[argv.type] || PACKET_TYPES.discovery;

  // Build a packet matching the SPRX plugin's format (8 bytes minimum)
  const buf = Buffer.alloc(8, 0x00);
  buf[0] = packetType;
  buf[1] = 1;       // zone = CENTER
  buf[2] = 0;       // sequence

  if (argv.type === 'keepalive') {
    buf[1] = 0;     // reserved
  }

  console.log(`[SIM-PS3] Sending type=0x${packetType.toString(16)} (${argv.type})`);
  if (argv.verbose) {
    console.log(`[SIM-PS3]  Hex: ${buf.toString('hex')}`);
  }

  // Send to server
  sock.send(buf, argv.port, argv.server, (err) => {
    if (err) {
      console.error(`[SIM-PS3] SEND ERROR: ${err.message}`);
      sock.close();
      process.exit(1);
    }
    console.log(`[SIM-PS3] Sent ${buf.length} bytes to ${argv.server}:${argv.port}`);

    if (argv.wait) {
      console.log(`[SIM-PS3] Waiting for response (Ctrl+C to quit)...`);
    } else {
      // Wait briefly for response, then exit
      setTimeout(() => {
        sock.close();
        process.exit(0);
      }, 500);
    }
  });
});

sock.on('error', (err) => {
  console.error(`[SIM-PS3] Socket error: ${err.message}`);
  process.exit(1);
});

// If a fake PS3 IP was specified, bind to it (requires admin on some OS)
// Most of the time the OS picks the right interface.
const bindHost = argv['ps3-ip'] || '0.0.0.0';
sock.bind(0, bindHost);
