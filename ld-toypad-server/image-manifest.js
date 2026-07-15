const fs = require('fs');
const path = require('path');

const imagesDir = path.join(__dirname, 'images');
const manifestPath = path.join(imagesDir, 'manifest.json');

function loadImageManifest() {
  try {
    return JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  } catch (err) {
    return {};
  }
}

function saveImageManifest(manifest) {
  fs.mkdirSync(imagesDir, { recursive: true });
  fs.writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`, 'utf8');
}

function getImagePathForItemId(itemId, manifest = loadImageManifest()) {
  const entry = manifest[String(itemId)];
  if (!entry || !entry.savedFileName) {
    return null;
  }
  return `/images/${entry.savedFileName}`;
}

module.exports = {
  imagesDir,
  manifestPath,
  loadImageManifest,
  saveImageManifest,
  getImagePathForItemId
};
