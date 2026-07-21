# Runtime assets

This directory contains production-ready cooked content loaded by the engine.
Large `.tdmodel` files are tracked through Git LFS; run `git lfs pull` after
cloning before building.

`corridor_section.tdmodel` was deterministically cooked with
`TheDawningAssetCompiler` from the Meshy source described by
`../generated/corridor_section_05445d3a804f4d42/manifest.json`.

- source GLB SHA-256: `a3c323749994598540060cd10e7b8f8d5ad7ca1e7487b47a38c561704fbd973d`
- cooked SHA-256: `ff03a907e3c09e64bf4d56434e879955abcf0810a9206cdaff69a17088ec6375`
- source bytes: `8,896,484`
- cooked bytes: `9,533,104`
- primitives: `1`
- vertices: `15,562`
- indices: `57,579`

The source GLB stays in the content-addressed generated cache and is ignored by
Git. Runtime and smoke tests consume only the cooked artifact; neither invokes
Meshy or reads `MESHY_API_KEY`.

From a configured checkout containing that cached source, reproduce the runtime
artifact with:

```powershell
.\build\Release\TheDawningAssetCompiler.exe `
  .\assets\generated\corridor_section_05445d3a804f4d42\model.glb `
  .\assets\runtime\corridor_section.tdmodel
```

The command must report the source hash above, and the resulting file must match
the cooked hash before it is staged.
