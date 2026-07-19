# THE DAWNING — Particles & Simulation Bible
# Session 6: Grass, Fur, Hair, Rain, Snow, Fire, Dust, Sparks,
#             Smoke, Explosions, Water Splash — exact parameters
# Sources: Blender particle system, Unreal Niagara, Unity VFX Graph,
#          real-world physics data, game VFX reference

---

## PART 1: GRASS RENDERING

Grass is typically NOT a particle system — it's GPU-instanced geometry.
Each blade is a quad (or 3-quad cross) placed procedurally on terrain.

### Grass Parameters

```hlsl
struct GrassParams
{
    float density;        // Blades per square meter
    float height;         // Blade height in meters
    float heightVariance; // Random variation (±fraction of height)
    float width;          // Blade width at base in meters
    float taper;          // Width at tip / width at base (0.0 = pointed)
    float bendAmount;     // How much blade bends from vertical (0-1)
    float3 baseColor;     // Color at blade base
    float3 tipColor;      // Color at blade tip (usually lighter/yellower)
    float windResponse;   // How much wind affects this grass (0-1)
    float playerResponse; // How much player displaces blades (0-1)
};
```

### Grass Presets by Biome

| Biome | Density/m² | Height m | Width m | Taper | Base Color | Tip Color |
|---|---|---|---|---|---|---|
| **Meadow** | 800-1200 | 0.10-0.25 | 0.008 | 0.1 | (0.05,0.12,0.02) | (0.15,0.25,0.05) |
| **Prairie** | 400-600 | 0.30-0.60 | 0.005 | 0.0 | (0.08,0.10,0.03) | (0.20,0.22,0.08) |
| **Lawn** | 2000-4000 | 0.03-0.06 | 0.004 | 0.2 | (0.06,0.15,0.03) | (0.08,0.18,0.04) |
| **Savanna** | 100-300 | 0.40-1.00 | 0.006 | 0.0 | (0.15,0.18,0.05) | (0.25,0.22,0.08) |
| **Tundra** | 200-500 | 0.05-0.10 | 0.006 | 0.3 | (0.08,0.10,0.04) | (0.12,0.12,0.06) |
| **Dry/Dead** | 100-300 | 0.15-0.40 | 0.004 | 0.0 | (0.20,0.18,0.08) | (0.25,0.22,0.10) |
| **Alien (biolum)** | 300-600 | 0.15-0.30 | 0.010 | 0.2 | (0.02,0.05,0.08) | emissive |
| **Alien (crystal)** | 50-150 | 0.20-0.50 | 0.015 | 0.5 | (0.30,0.35,0.40) | (0.50,0.60,0.70) |

### Wind Animation

```hlsl
float3 GrassWindDisplacement(float3 worldPos, float time, float3 windDir,
                                  float windStrength, float bladeHeight)
{
    // Primary wave (large-scale wind gusts)
    float primaryWave = sin(dot(worldPos.xz, windDir.xz) * 0.5 + time * 2.0);

    // Secondary wave (smaller ripples)
    float secondaryWave = sin(dot(worldPos.xz, windDir.xz) * 2.0 + time * 5.0) * 0.3;

    // Turbulence (per-blade variation)
    float turbulence = sin(worldPos.x * 10.0 + worldPos.z * 7.0 + time * 3.0) * 0.15;

    float totalWave = (primaryWave + secondaryWave + turbulence) * windStrength;

    // Displacement increases toward blade tip (base stays fixed)
    // vertexHeight is 0 at base, 1 at tip (encoded in vertex color or UV)
    float3 displacement = windDir * totalWave * bladeHeight;

    return displacement;
    // Apply: vertex.position += displacement * vertexHeight;
}
```

---

## PART 2: RAIN SYSTEM

### Rain Particle Parameters

| Parameter | Light Rain | Medium Rain | Heavy Rain | Torrential |
|---|---|---|---|---|
| **Particles/frame** | 200 | 800 | 2000 | 5000 |
| **Spawn area** | 30×30m | 40×40m | 50×50m | 60×60m |
| **Fall speed m/s** | 4-6 | 6-8 | 8-12 | 12-18 |
| **Streak length** | 0.10m | 0.20m | 0.35m | 0.50m |
| **Streak width** | 0.001m | 0.001m | 0.002m | 0.002m |
| **Lifetime (s)** | 2.0 | 1.5 | 1.0 | 0.8 |
| **Wind deflection** | ±5° | ±10° | ±15° | ±25° |
| **Splash on impact** | subtle | visible | prominent | explosive |
| **Color (linear)** | (0.7,0.7,0.8) | (0.6,0.6,0.7) | (0.5,0.5,0.6) | (0.4,0.4,0.5) |
| **Alpha** | 0.15 | 0.25 | 0.40 | 0.55 |
| **Fog density** | 0.001 | 0.003 | 0.008 | 0.015 |
| **Screen wetness** | 0.1 | 0.3 | 0.6 | 0.9 |
| **Audio volume** | 0.2 | 0.5 | 0.8 | 1.0 |

### Splash Particles (on ground impact)

```
Each raindrop that hits a surface spawns 3-8 tiny splash particles:
  Splash speed: 1-3 m/s (upward + outward)
  Splash lifetime: 0.1-0.3s
  Splash size: 0.005-0.015m
  Splash color: (0.8, 0.8, 0.9, 0.5)
  Ripple decal on water surface: radius 0.02-0.05m, fade over 0.5s
```

---

## PART 3: SNOW SYSTEM

| Parameter | Light Snow | Moderate Snow | Heavy Snow | Blizzard |
|---|---|---|---|---|
| **Particles/frame** | 300 | 1000 | 3000 | 8000 |
| **Fall speed m/s** | 0.5-1.5 | 1.0-2.0 | 1.5-3.0 | 2.0-5.0 |
| **Horizontal drift** | ±0.5 m/s | ±1.0 m/s | ±2.0 m/s | ±5.0 m/s |
| **Flake size** | 0.003-0.008m | 0.005-0.012m | 0.008-0.020m | 0.005-0.015m |
| **Tumble rate** | 0.5 rot/s | 1.0 rot/s | 1.5 rot/s | 3.0 rot/s |
| **Lifetime (s)** | 8.0 | 5.0 | 3.0 | 2.0 |
| **Color** | (0.95,0.95,0.98) | same | same | (0.85,0.85,0.90) |
| **Alpha** | 0.6 | 0.7 | 0.8 | 0.5 (wind blur) |
| **Accumulation rate** | 0.001m/min | 0.005m/min | 0.02m/min | 0.05m/min |
| **Visibility (m)** | 5000 | 1000 | 200 | 50 |

### Snow flake motion: turbulent descent

```hlsl
float3 SnowflakeMotion(float time, float seed, float3 windDir, float windStrength)
{
    // Snowflakes don't fall straight — they tumble and drift
    float3 drift;
    drift.x = sin(time * 1.5 + seed * 6.28) * 0.3 + windDir.x * windStrength;
    drift.y = -1.0; // Gravity (scaled by fall speed outside)
    drift.z = cos(time * 1.2 + seed * 3.14) * 0.3 + windDir.z * windStrength;
    return normalize(drift);
}
```

---

## PART 4: FIRE SYSTEM

Fire requires multiple overlapping particle systems: flames, embers, smoke, heat haze.

### Flame Particles

| Parameter | Campfire | Torch | Bonfire | Inferno |
|---|---|---|---|---|
| **Particles/frame** | 30 | 15 | 100 | 300 |
| **Emission radius** | 0.3m | 0.05m | 1.0m | 3.0m |
| **Rise speed m/s** | 2-4 | 1-3 | 3-6 | 5-10 |
| **Lifetime (s)** | 0.3-0.8 | 0.2-0.5 | 0.4-1.0 | 0.3-0.8 |
| **Start size** | 0.1m | 0.05m | 0.3m | 0.5m |
| **End size** | 0.3m | 0.15m | 0.8m | 1.5m |
| **Start color** | (1.0,0.9,0.3) | (1.0,0.8,0.2) | (1.0,0.9,0.3) | (1.0,0.7,0.1) |
| **End color** | (0.8,0.2,0.0) | (0.7,0.1,0.0) | (0.8,0.2,0.0) | (0.5,0.1,0.0) |
| **Start alpha** | 0.8 | 0.7 | 0.9 | 1.0 |
| **End alpha** | 0.0 | 0.0 | 0.0 | 0.0 |
| **Blend mode** | Additive | Additive | Additive | Additive |
| **Turbulence** | 0.5 | 0.3 | 0.8 | 1.5 |
| **Light radius** | 5m | 2m | 10m | 20m |
| **Light color** | (1.0,0.6,0.2) | same | same | (1.0,0.5,0.1) |
| **Light flicker** | 0.15 | 0.20 | 0.10 | 0.08 |

### Ember Particles
```
Count: 5-20 per frame (scale with fire size)
Speed: 1-5 m/s upward + random XZ scatter
Lifetime: 1-4 seconds
Size: 0.002-0.008m
Color: (1.0, 0.6, 0.1) fading to (0.8, 0.2, 0.0)
Alpha: 1.0 → 0.0 (fade out as they cool)
Gravity: -0.5 m/s² (reduced — hot air carries them)
Blend: Additive
```

### Smoke Particles (above flame)
```
Count: 10-30 per frame
Speed: 1-3 m/s upward (slower than flame)
Lifetime: 3-8 seconds
Start size: 0.2m, End size: 2.0m (expands as it rises)
Color: (0.15, 0.15, 0.15) (dark grey)
Alpha: 0.4 → 0.0
Blend: Alpha (NOT additive — smoke blocks light)
Turbulence: high (0.8-1.5)
```

---

## PART 5: DUST AND SAND

### Footstep Dust
```
Trigger: on player/NPC foot-down event
Count: 10-20 particles per step
Speed: 0.5-2.0 m/s (outward + slight upward)
Lifetime: 0.5-1.5s
Size: 0.02-0.10m (growing)
Color: matches terrain (sand=(0.5,0.4,0.25), dirt=(0.2,0.15,0.08))
Alpha: 0.3 → 0.0
Wind affected: yes (drift with wind)
```

### Dust Storm
```
Particles/frame: 2000-5000
Spawn area: 100×100m around player
Speed: 5-15 m/s (horizontal, matches wind)
Lifetime: 3-8s
Size: 0.05-0.30m
Color: terrain-derived, desaturated (0.5,0.4,0.3)
Alpha: 0.2-0.5
Visibility: 20-100m (heavy fog + particles)
Screen effect: grainy noise overlay
Audio: howling wind at 0.8-1.0 volume
```

---

## PART 6: EXPLOSION EFFECTS

### Standard Explosion (grenade/barrel)
```
Phase 1 — Flash (0-0.05s):
  Additive white-yellow sphere, 0.5-2.0m radius
  Fullscreen flash: 0.1 intensity, 0.05s duration

Phase 2 — Fireball (0-0.5s):
  50-100 fire particles (see fire system, scaled up)
  Expanding sphere of hot gas
  Light: 20m radius, (1.0, 0.7, 0.2), 5.0 intensity, rapid falloff

Phase 3 — Smoke (0.2-5.0s):
  100-200 smoke particles
  Rising mushroom shape
  Expands from 1m to 8m over 3 seconds

Phase 4 — Debris (0-3.0s):
  20-50 small chunks (physics-driven)
  Speed: 5-20 m/s outward
  Bounce on terrain
  Dust trail behind each chunk

Phase 5 — Shockwave (0-0.3s):
  Expanding ring decal on ground: 0 to 5m radius in 0.3s
  Distortion post-process ring expanding from center

Screen shake: 0.05-0.15m amplitude, 0.5s duration, distance-attenuated
Audio: explosion sound at 0.8-1.0 volume, 100m range
```

---

## PART 7: WATER EFFECTS

### Waterfall Mist
```
Particles/frame: 100-300
Spawn: at base of waterfall
Speed: 0.5-2.0 m/s (outward + slight upward)
Lifetime: 2-5s
Size: 0.1-0.5m (growing)
Color: (0.8, 0.85, 0.9, 0.15) — very faint white
Affected by wind: yes
```

### Ship Engine Exhaust (in atmosphere)
```
Particles/frame: 50-200
Speed: 10-30 m/s (backward from engine)
Lifetime: 1-3s
Start size: 0.5m (engine bell radius)
End size: 3.0m
Color: (0.8, 0.85, 0.95) → transparent (condensation trail)
In vacuum: no particles (space has no exhaust trail)
Engine glow: additive emission at nozzle (0.5, 0.7, 1.0) × thrust
```

---

## PART 8: FUR / HAIR (Shell-based or Strand-based)

### Shell-Based Fur (Performance)
```
Layers: 16-32 shells (concentric mesh copies offset along normals)
Shell spacing: 0.0005-0.002m per layer
Total fur length: spacing × layer count (0.008-0.064m)
Alpha: noise texture masks each shell (creates strand appearance)
Color: darkens toward root (ambient occlusion)
Wind: offset upper shells more than lower
Pros: Fast, works on any mesh
Cons: Looks thin at silhouette
```

### Strand-Based Fur (Quality)
```
Strands per sq cm: 10-50 (varies by species)
Strand length: 0.005-0.050m (short fur) to 0.10m+ (long fur/mane)
Strand width: 0.0003-0.001m
Curve segments per strand: 3-8 (more = smoother)
Clumping: group 5-10 strands into clump, share same curve
Color variation: ±15% brightness per strand
Root color: darker (less light reaches root)
Tip color: lighter (sun-bleached)
Wind: strand tip responds more than root
Physics: Verlet chain per strand (or per clump)
```

---

## PART 9: MISCELLANEOUS EFFECTS

### Sparks (welding, grinding, electrical)
```
Count: 20-100 per burst
Speed: 3-15 m/s
Lifetime: 0.3-1.5s
Size: 0.001-0.005m
Color: (1.0, 0.8, 0.3) → (0.8, 0.3, 0.0) (cooling)
Gravity: -9.81 m/s² (full gravity — sparks are solid particles)
Bounce: 0.3 restitution on floor (sparks bounce and skitter)
Trail: 0.02m motion-blur streak
Blend: Additive
```

### Leaves (falling from trees)
```
Count: 1-5 per second per tree (autumn × 10)
Speed: 0.5-2.0 m/s downward
Lateral drift: ±1.0 m/s (tumbling)
Rotation: random tumble on all axes (1-3 rev/s)
Lifetime: 3-10s (until ground)
Size: 0.02-0.05m
Color: season-dependent (green → yellow → orange → brown)
Ground accumulation: persist as ground clutter
```

### Breath Vapor (cold environment)
```
Trigger: every exhale cycle (every 3-4 seconds)
Count: 5-15 per exhale
Speed: 0.3-0.8 m/s (forward from mouth)
Lifetime: 0.5-2.0s
Size: 0.01m → 0.08m (expands rapidly)
Color: (0.9, 0.92, 0.95, 0.2) → transparent
Temperature threshold: below 5°C (visible)
Colder = denser/longer lasting
```

---

## PART 10: INTEGRATION WITH PROCGEN CURRICULUM

**Step 2265 (Grass rendering)**: Use Part 1 grass presets by biome. Wind function from wind animation code.

**Step 220 (Weather particles)**: Rain (Part 2) and Snow (Part 3) presets by weather intensity.

**Step 716 (Fire propagation)**: Flame, ember, smoke parameters from Part 4.

**Step 726 (Weather particles in immersion)**: Screen wetness from rain table, screen frost from snow.

**Step 2760 (Rain on surfaces audio)**: Rain intensity maps to splash particle count and audio volume.

**Step 489 (Explosive barrels)**: Explosion phases from Part 6.

**Steps 2285-2290 (Fur/hair generation)**: Shell-based for distant creatures (cheap), strand-based for close/hero (quality). Parameters from Part 8.

**Step 2124 (Waterfall generation)**: Mist particles from Part 7.

**Step 2271 (Fern generation)**: Can use hair particle system for fronds.

---

*All six research sessions complete.*
