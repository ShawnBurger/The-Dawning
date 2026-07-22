# Production Asset Generation Wave 1

Date: 2026-07-22

Status: generation started under WS-034

## Goal

Build the first reviewed high-detail source-art families for:

- the Longreach Expedition Cutter exterior and cockpit architecture;
- the Meridian Exchange orbital station exterior and interior kit;
- the Argent modular EVA suit;
- the Helix carbine and sidearm.

These assets establish original visual languages for The Dawning. They may draw
general lessons about proportion and orthographic presentation from the
user-provided `Flight_Tech_Ship_Images_Complete.pdf`, but may not copy its ship
silhouettes, brands, labels, or named designs.

The exact design dimensions, prompts, budget, and request inventory are in
`assets/design/production_wave1/wave1_plan.json`.

## Production Decision

Meshy is being used for high-detail source geometry and PBR material candidates.
It is not being asked for a complete functional ship or station in one object.
Exterior shells, pressure architecture, furniture, controls, moving parts, and
props remain separate. Blender and The Dawning's assembly metadata retain
authority over dimensions, axes, openings, pivots, collisions, walkability,
navigation, pressure zones, sockets, interaction anchors, LODs, and continuity.

The first paid batch is deliberately broad enough to establish all four visual
families while preserving review gates:

| Stage | Count | Cost each | Maximum |
| --- | ---: | ---: | ---: |
| GPT Image 2 multi-view concept | 5 | 9 | 45 |
| Meshy 6 text geometry preview | 11 | 20 | 220 |
| Conditional Meshy 6 textured multi-view | 5 | 30 | 150 |
| Conditional accepted-preview refinement | 11 | 10 | 110 |
| Corrective contingency | - | - | 75 |
| **Wave ceiling** | | | **600** |

The verified starting account balance is 9,927 credits. The ceiling is a guard,
not a target. Rejected geometry is not refined merely because credits remain.

Official API pricing currently charges 20 credits for a Meshy 6 text preview,
10 for its refine stage, 30 for a textured Meshy 6 multi-image generation, and
9 for a GPT Image 2 concept request:
https://docs.meshy.ai/en/api/pricing

The Text to 3D API supports PBR output, 4K base color, GLB-only targets, and
lighting removal. Removing generated highlights and shadows is required because
The Dawning supplies scene lighting:
https://docs.meshy.ai/en/api/text-to-3d

## Visual Direction

### Asterline frontier industrial

The Longreach and Meridian share a practical construction language:

- obvious load paths and pressure volumes;
- replaceable modules and reachable service panels;
- titanium and painted-alloy structure with dark composite covers;
- restrained amber safety and wayfinding accents;
- large, medium, and fine detail with a functional reason at each scale;
- no arbitrary greeble carpet, fake vents, glowing seams, or pseudo-labels.

The 52 x 18 x 11 m Longreach is a spacecraft, not an atmospheric fighter. Its
axial pressure keel, tucked aft drive pods, thermal hardware, docking collar,
airlock, ramp, RCS placement, and landing interfaces must remain readable. The
interior starts with exact cockpit frames, wall bays, deck bays, a seat, and a
flight console. Those modules dress a protected 6.4 x 3.2 x 8.5 m cockpit
whitebox; they do not redefine it.

The Meridian station is an assembled civic machine. The static core, docking
arms, cargo truss, pressure modules, tanks, radiator roots, and service access
must read as separate construction systems. A future rotating habitat, arrays,
elevators, and doors will be independent rigid parts. The first interior kit
uses a 2.4 m corridor grid, 2.6 m nominal clear height, and 1.2 m minimum clear
door width.

### Palisade field equipment

The Argent suit protects a moving human rather than replacing the body with a
solid sculpture. Hard shells bridge over a pressure garment, with explicit
joint gaps, reachable rescue hardware, tool mounts, and a compact serviceable
life-support pack. The concept is generated as one T-pose reference; production
promotion requires separation into named rigid shells around an authored human
skeleton and pressure envelope.

The Helix weapons use one functional family language. Receivers, barrel groups,
magazines, stocks, optics, controls, and sights must be readable as serviceable
parts. Generated pseudo-controls or fused moving parts fail production review.
Gameplay ballistics, collision, sockets, animation pivots, and first-person
ergonomics are authored later and cannot be inferred from the render mesh.

## Acceptance Sequence

1. Run every request as a zero-credit dry run and inspect the exact content hash,
   assembly linkage, projected cost, and redacted body.
2. Generate concept sheets and untextured geometry only.
3. Record explicit art review in each generated manifest.
4. Refine or reconstruct only candidates with accepted silhouette, role,
   proportions, openings, and module boundaries.
5. Inspect every GLB with The Dawning importer and Blender 4.5. Record bounds,
   axis, triangles, materials, textures, disconnected parts, manifold state, and
   suspect generated text or logos.
6. Render orthographic and close-view review sheets under neutral light.
7. Promote only accepted source masters to Git LFS. Keep rejected manifests and
   reasons; leave their binaries ignored.
8. Normalize accepted assets against authored dimensions, separate rigid parts,
   replace labels, repair topology, author pivots and sockets, make LODs, then
   run deterministic cooks in a separate integration lane.

## Hard Rejection Rules

- Ship length is not greater than beam, or height is outside the declared band.
- A ship reads as a conventional aircraft, disk, saucer, or stretched mesh.
- A station reads as one fused sculpture with no module or service logic.
- Interior geometry blocks protected walking, head, reach, sightline, or door
  clearances.
- Armor blocks expected neck, shoulder, elbow, wrist, hip, knee, or ankle range.
- A weapon's magazine, trigger, selector, stock, sight, or service seam is fused
  into implausible decorative geometry.
- Baked lighting, generated text, logos, impossible vents, floating components,
  or broad random damage survive the cleanup plan.
- The candidate is attractive only from one view or cannot survive close camera
  inspection.

## Current Evidence

- WS-034 branch and ownership are published before generation.
- Meshy balance was queried through the credential-safe repository client; no
  key was logged or added to tracked content.
- Official pricing and July 2026 API behavior were rechecked before spend.
- The 68-page user concept packet was rendered and visually reviewed as a
  reference library. Its SHA-256 is retained in the design plan without copying
  the PDF into the public repository.

Generation task IDs, credits, hashes, review decisions, and promoted-source
inventory will be appended here after the batch completes.
