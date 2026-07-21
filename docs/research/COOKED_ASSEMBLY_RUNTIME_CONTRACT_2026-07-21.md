# Cooked Assembly Runtime Contract

Date: 2026-07-21
Status: implemented by WS-022
Format extension: `.tdassembly`

## Purpose

The `.tdasset.json` manifest is the reviewed source contract for a boardable
ship or structure. It is intentionally expressive and human-readable, but the
runtime must not parse JSON, infer relationships from names, or accept a partial
interior graph. WS-022 therefore adds a deterministic offline boundary:

```text
validated .tdasset.json
        |
        v
compile_asset_manifest.py
        |
        v
versioned + authenticated .tdassembly
        |
        v
LoadCookedAssemblyFile/Memory
        |
        v
immutable, index-resolved assembly graph
```

This stage does not spawn entities, load meshes, build navigation, simulate
pressure, animate doors, or touch the GPU. Those operations belong to later
systems and may begin only after this loader returns a complete graph.

## Operator Workflow

Validate and compile the reference contract:

```powershell
python tools/validate_asset_manifest.py assets/manifests/reference_ship.tdasset.json
python tools/compile_asset_manifest.py `
  assets/manifests/reference_ship.tdasset.json `
  build/reference_ship.tdassembly
build\Debug\TheDawningAssemblyInspector.exe build\reference_ship.tdassembly
```

The compiler validates before opening an output, writes a temporary file in the
destination directory, flushes and verifies its bytes, and then replaces the
destination atomically. The source and destination may not be the same path or
hard-linked file.

## Header

All integer and floating-point fields are little-endian. The fixed header is 72
bytes:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | magic `TDASMB\0\0` |
| 8 | 4 | format version, currently 1 |
| 12 | 4 | header byte count, exactly 72 |
| 16 | 8 | complete file byte count |
| 24 | 8 | payload byte count |
| 32 | 32 | SHA-256 of the complete payload |
| 64 | 8 | reserved, must be zero |

The loader rejects unknown versions. New versions require an explicit reader;
there is no best-effort fallback.

## Payload

The payload starts with the SHA-256 of canonical source-manifest semantics,
followed by schema version, asset kind, required interior-contract flags,
identity, assembly revision, and dimensional constraints. It then stores these
tables in stable ID order:

1. provenance identities and request hashes;
2. modules, transforms, collision references, and ordered LOD chains;
3. oriented sockets and owning module indices;
4. pressure/navigation/walkable zones and owning module indices;
5. portals with resolved zone, socket, and closure indices;
6. interactions with states and resolved module/socket/part/portal indices;
7. addressable moving parts with pivots, axes, travel, and reciprocal indices;
8. entry and required-reachable zone indices.

Strings use an unsigned 32-bit byte length followed by strict UTF-8 bytes.
Vectors and distances use IEEE-754 binary64. Optional references and the
outside endpoint use `0xffffffff` where defined by `cooked_assembly.h`.

## Determinism

Identity tables, provenance records, portal tables, and required-zone sets are
canonicalized before hashing and writing. LOD order and interaction state order
remain meaningful and are not reordered. Semantically identical manifests with
different object-key or identity-table ordering produce byte-identical output.

The runtime payload deliberately omits:

- source manifest paths;
- downloaded or signed provider URLs;
- API credentials;
- editor-only generation prompts;
- JSON property names;
- inferred entity or renderer state.

Virtual visual, collision, navigation, and walkable-surface locators remain
because later cook stages need those authored bindings.

## Fail-Closed Loader

The loader applies configured limits before allocation and returns either a
`shared_ptr<const CookedAssembly>` or an error with no partial graph. It rejects:

- truncated files and inconsistent sizes;
- invalid magic, versions, reserved fields, UTF-8, or enum values;
- payload hash mismatches and trailing data;
- oversized files, strings, tables, aggregate records, LOD lists, or state lists;
- duplicate IDs, dangling indices, or incomplete reciprocal references;
- interactions or moving parts owned by the wrong module;
- moving interactions without portal sockets, open/closed states, or parts;
- reused portal sockets, closure/socket mismatches, or sockets assigned to the
  wrong zone side;
- invalid transforms, axes, distances, scales, or LOD chains;
- missing exterior/interior modules or unzoned interior modules;
- non-sealable or non-navigable portals;
- entry zones not connected directly to outside;
- any interior zone unreachable from outside.

The payload hash is an integrity and corruption check, not a trust signature.
Untrusted downloadable content will require a separately authenticated package
catalog before it can be admitted to the cooker.

## Integration Boundary

WS-026 now maps each immutable table entry to leased runtime handles through the
versioned `.tdcontent` contract described in
`RUNTIME_CONTENT_ASSEMBLY_CONTRACT_2026-07-21.md`. WS-025 preparation and commit
publish the entity graph transactionally only after model uploads retire.

Portals, pressure, navigation, interaction state, moving rigid parts, local
lighting, and streaming remain separate systems joined by the cooked identities
rather than hidden loader side effects. Their catalog records are preflight
contracts in Stage 4, not claims that those gameplay systems are already live.
