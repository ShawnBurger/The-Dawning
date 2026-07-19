# THE DAWNING — Audio Engine Deep Dive
# Batch 2, Topic 2: Spatial Audio, HRTF, Reverb Zones, Bus Architecture,
#                    Room Acoustics, Sound Propagation, Mixer Design
# Sources: Steam Audio, Wwise/FMOD docs, Meta VR audio guide, Springer VR acoustics

---

## PART 1: AUDIO BUS ARCHITECTURE

### Bus Hierarchy

```
Master Bus (final output, limiter at -1dBFS)
├── Music Bus (-6dB default)
│   ├── Combat Music
│   ├── Exploration Music
│   ├── Ambient Music
│   └── Cinematic Music
├── SFX Bus (0dB default)
│   ├── Weapons Bus
│   ├── Explosions Bus
│   ├── Ship Systems Bus
│   ├── UI Sounds Bus (2D, no spatialization)
│   ├── Footsteps Bus
│   └── Impacts Bus
├── Voice Bus (+3dB default, priority over SFX)
│   ├── Dialogue
│   ├── Radio Comms
│   └── AI Voice (ship computer)
├── Ambience Bus (-3dB default)
│   ├── Environment (wind, rain, machinery hum)
│   ├── Room Tone
│   └── Space Ambience
└── Reverb Return Bus
    ├── Early Reflections
    └── Late Reverb

// Each bus has: volume, mute, solo, send to reverb, compressor, EQ
// Voice bus has sidechain compressor ducking SFX bus by -6dB when active
// (dialogue is always audible over sound effects)
```

### Voice Priority System
```
64 simultaneous voices maximum (hardware/performance limit)
Priority assignment:
  Player weapon fire: priority 100 (never stolen)
  Dialogue: priority 95
  Explosions nearby: priority 90
  Ship alerts/alarms: priority 85
  Enemy weapons: priority 80
  Footsteps (player): priority 70
  Ambient SFX: priority 50
  Distant combat: priority 40
  Environment detail: priority 30
  Music layers: priority 20 (managed separately)

Voice stealing: lowest priority voice is stopped when a higher priority
sound needs to play and all 64 slots are occupied.
Fade-out on steal: 50ms crossfade (prevents click)
```

---

## PART 2: 3D SPATIALIZATION (HRTF)

Head-Related Transfer Functions encode how sound reaches each ear differently
depending on direction, creating the illusion of 3D positioning through headphones.

### HRTF Implementation

```cpp
struct HRTFData {
    // Impulse response per direction (typically 25×50 = 1250 directions)
    // Each IR: 128-512 samples at 48kHz
    int azimuthSteps;    // 25 (every ~14°)
    int elevationSteps;  // 50 (every ~3.6°)
    int irLength;        // 128-512 samples
    float* leftEar;      // [azimuth * elevation * irLength]
    float* rightEar;     // Same size
};

// Apply HRTF to mono source
void SpatializeHRTF(const float* monoInput, float* stereoOutput,
                       int sampleCount, const HRTFData& hrtf,
                       float azimuth, float elevation)
{
    // Find nearest HRTF direction and interpolate
    int azIdx = (int)((azimuth + PI) / TWO_PI * hrtf.azimuthSteps) % hrtf.azimuthSteps;
    int elIdx = (int)((elevation + HALF_PI) / PI * hrtf.elevationSteps);
    elIdx = std::clamp(elIdx, 0, hrtf.elevationSteps - 1);
    
    int irOffset = (azIdx * hrtf.elevationSteps + elIdx) * hrtf.irLength;
    
    // Convolve mono input with left and right ear IRs
    Convolve(monoInput, sampleCount, &hrtf.leftEar[irOffset], hrtf.irLength, 
             &stereoOutput[0], 2); // Left channel (stride 2 for interleaved)
    Convolve(monoInput, sampleCount, &hrtf.rightEar[irOffset], hrtf.irLength,
             &stereoOutput[1], 2); // Right channel
}

// For speakers (non-headphones): use amplitude panning instead of HRTF
// Left/right balance: pan = atan2(dir.x, dir.z) / PI  (-1 = left, +1 = right)
```

### Distance Attenuation Models

```
Linear:     volume = 1 - (distance / maxDistance)
Inverse:    volume = refDistance / distance  (most natural)
Logarithmic: volume = 1 - log(distance/refDistance) / log(maxDistance/refDistance)
Custom curve: artist-defined spline (most control)

Recommended for The Dawning:
  Weapons/impacts: inverse falloff, refDistance=1m, maxDistance=200m
  Ship engines: logarithmic, refDistance=5m, maxDistance=2000m
  Voice/dialogue: inverse, refDistance=1m, maxDistance=30m
  Ambience: linear, maxDistance=50m (sharp cutoff at boundary)
  Explosions: inverse, refDistance=10m, maxDistance=5000m
```

---

## PART 3: ROOM ACOUSTICS AND REVERB

### Reverb Presets by Environment

| Environment | RT60 (s) | PreDelay (ms) | HF Damp | Wet Mix | Room Size |
|---|---|---|---|---|---|
| **Cockpit (small)** | 0.3 | 2 | 0.8 | 0.15 | 3m |
| **Ship corridor** | 0.8 | 5 | 0.6 | 0.25 | 15m |
| **Ship bridge** | 1.2 | 10 | 0.5 | 0.30 | 30m |
| **Ship engine room** | 1.5 | 8 | 0.4 | 0.35 | 40m |
| **Ship cargo bay** | 2.0 | 15 | 0.4 | 0.40 | 60m |
| **Station hangar** | 3.0 | 25 | 0.3 | 0.45 | 200m |
| **Station concourse** | 2.5 | 20 | 0.35 | 0.40 | 150m |
| **Cave** | 3.5 | 30 | 0.2 | 0.50 | 100m |
| **Planet surface** | 0.0 | 0 | 1.0 | 0.00 | Open |
| **Planet forest** | 0.4 | 5 | 0.7 | 0.10 | Scattered |
| **Space (vacuum)** | 0.0 | 0 | 1.0 | 0.00 | N/A |
| **Underwater** | 1.0 | 3 | 0.1 | 0.60 | Variable |

```
RT60: time for reverb to decay 60dB (longer = bigger/more reflective space)
PreDelay: time before first reflection (listener-to-wall distance / speed of sound)
HF Damp: high-frequency absorption (0=bright reverb, 1=muffled)
  Metal walls: HF Damp 0.3 (very reflective)
  Concrete: HF Damp 0.5
  Fabric/carpet: HF Damp 0.8 (absorbs highs)
  Open air: HF Damp 1.0 (no reflections)
```

### Sabine Equation (RT60 from room geometry)

```cpp
// RT60 = 0.161 × Volume / (Surface_Area × avg_absorption_coefficient)
float ComputeRT60(float roomVolume_m3, float surfaceArea_m2, float avgAbsorption) {
    return 0.161f * roomVolume_m3 / (surfaceArea_m2 * avgAbsorption + 0.001f);
}

// Absorption coefficients by material (at 1kHz):
// Metal:     0.03  (very reflective)
// Concrete:  0.05
// Glass:     0.04
// Wood:      0.10
// Carpet:    0.30
// Fabric:    0.50
// Foam:      0.80  (acoustic treatment)
// Open door: 1.00  (perfect absorption = sound escapes)
```

### Sound in Vacuum (Space)

```
In space, there is no medium for sound to propagate through.
The Dawning handles this with:

1. Outside ship in space: NO environmental sounds
   - Player hears only: suit systems, breathing, radio comms
   - Hull vibrations transmitted through contact (mag-boots on hull)
   - Muffled thuds when touching surfaces (bone conduction simulation)

2. Inside ship: normal sound propagation
   - Hull breach: gradual loss of sound as atmosphere vents
   - Low atmosphere: sounds become thin, lose bass frequencies
   - Full vacuum interior: same rules as outside

3. Gameplay override: weapon sounds in space
   - Option A (realistic): silent weapons, visual-only feedback
   - Option B (cinematic): synthesized "internal hull sensor" sounds
   - Player setting: "Realistic Audio" vs "Cinematic Audio"
   
4. Ship-to-ship: radio chatter only (no sound propagation between ships)
```

---

## PART 4: SOUND PROPAGATION

### Occlusion (Wall Between Source and Listener)

```cpp
// Cast ray from source to listener
// If ray hits wall: apply low-pass filter based on wall material
struct WallOcclusion {
    float transmissionLoss_dB;  // How much volume is lost
    float lowPassCutoff_Hz;     // High frequencies blocked more
};

// Wall materials (from our EnvironmentalAudioPropagation system):
// Thin metal:  6dB loss, cutoff 2000Hz
// Thick hull: 15dB loss, cutoff 500Hz
// Glass:       3dB loss, cutoff 4000Hz
// Wood:        8dB loss, cutoff 1500Hz
// Concrete:   12dB loss, cutoff 800Hz
// Fabric:      2dB loss, cutoff 6000Hz
// Crystal:     1dB loss, cutoff 8000Hz (nearly transparent to sound)
```

### Portals (Sound Through Doorways)

```
When a door/opening connects two rooms:
1. Sound from Room A reaches the portal
2. Portal acts as a new point source in Room B
3. Portal source volume = original volume × portal area / (4π × distance²)
4. Room B's reverb applies to the portal source

Open door: full transmission
Half-open door: -6dB
Closed door: material-dependent occlusion
Sealed airlock: -40dB (nearly silent)
```

### Doppler Effect

```cpp
float DopplerShift(float sourceSpeed, float listenerSpeed, float speedOfSound) {
    // f_observed = f_source × (speedOfSound + listenerSpeed) / (speedOfSound + sourceSpeed)
    // Positive speed = moving toward (higher pitch)
    // Negative speed = moving away (lower pitch)
    float ratio = (speedOfSound + listenerSpeed) / (speedOfSound + sourceSpeed + 0.001f);
    return std::clamp(ratio, 0.5f, 2.0f);  // Clamp to ±1 octave
}

// Speed of sound by medium (from our audio propagation system):
// Air (sea level):    343 m/s
// Water:             1481 m/s
// Thin atmosphere:    280 m/s
// Thick atmosphere:   400 m/s
// Metal (hull):      5100 m/s (vibration, not air)
// Vacuum:               0 m/s (no propagation)
```

---

## PART 5: PROCEDURAL AUDIO

### Engine Sound Synthesis
```
Ship engines don't use looped recordings — they're synthesized:
1. Base tone: sawtooth oscillator at engine RPM frequency
   Idle: 40Hz, Cruise: 80Hz, Full thrust: 150Hz, Afterburner: 200Hz+
2. Harmonics: 2×, 3×, 4× base frequency at decreasing amplitude
3. Turbulence noise: filtered white noise, amount increases with throttle
4. Resonance: bandpass filter at hull resonant frequency (ship-size dependent)
5. Doppler: pitch shifts with relative velocity
6. Distance: bass frequencies travel further (low-pass with distance)

Parameters that affect engine sound:
- Throttle (0-1): pitch + volume + noise amount
- Engine health (0-1): add crackle/sputter when damaged
- Fuel type: changes harmonic content
- Engine size: larger = deeper base frequency
```

### Impact Sound Generation
```
Surface material → impact character:
  Metal on metal: bright, ringing (2-8kHz peak, long decay)
  Metal on rock: dull thud + scrape (200Hz-2kHz, short decay)
  Boot on deck: solid thump (100-500Hz, very short)
  Glass break: high shattering (4-12kHz burst + tinkle tail)
  Explosion: low boom + debris rattle (20-200Hz boom, 1-8kHz debris)

Velocity → volume + pitch:
  volume = clamp(impactVelocity / 10.0, 0.1, 1.0)
  pitch = 1.0 + (impactVelocity - 5.0) * 0.02  (faster = slightly higher)
  
Mass → character:
  Heavy (>100kg): more bass, longer sustain
  Light (<5kg): more treble, shorter sustain
```

---

## PART 6: CURRICULUM STEP UPDATES

**Step 401-410 (Audio foundation)**: XAudio2 initialization. Master voice, submix voices for bus hierarchy. 64 source voices. Ring buffer for streaming.

**Step 411-420 (3D spatialization)**: Distance attenuation (inverse model). Simple left/right panning. Doppler effect from relative velocity.

**Step 421-430 (HRTF)**: Load SOFA file with HRTF data. Convolve mono sources with direction-dependent IR pair. Bilinear interpolation between nearest HRTF directions.

**Step 431-440 (Reverb)**: Feedback delay network (FDN) reverb. 4-8 delay lines with allpass filters. Parameters from room preset table. Zone-based reverb switching with crossfade.

**Step 441-450 (Occlusion)**: Raycast from source to listener. Apply low-pass filter per wall hit. Portal propagation for doorways.

**Step 451-460 (Music system)**: Adaptive music with horizontal layers (instruments added/removed by intensity). Vertical transitions (crossfade between combat/explore/ambient tracks). Beat-synced transitions.

**Step 461-470 (Procedural audio)**: Engine synthesis from oscillators. Impact sounds from material + velocity + mass. Ambient soundscapes from biome parameters.

**Step 1431-1530 (Audio depth)**: Full convolution reverb with measured IRs. Ambisonics rendering for surround. Opus voice codec integration. HRTF personalization from head measurements. GPU-accelerated ray tracing for sound propagation (Steam Audio-style).
