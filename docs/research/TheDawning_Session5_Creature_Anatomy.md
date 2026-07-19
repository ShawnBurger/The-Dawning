# THE DAWNING — Creature Anatomy Bible
# Session 5: Body Plan Proportions, Mass Distribution, Joint Mechanics,
#             Gait Patterns, Gravity Adaptation
# Sources: Runevision procedural creatures, Bournemouth MSc thesis,
#          INRIA quadruped survey, Alexander gait analysis,
#          comparative anatomy research

---

## PART 1: BODY PLAN TEMPLATES

Every procedural creature is built from a body plan. These are the anatomical
rules that make organisms look alive instead of assembled from primitives.

### Template: Bilateral Quadruped (Most Common)

```
Proportions (normalized to body length = 1.0):

Head:    length 0.15-0.25, width 0.10-0.20, height 0.12-0.18
Neck:    length 0.10-0.30, radius 0.06-0.12
Torso:   length 0.35-0.50, width 0.15-0.25, height 0.15-0.30
         Ribcage: front 60% of torso (wider), Abdomen: rear 40% (narrower)
Pelvis:  width 0.80-0.95 of ribcage width
Tail:    length 0.20-0.80, taper ratio 0.3 (tip = 30% of base radius)

Front legs: total length 0.60-0.80 of torso height
  Upper:  40% of total leg length
  Lower:  35% of total leg length
  Foot:   25% of total leg length

Hind legs: total length 0.65-0.90 of torso height (usually slightly longer)
  Upper:  35% of total leg length
  Lower:  35% of total leg length
  Foot:   30% of total leg length

Leg stance width: 0.70-1.00 of torso width
```

### Template: Bilateral Biped

```
Proportions (normalized to total height = 1.0):

Head:     0.12-0.15 of height, positioned at top
Neck:     0.03-0.06 of height
Torso:    0.30-0.35 of height
  Chest:  upper 55% (wider, ribcage)
  Waist:  middle 15% (narrowest)
  Hips:   lower 30% (wider than waist)
Arms:     0.38-0.42 of height (fingertip to shoulder)
  Upper:  40% of arm length
  Lower:  35% of arm length
  Hand:   25% of arm length
Legs:     0.47-0.52 of height (ground to hip)
  Upper:  45% of leg length
  Lower:  35% of leg length
  Foot:   20% of leg length
```

### Template: Hexapod (Insectoid)

```
Proportions:
Head:     0.15-0.25 of body length, compound eyes (wide placement)
Thorax:   3 segments, each 0.10-0.15 of body length
          Each segment has 1 pair of legs
Abdomen:  0.30-0.50 of body length, no legs, tapers
Legs:     3 pairs, each: coxa → femur → tibia → tarsus
          Length: 0.5-1.2 of body length
Antennae: 0.20-0.60 of body length, 2-8 segments
Wings:    if present, 0.8-1.5 of body length span (optional)
```

### Template: Tentacled

```
Proportions:
Central body: roughly spherical or ovoid, diameter = reference
Tentacles: 4-12, each 1.0-3.0 of body diameter in length
           Thickness: 0.08-0.15 of body diameter at base
           Taper: linear to 0.2 of base at tip
           Segments: 8-20 per tentacle (more = more flexible)
Mantle:    if aquatic, 0.8-1.2 of body diameter
Eyes:      if present, 2-8, on body or on stalks
```

---

## PART 2: MASS DISTRIBUTION

Where the mass goes determines center of gravity, which determines balance and gait.

### Mass Percentage by Body Part (Quadruped Mammal)

| Part | % of Body Mass | Notes |
|---|---|---|
| **Head** | 3-8% | Larger for predators (jaw muscle), smaller for grazers |
| **Neck** | 5-10% | Longer necks = heavier (giraffe neck = 8% of body) |
| **Torso/Ribcage** | 45-55% | Organs, ribcage, spine — heaviest single section |
| **Front legs (pair)** | 10-15% | Slightly lighter than hind (quadrupeds) |
| **Hind legs (pair)** | 15-20% | More muscle for propulsion |
| **Tail** | 1-5% | Varies enormously (rat tail ~3%, horse tail ~1%) |

### Center of Mass
- Quadrupeds: 55-65% of body length from nose (closer to front legs)
- Front legs bear 55-60% of weight at rest
- Hind legs bear 40-45%
- During running: weight shifts rearward (hind legs drive)

### Scaling Laws (Allometry)

Creatures scale NON-linearly with body mass. A 10× heavier animal isn't just 10× bigger:

```
Bone diameter ∝ mass^0.36 (bones get proportionally thicker)
Leg length ∝ mass^0.26 (legs get shorter relative to body)
Step frequency ∝ mass^-0.14 (bigger animals take slower steps)
Step length ∝ mass^0.38 (but each step covers more ground)
Top speed ∝ mass^0.17 (up to ~300kg, then decreases)
Heart rate ∝ mass^-0.25 (bigger = slower heartbeat)

For procedural generation:
  legThickness = baseLegThickness * pow(massKg / referenceMass, 0.36)
  legLength = baseLegLength * pow(massKg / referenceMass, 0.26)
  walkSpeed = baseWalkSpeed * pow(massKg / referenceMass, 0.17)
```

---

## PART 3: GRAVITY ADAPTATION

The Dawning has planets with varying gravity. Creatures must adapt.

### Gravity Effects on Body Plan

| Gravity | Legs | Bone Thickness | Body Shape | Movement |
|---|---|---|---|---|
| **0.1-0.3G** | Fewer, thinner | ×0.6 | Tall, elongated | Bounding, floating |
| **0.3-0.7G** | Normal count | ×0.8 | Slightly taller | Efficient walk/run |
| **0.7-1.2G** (Earth-like) | Normal | ×1.0 | Reference | Normal gaits |
| **1.2-2.0G** | More, thicker | ×1.3 | Squat, wide | Slower, more stable |
| **2.0-3.0G** | Many, very thick | ×1.6 | Very flat, wide | Slow crawl |
| **3.0G+** | 6+ or tentacle/slug | ×2.0 | Pancake-flat | Creeping, no jumping |

### Maximum Creature Size by Gravity

```
// Larger gravity = smaller maximum creature (square-cube law)
// Legs support weight ∝ cross-section (area), weight ∝ volume
float MaxCreatureMassKg(float gravity)
{
    // On Earth (1G), largest land animal ≈ 10,000 kg (elephant)
    // Scale inversely with gravity squared
    return 10000.0 / (gravity * gravity);
    // 0.5G → 40,000 kg (megafauna possible!)
    // 2.0G → 2,500 kg (nothing bigger than a rhino)
    // 3.0G → 1,111 kg (cow-sized max)
}
```

### Wing Viability by Gravity and Atmosphere

```
// Flying is only possible under certain conditions
bool CanFly(float gravityG, float atmosphereDensity, float massKg)
{
    // Wing loading limit: mass per wing area
    // Higher gravity or lower atmosphere density = harder to fly
    // Earth's largest flying bird: 15kg (bustard)
    float maxFlyingMass = 15.0 * atmosphereDensity / gravityG;
    return massKg < maxFlyingMass;
    // Low gravity + thick atmosphere: large flyers possible
    // High gravity + thin atmosphere: no flight at all
}
```

---

## PART 4: GAIT PATTERNS

How creatures walk depends on their body plan and speed.

### Quadruped Gaits (from Alexander's gait analysis)

Gait transitions are driven by **Froude number**: Fr = v² / (g × h)
where v = speed, g = gravity, h = hip height.

| Froude Number | Gait | Description | Leg Pattern |
|---|---|---|---|
| 0 - 0.5 | **Walk** | Always ≥2 feet on ground | LH, LF, RH, RF (lateral sequence) |
| 0.5 - 2.5 | **Trot** | Diagonal pairs together | LF+RH, then RF+LH |
| 0.5 - 2.5 | **Pace** | Same-side pairs | LF+LH, then RF+RH (camels, giraffes) |
| 2.5 - 7.0 | **Canter** | Asymmetric 3-beat | LH, RH+LF, RF (lead foot last) |
| 5.0+ | **Gallop** | Full suspension phase | Both hinds, then both fronts |
| 5.0+ | **Bound** | Simultaneous pairs | Both hinds, suspension, both fronts |

### Gait Phase Timing (Walk)

```
// Walk cycle: each leg has 4 phases
// Stance: foot on ground, supporting weight
// Swing: foot in air, moving forward
// Duty factor: fraction of cycle foot is on ground (walk ≈ 0.6-0.7)

struct GaitPhase {
    float stanceDuration;  // Fraction of cycle on ground
    float swingDuration;   // Fraction in air (1 - stance)
    float phaseOffset;     // When this leg starts relative to cycle (0-1)
};

// Quadruped walk (lateral sequence):
// Left Hind:   offset 0.00, stance 0.65
// Left Front:  offset 0.25, stance 0.65
// Right Hind:  offset 0.50, stance 0.65
// Right Front: offset 0.75, stance 0.65

// Quadruped trot:
// Left Front + Right Hind:  offset 0.00, stance 0.50
// Right Front + Left Hind:  offset 0.50, stance 0.50

// Biped walk:
// Left:  offset 0.00, stance 0.60
// Right: offset 0.50, stance 0.60

// Hexapod walk (tripod gait):
// L1 + R2 + L3: offset 0.00, stance 0.50
// R1 + L2 + R3: offset 0.50, stance 0.50
```

### Procedural Walk Generation

```hlsl
// Compute foot target position for IK
float3 ComputeFootTarget(float cycleTime, float phaseOffset,
                            float3 hipPosition, float3 forwardDir,
                            float stepLength, float stepHeight, float stanceWidth)
{
    float phase = frac(cycleTime + phaseOffset);

    // Stance: foot slides backward on ground (body moves forward over it)
    // Swing: foot lifts, arcs forward, plants
    float3 target;

    if (phase < 0.65) // Stance
    {
        float stanceProgress = phase / 0.65;
        target = hipPosition + forwardDir * stepLength * (0.5 - stanceProgress);
        target.y = 0; // On ground
    }
    else // Swing
    {
        float swingProgress = (phase - 0.65) / 0.35;
        target = hipPosition + forwardDir * stepLength * (-0.5 + swingProgress);

        // Arc: foot lifts up in a sine curve during swing
        target.y = sin(swingProgress * PI) * stepHeight;
    }

    // Offset to side for stance width
    target += cross(forwardDir, float3(0,1,0)) * stanceWidth;

    return target;
}
```

---

## PART 5: SPINE AND BODY DYNAMICS

### Spine Motion During Walk
- Lateral flexion: spine bends side-to-side with each step (lizards exaggerate this)
- Vertical bounce: center of mass rises/falls ~2-4cm per step (mammals)
- Rotation: slight counter-rotation of shoulders vs hips
- Gallop: spine flexes and extends dramatically (cheetah spine adds 30% to stride)

```
// Spine oscillation parameters:
// Walk:   lateral ±3°, vertical ±2cm, rotation ±2°
// Trot:   lateral ±1°, vertical ±4cm, rotation ±3°
// Gallop: lateral ±0°, vertical ±8cm, spinal flex ±15°
```

### Tail Motion
- Counterbalance: tail swings opposite to body motion
- Emotion indicator: up = alert/aggressive, down = relaxed/submissive, tucked = fearful
- Running: tail extends horizontally for balance (cheetah, velociraptor)

### Head Motion
- Stabilization: head stays relatively stable during locomotion (vestibulo-ocular reflex)
- Birds bob: head thrusts forward then holds still (stabilization during walk)
- Predators: eyes face forward, head tracks target

---

## PART 6: INTEGRATION WITH PROCGEN CURRICULUM

**Step 2381-2400 (Creature mesh generation)**: Use Part 1 body plan templates. Select template from species body plan enum, apply proportions from the tables.

**Step 2383 (Limb generation)**: Leg length and thickness from Part 2 scaling laws. Adjust by planet gravity using Part 3 formulas.

**Step 2389 (Wing generation)**: Only generate wings if CanFly() returns true (Part 3).

**Step 2399 (Sexual dimorphism)**: Males 10-20% larger, thicker neck, possible ornaments (horns, manes). Females slightly smaller, wider hips.

**Step 2400 (Juvenile proportions)**: Head 1.3× adult proportion, limbs 0.8× adult proportion, eyes 1.2× adult proportion. This triggers "cute" response.

**Step 2402 (Walk cycle generation)**: Use Part 4 gait tables. Select gait from Froude number. Phase offsets from the gait pattern tables.

**Steps 2461-2480 (Behavioral simulation)**: Gait speed derived from mass via allometry (Part 2). Flight behaviors only for species that pass CanFly check.

**Step 2615 (Low-G biome)**: Creatures use Part 3's low-G adaptation: fewer thinner legs, taller bodies, bounding locomotion.

**Step 2614 (High-G biome)**: Squat, wide creatures with many thick legs, slow movement (Part 3).
