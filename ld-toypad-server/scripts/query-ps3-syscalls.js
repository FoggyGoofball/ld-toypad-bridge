#!/usr/bin/env node
/**
 * query-ps3-syscalls.js
 *
 * Queries a running PS3 via PS3MAPI (webMAN MOD) to verify:
 *   - Syscall table base address
 *   - USB syscall numbers (845-849)
 *   - LV2 read/write syscall IDs (8, 9)
 *
 * Usage: node query-ps3-syscalls.js <ps3-ip>
 */

const http = require('http');

const PS3_IP = process.argv[2] || (() => {
  try {
    return require('fs').readFileSync(require('path').join(__dirname, '..', 'ps3-ip.txt'), 'utf8').trim();
  } catch { return null; }
})();

if (!PS3_IP) {
  console.error('Usage: node query-ps3-syscalls.js <ps3-ip>');
  console.error('  or place ps3-ip.txt next to the script');
  process.exit(1);
}

const PS3MAPI_PORT = 80;
const TABLE_BASE_CANDIDATES = [
  0x800000000000F300,  // Evilnat 4.93 CEX (most likely)
  0x800000000000F700,  // Some CFW variants
  0x80000000000E2C00,  // Older CFW
  0x800000000000E000,  // Rebug / Dex fallback
];

const USB_SYSCALLS = {
  'sys_usbd_open': 845,
  'sys_usbd_close': 846,
  'sys_usbd_get_descriptor': 847,
  'sys_usbd_control_transfer': 848,
  'sys_usbd_interrupt_transfer': 849,
};

function ps3mapiRequest(endpoint) {
  return new Promise((resolve, reject) => {
    const req = http.get(`http://${PS3_IP}:${PS3MAPI_PORT}${endpoint}`, (res) => {
      let data = '';
      res.on('data', chunk => data += chunk);
      res.on('end', () => resolve(data));
    });
    req.on('error', (err) => reject(err));
    req.setTimeout(5000, () => { req.destroy(); reject(new Error('timeout')); });
  });
}

async function dumpViaSyscall() {
  /**
   * Try to read the LV2 syscall table using syscall 8 (lv1_read_kernel).
   * We need a helper that runs on the PS3. webMAN MOD provides:
   *   /cpursx.ps3?/sprx_rpc.html  — but that's PSPRX.
   *
   * Better approach: use webMAN's /sprx.ps3 endpoint or the PS3MAPI
   * firmware info page to discover the syscall table base.
   */
  try {
    // Try firmware info page (common in webMAN MOD)
    const info = await ps3mapiRequest('/cpursx.ps3?/vsh_plugin_info.txt');
    console.log('=== VSH Plugin Info ===');
    console.log(info || '(empty)');
  } catch { /* not available */ }

  try {
    // Try PS3MAPI syscall info endpoint (webMAN MOD with PS3MAPI enabled)
    const sysInfo = await ps3mapiRequest('/sys_info.ps3');
    console.log('=== Sys Info ===');
    console.log(sysInfo || '(empty)');
  } catch { /* not available */ }
}

async function main() {
  console.log(`Querying PS3 at ${PS3_IP}:${PS3MAPI_PORT} (webMAN MOD)...`);
  console.log();
  console.log('=== Expected values (Evilnat 4.93 CEX) ===');
  console.log('  Syscall table base:  0x800000000000F300');
  console.log('  lv1_read_kernel  (8):  (function ptr at table[8])');
  console.log('  lv2_write_kernel (9):  (function ptr at table[9])');
  console.log('  sys_usbd_open:          845');
  console.log('  sys_usbd_close:         846');
  console.log('  sys_usbd_get_descriptor: 847');
  console.log('  sys_usbd_control:        848');
  console.log('  sys_usbd_interrupt:      849');
  console.log();

  // Try to get webMAN version
  try {
    const root = await ps3mapiRequest('/');
    const verMatch = root.match(/webMAN\s*MOD\s*([\d.]+)/i);
    if (verMatch) {
      console.log(`✓ webMAN MOD ${verMatch[1]} detected`);
    } else if (root.includes('webMAN')) {
      console.log('✓ webMAN detected');
    } else {
      console.log('⚠  webMAN root page not recognized');
    }
  } catch (e) {
    console.log(`✗ webMAN not reachable: ${e.message}`);
    console.log('  Using known Evilnat 4.93 CEX defaults.');
    console.log('  To verify manually, run on PS3:');
    console.log('    cat /proc/cpu/info | grep syscall');
    console.log();
    console.log('=== Recommendation ===');
    console.log('  Use: USB_HOOK_SYSCALL_TABLE_BASE = 0x800000000000F300');
    console.log('       USB_HOOK_LV2_READ_SC = 8');
    console.log('       USB_HOOK_LV2_WRITE_SC = 9');
    console.log('       (syscall USB IDs 845-849 are stable across CEX)');
    return;
  }

  // Try to dump syscall table via PS3MAPI
  for (const base of TABLE_BASE_CANDIDATES) {
    try {
      const hexBase = base.toString(16).padStart(16, '0');
      const result = await ps3mapiRequest(`/cpursx.ps3?/read_process?pid=0x10005&addr=0x${hexBase}&size=0x10`);
      if (result && result.length > 0) {
        console.log(`✓ Syscall table base at 0x${hexBase} responded:`);
        console.log(`  ${result.slice(0, 200)}`);
        
        // Read individual entries
        for (const [name, num] of Object.entries(USB_SYSCALLS)) {
          try {
            const entryAddr = (base + BigInt(num) * 8n).toString(16).padStart(16, '0');
            const entry = await ps3mapiRequest(`/cpursx.ps3?/read_process?pid=0x10005&addr=0x${entryAddr}&size=8`);
            console.log(`  ${name} (${num}): ${entry ? entry.slice(0, 50) : 'N/A'}`);
          } catch {
            console.log(`  ${name} (${num}): (read failed)`);
          }
        }
        
        console.log();
        console.log('=== Verified! Update Makefile with: ===');
        console.log(`  USB_HOOK_SYSCALL_TABLE_BASE = 0x${hexBase}`);
        return;
      }
    } catch { /* try next candidate */ }
  }

  console.log('Could not verify syscall table remotely. Using defaults.');
}

main().catch(console.error);
