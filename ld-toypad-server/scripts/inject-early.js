#!/usr/bin/env node
/**
 * inject-early.js — EARLY INJECTION for LEGO Dimensions
 *
 * Polls aggressively (200ms) waiting for game PID to appear.
 * Injects IMMEDIATELY (0s wait) to catch RegisterExtraLdd
 * before the game calls it during boot.
 *
 * USAGE: node inject-early.js --ps3-ip 192.168.0.22
 *        1. Run this script while PS3 is at XMB
 *        2. Launch LEGO Dimensions from XMB
 *        3. Script detects PID and injects in <1 second
 */

const http = require('http');
const fs = require('fs');
const path = require('path');

const PS3_IP = (() => {
  const idx = process.argv.indexOf('--ps3-ip');
  if (idx !== -1 && process.argv[idx + 1]) return process.argv[idx + 1];
  try { return fs.readFileSync(path.join(__dirname, '..', 'ps3-ip.txt'), 'utf8').trim(); }
  catch { return null; }
})();

const SPRX_PATH = '/dev_hdd0/plugins/ldtoypad.sprx';
const POLL_MS = 200;
const PORT = 80;
const VERBOSE = process.argv.includes('--verbose') || process.argv.includes('-v');

function ps3mapi(endpoint, timeoutMs = 3000) {
  return new Promise((resolve, reject) => {
    const req = http.get(`http://${PS3_IP}:${PORT}${endpoint}`, (res) => {
      let data = '';
      res.on('data', c => data += c);
      res.on('end', () => resolve(data));
    });
    req.on('error', reject);
    req.setTimeout(timeoutMs, () => { req.destroy(); reject(new Error('timeout')); });
  });
}

function log(msg) {
  const ts = new Date().toISOString().replace(/T/, ' ').replace(/\..+/, '');
  console.log(`[${ts}] ${msg}`);
}
function vlog(msg) { if (VERBOSE) log(`[DBG] ${msg}`); }

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

function parsePid(resp) {
  if (!resp) return null;
  try {
    const j = JSON.parse(resp);
    const s = (j && j.response) ? String(j.response) : (j && j.result) ? String(j.result) : null;
    if (s) {
      const m = s.match(/0x([0-9a-fA-F]+)/);
      if (m) { const p = parseInt(m[1], 16); if (p > 0 && p !== 0x10005) return p; }
    }
  } catch {}
  const m = resp.match(/0x([0-9a-fA-F]+)/);
  return m ? parseInt(m[1], 16) : null;
}

async function injectSprx(pid) {
  const pidHex = '0x' + pid.toString(16).toUpperCase();

  // Unload previous
  try {
    await ps3mapi(`/ps3mapi.ps3?MODULE%20UNLOAD%20${pidHex}%20${encodeURIComponent(SPRX_PATH)}`, 5000);
    await sleep(500);
  } catch {}

  // Delete stale logs
  for (const f of ['ldtoypad_boot.log', 'ld_hooks.tmp', 'ld_hooks_ready.txt']) {
    try { await ps3mapi(`/dev_hdd0/plugins/${f}?delete`, 2000); } catch {}
  }
  await sleep(200);

  // Load
  for (let attempt = 1; attempt <= 3; attempt++) {
    try {
      const resp = await ps3mapi(
        `/ps3mapi.ps3?MODULE%20LOAD%20${pidHex}%20${encodeURIComponent(SPRX_PATH)}`, 15000);
      log(`  Load response (attempt ${attempt}/3): ${resp.trim()}`);
      if (resp.includes('"OK"') || resp.includes('200')) {
        log('✓ SPRX injection SUCCESSFUL!');
        return true;
      }
    } catch (e) {
      log(`  Load failed (attempt ${attempt}/3): ${e.message}`);
      await sleep(1000);
    }
  }
  return false;
}

async function main() {
  console.log('╔══════════════════════════════════════════════════╗');
  console.log('║   LD-ToyPad Bridge — EARLY INJECTION MODE      ║');
  console.log('╚══════════════════════════════════════════════════╝');
  log(`PS3: ${PS3_IP}  Poll: ${POLL_MS}ms  Mode: AGGRESSIVE EARLY`);
  log('');
  log('Waiting for game to launch from XMB...');
  log('  (Launch LEGO Dimensions NOW if you haven\'t already)');
  log('');

  // Step 1: Wait for game PID to appear (VSH → game transition)
  let lastPid = 0;
  while (true) {
    try {
      const resp = await ps3mapi('/ps3mapi.ps3?PROCESS%20GETCURRENTPID', 2000);
      const pid = parsePid(resp);

      if (pid && pid >= 0x1010000 && pid !== lastPid) {
        const pidHex = '0x' + pid.toString(16).toUpperCase();
        log(`🎯 GAME LAUNCHED! PID=${pidHex} — INJECTING NOW!`);
        
        // Inject IMMEDIATELY — no wait
        const ok = await injectSprx(pid);
        if (ok) {
          log('');
          log('╔══════════════════════════════════════════════════╗');
          log('║  EARLY INJECTION COMPLETE                       ║');
          log('║  Registration hooks are active BEFORE the game  ║');
          log('║  calls RegisterExtraLdd. Watch papertrail for:  ║');
          log('║  *** STOLEN LddOps via 0xXXXX! ***              ║');
          log('╚══════════════════════════════════════════════════╝');
          return;
        } else {
          log('⚠ Injection failed. Check PS3MAPI connectivity.');
          return;
        }
      }

      if (pid && pid < 0x1010000 && lastPid === 0) {
        log('  PS3 is at XMB — waiting for game launch...');
        lastPid = pid;
      }
    } catch (e) {
      // PS3MAPI not ready yet — keep polling
    }

    await sleep(POLL_MS);
  }
}

main().catch(e => { log(`FATAL: ${e.message}`); process.exit(1); });
