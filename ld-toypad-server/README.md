# LD-ToyPad Network Bridge Server

Network bridge for LEGO Dimensions Toy Pad emulation on PS3 CFW.

## Architecture

The server runs on a development PC and communicates with the PS3 via UDP.

```
PS3 (.sprx plugin) ←── UDP ──→ PC (Node.js server)
```

The PS3 plugin hooks sys_usbd and routes Toy Pad USB traffic over UDP.
This server responds with appropriate Toy Pad USB HID data.

## Quick Start

```bash
# Install dependencies
npm install

# Start the server
node server.js

# With options
node server.js --port 28472 --verbose

# Enable dedicated SPRX debug log listener (default: 28473)
node server.js --port 28472 --debug-port 28473 --verbose

# Start with browser UI on custom HTTP port
node server.js --port 28472 --debug-port 28473 --http-port 8080 --verbose
```

## Command Line Options

| Flag | Alias | Default | Description |
|------|-------|---------|-------------|
| `--port` | `-p` | 28472 | UDP port to listen on |
| `--host` | `-H` | 0.0.0.0 | Host address to bind to |
| `--ps3-ip` | `-P` | auto | PS3 IP address (for targeted responses) |
| `--verbose` | `-v` | false | Verbose packet logging |
| `--debug-port` |  | 28473 | UDP port for SPRX debug log stream |
| `--http-port` |  | 8080 | HTTP port for browser UI/API |
| `--delay` | `-d` | 0 | Artificial delay in ms (for testing) |

## Remote SPRX Logs

The SPRX can stream runtime logs over UDP to this server on the debug port
(default `28473`). When packets are received, logs are printed with a
`[SPRX ip:port]` prefix.

## Browser UI

Open `http://localhost:8080` (or your configured `--http-port`) while the
server is running.

The UI provides:
- Current zone state view (LEFT/CENTER/RIGHT)
- Place toy control with tabbed catalog browsing
- Type-first navigation (Characters, Vehicles / Items)
- Franchise tabs within each type
- Search filtering inside the current tab scope
- Card-based selection with optional thumbnail support
- Remove toy buttons per zone
- Live status polling (client, uptime, ports)

## Interactive Commands

Once running, type commands in the server console:

| Command | Description |
|---------|-------------|
| `place <zone> <toyId>` | Place a toy on a zone (0=LEFT, 1=CENTER, 2=RIGHT) |
| `remove <zone>` | Remove a toy from a zone |
| `list` | List available toys and pad state |
| `status` | Show server status and uptime |
| `quit` / `exit` | Shutdown the server |

## Catalog Data

The server imports the upstream LD-ToyPad character and token maps and exposes
322 usable catalog entries after filtering placeholders and unreleased/test
records.

The browser groups this catalog by:
- Type first (`character`, `token`)
- Franchise/world second
- Optional text filter within the active scope

## Optional Images

The original LD-ToyPad project supports user-provided images and does not ship
copyrighted thumbnails. This server follows the same model.

To add thumbnails:
- Create PNG files in `ld-toypad-server/images/`
- Name each file as `(itemId).png`
- Example: `1.png` for Batman, `1006.png` for Batmobile

The browser will request images from `/images/<itemId>.png` and automatically
fall back to initials if no file exists.

## Wiki Image Sync

This repo includes a respectful image importer that uses the LEGO Dimensions
Wiki MediaWiki API instead of scraping rendered HTML.

Run it with:

```bash
npm run images:sync
```

Useful options:

```bash
# Small sample run
npm run images:sync -- --limit 5

# Only specific numeric item IDs
npm run images:sync -- --item-ids 1,48,55,1006

# Dry run without downloading files
npm run images:sync -- --limit 10 --dry-run

# Force re-download even if manifest entries exist
npm run images:sync -- --force
```

Behavior:
- uses the wiki `api.php` endpoints allowed by `robots.txt`
- rate limits requests by default (`1500ms` between API calls)
- backs off and retries on `429` and `5xx`
- caches results in `images/manifest.json`
- saves files into `images/` and exposes them through `/images/<file>`

The importer is intentionally conservative. It does not attempt stealth,
bypass, or anti-ban evasion.

## Network Protocol

### Packet Format (PS3 -> PC) - 8 bytes

| Offset | Size | Description |
|--------|------|-------------|
| 0 | 1 | Packet type (0x01=Poll, 0x02=ReadTag, 0x03=WriteTag) |
| 1 | 1 | Zone (0=LEFT, 1=CENTER, 2=RIGHT) |
| 2 | 1 | Sequence number |
| 3-7 | 5 | Reserved / payload |

### Packet Format (PC -> PS3) - 80 bytes

| Offset | Size | Description |
|--------|------|-------------|
| 0 | 1 | Status (0x00=OK, 0x01=NoTag, 0xFF=Error) |
| 1 | 1 | Zone (echoed) |
| 2 | 1 | Sequence number (echoed) |
| 3-79 | 77 | Response data (USB HID report) |

## Development

### Updating Catalog Data

Catalog generation is handled in `virtual-toys.js` from the vendored upstream
JSON files in `data/`.

If you need to refresh the catalog source:
- update `data/charactermap.json`
- update `data/tokenmap.json`
- keep the placeholder/unreleased filters in `virtual-toys.js` aligned

### Protocol Reference

See `toypad-protocol.js` for the full Toy Pad USB HID protocol implementation.
