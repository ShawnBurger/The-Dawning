# THE DAWNING — Shader Recipe Bible
# Session 4: HLSL Recipes for Translucent Leaves, Skin SSS,
#             Wet Surfaces, Crystal Refraction, Iridescence,
#             Bioluminescence, Emissive Glow, Hologram
# Sources: Blender Principled BSDF, cgbookcase, Ben Simonds SSS,
#          Sébastien Lagarde wet surfaces, Wikipedia SSS

---

## RECIPE 1: TRANSLUCENT LEAVES

Leaves transmit light from behind (backlighting creates a warm green glow).
In Blender: Principled BSDF + Translucent BSDF through Add Shader.

```hlsl
// leaf_shader.hlsl
float3 LeafShading(float3 albedo, float3 normal, float3 lightDir,
                     float3 viewDir, float thickness, float translucency)
{
    // Front-face diffuse (standard Lambert)
    float NdotL = saturate(dot(normal, lightDir));
    float3 frontLight = albedo * NdotL;

    // Back-face translucency
    // Light passes THROUGH the leaf and exits the back
    // The thinner the leaf, the more light passes through
    float3 backNormal = -normal;
    float NdotL_back = saturate(dot(backNormal, lightDir));

    // Subsurface color: red/orange components survive longer through tissue
    // This is why backlit leaves glow warm yellow-green
    float3 sssColor = float3(
        albedo.r * 1.2,   // Red passes through easily
        albedo.g * 0.9,   // Green moderate
        albedo.b * 0.3    // Blue absorbed quickly
    );

    float3 backLight = sssColor * NdotL_back * translucency * (1.0 - thickness);

    // View-dependent wrap (softens the transition)
    float wrap = saturate(dot(normal, lightDir) * 0.5 + 0.5);

    // Fresnel on front face (waxy leaf surface)
    float fresnel = pow(1.0 - saturate(dot(normal, viewDir)), 4.0) * 0.15;

    return frontLight * wrap + backLight + fresnel;
}

// Typical values:
// albedo: (0.07, 0.14, 0.04) — dark green top
// thickness: 0.3 (thin leaf) to 0.8 (thick succulent)
// translucency: 0.3-0.6
// roughness: 0.50-0.70 (waxy top), 0.80-0.95 (matte bottom)
```

---

## RECIPE 2: SKIN SUBSURFACE SCATTERING

Three-layer skin: epidermis (thin, scatters minimally), dermis (pink/red from blood), 
subcutaneous (deep, orange-red scatter).

```hlsl
// skin_sss.hlsl — Screen-space SSS approximation
// Full random-walk SSS is offline-only; this is the real-time approximation

float3 SkinSSS(float3 albedo, float3 normal, float3 lightDir,
                 float3 viewDir, float3 sssRadius, float sssStrength,
                 float curvature)
{
    // Standard diffuse
    float NdotL = saturate(dot(normal, lightDir));

    // Pre-integrated SSS lookup (approximation)
    // The "wrap" lighting simulates light scattering under the surface
    // and appearing on the dark side of a curved surface
    float3 sssLight;
    float wrap = saturate(dot(normal, lightDir) * 0.5 + 0.5);

    // Curvature-dependent scatter: tight curves (nose, ears) scatter more
    // Curvature can be computed from screen-space normal derivatives:
    // float curvature = length(fwidth(normal));
    float scatterWidth = curvature * sssStrength;

    // Per-channel scatter based on radius
    // Red scatters farthest (3mm for skin), blue barely (0.4mm)
    sssLight.r = saturate(wrap + scatterWidth * sssRadius.r);
    sssLight.g = saturate(wrap + scatterWidth * sssRadius.g * 0.5);
    sssLight.b = saturate(wrap + scatterWidth * sssRadius.b * 0.25);

    // Combine with albedo
    float3 diffuse = albedo * sssLight;

    // Specular: dual-lobe (skin has oily surface layer + rough underlayer)
    float3 halfVec = normalize(lightDir + viewDir);
    float NdotH = saturate(dot(normal, halfVec));

    // Primary lobe: broad, rough (dermis)
    float spec1 = pow(NdotH, 20.0) * 0.3;
    // Secondary lobe: sharp, tight (oily surface)
    float spec2 = pow(NdotH, 200.0) * 0.7;
    float specular = spec1 + spec2;

    // Fresnel: skin reflects more at grazing angles
    float fresnel = pow(1.0 - saturate(dot(normal, viewDir)), 5.0);
    specular *= lerp(0.04, 1.0, fresnel);

    return diffuse + specular;
}

// Skin SSS profiles (from PBR Material Bible):
//                   Scatter Color         Scatter Radius (mm)   Strength
// Light skin:       (0.80, 0.30, 0.15)   (3.0, 1.0, 0.4)      0.5
// Medium skin:      (0.70, 0.25, 0.12)   (2.5, 0.8, 0.35)     0.5
// Dark skin:        (0.50, 0.20, 0.10)   (2.0, 0.6, 0.3)      0.4
// Alien (cool):     (0.40, 0.50, 0.70)   (2.0, 2.5, 3.0)      0.4
// Alien (warm):     (0.70, 0.40, 0.20)   (3.0, 1.5, 0.5)      0.5
```

---

## RECIPE 3: CRYSTAL / GLASS REFRACTION

Crystals bend light (refraction) and split it into colors (dispersion).
High IOR (1.5-2.4) and low roughness (0.0-0.1).

```hlsl
// crystal_shader.hlsl
float3 CrystalShading(float3 viewDir, float3 normal, float3 lightDir,
                         float3 crystalColor, float ior, float roughness,
                         float dispersion, TextureCube envMap)
{
    // Fresnel: how much reflects vs refracts (Schlick approximation)
    float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - saturate(dot(normal, viewDir)), 5.0);

    // Reflection (sharp for smooth crystals)
    float3 reflDir = reflect(-viewDir, normal);
    float mipLevel = roughness * 8.0;  // Rough = blurry reflection
    float3 reflection = envMap.SampleLevel(linearSampler, reflDir, mipLevel).rgb;

    // Refraction (light bends through crystal)
    float3 refractDir = refract(-viewDir, normal, 1.0 / ior);

    // Chromatic dispersion: split R, G, B at slightly different IORs
    float3 refractR = refract(-viewDir, normal, 1.0 / (ior - dispersion * 0.02));
    float3 refractG = refract(-viewDir, normal, 1.0 / ior);
    float3 refractB = refract(-viewDir, normal, 1.0 / (ior + dispersion * 0.02));

    float3 refraction;
    refraction.r = envMap.SampleLevel(linearSampler, refractR, mipLevel).r;
    refraction.g = envMap.SampleLevel(linearSampler, refractG, mipLevel).g;
    refraction.b = envMap.SampleLevel(linearSampler, refractB, mipLevel).b;

    // Tint refracted light by crystal body color
    refraction *= crystalColor;

    // Combine reflection and refraction via Fresnel
    float3 color = lerp(refraction, reflection, fresnel);

    // Internal sparkle: when light hits internal facets
    float sparkle = pow(saturate(dot(reflect(-lightDir, normal), viewDir)), 256.0);
    color += sparkle * 2.0 * crystalColor;

    return color;
}

// Crystal presets:
//              Color                  IOR    Roughness  Dispersion
// Quartz:      (0.95, 0.95, 0.95)   1.54   0.02       0.5
// Diamond:     (1.00, 1.00, 1.00)   2.42   0.00       2.0 (high!)
// Sapphire:    (0.20, 0.30, 0.90)   1.77   0.01       0.8
// Emerald:     (0.10, 0.70, 0.20)   1.58   0.02       0.6
// Ruby:        (0.80, 0.05, 0.10)   1.77   0.01       0.8
// Amethyst:    (0.50, 0.15, 0.65)   1.54   0.02       0.5
// Alien crystal: (0.30, 0.90, 0.70) 1.80   0.03       1.2
```

---

## RECIPE 4: IRIDESCENCE (Thin-Film Interference)

Soap bubbles, beetle shells, oil on water — color shifts with viewing angle.
Caused by thin-film interference where reflected wavelengths depend on film thickness and angle.

```hlsl
// iridescence.hlsl
float3 ThinFilmIridescence(float3 normal, float3 viewDir,
                               float filmThickness, float filmIOR)
{
    // Thin film interference creates angle-dependent color
    // Film thickness in nanometers (200-800nm for visible colors)
    float cosTheta = saturate(dot(normal, viewDir));

    // Optical path difference (simplified)
    float opd = 2.0 * filmIOR * filmThickness * cosTheta;

    // Convert optical path to wavelength-dependent interference
    // When opd = integer multiple of wavelength → constructive
    // When opd = half-integer multiple → destructive
    float3 color;
    color.r = 0.5 + 0.5 * cos(opd / 650.0 * 2.0 * PI); // Red: 650nm
    color.g = 0.5 + 0.5 * cos(opd / 530.0 * 2.0 * PI); // Green: 530nm
    color.b = 0.5 + 0.5 * cos(opd / 460.0 * 2.0 * PI); // Blue: 460nm

    return color;
}

// Usage: blend with base material
// float3 iridescentColor = ThinFilmIridescence(normal, viewDir, thickness, 1.4);
// float iridescentStrength = 0.3; // How much iridescence vs base color
// finalColor = lerp(baseColor, iridescentColor * baseColor, iridescentStrength * fresnel);

// Presets (film thickness in nm):
// Soap bubble:     200-600nm (shifts across surface), IOR 1.33
// Beetle shell:    400-500nm (relatively uniform), IOR 1.56
// Oil on water:    300-700nm (rainbow), IOR 1.47
// Abalone shell:   350-550nm (pearl-like), IOR 1.68
// Alien chitin:    250-800nm (dramatic shifts), IOR 1.60
```

---

## RECIPE 5: BIOLUMINESCENCE

Living organisms that emit light. The glow pulsates with biological rhythm.

```hlsl
// bioluminescence.hlsl
float3 BioluminescentGlow(float3 baseColor, float3 glowColor,
                              float glowIntensity, float time,
                              float pulseRate, float pulseDepth,
                              float3 worldPos, float noiseScale)
{
    // Pulsation: biological breathing/heartbeat rhythm
    float pulse = sin(time * pulseRate * 2.0 * PI) * 0.5 + 0.5;
    pulse = lerp(1.0 - pulseDepth, 1.0, pulse);

    // Spatial variation: glow isn't uniform across the organism
    float spatialNoise = fbm(worldPos * noiseScale, 3) * 0.5 + 0.5;

    // Final emission
    float3 emission = glowColor * glowIntensity * pulse * spatialNoise;

    // Bioluminescence also affects the diffuse — glowing areas appear lighter
    float3 diffuseBoost = baseColor * (1.0 + emission * 0.3);

    return diffuseBoost; // Add emission separately in the final composite
    // In the pixel shader output: emissive = emission;
}

// Presets:
//                  Glow Color              Intensity  Pulse Rate  Pulse Depth
// Firefly:         (0.70, 0.90, 0.20)     3.0        0.5 Hz      0.8
// Deep sea fish:   (0.10, 0.40, 0.90)     2.0        0.2 Hz      0.4
// Jellyfish:       (0.30, 0.80, 0.90)     1.5        0.3 Hz      0.6
// Fungi:           (0.20, 0.80, 0.30)     1.0        0.05 Hz     0.3  (very slow)
// Alien plant:     (0.60, 0.20, 0.90)     2.5        0.1 Hz      0.5
// Alien skin:      varies by species       1.0-4.0   0.1-2.0 Hz  0.2-0.8
// Crystal:         (0.50, 0.80, 1.00)     5.0        0.0 Hz      0.0  (constant)
```

---

## RECIPE 6: HOLOGRAM / HELIX DISPLAY

For ship HUD, station interfaces, and The Dawning's HELIX UI system.

```hlsl
// hologram.hlsl
float3 HologramEffect(float3 viewDir, float3 normal, float3 holoColor,
                         float time, float3 worldPos, float scanlineFreq)
{
    // Scanlines (horizontal interference lines)
    float scanline = sin(worldPos.y * scanlineFreq + time * 3.0) * 0.5 + 0.5;
    scanline = pow(scanline, 8.0); // Sharp lines

    // Flicker (random temporal noise)
    float flicker = 1.0 - frac(sin(time * 43758.5453) * 2.0) * 0.05;

    // Edge glow (Fresnel — edges of hologram glow brighter)
    float fresnel = pow(1.0 - saturate(dot(normal, viewDir)), 2.0);
    float edgeGlow = fresnel * 2.0;

    // Combine
    float3 color = holoColor * (0.3 + edgeGlow + scanline * 0.2) * flicker;

    // Alpha: holograms are transparent, edges more opaque
    // float alpha = saturate(0.3 + fresnel * 0.7 + scanline * 0.1);

    return color;
}

// HELIX UI preset:
// holoColor: (0.3, 0.7, 1.0) — blue-cyan
// scanlineFreq: 200.0 (tight lines)
// Alpha blending: additive
```

---

## RECIPE 7: DAMAGE/WEAR OVERLAY

Procedural damage state for ships, armor, structures.

```hlsl
float3 ApplyDamage(float3 baseColor, float roughness, float metallic,
                      float damageAmount, float3 worldPos)
{
    // Damage noise (where the damage appears)
    float damageNoise = fbm(worldPos * 3.0, 4);
    float damageMask = saturate((damageNoise - (1.0 - damageAmount)) * 5.0);

    // Scratches (fine directional lines)
    float scratches = abs(sin(worldPos.x * 50.0 + worldPos.z * 30.0));
    scratches = pow(scratches, 20.0) * damageAmount;

    // Scorching (dark burns around damage)
    float3 scorchColor = float3(0.02, 0.01, 0.005);
    float scorchMask = saturate((damageNoise - (1.0 - damageAmount * 1.2)) * 3.0);

    // Apply
    float3 result = lerp(baseColor, scorchColor, scorchMask * 0.8);
    result = lerp(result, float3(0.35, 0.15, 0.05), damageMask * metallic); // Rust on metal
    // Roughness increases with damage
    // roughness = saturate(roughness + damageAmount * 0.3 + scratches * 0.1);

    return result;
}
```

---

## INTEGRATION WITH PROCGEN CURRICULUM

**Step 2249 (Leaf generation)**: Use Recipe 1 leaf shader. Set thickness from species morphology.
**Step 2396 (Creature covering)**: Chitin covering → apply Recipe 4 iridescence. Bioluminescent flag → Recipe 5.
**Step 2398 (Bioluminescence)**: Recipe 5 with species-specific glow color and pulse rate from seed.
**Step 2254 (Crystalline trees)**: Recipe 3 crystal shader on trunk/branches.
**Step 2326 (Ship hull wear)**: Recipe 7 damage overlay, amount from ship age parameter.
**Step 272 (Cockpit HUD)**: Recipe 6 hologram for HELIX display elements.
**All skin rendering**: Recipe 2 SSS with profiles from PBR Material Bible.
