# THE DAWNING — MASTER INDEX
# Read This First. This is the map to everything.
# Last updated: March 9, 2026

---

## WHAT THIS PROJECT IS

The Dawning is a D3D12 space game engine built from scratch in C++20.
V1 reached 287 source files, 60,323 lines, 16 HLSL shaders, 43 system ticks.
V2 is a clean rebuild using the 3000-step curriculum with research-backed values.

---

## DOCUMENT MAP (Read Order for Desktop Claude)

### STEP 1: Understand the Architecture
| Document | Size | Read When |
|---|---|---|
| `ARCHITECTURE_REFERENCE.md` | 22KB | FIRST. File manifest, namespace map, stub vs heavy .cpp rules. |
| `TheDawning_v2_REBUILD_BLUEPRINT.md` | 30KB | SECOND. Clean v2 plan with proper src/ tree and .h/.cpp split. |
| `TheDawning_GameDesignDocument.docx` | 20KB | For understanding game design intent (what we're building). |

### STEP 2: Follow the Curriculum
| Document | Size | Steps | Content |
|---|---|---|---|
| `TheDawning_1000_Step_Curriculum.md` | 73KB | 1-1000 | Window → playable game in 16 phases |
| `TheDawning_2000_Step_AAA_Curriculum.md` | 57KB | 1001-2000 | 10 depth passes: renderer, assets, animation, physics, audio, AI, net, UI, scripting, integration |
| `TheDawning_ProcGen_Steps_2001_3000.md` | 65KB | 2001-3000 | Procedural universe: galaxy, terrain, ecosystems, cultures, rendering |

### STEP 3: Reference Deep Dives (Consult When Implementing Each System)

**Visual & Rendering:**
| Document | Consult During Steps |
|---|---|
| `TheDawning_PBR_Material_Bible.md` | Any step involving materials, textures, surface properties |
| `TheDawning_Atmosphere_Lighting_Bible.md` | Steps 881-900 (sky shader), 2141-2146 (planet atmosphere) |
| `TheDawning_Session3_Topology_Bible.md` | Steps 1151-1200 (mesh pipeline), 2381-2400 (creature mesh) |
| `TheDawning_Session4_Shader_Recipes.md` | Any step involving special materials (leaves, skin, crystal, holograms) |
| `TheDawning_Session6_Particles_Bible.md` | Steps 716-726 (particles), 2265 (grass), any weather/VFX |
| `TheDawning_Batch1_D3D12_Rendering_Deep_Dive.md` | Steps 1001-1150 (renderer depth pass) |

**Physics & Flight:**
| Document | Consult During Steps |
|---|---|
| `TheDawning_Batch1_Physics_Collision_Deep_Dive.md` | Steps 121-180 (physics), 1301-1430 (physics depth) |
| `TheDawning_Batch1_Orbital_Mechanics_Deep_Dive.md` | Steps 331-400 (galaxy/orbits), 1301-1340 (N-body) |
| `TheDawning_Batch1_Ship_Flight_Model_Deep_Dive.md` | Steps 251-330 (ship flight + cockpit) |

**Gameplay Systems:**
| Document | Consult During Steps |
|---|---|
| `TheDawning_Batch2_Combat_System_Deep_Dive.md` | Steps 471-540 (combat + encounters) |
| `TheDawning_Batch2_Economy_System_Deep_Dive.md` | Steps 601-670 (economy + crafting) |
| `TheDawning_Batch4_Crafting_Progression_Deep_Dive.md` | Steps 641-670 (crafting, XP, skill trees) |
| `TheDawning_Batch4_Story_Quest_Deep_Dive.md` | Steps 671-730 (story + quests) |

**Engine Infrastructure:**
| Document | Consult During Steps |
|---|---|
| `TheDawning_Batch3_ECS_Architecture_Deep_Dive.md` | Steps 81-120 (ECS + scene management) |
| `TheDawning_Batch3_AI_Behavior_Trees_Deep_Dive.md` | Steps 531-600 (AI + NPCs), 1531-1630 (AI depth) |
| `TheDawning_Batch2_Audio_Engine_Deep_Dive.md` | Steps 401-470 (audio), 1431-1530 (audio depth) |
| `TheDawning_Batch2_Networking_Deep_Dive.md` | Steps 731-820 (multiplayer), 1631-1730 (net depth) |
| `TheDawning_Batch3_UI_UX_Deep_Dive.md` | Steps 801-880 (UI + menus), 1731-1830 (UI depth) |
| `TheDawning_Batch3_SaveLoad_Serialization_Deep_Dive.md` | Steps 671-730 (save/load within story phase) |
| `TheDawning_Batch4_Terrain_Streaming_Deep_Dive.md` | Steps 181-250 (terrain), 1131-1150 (terrain depth) |
| `TheDawning_Batch4_Animation_System_Deep_Dive.md` | Steps 1201-1350 (animation system depth) |

**Procedural Generation:**
| Document | Consult During Steps |
|---|---|
| `TheDawning_Session5_Creature_Anatomy.md` | Steps 2381-2480 (creature generation + animation) |
| All 6 Bibles + deep dives | Steps 2001-3000 use values from every research document |

### STEP 4: Beginner's Code Guide (For Human Developer)
| Document | Size | Steps Covered |
|---|---|---|
| `TheDawning_Beginner_Guide_Part1_Foundation.md` | 43KB | 1-30 (window, types, input, timer) |
| `TheDawning_Beginner_Guide_Part2_D3D12.md` | 27KB | 31-53 (D3D12 init, blue screen) |

---

## V1 SOURCE CODE REFERENCE

The zip `TheDawning_FinalBeta.zip` contains the complete v1 codebase.
Use it to extract proven algorithms — do NOT copy it wholesale into v2.
V2 must have proper .h/.cpp split (v1 was mostly header-only).

Key files worth extracting logic from:
- `EmergentAISystem.h` (761 lines) — gossip, memory, witness, faction strategy
- `DestructionPhysicsSystem.h` (484 lines) — debris, breaches, zero-G, fire
- `ProceduralMusicComposer.h` (614 lines) — scale/theme/contour melody generation
- `EnvironmentalAudioPropagation.h` (380 lines) — wall occlusion, media types
- `SpeciesAudioSystem.h` (441 lines) — voice from body mass, biome soundscapes
- `GroundEncounterSystem.h` (814 lines) — 10 encounter types, 12 enemy archetypes
- `StoryCampaign.cpp` (571 lines) — 22 main quests with ground encounters

---

## KNOWN PITFALLS FROM V1 (Avoid in V2)

### 1. Header-Only Architecture
V1 put all implementation in .h files. This means every .cpp that includes
a header recompiles ALL that code. V2 MUST split into .h (declarations) and
.cpp (implementations). This alone will cut compile times by 80%.

### 2. Dead Code and Naming Collisions
V1 accumulated duplicate function names across headers (e.g., two different
`GenerateTerrain` functions in different namespaces). V2: use unique, fully
qualified names. Run `grep -rn "functionName"` before adding any new function.

### 3. Stub Files
V1 has 12 .cpp files that are intentional single-line stubs (the real code
is in the .h). DO NOT add code to these .cpp files in v2. Instead, properly
split the header implementations into their .cpp counterparts.

### 4. Missing Includes
V1 had 6 files missing `<cstdint>` which caused compile errors on strict
compilers. V2: always include `<cstdint>` if using uint32_t, uint64_t, etc.

### 5. Brace Counting
V1's Main.cpp reached 2974 lines with 655 brace pairs. Every edit was verified
with `grep -o "{" Main.cpp | wc -l` matching `grep -o "}" Main.cpp | wc -l`.
V2: keep Main.cpp THIN (just init + game loop + shutdown). Move systems to
their own .cpp files. Main.cpp should be <200 lines.

### 6. Star Trek References
V1 originally used Star Trek terms. ALL were renamed:
Starfleet→Vanguard, LCARS→HELIX, Phaser→Arc Lance, Photon Torpedo→Fusion Torpedo,
Dilithium→Helion, Nacelle→Drive Pylon, Transporter→Displacer, Tricorder→Spectrascope,
EMH→AMP, Subspace→Foldspace, Federation→Commonwealth, Turbolift→Grav-Lift,
Romulan→Vesk, Klingon→Xenoes.
V2: NEVER introduce any copyrighted terminology.

### 7. Float Precision at Scale
V1 uses Vec3d (double) for world positions — this is correct and must continue.
GPU receives Vec3f after subtracting camera position (camera-relative rendering).
NEVER use float for world-space positions. ALWAYS use double.

### 8. Build System
V1 uses CMake + MSVC. The build script is `SETUP_AND_BUILD.bat`.
V2 should maintain CMake compatibility. Target: Visual Studio 2022, C++20,
Windows 10/11 SDK. D3D12 is Windows-only (no cross-platform needed).

---

## PROJECT STATISTICS

| Metric | Value |
|---|---|
| **V1 source files** | 287 |
| **V1 source lines** | 60,323 |
| **V1 HLSL shaders** | 16 (1,955 lines) |
| **V1 system ticks** | 43 |
| **Curriculum steps** | 3,000 |
| **Research documents** | 24 |
| **Research lines** | 7,688 |
| **Total documentation** | ~300KB |
| **V2 target (with middleware)** | ~1M+ lines |
| **Factions** | 6 (Commonwealth, Xenoes, Vesk, Wardens, Nomads, Pirates) |
| **Campaign missions** | 22 main + 50+ side |
| **Ship classes** | 7 (fighter through station) |
| **Biome types** | 15+ alien + Earth standard |
| **Atmosphere presets** | 12 |
| **PBR materials** | 80+ |
| **Particle effect presets** | 15+ |

---

## BUILD INSTRUCTIONS (V1, for reference)

1. Install Visual Studio 2022 with "Desktop development with C++" + Windows SDK
2. Extract `TheDawning_FinalBeta.zip`
3. Double-click `SETUP_AND_BUILD.bat`
4. Output: `build\Release\TheDawning.exe`

---

## WHAT TO BUILD NEXT

Desktop Claude should:
1. Read this Master Index
2. Read the Rebuild Blueprint
3. Read the Architecture Reference
4. Start at Step 1 of the 1000-Step Curriculum
5. At each step, consult the relevant deep dive document for exact values
6. Compile after every step (never advance without green build)
7. At Step 1000: playable game
8. Continue to Steps 1001-2000 for AAA depth
9. Continue to Steps 2001-3000 for procedural universe

The research library ensures every number, every formula, every parameter
is physically measured, artistically validated, and production-proven.
Nothing is guesswork. Build with confidence.

---

*"In the beginning there was nothing. Then there was The Dawning."*
