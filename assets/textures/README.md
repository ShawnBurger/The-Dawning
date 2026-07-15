# Texture Assets

Runtime texture assets for The Dawning V3.

The current Layer 4 path prefers PNG textures from this folder via WIC, then
falls back to DDS. If the starter albedo DDS files are missing from the
executable directory, the app writes generated checker DDS files as a fallback.
PNG/WIC textures receive CPU-generated mip chains at load time, and DDS files
upload all declared mip levels.

Recognized demo texture names:

- `ground_grid.png` / `ground_grid.dds`
- `blue_panels.png` / `blue_panels.dds`
- `ground_normal.png` / `ground_normal.dds`
- `cube_normal.png` / `cube_normal.dds`

If normal maps are absent, the demo creates procedural wave normal textures.
