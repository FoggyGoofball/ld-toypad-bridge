// sync-api.js — on-demand image sync endpoint for Berny23's server
// Injected by run-ui.sh — provides POST /api/sync-images
const { spawn } = require('child_process');
const path = require('path');

module.exports = function(app) {
  app.post('/api/sync-images', (req, res) => {
    res.setHeader('Content-Type', 'application/x-ndjson');
    res.setHeader('Transfer-Encoding', 'chunked');

    const scriptPath = path.join(__dirname, 'sync-images.js');
    const child = spawn('node', [scriptPath], {
      cwd: __dirname,
      env: { ...process.env }
    });

    child.stdout.on('data', (data) => {
      const lines = data.toString().split('\n').filter(l => l.trim());
      for (const line of lines) {
        // Parse progress lines like "  5 ok, 10 skipped, 2 fail — Batman"
        const m = line.match(/(\d+)\s+ok.*?(\d+)\s+skipped.*?(\d+)\s+fail/i);
        if (m) {
          const ok = parseInt(m[1]), skipped = parseInt(m[2]), fail = parseInt(m[3]);
          const total = ok + skipped + fail;
          res.write(JSON.stringify({ ok, skipped, fail, total, pct: total > 0 ? Math.round((ok + skipped) / (ok + skipped + fail + 1) * 100) : 0, msg: line.trim() }) + '\n');
        } else {
          res.write(JSON.stringify({ msg: line.trim() }) + '\n');
        }
      }
    });

    child.stderr.on('data', (data) => {
      res.write(JSON.stringify({ msg: data.toString().trim() }) + '\n');
    });

    child.on('close', (code) => {
      res.write(JSON.stringify({ status: 'done', code }) + '\n');
      res.end();
    });

    child.on('error', (err) => {
      res.write(JSON.stringify({ status: 'error', msg: err.message }) + '\n');
      res.end();
    });

    req.on('close', () => { child.kill(); });
  });
};
