const http = require('http');
const PS3 = '192.168.0.22';
const PID = '0x1010200';

function ps3mapi(endpoint) {
  return new Promise((resolve, reject) => {
    http.get('http://' + PS3 + endpoint, (res) => {
      let d = ''; res.on('data', c => d += c); res.on('end', () => resolve(d));
    }).on('error', reject).setTimeout(8000, function() { this.destroy(); reject(new Error('timeout')); });
  });
}

async function memRead(addr, size) {
  const hex = '0x' + addr.toString(16).toUpperCase();
  const sz = '0x' + size.toString(16).toUpperCase();
  return await ps3mapi('/ps3mapi.ps3?MEMORY%20GET%20' + PID + '%20' + hex + '%20' + sz);
}

async function memWrite(addr, hexBytes) {
  const hex = '0x' + addr.toString(16).toUpperCase();
  return await ps3mapi('/ps3mapi.ps3?MEMORY%20SET%20' + PID + '%20' + hex + '%20' + hexBytes);
}

(async () => {
  // Try searching for the PID bytes 0241 and also try reading known EBOOT address
  console.log('Trying different search strategies...');

  // First: verify EBOOT base is readable at 0x00010000
  try {
    const r = await memRead(0x00010000, 16);
    console.log('0x00010000: ' + r.trim());
  } catch(e) { console.log('0x00010000 unreadable'); }

  // Try broader search: find 0241 or 38A0 patterns
  let found = false;
  for (let page = 0x00010000; page < 0x02000000 && !found; page += 0x10000) {
    try {
      const resp = await memRead(page, 0x10000);
      const m = resp.match(/"response": "([0-9A-Fa-f]+)"/);
      if (!m) continue;
      const data = m[1].toUpperCase();
      
      // Look for li r5 pattern: 38A0xxxx
      for (let i = 0; i < data.length - 7; i += 2) {
        if (data.substring(i, i+4) === '38A0') {
          const addr = page + i/2;
          const val = data.substring(i+4, i+8);
          console.log('38A0xxxx at 0x' + addr.toString(16).toUpperCase() + ' val=0x' + val);
          // Check if next instruction looks like branch to RegisterExtraLdd
          if (addr < 0x02000000) found = true;
        }
      }
    } catch(e) {}
  }
  console.log('done');
})().catch(e => console.error(e));
