#!/usr/bin/env node
/**
 * scan-game-nids.js — PC-side NID scanner for LEGO Dimensions
 *
 * FIXED 2026-07-24 (based on expert analysis):
 *   Fix #1: Start address 0x00010000 (was 0x00100000 — typo off by 16x!)
 *     Standard PS3 executables load at 0x00010000 (64KB). The .sceStub.rodata
 *     and .got sections are in the first ~1MB which we were SKIPPING entirely.
 *   
 *   Fix #2: Official SDK uses parallel arrays (scelibstub), NOT interleaved
 *     triplets. NIDs are packed back-to-back as uint32_t[]. The GOT pointer
 *     table is a SEPARATE parallel array, not interleaved with NIDs.
 *     So we search for raw NID byte patterns anywhere in memory.
 *
 * Usage: node scan-game-nids.js [--ps3-ip <ip>] [--pid 0xNNNNN] [--verbose]
 */
 
const http = require('http');
const path = require('path');
const fs = require('fs');

const PS3_IP = (() => {
  const idx = process.argv.indexOf('--ps3-ip');
  if (idx !== -1 && process.argv[idx + 1]) return process.argv[idx + 1];
  try {
    return fs.readFileSync(path.join(__dirname, '..', 'ps3-ip.txt'), 'utf8').trim();
  } catch { return '192.168.0.47'; }
})();

const VERBOSE = process.argv.includes('--verbose') || process.argv.includes('-v');
const PS3MAPI_PORT = 80;

// Known cellUsbd NIDs
const NIDS_32 = [
  0x7F5F00D3,  // cellUsbdInit
  0x1AB6D80B,  // cellUsbdOpenPipe
  0x7B4436CE,  // cellUsbdTransfer
  0x2F82F1A5,  // cellUsbdClosePipe
];

const NID_NAMES = {
  '0x7F5F00D3': 'cellUsbdInit',
  '0x1AB6D80B': 'cellUsbdOpenPipe',
  '0x7B4436CE': 'cellUsbdTransfer',
  '0x2F82F1A5': 'cellUsbdClosePipe',
};

// FIXED: Start at 0x00010000 (64KB), not 0x00100000 (1MB).
// The .sceStub.rodata with NID tables is almost certainly in the first MB.
const SCAN_REGIONS = [
  { start: 0x00010000, size: 0x000F0000, desc: 'game .text low (0x00010000)' },
  { start: 0x00100000, size: 0x00800000, desc: 'game .text region 1' },
  { start: 0x01000000, size: 0x01000000, desc: 'game .text region 2' },
  { start: 0x02000000, size: 0x01000000, desc: 'game .text region 3' },
  { start: 0x03000000, size: 0x02000000, desc: 'game .text region 4' },
  { start: 0x30000000, size: 0x00800000, desc: 'cellUsbd PRX' },
  { start: 0x40000000, size: 0x01000000, desc: 'cellUsbd PRX alt' },
];

const CHUNK_SIZE = 0x8000; // 32KB chunks

function getmem(pid, addr, len) {
  return new Promise((resolve, reject) => {
    const endpoint = `/getmem.ps3mapi?proc=${pid}&addr=0x${addr.toString(16)}&len=${len}`;
    const req = http.get(`http://${PS3_IP}:${PS3MAPI_PORT}${endpoint}`, (res) => {
      let data = '';
      res.on('data', chunk => data += chunk);
      res.on('end', () => resolve(data));
    });
    req.on('error', (err) => reject(err));
    req.setTimeout(10000, () => { req.destroy(); reject(new Error('timeout')); });
  });
}

/** Parse hex text from the getmem response */
function extractHexFromResponse(resp) {
  const textareaMatch = resp.match(/<textarea[^>]*>([\s\S]*?)<\/textarea>/);
  const hexStr = textareaMatch ? textareaMatch[1].trim() : resp.trim();
  if (/^[0-9a-fA-F]+$/.test(hexStr)) return Buffer.from(hexStr, 'hex');
  const clean = hexStr.replace(/\s+/g, '');
  if (/^[0-9a-fA-F]+$/.test(clean)) return Buffer.from(clean, 'hex');
  return null;
}

/**
 * Find NIDs as raw 32-bit values in memory.
 * NIDs are packed back-to-back (official SDK scelibstub format).
 * We search both BE and LE byte orders.
 */
function findPackedNids(buf, baseAddr) {
  const results = [];
  
  for (const nid of NIDS_32) {
    const name = NID_NAMES[`0x${nid.toString(16).toUpperCase()}`] || 'UNKNOWN';
    
    // Try BE first (native PowerPC byte order)
    const nidBE = Buffer.alloc(4);
    nidBE.writeUInt32BE(nid, 0);
    let idx = 0;
    while ((idx = buf.indexOf(nidBE, idx)) >= 0) {
      const physAddr = baseAddr + idx;
      // Avoid duplicates
      if (!results.some(r => r.physAddr === physAddr && r.endian === 'BE')) {
        results.push({ nid, name, physAddr, endian: 'BE' });
      }
      idx += 4;
    }
    
    // Try LE
    const nidLE = Buffer.alloc(4);
    nidLE.writeUInt32LE(nid, 0);
    idx = 0;
    while ((idx = buf.indexOf(nidLE, idx)) >= 0) {
      const physAddr = baseAddr + idx;
      if (!results.some(r => r.physAddr === physAddr)) {
        results.push({ nid, name, physAddr, endian: 'LE' });
      }
      idx += 4;
    }
  }
  
  return results;
}

/**
 * Dump memory around a given address as 32-bit words.
 */
async function dumpContext(pid, centerAddr, halfSize = 128) {
  const start = centerAddr - halfSize;
  const end = centerAddr + halfSize;
  try {
    const resp = await getmem(pid, start, end - start);
    const buf = extractHexFromResponse(resp);
    if (!buf) return null;
    
    const words = [];
    const wordBuf = Buffer.alloc(4);
    for (let i = 0; i + 4 <= buf.length; i += 4) {
      const wAddr = start + i;
      wordBuf[0] = buf[i]; wordBuf[1] = buf[i+1];
      wordBuf[2] = buf[i+2]; wordBuf[3] = buf[i+3];
      words.push({ addr: wAddr, val: wordBuf.readUInt32BE(0), valLE: wordBuf.readUInt32LE(0) });
    }
    return words;
  } catch { return null; }
}

async function scanMemory(pid) {
  console.log(`\n=== Scanning PS3 game memory for cellUsbd NIDs ===`);
  console.log(`PS3 IP: ${PS3_IP}`);
  console.log(`Game PID: 0x${pid.toString(16)} (${pid})`);
  console.log('');
  console.log(`FIX #1: START ADDRESS corrected to 0x00010000 (was 0x00100000)`);
  console.log(`FIX #2: Scanning for packed NIDs (official SDK parallel array format)`);
  console.log('');
  
  let totalFound = {};
  for (const nid of NIDS_32) totalFound[nid] = [];

  for (const region of SCAN_REGIONS) {
    const end = region.start + region.size;
    let addr = region.start;
    let chunkCount = 0;
    let regionFound = [];

    while (addr < end) {
      const chunkLen = Math.min(CHUNK_SIZE, end - addr);
      try {
        const resp = await getmem(pid, addr, chunkLen);
        const buf = extractHexFromResponse(resp);
        if (buf) {
          const found = findPackedNids(buf, addr);
          for (const f of found) {
            if (!regionFound.some(r => r.physAddr === f.physAddr && r.endian === f.endian)) {
              regionFound.push(f);
              totalFound[f.nid].push(f);
            }
          }
        }
      } catch { /* memory read failure - skip */ }
      
      addr += CHUNK_SIZE;
      chunkCount++;
      
      if (VERBOSE && chunkCount % 8 === 0) {
        const pct = Math.round(((addr - region.start) / region.size) * 100);
        process.stdout.write(`  ${region.desc}: ${pct}% scanned (0x${addr.toString(16)})\r`);
      }
    }
    
    if (regionFound.length > 0) {
      console.log(`✓ ${region.desc}: found ${regionFound.length} NID occurrences`);
    } else if (VERBOSE) {
      console.log(`  - ${region.desc}: no NIDs found`);
    }
  }

  // Results
  console.log('\n═══ SCAN RESULTS ═══');
  let allFound = true;
  for (const nid of NIDS_32) {
    const name = NID_NAMES[`0x${nid.toString(16).toUpperCase()}`] || 'UNKNOWN';
    if (totalFound[nid].length > 0) {
      console.log(`\n✓ ${name}: FOUND (${totalFound[nid].length} occurrence(s)):`);
      for (const f of totalFound[nid]) {
        console.log(`   0x${f.physAddr.toString(16)} (${f.endian})`);
      }
    } else {
      console.log(`\n✗ ${name}: NOT FOUND`);
      allFound = false;
    }
  }

  // If NIDs found, dump context around each one to find the GOT pointer table
  if (allFound) {
    console.log('\n═══ CONTEXT DUMP (around NID locations) ═══');
    console.log('Looking for the GOT pointer table (separate parallel array)\n');
    
    // Get all unique NID addresses (first occurrence of each NID)
    const firstOccurrences = [];
    for (const nid of NIDS_32) {
      if (totalFound[nid].length > 0) {
        firstOccurrences.push(totalFound[nid][0]);
      }
    }
    
    // Dump context around the first NID location
    const refAddr = firstOccurrences[0].physAddr;
    console.log(`Dumping memory around 0x${refAddr.toString(16)} (first NID):`);
    console.log('(Looking for: NID table layout, GOT pointer array, and scelibstub header)');
    console.log('');
    
    const words = await dumpContext(pid, refAddr, 256);
    if (words) {
      for (const w of words) {
        const isNid = NIDS_32.includes(w.val);
        const isPotentialGotPtr = w.val >= 0x00010000 && w.val <= 0x04FFFFFF && w.val !== 0;
        const isFuncAddr = w.val >= 0x30000000 && w.val <= 0x4FFFFFFF;
        
        let marker = '';
        if (isNid) marker = ' <-- NID';
        else if (isFuncAddr) marker = ' <-- FUNC ADDR (libusbd.sprx)';
        else if (isPotentialGotPtr) marker = ' <-- possible GOT ptr';
        
        if (marker || (w.addr >= refAddr - 16 && w.addr <= refAddr + 64)) {
          console.log(`  0x${w.addr.toString(16)}: 0x${w.val.toString(16).padStart(8, '0')}${marker}`);
        }
      }
    }
    
    // Check if all 4 NIDs are sequential (packed NID table)
    const addrs = firstOccurrences.map(f => f.physAddr).sort((a, b) => a - b);
    const minAddr = addrs[0];
    const maxAddr = addrs[addrs.length - 1];
    const span = maxAddr - minAddr;
    
    console.log(`\nNID addresses span: 0x${minAddr.toString(16)} - 0x${maxAddr.toString(16)} (${span} bytes)`);
    
    if (span <= 32) {
      console.log('→ NIDs are tightly packed (parallel array format confirmed)');
      // The GOT pointer table should be at a known offset from the NID table
      // In scelibstub, the header contains func_nidtable_ptr and func_table_ptr
      console.log('\nSearching for the GOT pointer table...');
      console.log('(Checking memory before and after NID table for GOT pointers)');
      console.log('');
      
      // Dump a wider range around the NID table start
      const wideDump = await dumpContext(pid, minAddr, 1024);
      if (wideDump) {
        // Look for addresses that point to .got section (usually 0x008XXXXX or similar)
        const gotPtrCandidates = wideDump.filter(w => 
          w.val >= 0x00100000 && w.val <= 0x03000000 && 
          w.val !== 0 && w.addr >= minAddr - 256 && w.addr <= minAddr + 256
        );
        
        if (gotPtrCandidates.length > 0) {
          console.log('Potential GOT pointer table entries (near NID table):');
          for (const w of gotPtrCandidates.slice(0, 20)) {
            const ptr = w.val;
            // Try to read the value at this pointer (it should be a function address)
            const marker = w.val >= 0x30000000 && w.val <= 0x4FFFFFFF ? ' <-- resolved GOT slot!' : '';
            console.log(`  0x${w.addr.toString(16)}: 0x${w.val.toString(16).padStart(8, '0')}${marker}`);
          }
        }
        
        // Check if any nearby values ARE the resolved function addresses
        const funcAddrs = wideDump.filter(w => 
          w.val >= 0x30000000 && w.val <= 0x4FFFFFFF &&
          w.addr >= minAddr - 256 && w.addr <= minAddr + 256
        );
        if (funcAddrs.length > 0) {
          console.log(`\nResolved function addresses (libusbd.sprx) near NID table:`);
          for (const w of funcAddrs.slice(0, 10)) {
            console.log(`  0x${w.addr.toString(16)}: 0x${w.val.toString(16).padStart(8, '0')}`);
          }
        }
      }
    }
  } else {
    console.log('\n⚠ Not all NIDs found. Dumping low memory for analysis...');
    try {
      const resp = await getmem(pid, 0x00010000, 2048);
      const buf = extractHexFromResponse(resp);
      if (buf) {
        console.log('\n--- First 2048 bytes at 0x00010000 (as 32-bit BE words) ---');
        const wordBuf = Buffer.alloc(4);
        for (let i = 0; i < buf.length; i += 4) {
          if (i + 4 > buf.length) break;
          const wAddr = 0x00010000 + i;
          wordBuf[0] = buf[i]; wordBuf[1] = buf[i+1];
          wordBuf[2] = buf[i+2]; wordBuf[3] = buf[i+3];
          const val = wordBuf.readUInt32BE(0);
          
          const isNid = NIDS_32.includes(val);
          console.log(`  0x${wAddr.toString(8)}: 0x${val.toString(16).padStart(8, '0')}${isNid ? ' <-- NID!' : ''}`);
        }
      }
    } catch (e) {
      console.log(`  Dump failed: ${e.message}`);
    }
  }
}

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function main() {
  let pid = null;
  const pidIdx = process.argv.indexOf('--pid');
  if (pidIdx !== -1 && process.argv[pidIdx + 1]) {
    const pidStr = process.argv[pidIdx + 1];
    pid = parseInt(pidStr.startsWith('0x') ? pidStr : '0x' + pidStr, 16);
  } else {
    pid = 0x1010200;
  }
  
  await scanMemory(pid);
  console.log('\nDone.');
}

main().catch(err => {
  console.error(`Fatal: ${err.message}`);
  process.exit(1);
});
