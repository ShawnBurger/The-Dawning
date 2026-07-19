# THE DAWNING — Organic Topology Bible
# Session 3: Edge Flow, Deformation Rules, Triangle Budgets
# Sources: Polycount wiki, TopologyGuides.com, Thunder Cloud Studio,
#          Reallusion, Meshy, GarageFarm, Blender documentation

---

## PART 1: TRIANGLE BUDGETS PER PLATFORM

| Context | Triangle Budget | Notes |
|---|---|---|
| **Mobile game character** | 2,000-5,000 | Aggressive optimization |
| **PC/Console hero character** | 30,000-100,000 | Main player character |
| **PC/Console NPC (close)** | 10,000-30,000 | Conversation distance |
| **PC/Console NPC (distant)** | 2,000-5,000 | Crowd/background |
| **Face only (PC hero)** | 5,000-15,000 | Eyes/mouth need density |
| **Face only (mobile)** | 500-2,000 | |
| **Creature (large, close)** | 10,000-50,000 | Boss-level detail |
| **Creature (medium, herd)** | 3,000-8,000 | Multiple on screen |
| **Creature (small, swarm)** | 500-2,000 | Insects, fish |
| **Tree (full mesh)** | 1,000-5,000 | LOD0 |
| **Tree (billboard)** | 2-8 | LOD3, crossing quads |
| **Rock (close)** | 500-2,000 | |
| **Building (exterior)** | 5,000-20,000 | |
| **Ship (hero, close)** | 20,000-80,000 | Player's ship |
| **Ship (distant)** | 1,000-5,000 | |

### LOD Level Triangle Ratios
- LOD0: 100% (full detail, < 10m from camera)
- LOD1: 50% (medium, 10-50m)
- LOD2: 25% (low, 50-200m)
- LOD3: 10% or billboard (200m+)

Transition distances scale with object size: a 50m ship has wider LOD ranges than a 2m character.

---

## PART 2: FACE TOPOLOGY RULES

The human face has 43 muscles. Topology must follow muscle flow for believable deformation.

### Critical Edge Loop Placements

**Eyes (MOST IMPORTANT)**:
- Minimum 8 vertices around each eye opening (12-16 for hero characters)
- Edge loops follow the orbicularis oculi muscle (circular around eye socket)
- Upper and lower eyelids each need their own loop for independent blink
- Inner corner (tear duct) needs extra density for squint/narrow expressions
- Outer corner needs clean flow into cheekbone and temple

**Mouth**:
- Minimum 8 vertices around mouth opening (12-16 for hero)
- Loops follow orbicularis oris (circular muscle around lips)
- 3-4 loops around lips for speech visemes and expressions
- Nasolabial fold: edge loop from nose wing to mouth corner
- Chin dimple: flow from lower lip over chin

**Nose**:
- Edge loops follow the nasal bone down to nostrils
- Nostril openings need their own small loops
- Bridge of nose connects to brow loops

**Forehead/Brow**:
- Horizontal loops for eyebrow raise
- Corrugator loop for frown (between eyebrows)
- Frontalis loops running forehead to scalp

### Pole Placement Rules
- **Poles** = vertices with ≠ 4 connecting edges
- 3-edge poles and 5-edge poles are acceptable but must be managed
- NEVER place poles: at eye corners, mouth corners, nostril edges, or any deforming area
- ALWAYS place poles: on flat, non-deforming areas (forehead center, back of head, top of skull)
- Poles create pinching under subdivision — hide them where pinching won't be seen

### Topology Flow Patterns
```
FOREHEAD: Horizontal loops ═══════════════
                           ╲             ╱
BROW: Loops converge  →    ╲═══════════╱
                             ║  EYE   ║   ← Circular loops around eye
CHEEK: Loops flow from  →   ╚═══════╝
       eye to mouth              ↓
                           ╱═══════════╲
MOUTH: Circular loops →  ║   MOUTH   ║
                           ╲═══════════╱
CHIN: Loops continue down     ↓
```

---

## PART 3: BODY TOPOLOGY RULES

### Shoulder/Armpit (Hardest Area)
- The shoulder is the most complex joint — 3 degrees of freedom
- Edge loops must spiral from chest → deltoid → upper arm
- Armpit needs HIGHER density than surrounding areas
- Avoid triangles in the armpit crease — they pinch when arm raises
- 3-4 loops through the armpit minimum

### Elbow
- Minimum 3 edge loops across the elbow joint
- Inner elbow (crease side): more density for compression
- Outer elbow: can be lower density
- Support loop above and below the joint to prevent subdivision rounding

### Knee
- Same principle as elbow: 3+ loops across joint
- Back of knee (popliteal fossa): higher density for compression fold
- Front of knee (patella): can be flatter topology

### Hip/Crotch
- Second hardest area after shoulder
- Edge loops must flow from waist → hip → inner thigh without twisting
- The crotch area needs careful quad flow — no triangles
- Gluteal fold needs its own loop for sitting/bending

### Wrist/Ankle
- 2-3 loops across joint
- Wrist needs enough resolution for hand rotation (pronation/supination)
- Ankle needs loops for plantar/dorsiflexion

### Fingers
- Minimum 2 loops per finger joint (3 joints = 6 loops per finger)
- Fingertip: 6-8 vertex cap
- Webbing between fingers needs clean flow

---

## PART 4: PROCEDURAL CREATURE TOPOLOGY RULES

For The Dawning's procedural creatures, topology is generated from body plan parameters.

### Quadruped Skeleton Template
```
Bone Hierarchy:
  Root (center of mass, between shoulder and hip)
  ├── Spine (5-8 bones: pelvis → mid-back → shoulder → neck → head)
  │   ├── Head
  │   │   ├── Jaw
  │   │   └── Eyes (×2)
  │   ├── Tail (3-8 bones, more = longer/more flexible)
  │   ├── Front_L leg
  │   │   ├── Shoulder (1 bone)
  │   │   ├── Upper arm (1 bone)
  │   │   ├── Forearm (1 bone)
  │   │   ├── Wrist/Pastern (1 bone)
  │   │   └── Foot/Hoof (1-3 bones for toes)
  │   ├── Front_R leg (mirror)
  │   ├── Hind_L leg
  │   │   ├── Hip (1 bone)
  │   │   ├── Thigh (1 bone)
  │   │   ├── Shin (1 bone)
  │   │   ├── Ankle/Hock (1 bone)
  │   │   └── Foot (1-3 bones)
  │   └── Hind_R leg (mirror)
```

### Joint Rotation Limits (Degrees)

| Joint | Flexion | Extension | Abduction | Adduction | Rotation |
|---|---|---|---|---|---|
| **Neck** | 45 | 30 | 40 | 40 | 60 |
| **Spine (per segment)** | 15 | 10 | 10 | 10 | 20 |
| **Shoulder/Hip** | 120 | 30 | 45 | 30 | 45 |
| **Elbow/Knee** | 150 | 0 | 0 | 0 | 0 |
| **Wrist/Ankle** | 80 | 70 | 20 | 30 | 15 |
| **Jaw** | 35 | 0 | 5 | 5 | 0 |
| **Tail (per segment)** | 25 | 25 | 30 | 30 | 15 |

Scale these by creature type:
- Reptilian: ×0.7 (stiffer, except lateral spine flexion ×1.5)
- Insectoid: ×0.5 limbs (rigid exoskeleton), ×2.0 at joints
- Tentacled: ×3.0 all axes (extremely flexible)
- Crystalline: ×0.3 (nearly rigid)

### Vertex Count Per Body Section (LOD0, medium creature)
- Head: 500-1500 (eyes and mouth need density)
- Torso: 1000-3000 (smooth tube with shoulder/hip detail)
- Each leg: 400-800 (joints need loops)
- Tail: 200-600 (fewer joints = less)
- Total: 3000-8000 triangles for a medium creature

### Skinning Weight Rules
- Each vertex influenced by maximum 4 bones
- Bone weight sum must equal 1.0 per vertex
- Weight falloff: smooth gradient across 2-3 edge loops at each joint
- No hard weight boundaries (causes creasing)
- Joint center: 50/50 split between parent and child bone

---

## PART 5: HARD SURFACE TOPOLOGY (Ships, Stations)

### Support Loop Technique
For sharp edges on subdivided surfaces, place support loops 1-2 edges away from the corner:
```
Without support:     With support:
  ┌─────────┐          ┌═══════════┐
  │         │          ║           ║   ← Support loops (═) preserve
  │         │    →     ║           ║      edge sharpness under
  │         │          ║           ║      subdivision
  └─────────┘          └═══════════┘
  (rounds off)         (stays sharp)
```

### Panel Line Generation
- Voronoi partition of hull surface creates natural panel edges
- Each panel: inset 0.002-0.005 of panel size for gap
- Panel rivets: 0.001m radius bumps along edges (normal map, not geometry)
- Panel count scales with hull size: ~1 panel per 2m² of surface

### Edge Bevels for Hard Surfaces
- Real objects never have infinitely sharp edges
- Bevel radius: 0.002-0.01m for most manufactured objects
- This catches light realistically (specular highlight along beveled edge)
- 2-3 segments in the bevel for smooth curvature

---

## PART 6: INTEGRATION WITH PROCGEN CURRICULUM

**Step 2382 (Torso generation)**: Ensure torso mesh has even quad distribution, higher density at shoulder and hip attachment points (per Part 3).

**Step 2383 (Limb generation)**: 3+ edge loops per joint, rotation limits from Part 4 table.

**Step 2384 (Head generation)**: If creature has visible eyes/mouth, follow face topology rules from Part 2. Minimum 8 vertices around each opening.

**Step 2401 (Skeleton autogeneration)**: Use quadruped template from Part 4, scale bone hierarchy by limb count and body plan.

**Step 2402 (Walk cycle)**: Joint rotation limits from Part 4 constrain IK solver.

**Step 2314 (Ship hull panels)**: Use panel line generation from Part 5. Voronoi + inset for panels, normal map for rivets.

**Step 2330 (Ship LOD)**: Follow triangle budgets from Part 1. Hero ship LOD0: 20-80K, LOD3: 1-5K.

**Steps 1201-1300 (Animation system)**: All skeleton, blend, and IK steps should respect joint limits from Part 4 and edge loop density rules from Parts 2-3.
