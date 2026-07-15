# Optional Toy Images

Drop PNG thumbnails in this folder using the numeric LEGO Dimensions item ID.

Examples:
- `1.png` -> Batman
- `2.png` -> Gandalf
- `3.png` -> Wyldstyle
- `1006.png` -> Batmobile

The browser UI requests images at `/images/<itemId>.png`.
If an image is missing, the UI falls back to initials automatically.

This folder is intentionally empty by default. The upstream LD-ToyPad project
uses user-provided images and does not bundle copyrighted artwork.

You can also populate this folder automatically with the respectful wiki importer:

```bash
npm run images:sync
```

The importer writes a cache manifest to `manifest.json` and may save files with
extensions other than `.png` if the source asset is not a PNG. The server API
will expose the correct path for the browser UI.
