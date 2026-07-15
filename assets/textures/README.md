# Texture Assets

Runtime texture assets for The Dawning V3.

The current Layer 4 path prefers PNG albedo textures from this folder via WIC,
then falls back to DDS. If the starter DDS files are missing from the executable
directory, the app writes generated checker DDS files as a fallback. PNG/WIC
textures receive CPU-generated mip chains at load time, and DDS files upload all
declared mip levels.
