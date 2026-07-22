# Modular Asset Production Standard

**Status:** Active production policy
**Applies to:** Weapons, ships, stations, armor, characters, props, and interiors

## Core rule

The Dawning produces one asset family at a time and one component within that
family at a time. A component cannot enter production until its envelope,
origin, interfaces, function, material language, and acceptance tests exist in
a machine-readable design plan. The next component does not become active until
the current component is accepted or explicitly rejected with evidence.

This policy gives each part enough visual and technical attention, keeps Meshy
requests narrow, makes failures cheap, and prevents generated geometry from
silently defining gameplay contracts.

## Required decompositions

### Weapons

A production firearm or energy weapon is assembled from independently authored
upper receiver, lower receiver, barrel or accelerator group, thermal handguard,
muzzle device, stock, magazine or power pack, and optic or sight components.
Attachments, batteries, suppressors, foregrips, launchers, and alternate mission
modules are additional components rather than fused texture details.

Weapon plans must define grip and shoulder references, sight line, muzzle axis,
magazine insertion axis, trigger interaction, moving-part pivots, attachment
sockets, ballistics origin, collision proxy ownership, and suited-hand clearance.

### Ships and stations

A ship is assembled from a cockpit module; one or more mission-body modules such
as cargo, carrier, habitat, medical, exploration, or weapons sections; discrete
engine and maneuvering-thruster modules; wing, pylon, radiator, or aerodynamic
body modules; landing gear; exterior closures; and optional turret or utility
modules. Ships remain longer than they are wide, with height derived from deck,
equipment, pressure-vessel, and landing-clearance requirements.

Every habitable exterior module has a paired interior module. The pair shares
authored portal planes, pressure boundaries, deck levels, structural keep-outs,
utility trunks, damage zones, and transform anchors. Interiors are decomposed
into rooms, corridors, airlocks, doors, consoles, seats, storage, fixtures, and
service panels. A generated shell may supply visible surface detail but never
owns navigation, collision, airtightness, interaction sockets, or door state.

A station follows the same rules at larger scale: core, docking, habitation,
operations, power, thermal, cargo, transit, defense, and exterior-structure
modules each receive their own interior and portal contracts where habitable.

### Armor and characters

Armor is produced as helmet, chest and back torso, left and right arm, left and
right leg, glove, and boot components. Optional backpack, life-support, tool,
weapon, and rank or faction pieces remain removable modules. Each part defines
body attachment landmarks, motion clearances, sealing surfaces, collision
ownership, material layers, damage regions, and first-person visibility rules.

The base character is separate from armor. Body, head, eyes, hair, hands, and
rig are reviewed independently before the full character assembly is accepted.

## Component contract

Every component plan records:

- stable asset, component, and module IDs;
- units, handedness, width/up/forward axes, origin, and assembly transform;
- maximum envelope and minimum fill expectations;
- reciprocal interfaces with local position, normal, mate, clearance, and type;
- visible function and required moving or removable subparts;
- material zones and forbidden colors, symbols, text, glow, or style shortcuts;
- authored collision, navigation, interaction, animation, physics, and damage
  ownership outside generated render geometry;
- source, provider, request, task, hash, credit, and review provenance; and
- explicit status from `pending` through `source_promoted` or a named rejection.

Socket pairs must be reciprocal and use opposite unit normals. Component bounds
must fit the final assembly envelope before generation. Generated socket
positions are never authoritative.

## Production sequence

1. **Design authority:** Freeze the component envelope, interfaces, function,
   material brief, and exclusions.
2. **Authored structure:** Build exact hard-surface geometry in Blender when
   shape, fit, portals, ergonomics, or moving boundaries matter. Organic source
   generation is allowed only inside the same envelope and interface contract.
3. **Local validation:** Verify bounds, axes, topology, connected parts, object
   names, material regions, and engine import before spending credits.
4. **Meshy surface pass:** Prefer retexture for precision modules. Use isolated
   text/image-to-3D only when it can be rejected without affecting authored
   assembly contracts. Every request has a hard credit cap.
5. **Four-angle art review:** Inspect principal axes and a three-quarter render
   for silhouette, component scope, realism, materials, pseudo-text, baked
   lighting, detached detail, and interface obstruction.
6. **Deterministic acceptance:** Retain the exact provider source, verify hashes,
   restore only uniform provider normalization, require topology/connectivity
   preservation, and emit an exact-scale production GLB plus report.
7. **Gameplay authoring:** Add collision, sockets, pivots, interaction states,
   animation, physics, damage, navigation, and pressure behavior from the design
   plan rather than inferred render geometry.
8. **Assembly review:** Test neighboring components at their authored transforms,
   then verify first-person, third-person, interior, exterior, and damage-state
   use before opening the next asset family.

## Realism bar

Realism comes from functional construction, readable material response, scale-
appropriate seams and fasteners, service access, plausible clearances, and wear
located where hands, tools, airflow, heat, or impacts justify it. Detail must
support function. Broad grime, random greebles, decorative light strips,
pseudo-labels, familiar real-world weapon copies, monolithic ship shells, and
blockout-quality interiors are rejection conditions.

## Credit discipline

The monthly Meshy allowance is a production budget, not a target. Plans reserve
credits per component and a separate contingency for rejected explorations.
Cached requests are reused by content hash. Paid work starts only after a dry
run shows the exact request, cache state, expected output path, and hard maximum.
No batch may activate multiple components or asset families.

## First implementation

The Helix Induction Carbine Mk1 is the first asset governed by this standard.
Its upper receiver was authored as 32 geometric subparts, retextured in one
packed PBR material, restored to a 300 mm exact length, and accepted only after
engine import, topology, connectivity, scale, material, and four-angle review.
The lower receiver is the next eligible component.
