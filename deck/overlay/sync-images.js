#!/usr/bin/env node
// sync-images.js — standalone toy image downloader for Berny23's emulator
// Reads server/json/*.json, downloads thumbnails from Fandom wiki to server/images/
// Usage: node sync-images.js

const fs = require('fs');
const https = require('https');
const path = require('path');

const IMAGES_DIR = path.join(__dirname, 'server', 'images');
const CHAR_MAP = path.join(__dirname, 'server', 'json', 'charactermap.json');
const TOKEN_MAP = path.join(__dirname, 'server', 'json', 'tokenmap.json');
const DELAY_MS = 1500; // be nice to the wiki
const API = 'https://lego-dimensions.fandom.com/api.php';

function fetch(url) {
  return new Promise((resolve, reject) => {
    https.get(url, { headers: { 'User-Agent': 'ld-toypad-sync/1.0' } }, res => {
      let data = '';
      res.on('data', c => data += c);
      res.on('end', () => resolve(data));
    }).on('error', reject);
  });
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

async function getImageUrl(pageTitle) {
  const params = new URLSearchParams({
    action: 'query', format: 'json', prop: 'pageimages',
    piprop: 'thumbnail', pithumbsize: '200', titles: pageTitle
  });
  const data = await fetch(`${API}?${params}`);
  try {
    const pages = JSON.parse(data).query.pages;
    const page = Object.values(pages)[0];
    return page?.thumbnail?.source || null;
  } catch { return null; }
}

async function downloadImage(url, filepath) {
  if (fs.existsSync(filepath)) return 'skipped';
  return new Promise((resolve, reject) => {
    https.get(url, { headers: { 'User-Agent': 'ld-toypad-sync/1.0' } }, res => {
      if (res.statusCode === 301 || res.statusCode === 302) {
        return downloadImage(res.headers.location, filepath).then(resolve).catch(reject);
      }
      const file = fs.createWriteStream(filepath);
      res.pipe(file);
      file.on('finish', () => { file.close(); resolve('ok'); });
    }).on('error', reject);
  });
}

async function main() {
  if (!fs.existsSync(CHAR_MAP) || !fs.existsSync(TOKEN_MAP)) {
    console.log('Character/token maps not found. Skipping image sync.');
    return;
  }

  fs.mkdirSync(IMAGES_DIR, { recursive: true });

  const chars = JSON.parse(fs.readFileSync(CHAR_MAP, 'utf8')).filter(c => c.name && c.name !== 'Unknown');
  const tokens = JSON.parse(fs.readFileSync(TOKEN_MAP, 'utf8')).filter(t => t.name && t.name !== 'Unknown');
  const all = [...chars, ...tokens];
  let ok = 0, skipped = 0, failed = 0;

  console.log(`Syncing images for ${all.length} toys (${DELAY_MS}ms delay)...`);

  for (const toy of all) {
    const filepath = path.join(IMAGES_DIR, `${toy.id}.png`);
    if (fs.existsSync(filepath)) { skipped++; continue; }

    const title = toy.name.replace(/ /g, '_');
    const url = await getImageUrl(title);
    if (url) {
      const result = await downloadImage(url, filepath).catch(() => 'fail');
      if (result === 'ok') ok++;
      else failed++;
    } else {
      failed++;
    }
    process.stdout.write(`\r  ${ok} ok, ${skipped} skipped, ${failed} fail — ${toy.name}`);
    await sleep(DELAY_MS);
  }

  console.log(`\nDone. ${ok} new, ${skipped} existing, ${failed} failed.`);
}

main().catch(e => { console.error(e); process.exit(1); });
