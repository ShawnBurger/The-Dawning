# Atmospheric flight — implementation reference for the sim

Companion to `RELATIVISTIC_SIM_ARCHITECTURE.md` §6 (atmosphere) and the
deliberately-thin §6 of `PHYSICS_RESEARCH_REFERENCE.md`, which flagged
atmospheric flight as the one research angle the prior pass under-covered and
said "do not implement from memory without the harness pass." **This document is
that pass.** It supersedes `PHYSICS_RESEARCH_REFERENCE.md` §6 with the
load-bearing equations, their domains of validity, the exact constants in SI, the
numerical pitfalls, the game-fidelity precedent, and the analytic test cases with
negative controls.

Conventions inherited from `CLAUDE.md` RULES and the shipped code, unchanged
here: world positions are `Vec3d`, narrowed to `Vec3f` only after camera
subtraction; the coordinate system is **left-handed, +Z forward**; physics runs
at a **fixed timestep** (default 60 Hz) through the accumulator; the pose owner is
`sim::IntegrateRigidBody` (`src/sim/rigid_body.h`), which takes an
`externalForce` (`Vec3d`, WORLD frame) and an `externalTorque` (`Vec3f`, BODY
frame). **Atmosphere adds nothing to that integrator.** It is a set of external
force/torque terms summed into the hooks gravity already uses (§7 argues this is
not a separate regime). All quantities are SI: metres, kilograms, seconds,
kelvin, pascals, newtons.

## Epistemic status

Every load-bearing formula below carries its source. Confidence tags mirror the
companion docs:

- **[PRIMARY]** — taken from a named authoritative primary source (a NASA
  technical report, the US Standard Atmosphere 1976 definition, or a
  peer-reviewed correlation) that this pass fetched and read in full.
- **[SOURCED]** — a standard aerodynamics/astrodynamics textbook result
  (Anderson, Vallado, NASA Glenn *Beginner's Guide to Aeronautics*) cross-checked
  against a fetched source, but where the primary derivation was not read
  line-by-line.
- **[FLAGGED]** — a specific numeric or claim where the popular form and the
  primary source **disagree in provenance**; called out explicitly rather than
  papered over. Two of these matter (the Sutton–Graves SI constant, §4; and the
  Children-of-a-Dead-Earth precedent, §8).

Sources are collected at the end.

---

## 1. Atmosphere density models

### 1.1 Isothermal exponential model (the cheap default)

Integrate hydrostatic equilibrium `dP/dh = −ρ·g` with the ideal gas law
`P = ρ·R·T` under the assumption that temperature `T` is **constant** (isothermal)
and `g` is constant. The result is a single exponential in altitude `h`:

```
rho(h) = rho0 · exp(−h / H)                       // [SOURCED]  isothermal barometric
H      = R · T / g0                                // scale height (metres)
```

- `rho0 = 1.225 kg/m^3` — sea-level density (USSA76). **[PRIMARY]**
- `R    = 287.053 J/(kg·K)` — specific gas constant for **air**
  (`R_universal / M = 8314.32 / 28.9644`). **[PRIMARY]**
- `g0   = 9.80665 m/s^2` — standard gravity. **[PRIMARY]**
- At the sea-level temperature `T0 = 288.15 K`,
  `H = 287.053·288.15/9.80665 = 8434.5 m ≈ 8.4 km`. This is the **pressure**
  scale height. The commonly quoted "H ≈ 8.5 km for Earth" is this value; a
  single-exponential fit to the *density* profile over 0–100 km is often taken
  nearer 7–7.3 km because the lapse rate makes true density fall slightly faster
  than a pure isothermal. **Pick one and document which** — do not silently mix
  8.4 and 7.2 in the same body.

**Domain of validity.** Exact only where `T` and `g` are truly constant, i.e.
locally. Good to ~10–20 % over a scale height; useful for the whole atmosphere
only as a coarse approximation. Its virtue is that it is **C-infinity, monotone,
and cheap**, and it generalises to any body by supplying `(rho0, H)` — the only
practical choice for non-Earth bodies whose layered structure is unknown.

### 1.2 US Standard Atmosphere 1976 (the reference model)

USSA76 divides the atmosphere into layers of **constant temperature lapse rate**
`L = dT/dh` and integrates the same hydrostatic + ideal-gas system per layer.
Below ~86 km the seven base layers below are all that is needed; above 86 km the
model switches to number-density/diffusion tables and is not needed for flight.

**All altitudes here are GEOPOTENTIAL altitude `h`** (USSA76 defines its layers in
geopotential altitude, which folds the `1/r^2` weakening of gravity into a
constant-`g0` formulation). Convert from geometric (true) altitude `z` with the
Earth geopotential radius `r0 = 6,356,766 m`: **[PRIMARY]**

```
h = r0 · z / (r0 + z)                              // geometric z  ->  geopotential h
```

The seven base layers (base geopotential altitude, base temperature, lapse
rate), **[PRIMARY]** — verbatim from the USSA76 definition (Hawley's transcription
of the standard, cross-checked against the barometric outputs in §1.3):

| Layer | h_b (m) | T_b (K) | L (K/m)  | Name          |
|:-----:|--------:|--------:|---------:|:--------------|
|  0    |       0 | 288.15  | −0.0065  | Troposphere   |
|  1    |  11,000 | 216.65  |  0.0     | Tropopause    |
|  2    |  20,000 | 216.65  | +0.0010  | Stratosphere  |
|  3    |  32,000 | 228.65  | +0.0028  | Stratosphere  |
|  4    |  47,000 | 270.65  |  0.0     | Stratopause   |
|  5    |  51,000 | 270.65  | −0.0028  | Mesosphere    |
|  6    |  71,000 | 214.65  | −0.0020  | Mesosphere    |
| top   |  84,852 | 186.87  |  —       | (86 km geom.) |

Each `T_b` is the previous layer's top temperature, so the table is
self-consistent: e.g. `T_1 = 288.15 − 0.0065·11000 = 216.65 K`;
`T_3 = 216.65 + 0.0010·12000 = 228.65 K`;
`T_6 = 270.65 − 0.0028·20000 = 214.65 K`. Base 0 is `T0 = 288.15 K`.

Base pressures at the layer boundaries (geopotential), computed once at init and
tabulated so any query is a single-layer evaluation: **[PRIMARY]**

| h_b (m) | P_b (Pa)   |
|--------:|-----------:|
|       0 | 101 325    |
|  11,000 | 22 632.1   |
|  20,000 | 5 474.89   |
|  32,000 | 868.019    |
|  47,000 | 110.906    |
|  51,000 | 66.9389    |
|  71,000 | 3.95642    |

### 1.3 State variables versus altitude

Within layer `n` (base `h_b, T_b, P_b`, lapse `L`): **[PRIMARY]**

```
T(h) = T_b + L · (h − h_b)                                    // temperature, always linear

// pressure — two cases, kept separate:
if L != 0:   P(h) = P_b · ( T(h) / T_b )^( −g0 / (R·L) )      // power law (non-isothermal layer)
if L == 0:   P(h) = P_b · exp( −g0 · (h − h_b) / (R·T_b) )    // exponential (isothermal layer)

rho(h) = P(h) / (R · T(h))                                    // density, ideal gas
a(h)   = sqrt( gamma · R · T(h) ) = sqrt( gamma · P(h) / rho(h) )   // speed of sound
gamma  = 1.4                                                  // ratio of specific heats, air (diatomic)
```

Sanity anchors from the standard (geopotential): sea level `T=288.15 K,
P=101325 Pa, rho=1.225 kg/m^3, a=340.294 m/s`; tropopause 11 km `T=216.65 K,
P=22632 Pa, rho=0.3639 kg/m^3, a=295.07 m/s`. The two `P` branches must **agree
at the layer boundary by construction** — the `L≠0` branch evaluated at `h=h_b`
returns `P_b` (the `(T/T_b)` ratio is 1), and the `L=0` branch likewise, so the
piecewise profile is **C0 continuous** across every boundary. It is *not* C1 (the
lapse rate jumps), which is physically correct and harmless.

Dynamic viscosity, if ever needed for Reynolds number, is Sutherland's law:
`mu(T) = beta·T^1.5 / (T + S)`, `beta = 1.458e-6 kg/(m·s·K^0.5)`, `S = 110.4 K`,
giving `mu0 = 1.789e-5 kg/(m·s)` at sea level. **[PRIMARY]**

### 1.4 What games actually use, and the recommendation

**Recommendation for this engine: layered-USSA76 for Earth-like bodies, the
exponential model everywhere else.** The rationale is precision-vs-cost:

- USSA76 is exact-to-standard and only marginally more expensive than the
  exponential once the seven boundary pressures are precomputed — every query is
  one branch, one `pow`/`exp`, one divide. It gives the correct temperature
  profile, which *matters* because the speed of sound (§3) and hence Mach (§2)
  are functions of `T`, and Cd is a function of Mach.
- The exponential model is the honest choice for a body whose real profile is
  unknown (any fictional or poorly-characterised planet): you cannot invent seven
  lapse-rate layers you have no data for, but you can fit `(rho0, H)`.
- Kerbal Space Program's stock model is a **per-body pressure/temperature curve
  keyframed against altitude** — effectively a tabulated layered model, not a
  pure exponential — precisely so temperature (and thus reentry heating and Mach)
  is available. This is the pattern to follow: a per-body density/temperature
  model behind one interface, USSA76-parameterised for Earth.

Implement both behind a single `AtmosphereModel` interface returning
`{T, P, rho, a}` from geopotential altitude, so the drag/lift/heating code (§2–4)
is agnostic to which model a body uses.

---

## 2. Aerodynamic forces

### 2.1 Dynamic pressure and the coefficient form

The relative airspeed drives everything. The atmosphere co-rotates with the
planet, so the aerodynamically relevant velocity is relative to the local air:

```
v_atm = omega_planet × r          // co-rotating atmosphere velocity (planetocentric)
v_rel = v_body − v_atm            // airspeed vector
q     = 0.5 · rho · |v_rel|^2     // [SOURCED]  dynamic pressure, Pa
```

Drag and lift are then coefficient × dynamic pressure × reference area
(NASA Glenn *Beginner's Guide to Aeronautics*, drag- and lift-coefficient pages):
**[SOURCED]**

```
F_drag = − q · Cd · A · v_hat_rel                 // opposes airspeed, along −v_rel
F_lift =   q · Cl · A · l_hat                      // perpendicular to v_rel
```

where `v_hat_rel = v_rel / |v_rel|`, `A` is the **reference area**, and `l_hat` is
the unit lift direction (⊥ `v_rel`, in the plane spanned by `v_rel` and the body's
lift axis). NASA Glenn's explicit forms are `Cd = D/(q·A)` and `Cl = L/(q·A)`,
i.e. `D = Cd·A·½·rho·V^2`. **The reference area is a convention baked into the
coefficient**: frontal area for a blunt body, wing planform for a lifting body.
NASA Glenn is explicit that "the choice of reference area … will affect the
numerical value of the drag coefficient" and it must be reported alongside Cd.
**Store `A` and the `Cd`/`Cl` curves against the same reference area, per body.**

### 2.2 Drag coefficient versus Mach, Reynolds, angle of attack

`Cd` is not a constant. Its dependence on Mach number `M` (§3) is the dominant
effect for anything transonic or faster, and it has a characteristic shape
(NASA Glenn; braeunig/Anderson supersonic-sphere data): **[SOURCED]**

- **Subsonic (M < ~0.7):** `Cd` roughly flat, mild rise at very low speed from
  boundary-layer/viscous effects.
- **Transonic drag rise (M ~ 0.7 → 1.2):** `Cd` climbs steeply — local pockets of
  supersonic flow form shock waves that add **wave drag**. `Cd` peaks near
  **M = 1.0**. This is the "drag-divergence" / "sound barrier" region; the
  drag-divergence Mach number is where `dCd/dM` first spikes.
- **Supersonic (M ~ 1.2 → 5):** past the peak `Cd` **falls** again as the shock
  system stabilises; for many bodies it continues declining toward a plateau.
- **Hypersonic (M > ~5):** `Cd` tends to a roughly Mach-independent continuum
  value (a blunt body settles near a modified-Newtonian `Cd ≈ 0.9–1.0` on frontal
  area). Real-gas dissociation lowers the effective `gamma`, but for the sim
  `gamma = 1.4` is the standard modelling choice.

Reynolds number `Re = rho·|v_rel|·Lref/mu` sets the viscous/laminar-turbulent
behaviour; NASA Glenn notes matching `Re` is "even more important" than Mach for
low-speed correspondence, but for a space-sim its effect is second order behind
Mach and can be folded into the `Cd(M)` curve for the flight regime of interest.

Angle of attack `alpha` (the angle between `v_rel` and the body axis) modulates
both `Cd` and `Cl`. Implement `Cd` and `Cl` as **1-D lookups against Mach with an
angle-of-attack multiplier**, or a small 2-D `(M, alpha)` table — this is exactly
what flight sims and Orbiter do (§8). A useful closed-form default for a slender
body is `Cd(alpha) ≈ Cd0 + k·sin^2(alpha)` (crossflow drag rises with AoA).

### 2.3 Lift coefficient, angle of attack, stall, and L/D

For a thin airfoil the lift coefficient is **linear in angle of attack** up to
stall, with the thin-airfoil-theory slope (Anderson, *Fundamentals of
Aerodynamics*): **[SOURCED]**

```
Cl(alpha) = Cl_alpha · (alpha − alpha_0)          // linear region
Cl_alpha  = 2·pi  per radian  (≈ 0.11 per degree)  // thin-airfoil ideal; real wings a bit less
```

- `alpha_0` is the zero-lift angle (0 for a symmetric section).
- The linear region holds to the **stall angle** (~12–16° for typical airfoils),
  where flow separates, `Cl` reaches `Cl_max` and then **drops sharply**. Beyond
  stall `Cl` collapses and `Cd` rises steeply. Model stall as a capped/rolled-off
  `Cl(alpha)` curve — never let `Cl` grow linearly without bound.
- Finite (3-D) wings have reduced slope from wingtip downwash; capsules and
  rockets are low-`Cl`, high-`Cd` bodies whose "lift" is small and mostly a
  steering vector.
- **Lift-to-drag ratio `L/D = Cl/Cd`** is the single number that characterises how
  much a body can glide/steer; it governs the reentry style (§5). A capsule has
  `L/D ~ 0.3`, the Shuttle ~1 hypersonic / ~4.5 subsonic, a glider 20+.

### 2.4 The aerodynamic wrench into the rigid body

The total aerodynamic force `F_aero = F_drag + F_lift` acts at the **centre of
pressure (CoP)**, generally offset from the **centre of mass (CoM)**. That offset
produces a torque, and this is *why atmosphere is force-integrated and couples
orientation into translation* — the whole point of §6 of the architecture. In the
engine's frame conventions:

```
F_aero  (Vec3d, WORLD)  ->  summed into IntegrateRigidBody's externalForce
r_cp    = cop_body − com_body                     // lever arm, BODY frame
tau_body = r_cp × R^T · F_aero                     // torque about CoM, rotate F into body frame
tau_body (Vec3f, BODY)  ->  summed into externalTorque
```

This reuses the exact hooks gravity uses (VERIFIED against `rigid_body.h`:
`externalForce` is `Vec3d` world, `externalTorque` is `Vec3f` body). No new
integrator, no new pose owner.

**Body-frame vs wind-frame decomposition.** Drag and lift are naturally defined in
the **wind frame** (aligned with `v_rel`): drag along `−v_hat_rel`, lift ⊥ to it.
The **pitching moment** and the CoP location are naturally **body-frame**
quantities. The clean recipe: compute `q`, `alpha`, `Cd`, `Cl` in the wind frame;
build `F_aero` in world; transform the lever-arm cross product through the body
orientation for the torque. Do not conflate the two frames — a lift force applied
along a body axis instead of ⊥ to `v_rel` is a common and silent bug.

**Static stability (the pitching moment sign).** Whether the aerodynamic torque
*restores* or *diverges* attitude depends on the CoP–CoM ordering (NASA
launch-vehicle stability notes; NASA Glenn): **[SOURCED]**

- **CoP behind CoM (aft of it along the flight axis):** a disturbance in `alpha`
  produces a **restoring** moment that reduces `alpha` — **statically stable**.
  This is the rocket/dart configuration (fins move the CoP aft).
- **CoP ahead of CoM:** the moment **grows** the disturbance — **statically
  unstable** (tumbles). Aircraft phrase the same fact as "CoG must be ahead of the
  neutral point."

Because `r_cp × F_aero` is computed directly, static stability is **emergent**: a
body whose CoP is aft of its CoM will self-right in the sim with no special-case
code, and one whose CoP is forward will tumble — both correctly. This is a
free correctness check (test §9.6).

---

## 3. Mach number and speed of sound

```
M = |v_rel| / a                                   // [SOURCED]  Mach number
a = sqrt( gamma · R · T )                          // speed of sound, ideal gas
```

`a` depends **only on temperature** for an ideal gas (`gamma`, `R` fixed), so the
speed of sound — and therefore the Mach number for a given airspeed — is a direct
function of the atmosphere model's `T(h)`. This is the coupling that ties
aerodynamics to the density model: **you cannot compute Cd(M) without the
atmosphere's temperature**, which is the concrete reason to prefer USSA76's real
temperature profile over a bare exponential for Earth (§1.4). Worked anchors
(`gamma=1.4, R=287.053`): sea level `a = sqrt(1.4·287.053·288.15) = 340.3 m/s`;
tropopause/stratosphere (216.65 K) `a = 295.1 m/s`. A body at fixed airspeed is at
a **higher Mach at altitude** because the colder air has a lower `a` — so the
transonic drag rise (§2.2) can be triggered by climbing, not only by
accelerating.

---

## 4. Reentry aerodynamic heating

### 4.1 Sutton–Graves stagnation-point convective heating — provenance FLAGGED

The near-universal engineering form for stagnation-point **convective** heat flux
is:

```
q_dot_s = k · sqrt( rho_inf / R_N ) · V_inf^3       // convective stagnation heating (W/m^2)
k ≈ 1.7415e-4   (SI, Earth air)                     // W·m^-2 per (kg^0.5·m^-2·(m/s)^3)
```

with `rho_inf` free-stream density (kg/m^3), `R_N` nose radius (m), `V_inf`
airspeed (m/s). Heat flux **rises with the cube of velocity** and **falls with the
square root of nose radius** — the physical origin of the *blunt* reentry body: a
larger `R_N` spreads the shock and cuts stagnation heating. This form and the
constant `k = 1.74153e-4` for Earth are confirmed by the planar-reentry-equations
derivation (Wikipedia, citing the Allen–Eggers / Sutton–Graves lineage).
**[SOURCED]**

**FLAGGED — the primary source does not state this form.** This pass read
Sutton & Graves, NASA TR R-376 (1971), *A General Stagnation-Point
Convective-Heating Equation for Arbitrary Gas Mixtures*, in full. **TR R-376 does
not present `q = k·sqrt(rho/Rn)·V^3` and does not contain the number
`1.7415e-4`.** What it actually gives is a *heat-transfer coefficient* `K`:

```
K = q_dot_w · sqrt(R_N / p_s) / (h_s − h_w)          // TR R-376 eq. (33)  [PRIMARY]
K(air) = 0.1113  kg·s^-1·m^-3/2·atm^-1/2              // TR R-376 Table II / eq. (42), N_Pr,w = 0.69
```

where `p_s` is stagnation pressure, `h_s − h_w` the stagnation-to-wall enthalpy
difference. The popular `1.7415e-4` velocity-cubed form is the **derived
engineering reduction**: substitute the air `K`, approximate stagnation enthalpy
`h_s ≈ V^2/2` and stagnation pressure by Newtonian theory `p_s ≈ rho_inf·V^2`,
and the `sqrt(rho/Rn)·V^3` scaling and its lumped constant fall out. It is a
legitimate, widely-cited simplification (Vallado, Detra–Kemp–Riddell lineage,
every reentry-heating calculator), but **it is one derivation-step removed from
TR R-376, not a quote from it.** Implement the `1.7415e-4` cube form (it is what
the field uses and what the reentry-trajectory closed forms in §5 assume), but
label it as the derived Sutton–Graves *engineering correlation*, not as a verbatim
TR R-376 result. If per-gas fidelity is ever wanted, TR R-376's `K` table gives
coefficients for N2, O2, CO2, Ar, He, etc. (e.g. air 0.1113, CO2 0.1210, Ar
0.1495 kg·s^-1·m^-3/2·atm^-1/2) — the honest primary path for non-Earth
atmospheres.

### 4.2 Radiative heating and when it dominates

At very high entry velocity the shock layer becomes hot enough to **radiate**, a
second heating channel that TR R-376 explicitly excludes ("Radiative heating was
neglected in the present study"). The standard engineering correlation is
**Tauber–Sutton**, of the form `q_rad ∝ R_N · rho^a · f(V)` with a **very steep
velocity dependence** — radiative heat flux scales roughly as the **eighth power
of velocity**, versus the cube for convection (per the atmospheric-entry
literature). **[SOURCED]** Consequences:

- Radiative heating is **negligible below ~8–9 km/s** and dominates for
  **lunar/interplanetary return** speeds (~11 km/s and up).
- Its `R_N` dependence is the **opposite sign** of convective: radiation *grows*
  with nose radius (more hot gas in the shock layer), whereas convection *falls*
  with `R_N`. This is why very-high-speed entry bodies balance the two.
- **FLAGGED:** this pass did not fetch the exact Tauber–Sutton coefficients and
  velocity-function table. If radiative heating is implemented beyond the
  qualitative `V^~8` scaling, pull the constants from Tauber & Sutton (1991,
  *J. Spacecraft and Rockets*) directly — do not invent them.

### 4.3 Heat load, peak heating vs peak deceleration, ballistic coefficient

Two distinct extrema govern an entry, and they **do not coincide** (Allen–Eggers,
NASA TR 1381, 1958, via the planar-reentry closed form): **[SOURCED]**

- **Peak heating rate** `q_dot_max` occurs high and fast (thin air, high speed).
- **Peak deceleration** `a_max` (g-load) occurs lower and slower.
- The **total heat load** is the time integral `Q = ∫ q_dot dt` — the quantity a
  thermal-protection system must survive, distinct from the instantaneous peak.

The **ballistic coefficient** sets the whole trajectory:

```
beta = m / (Cd · A)                                // kg/m^2   [SOURCED]
```

A **high** `beta` (heavy, slender, low-drag) penetrates deep before decelerating —
steep, hot, brief. A **low** `beta` (light, blunt, high-drag) sheds speed high in
thin air — shallow, cooler, gentler. `beta` is the same quantity the architecture
measures elsewhere (RELATIVISTIC_SIM_ARCHITECTURE notes `beta ~ 1e-5` for
atmospheric bodies, meaning the aerodynamic term dwarfs relativistic coupling).

The **entry corridor** is the band of entry flight-path angles `gamma` that
simultaneously keeps `a_max` below the structural/crew limit (too steep → crushing
g and heating) and keeps the body from skipping back out (too shallow → skip-out
or overheat from a long soak). Allen & Eggers' foundational result: the heat load
is **inversely proportional to `Cd`** — a blunter body (higher `Cd`, lower `beta`)
takes *less* total heat, the counterintuitive reason reentry bodies are blunt.

---

## 5. Terminal velocity and equilibrium-glide / skip regimes

### 5.1 Terminal velocity (closed form — the anchor test)

For steady vertical fall, drag balances gravity `q·Cd·A = m·g`, giving:

```
v_terminal = sqrt( 2·m·g / (rho · Cd · A) )        // [SOURCED]
```

Equivalently `v_terminal = sqrt(2·g·beta/rho)` in ballistic-coefficient form. This
is the **primary analytic anchor** for verification (§9.1): a body released from
rest under constant `g` and quadratic drag approaches `v_terminal` monotonically,
with the exact transient `v(t) = v_terminal · tanh(t / tau)`,
`tau = v_terminal / g`. Both the limit and the `tanh` transient are closed-form
and testable to machine precision.

### 5.2 Planar reentry equations and the Allen–Eggers solution

The unpowered planar (2-D) entry equations of motion, with `gamma` the
flight-path angle below local horizontal and `sigma` the bank angle
(Wikipedia *Planar reentry equations*, standard astrodynamics): **[SOURCED]**

```
dV/dt     = − rho·V^2 / (2·beta) + g·sin(gamma)                              // speed
dgamma/dt = − (V·cos gamma)/r − (rho·V)/(2·beta)·(L/D)·cos(sigma) + (g·cos gamma)/V
dh/dt     = − V·sin(gamma)
```

The first term of `dV/dt` is drag deceleration; note it is `∝ 1/beta`. For a
**ballistic** entry (`L = 0`) into an exponential atmosphere, neglecting gravity
against drag, this integrates to the **Allen–Eggers closed form**: **[SOURCED]**

```
V(h) = V_atm · exp[ (rho0·H) / (2·beta·sin gamma) · exp(−h/H) ]     // gamma < 0 in descent
```

with peak deceleration magnitude, altitude, and the corresponding peak heating:

```
|a_max|  = V_atm^2 · |sin gamma| / (2·e·H)          // e = Euler's number; NOTE: independent of beta and Cd
h(a_max) = H · ln[ −rho0·H / (beta·sin gamma) ]
V(a_max) = V_atm · e^(−1/2)                          // ~60.6 % of entry speed, always
q_dot_max = k · sqrt[ −beta·sin gamma / (3·H·R_N·e) ] · V_atm^3
```

The famous Allen–Eggers insight, visible in the formula: **peak deceleration is
independent of the ballistic coefficient** — it depends only on entry speed,
flight-path angle, and scale height. `beta` shifts the *altitude* at which the
peak occurs, not its magnitude. This is a strong sanity check for any reentry
implementation.

### 5.3 Equilibrium glide and skip (qualitative)

- **Equilibrium glide:** a lifting body (`L/D > 0`) at a bank angle where the
  vertical lift component nearly balances gravity-minus-centrifugal descends
  slowly and shallowly, trading a long heated flight for low g-load and long
  cross-range. The Shuttle's S-turns bleed energy while holding this balance.
- **Skip reentry:** entering too shallow (or pitching lift up), the body's lift
  arcs it **back out of the atmosphere** into a suborbital hop before re-entering
  — extends range but risks a long thermal soak on each pass. It is the shallow
  edge of the entry corridor (§4.3).

These are emergent behaviours of the §5.2 equations with `L/D > 0` and a bank
angle, not separate code paths.

---

## 6. Numerical / implementation pitfalls

### 6.1 Drag is stiff at high dynamic pressure — the same instability class as the gyro term

The quadratic-drag velocity update, done explicitly, is
`v_{n+1} = v_n − (rho·Cd·A / (2m))·|v_n|·v_n·dt`. Define the drag response
timescale:

```
tau_drag = 2·beta / (rho · |v_rel|) = 2·m / (rho·Cd·A·|v_rel|)     // seconds
```

Explicit (forward / semi-implicit) Euler on this term is **stable only while
`dt < tau_drag`**; as `dt/tau_drag → 1` the update stops decelerating smoothly,
and past `dt/tau_drag > 2` the linearised update **overshoots through zero and
grows in magnitude — it injects energy**, exactly the failure mode Stage 1 hit
with the explicit gyroscopic term (NaN in ~1e4 steps; fixed by an implicit solve,
per `PHYSICS_RESEARCH_REFERENCE` §2). Drag reproduces that instability class:
non-conservative, and stiff whenever `tau_drag` shrinks below the fixed `dt`.

`tau_drag` is smallest — the stiff case — for a **low-`beta` body (light, high
drag: parachute, debris, blunt capsule) at high `rho` and high `|v_rel|`**, i.e.
near periapsis of a grazing entry or at low altitude. Worked example: a light
high-drag body `beta = 1 kg/m^2` at sea-level `rho = 1.2` and `|v| = 100 m/s` has
`tau_drag = 2/(1.2·100) = 0.017 s ≈ dt` at 60 Hz — right at the stability edge. A
heavy slender body (`beta = 1000`) is never stiff at those conditions. **The sim
must detect and handle the low-`beta` / high-`q` corner**, not assume 60 Hz is
always fine.

### 6.2 The fix: analytic / semi-implicit drag substep (operator splitting)

Do **not** apply drag with the same naive explicit update as everything else.
Split the step: advance gravity and thrust with the shipped symplectic Euler, and
advance **drag with an unconditionally-stable update**. Two standard options:

- **Analytic exponential update (best for the linear-drag limit).** For a linear
  drag law `F = −b·v` (or a quadratic law linearised over the substep with `b =
  rho·Cd·A·|v_n|/2`), the exact solution is
  `v(t) = v_terminal + (v_0 − v_terminal)·exp(−t/tau_drag)`. Stepping with this
  closed form is **exact for constant coefficients and unconditionally stable and
  monotone for any `dt`** — it can never overshoot or gain energy, because
  `exp(−dt/tau) ∈ (0,1]`. This is the exponential-integrator idea (the linear part
  is solved exactly); it is the drag analogue of the on-rails Kepler propagator
  that "cannot drift."
- **Semi-implicit frozen-speed update.** `v_{n+1} = v_n / (1 + (b/m)·dt)` is
  backward Euler for the linear law. For quadratic drag, freezing the coefficient
  at the start-of-step speed gives
  `v_{n+1} = v_n / (1 + c·|v_n|·dt)`, `c = rho·Cd·A/(2m)`. That quadratic form is
  linearly implicit, not the exact nonlinear backward-Euler root
  `s_{n+1} + c·dt·s_{n+1}² = s_n`. It is nevertheless always contractive
  (`0 < 1/(1+positive) < 1`), never overshoots, and is cheaper than the `exp`.

Either removes the `dt < tau_drag` constraint. Where the coefficient itself varies
fast within a step (deep periapsis, §6.3), **substep** the drag update
adaptively so `rho` and `|v|` are approximately constant across each substep.

### 6.3 Exponential density varies fast near periapsis

Because `rho = rho0·exp(−h/H)`, density changes by a factor `e` for every scale
height `H ≈ 8.4 km` of altitude. A fast body crossing periapsis can traverse
several scale heights in one 60 Hz step, so `rho` (and thus the drag force) can
change by an order of magnitude *within* a single `dt`. A fixed-`dt` explicit step
that samples `rho` once at the start of the step is then badly wrong. Mitigation:
**substep when the altitude change per step exceeds a fraction of `H`** (e.g.
require `|Δh| < 0.1·H` per drag substep), or evaluate `rho` at the mid-step
altitude. This is the atmospheric analogue of the periapsis-precision problem the
architecture already flags for orbits.

### 6.4 Soft boundary, not a hard cutoff — the gravity-softening lesson, repeated

Set `rho` to **exactly 0.0 at and above a cutoff altitude**, but do not branch from
the unmodified positive profile directly to zero. Over a terminal fade interval
`w`, multiply density and pressure by `smoothstep(0, 1, (ceiling-h)/w)`. This
factor is one below the fade, approaches zero with zero slope from below, and is
exactly zero at the ceiling, making aerodynamic force C0 (and slope-continuous)
at the shell. The shipped defaults use 5 km for Earth USSA76 and one scale height
for an exponential body. This taper is a simulation boundary extension; USSA76
table values remain authoritative below its final fade interval.

Also floor deep-negative `h` at the surface so `exp(−h/H)` cannot overflow. A
**hard** cutoff (a discontinuous `rho` jump at the boundary) injects a force
impulse at the crossing, the identical defect to the deep-dive's
`if(dist<1) continue` gravity cutoff that the architecture rejects. The rule is
the same in both places: **soften the boundary; never step-discontinue a force.**

### 6.5 Heating never feeds the integrator

Aerodynamic heating (§4) drives a cosmetic/gameplay scalar (skin temperature,
damage), accumulated like proper time. The `V^3` (convective) and `V^~8`
(radiative) terms can overflow for a non-finite or huge `|v_rel|`, so **floor
`rho`, compute a scaled finite norm, reject non-finite `v`, and evaluate the
power in the log domain with finite saturation**.
Because heating is never summed back into `externalForce`/`externalTorque`, a
bad heating value can corrupt a HUD number but **cannot destabilise the
trajectory** — an important firewall (RELATIVISTIC_SIM_ARCHITECTURE named-blowups
table).

---

## 7. The orbit-to-atmosphere seam — argue that atmosphere is a force term, not a regime

**Claim: atmosphere is not a separate simulation regime. It is one more external
force term summed into the same rigid-body integrator, and it vanishes to exactly
zero above the atmosphere because `rho → 0`.** The argument:

1. **The force is already zero where there is no air.** Drag and lift are `∝ rho`.
   Above the soft cutoff (§6.4), `rho ≡ 0`, so `F_drag = F_lift = 0` identically —
   not "small," exactly zero. A body in vacuum receives the aerodynamic force term
   and it contributes nothing. There is no state to switch, no branch to take: the
   same code that computes drag returns the zero vector.
2. **The only real transition is on-rails ↔ force-integrated, which already
   exists.** A passive orbit is propagated on analytic Kepler rails (cannot
   drift). Drag makes the trajectory **non-Keplerian**, so a body in the
   atmosphere must be **force-integrated** — but that is the *same* seam the
   architecture already crosses for any actively-perturbed body (thrust,
   third-body). Atmosphere does not add a new seam; it is one more reason a body
   is on the force-integrated side of the existing one. Entering is the same
   state-vector construction as SOI entry (`r_ = r_+`, `v_ = v_+`); leaving — once
   `rho` underflows to 0 and no thrust acts — is the reverse, and the body may
   convert back to on-rails.
3. **A separate regime would reintroduce the very discontinuity we forbid.** If
   atmosphere were a distinct regime with a hard on/off boundary, the crossing
   would be a force (and possibly integrator) discontinuity — the gravity-cutoff
   defect again (§6.4). The continuous approach — `rho` ramping from exactly 0,
   drag as a summed force term — is both **simpler** (no regime bookkeeping) and
   **more robust** (C0 by construction).

Therefore: implement atmosphere as `AtmosphereForces(body, atmos) → {F_world,
tau_body}` that returns zero above the cutoff, summed into the existing hooks. The
force-integration ownership already handles the rest. **No separate atmosphere
regime is needed or wanted.**

---

### 7.1 Production ECS adapter contract

`ApplyAtmosphereToEntity` is the GPU-free bridge from this model to a live
`Transform + SpatialFrame + RigidBody + AerodynamicBody`. It resolves position
and velocity through `FrameGraph`, computes `v_atm = v_linear + omega x r`, and
samples the current altitude without caching density.

The shipped stiff-drag policy refines the force-only shorthand above: drag is
applied exactly once with `SemiImplicitDragAirspeed` as an operator-split velocity
update. It is **not** also added to `forceAccum`. Its equivalent impulse-over-dt
force participates only in the CoP moment, so off-centre drag still rotates the
body without duplicating its translational impulse. Lift remains a world force in
`forceAccum`; the complete aerodynamic force is rotated into body space before
forming the body-frame torque. The adapter must run once before
`StepFlightPhysics` in the same fixed step.

All numeric state and frame conversions are staged before any component write.
Malformed requests reject without changing the registry. A valid vacuum or
ceiling sample is an accepted exact no-op. Positive density promotes an optional
`GravitationalBody` from rails or passive N-body motion to `ForceIntegrated`;
`OrbitState` remains intact for a later deterministic demotion. Any stale
accumulators from the previous mover are cleared at that boundary before the new
aerodynamic wrench is staged. Diagnostics (density, dynamic pressure, Mach,
angle of attack, and heat flux) are returned to gameplay and never feed back into
the trajectory except through the explicit drag/lift path above.

This first adapter samples density once at the fixed-step position. The adaptive
altitude substepping described in section 6.3 remains scheduler work for steps
that cross a material fraction of a scale height; unconditional drag contraction
prevents energy injection but does not by itself recover a rapidly varying
density field.

---

## 8. Game fidelity precedent

Where the bar sits, from serious to arcade, and where this engine should aim:

- **Children of a Dead Earth** — the most physically-rigorous orbital-combat sim
  (full N-body, Forest–Ruth symplectic, textbook-derived hardware). **It does not
  model atmospheres at all** — it is pure-vacuum orbital mechanics and combat.
  **FLAGGED:** this claim comes from the community/TV-Tropes description ("The
  effects of atmospheres are not yet modeled"), not a developer statement I could
  fetch directly; but it is consistent with the game being entirely orbital. The
  takeaway: **CoDE is *not* an atmospheric-flight precedent** — for this angle we
  cannot lean on it, which is one more reason this doc had to go to primary
  aerodynamics sources.
- **Orbiter** (space flight simulator) — models a per-planet atmospheric density
  profile and computes drag and lift from **per-vessel airfoil definitions**
  (`Cl`, `Cd`, `Cm` as lookups against angle of attack and Mach), giving realistic
  6-DOF reentry and aerodynamic flight with heating/blackout as gameplay. This is
  the **target fidelity class** for "robust-but-real": table/curve-driven
  coefficients over a real atmosphere model, not CFD.
- **Kerbal Space Program** — two eras worth distinguishing:
  - **Stock KSP1 "soupy" drag (pre-1.0):** drag was per-part and proportional to
    part *mass*, computed from how parts were placed in the editor rather than the
    vehicle's aerodynamic shape — a part clipped inside the hull still dragged.
    No Mach effects, no area ruling. Widely criticised as unphysical ("soupy").
  - **Stock KSP1 (1.0+):** replaced with an occlusion-aware per-part "drag cube"
    model plus a keyframed pressure/temperature-vs-altitude atmosphere and a
    simplified convective reentry-heating model driving part temperature and
    destruction — much better, still per-part, still not whole-vehicle.
  - **FAR (Ferram Aerospace Research):** replaces stock with **voxel-based
    whole-vehicle aerodynamics** — the actual craft shape controls lift/drag,
    long/thin bodies drag less, hollow shells shield their interiors, with
    **Mach-dependent lift and drag and area ruling** and realistic stall. This is
    the gold standard the community reaches for.
  - **KSP2** aimed to improve stock aero but the community still points at
    FAR-class shape-based aerodynamics as the reference; the KSP1-stock-vs-FAR gap
    is the "coefficient-per-part" vs "shape-resolved" divide.
- **Flight sims** — DCS and MSFS use **table-driven coefficient lookups**
  (`Cl`, `Cd`, `Cm` vs AoA, Mach, control deflection); X-Plane uses **blade-element
  / strip theory** (real-time aerodynamics over discretised surfaces). Both are
  full 6-DOF with stall and spin. Higher fidelity than any space-sim needs for
  atmospheric passage, but the **coefficient-lookup pattern is the one to borrow**.

**Recommended level for this engine (Orbiter-class):** USSA76 (or per-body
exponential) density with real temperature → speed of sound → Mach; **quadratic
drag with a `Cd(M[, alpha])` curve**; optional **lift from `Cl(alpha)` with
stall**; the **aerodynamic wrench about the CoP** for emergent static stability;
**Sutton–Graves convective heating** (§4.1, cube form) as a cosmetic/gameplay
scalar; and the **analytic/semi-implicit drag substep** (§6.2) for stiffness. This
sits above stock KSP, at Orbiter/FAR fidelity, well below CFD — real physics,
robustly integrated, no shape-resolved solver.

---

## 9. Verification — analytic test cases with negative controls

Per the project rule that *an assertion nobody has watched fail is not evidence*,
each test below pairs a positive assertion with a **negative control** that must
be watched failing. These are the atmosphere entries the architecture's §8/§9
verification tables must carry.

**9.1 Terminal velocity converges to the closed form.**
Release a body from rest under constant `g` and quadratic drag. **Assert** speed
converges to `v_terminal = sqrt(2·m·g/(rho·Cd·A))` and the transient tracks
`v(t) = v_terminal·tanh(t/tau)`, `tau = v_terminal/g`, to a tight tolerance.
*Negative control:* halve `Cd` and watch `v_terminal` move by `sqrt(2)` (the
closed form is load-bearing, not a fitted constant) — watched failing if the code
ignores `Cd`.

**9.2 Zero airspeed produces zero aerodynamic force (negative control).**
With `v_rel = 0`, **assert** `F_drag = 0` and `F_lift = 0` **exactly**, and that
no NaN is produced by the `v_hat_rel = v_rel/|v_rel|` normalisation (the `|v|=0`
guard must fire). *This is itself the negative control* for the drag term: a body
at rest in still air is pushed by nothing.

**9.3 The seam is continuous — above-atmosphere gives exactly zero force.**
Place a body above the cutoff (`rho → 0`). **Assert** `F_aero = 0` exactly and the
on-rails body receives no aerodynamic force. **Assert** that crossing the cutoff
shell produces **no force step** (sample `F_aero` on both sides of the boundary
and require C0). *Negative control:* replace the soft ramp with a hard `rho`
cutoff and watch a force impulse appear at the crossing — watched failing,
proving the soft boundary is load-bearing (the gravity-softening lesson).

**9.4 Dynamic pressure and Mach are exact functions of state.**
**Assert** `q = 0.5·rho·|v_rel|^2` and `M = |v_rel|/a` to machine precision
against an independent recompute from `{rho, v_rel, T}`. *Negative control:* feed
the drag code a stale `rho` (previous frame's) and watch `q` disagree with the
state — proving the force reads live density, not a cached value.

**9.5 Stiff drag does NOT gain energy (the key robustness test).**
Drop a low-`beta` body into dense atmosphere with a `dt` chosen so `dt >
tau_drag` (deliberately stiff). Using the analytic/semi-implicit drag update
(§6.2), **assert speed decreases monotonically and never oscillates or increases**
— kinetic energy is non-increasing every step. *Negative control:* swap in the
naive explicit update at the same `dt` and watch the speed **overshoot through
zero, oscillate, and grow** (energy injection) — watched failing. This is the
atmosphere analogue of the gyroscopic-term instability and its implicit fix.

**9.6 Static stability is emergent from the CoP–CoM ordering.**
Build a body with CoP **aft** of CoM and perturb its angle of attack. **Assert**
the aerodynamic torque is **restoring** (drives `alpha` back toward trim). *Negative
control:* move the CoP **forward** of CoM and watch the torque **diverge** the
attitude (tumble) — proving `r_cp × F_aero` carries the stability, with no
special-case stability code.

**9.7 Density model is total and non-overflowing.**
**Assert** `exp` density neither overflows at deep-negative `h` (sub-surface) nor
retains a finite jump at the cutoff, and that the USSA76 branches agree at every
layer boundary (C0, §1.3). Sample the ceiling limit from below and require the
terminal smoothstep to approach exact zero. *Negative control:* remove the
deep-`h` clamp and watch `rho` overflow to `inf`, or replace the taper with a hard
cut and watch a nonzero force step — both are watched failures.

**9.8 Heating cannot destabilise the trajectory (firewall).**
Force a huge/non-finite `|v_rel|` into the heating term. **Assert** the heating
scalar is guarded (floored `rho`, scaled norm, non-finite rejected, finite
saturation) **and** that
`externalForce`/`externalTorque` are byte-unchanged by the heating computation —
heating never feeds the integrator. *Negative control:* wire the heating value
into `externalForce` and watch the trajectory blow up — proving the firewall is
load-bearing.

---

## Sources

Primary and authoritative, fetched and read this pass:

- **US Standard Atmosphere 1976** — layer table, barometric formulas, and all SI
  constants (g0, R=287.053, r0=6,356,766 m, gamma=1.4, sea-level P/T/rho, base
  pressures, Sutherland viscosity), transcribed with source code from the standard
  by J. Hawley, *Formulae and code for the U.S. Standard Atmosphere (1976)*
  (2015): http://jimhawley.ca/downloads/Ballistics/Formulae_and_code_US_Standard_Atmosphere_1976.pdf
  — the model itself: NOAA/NASA/USAF, *U.S. Standard Atmosphere, 1976*,
  U.S. Govt. Printing Office. Public-domain aeronautical software index:
  https://www.pdas.com/atmos.html
- **Sutton, K. & Graves, R. A., Jr.**, *A General Stagnation-Point
  Convective-Heating Equation for Arbitrary Gas Mixtures*, NASA TR R-376 (1971) —
  the heat-transfer coefficient `K` (air 0.1113 kg·s^-1·m^-3/2·atm^-1/2, eq. 42 /
  Table II), the primary source for §4.1 and the provenance FLAG:
  https://ntrs.nasa.gov/api/citations/19720003329/downloads/19720003329.pdf
- **NASA Glenn Research Center, *Beginner's Guide to Aeronautics*** — drag
  coefficient (`Cd = D/(q·A)`, reference-area convention, Mach/Re dependence) and
  lift coefficient (`Cl = L/(q·A)`):
  https://www1.grc.nasa.gov/beginners-guide-to-aeronautics/drag-coefficient/ ,
  https://www1.grc.nasa.gov/beginners-guide-to-aeronautics/lift-coefficient/ ;
  Mach number: https://www.grc.nasa.gov/www/k-12/airplane/mach.html
- **Planar reentry equations** (equations of motion, Allen–Eggers closed form,
  peak deceleration/heating, `k = 1.74153e-4` Earth Sutton–Graves engineering
  constant) — Wikipedia, used as a pointer to and consolidation of Allen & Eggers,
  *A Study of the Motion and Aerodynamic Heating of Ballistic Missiles Entering the
  Earth's Atmosphere at High Supersonic Speeds*, NASA TR 1381 (1958):
  https://en.wikipedia.org/wiki/Planar_reentry_equations

Standard-text and secondary, cross-checked:

- **Anderson, J. D.**, *Fundamentals of Aerodynamics* — thin-airfoil lift-curve
  slope `Cl_alpha = 2π/rad`, stall, transonic drag rise, L/D.
- **Vallado, D. A.**, *Fundamentals of Astrodynamics and Applications* — ballistic
  coefficient, drag as an external perturbing force, exponential and layered
  atmosphere use in propagation.
- Transonic drag-divergence and supersonic/hypersonic `Cd` shape:
  https://en.wikipedia.org/wiki/Drag-divergence_Mach_number and NASA Glenn.
- Numerical stiffness of explicit drag and the implicit/exponential fix:
  Gaffer On Games, *Integration Basics* (semi-implicit Euler stability); dust–gas
  drag literature on stiff-regime implicit updates (Athena++/Athena multi-fluid
  dust modules); exponential-integrator surveys. Used for the §6 pitfalls.
- Game fidelity: Ferram Aerospace Research (FAR) —
  https://github.com/ferram4/Ferram-Aerospace-Research ; Orbiter reentry —
  https://www.orbiterwiki.org/wiki/GPIS_6:_Reentry ; Children of a Dead Earth
  atmosphere status (FLAGGED, community source) —
  https://tvtropes.org/pmwiki/pmwiki.php/VideoGame/ChildrenOfADeadEarth

Radiative heating (Tauber–Sutton, `q_rad ∝ V^~8`) is FLAGGED §4.2: the scaling is
sourced from the atmospheric-entry literature, but the exact coefficient table was
not fetched — pull it from Tauber & Sutton (1991, *J. Spacecraft and Rockets*) if
radiative heating is implemented beyond the qualitative scaling.
