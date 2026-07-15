#!/usr/bin/env node

const fs = require('fs');
const https = require('https');
const path = require('path');
const { VirtualToyManager } = require('../virtual-toys');
const {
  imagesDir,
  loadImageManifest,
  saveImageManifest,
  getImagePathForItemId
} = require('../image-manifest');

const API_BASE = 'https://lego-dimensions.fandom.com/api.php';
const USER_AGENT = 'ld-toypad-server-image-sync/1.0 (local use; rate limited; respects robots)';
const DEFAULT_DELAY_MS = 1500;
const DEFAULT_IMAGE_DELAY_MS = 250;
const DEFAULT_RETRIES = 3;
const IMAGE_NAME_PATTERN = /\.(png|jpe?g|webp)$/i;
const TITLE_OVERRIDES = {
  'acu': 'ACU Trooper',
  'bat-girl': 'Batgirl',
  'beetle-juice': 'Betelgeuse',
  'cloud-cukko-car': 'Cloud Cuckoo Car',
  'delorean': 'DeLorean Time Machine',
  'e-t': 'E.T.',
  'gandalf': 'Gandalf the Grey',
  'gyro-sphere': 'Gyrosphere',
  'k9': 'K-9',
  'marty-mcfly': 'Marty McFly',
  'newt': 'Newt Scamander',
  'ninja-copter': 'NinjaCopter',
  'owen': 'Owen Grady',
  'senseiwu': 'Sensei Wu',
  'smeagol': 'Gollum',
  'superwoman': 'Wonder Woman',
  'tina': 'Tina Goldstein'
};

function parseArgs(argv) {
  const options = {
    delayMs: DEFAULT_DELAY_MS,
    imageDelayMs: DEFAULT_IMAGE_DELAY_MS,
    retries: DEFAULT_RETRIES,
    limit: null,
    force: false,
    dryRun: false,
    itemIds: null
  };

  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    switch (arg) {
      case '--delay-ms':
        options.delayMs = Number(argv[++index] || DEFAULT_DELAY_MS);
        break;
      case '--image-delay-ms':
        options.imageDelayMs = Number(argv[++index] || DEFAULT_IMAGE_DELAY_MS);
        break;
      case '--retries':
        options.retries = Number(argv[++index] || DEFAULT_RETRIES);
        break;
      case '--limit':
        options.limit = Number(argv[++index] || 0) || null;
        break;
      case '--item-ids':
        options.itemIds = String(argv[++index] || '')
          .split(',')
          .map((value) => Number(value.trim()))
          .filter((value) => Number.isInteger(value));
        break;
      case '--force':
        options.force = true;
        break;
      case '--dry-run':
        options.dryRun = true;
        break;
      default:
        break;
    }
  }

  return options;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function buildApiUrl(params) {
  const url = new URL(API_BASE);
  Object.entries(params).forEach(([key, value]) => {
    if (value !== undefined && value !== null && value !== '') {
      url.searchParams.set(key, value);
    }
  });
  return url;
}

function requestBuffer(url, retriesLeft, attempt = 0) {
  return new Promise((resolve, reject) => {
    const req = https.request(url, {
      method: 'GET',
      headers: {
        'User-Agent': USER_AGENT,
        'Accept': '*/*'
      }
    }, async (res) => {
      const chunks = [];
      res.on('data', (chunk) => chunks.push(chunk));
      res.on('end', async () => {
        const body = Buffer.concat(chunks);
        const statusCode = res.statusCode || 0;

        if (statusCode >= 200 && statusCode < 300) {
          resolve({
            statusCode,
            headers: res.headers,
            body
          });
          return;
        }

        if ((statusCode === 429 || statusCode >= 500) && retriesLeft > 0) {
          const backoffMs = Math.max(DEFAULT_DELAY_MS, (attempt + 1) * DEFAULT_DELAY_MS);
          console.warn(`[ImageSync] HTTP ${statusCode} from ${url}. Backing off for ${backoffMs}ms.`);
          await sleep(backoffMs);
          try {
            resolve(await requestBuffer(url, retriesLeft - 1, attempt + 1));
          } catch (err) {
            reject(err);
          }
          return;
        }

        reject(new Error(`HTTP ${statusCode} for ${url}`));
      });
    });

    req.on('error', async (err) => {
      if (retriesLeft > 0) {
        const backoffMs = Math.max(DEFAULT_DELAY_MS, (attempt + 1) * DEFAULT_DELAY_MS);
        console.warn(`[ImageSync] Request error for ${url}: ${err.message}. Retrying in ${backoffMs}ms.`);
        await sleep(backoffMs);
        try {
          resolve(await requestBuffer(url, retriesLeft - 1, attempt + 1));
        } catch (retryErr) {
          reject(retryErr);
        }
        return;
      }
      reject(err);
    });

    req.end();
  });
}

async function requestJson(url, retries) {
  const response = await requestBuffer(url, retries);
  return JSON.parse(response.body.toString('utf8'));
}

function sanitizeFileTitle(fileTitle) {
  return String(fileTitle || '').replace(/^File:/i, '').trim();
}

function chooseBestImage(imageEntries) {
  const candidates = (imageEntries || [])
    .map((entry) => sanitizeFileTitle(entry.title || entry))
    .filter((title) => IMAGE_NAME_PATTERN.test(title));

  if (candidates.length === 0) {
    return null;
  }

  candidates.sort((left, right) => {
    const leftExt = path.extname(left).toLowerCase();
    const rightExt = path.extname(right).toLowerCase();
    const priority = ['.png', '.webp', '.jpg', '.jpeg'];
    return priority.indexOf(leftExt) - priority.indexOf(rightExt);
  });

  return candidates[0];
}

function buildTitleCandidates(toy) {
  const candidates = new Set();
  const cleanName = String(toy.name || '').replace(/^\*\s*/, '').trim();
  const override = TITLE_OVERRIDES[toy.id];

  if (override) {
    candidates.add(override);
  }
  if (cleanName) {
    candidates.add(cleanName);
  }
  if (cleanName.includes("'")) {
    candidates.add(cleanName.replace(/'/g, ''));
  }
  if (cleanName.includes('-')) {
    candidates.add(cleanName.replace(/-/g, ' '));
  }
  if (cleanName.includes(' ')) {
    candidates.add(cleanName.replace(/\s+/g, '_'));
  }

  return Array.from(candidates);
}

function getFirstPage(payload) {
  const pages = payload && payload.query && payload.query.pages;
  if (!pages) {
    return null;
  }
  const values = Object.values(pages);
  return values.length > 0 ? values[0] : null;
}

async function queryPageImages(title, retries) {
  const url = buildApiUrl({
    action: 'query',
    titles: title,
    prop: 'images',
    imlimit: 10,
    redirects: 1,
    format: 'json'
  });
  const payload = await requestJson(url, retries);
  const page = getFirstPage(payload);
  if (!page || page.missing !== undefined) {
    return null;
  }
  return {
    title: page.title,
    imageFile: chooseBestImage(page.images)
  };
}

async function searchPageTitles(searchTerm, retries) {
  const url = buildApiUrl({
    action: 'query',
    list: 'search',
    srsearch: `"${searchTerm}"`,
    srnamespace: 0,
    srlimit: 3,
    format: 'json'
  });
  const payload = await requestJson(url, retries);
  return (payload.query && payload.query.search ? payload.query.search : []).map((entry) => entry.title);
}

async function resolveImageUrl(fileTitle, retries) {
  const url = buildApiUrl({
    action: 'query',
    titles: `File:${sanitizeFileTitle(fileTitle)}`,
    prop: 'imageinfo',
    iiprop: 'url',
    format: 'json'
  });
  const payload = await requestJson(url, retries);
  const page = getFirstPage(payload);
  const imageinfo = page && page.imageinfo && page.imageinfo[0];
  if (!imageinfo || !imageinfo.url) {
    return null;
  }
  return imageinfo.url;
}

async function downloadFile(url, filePath, retries) {
  const response = await requestBuffer(url, retries);
  fs.writeFileSync(filePath, response.body);
}

async function findImageForToy(toy, options) {
  const attemptedTitles = [];
  for (const candidate of buildTitleCandidates(toy)) {
    attemptedTitles.push(candidate);
    const page = await queryPageImages(candidate, options.retries);
    if (page && page.imageFile) {
      return {
        pageTitle: page.title,
        fileTitle: page.imageFile,
        attemptedTitles
      };
    }
    await sleep(options.delayMs);
  }

  const searchMatches = await searchPageTitles(toy.name.replace(/^\*\s*/, '').trim(), options.retries);
  await sleep(options.delayMs);

  for (const match of searchMatches) {
    attemptedTitles.push(`search:${match}`);
    const page = await queryPageImages(match, options.retries);
    if (page && page.imageFile) {
      return {
        pageTitle: page.title,
        fileTitle: page.imageFile,
        attemptedTitles
      };
    }
    await sleep(options.delayMs);
  }

  return {
    pageTitle: null,
    fileTitle: null,
    attemptedTitles
  };
}

function filterToys(toys, options) {
  let filtered = toys;
  if (options.itemIds && options.itemIds.length > 0) {
    const itemIdSet = new Set(options.itemIds);
    filtered = filtered.filter((toy) => itemIdSet.has(toy.itemId));
  }
  if (options.limit) {
    filtered = filtered.slice(0, options.limit);
  }
  return filtered;
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  const toys = filterToys(VirtualToyManager.listAvailableToys(), options);
  const manifest = loadImageManifest();

  fs.mkdirSync(imagesDir, { recursive: true });

  console.log(`[ImageSync] Starting import for ${toys.length} entries.`);
  console.log(`[ImageSync] Delay ${options.delayMs}ms between API calls, ${options.imageDelayMs}ms after downloads.`);

  for (const toy of toys) {
    const manifestEntry = manifest[String(toy.itemId)];
    const existingPath = getImagePathForItemId(toy.itemId, manifest);
    if (!options.force && manifestEntry && existingPath) {
      const existingFile = path.join(imagesDir, path.basename(existingPath));
      if (fs.existsSync(existingFile)) {
        console.log(`[ImageSync] Skipping ${toy.name} (#${toy.itemId}) - already cached.`);
        continue;
      }
    }

    console.log(`[ImageSync] Resolving ${toy.name} (#${toy.itemId})...`);
    const result = await findImageForToy(toy, options);

    if (!result.fileTitle) {
      manifest[String(toy.itemId)] = {
        itemId: toy.itemId,
        name: toy.name,
        status: 'missing',
        attemptedTitles: result.attemptedTitles,
        updatedAt: new Date().toISOString()
      };
      saveImageManifest(manifest);
      console.warn(`[ImageSync] No image found for ${toy.name} (#${toy.itemId}).`);
      continue;
    }

    const sourceUrl = await resolveImageUrl(result.fileTitle, options.retries);
    await sleep(options.delayMs);
    if (!sourceUrl) {
      manifest[String(toy.itemId)] = {
        itemId: toy.itemId,
        name: toy.name,
        status: 'missing-url',
        sourcePageTitle: result.pageTitle,
        sourceFileTitle: result.fileTitle,
        attemptedTitles: result.attemptedTitles,
        updatedAt: new Date().toISOString()
      };
      saveImageManifest(manifest);
      console.warn(`[ImageSync] Image URL missing for ${toy.name} (#${toy.itemId}).`);
      continue;
    }

    const extension = path.extname(new URL(sourceUrl).pathname) || path.extname(result.fileTitle) || '.png';
    const savedFileName = `${toy.itemId}${extension.toLowerCase()}`;
    const destination = path.join(imagesDir, savedFileName);

    if (options.dryRun) {
      console.log(`[ImageSync] Dry run: would download ${sourceUrl} -> ${savedFileName}`);
    } else {
      await downloadFile(sourceUrl, destination, options.retries);
      await sleep(options.imageDelayMs);
    }

    manifest[String(toy.itemId)] = {
      itemId: toy.itemId,
      name: toy.name,
      status: options.dryRun ? 'dry-run' : 'downloaded',
      sourcePageTitle: result.pageTitle,
      sourceFileTitle: result.fileTitle,
      sourceUrl,
      savedFileName,
      attemptedTitles: result.attemptedTitles,
      updatedAt: new Date().toISOString()
    };
    saveImageManifest(manifest);
    console.log(`[ImageSync] ${options.dryRun ? 'Planned' : 'Saved'} ${savedFileName} for ${toy.name}.`);
  }

  console.log('[ImageSync] Complete.');
}

main().catch((err) => {
  console.error(`[ImageSync] Failed: ${err.message}`);
  process.exitCode = 1;
});
