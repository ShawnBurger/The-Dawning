# THE DAWNING — Orbital Mechanics Deep Dive
# Batch 1, Topic 3: Kepler Orbits, Vis-Viva, Patched Conics, Hohmann Transfers
# Sources: MIT OCW 16.07, orbital-mechanics.space, Wikipedia, KSP community

---

## PART 1: FUNDAMENTAL EQUATIONS

### The Vis-Viva Equation (Most Important Single Equation)
Relates velocity at any point in an orbit to the orbital parameters:

```
v² = μ × (2/r - 1/a)

Where:
  v = velocity at current position (m/s)
  μ = gravitational parameter = G × M (m³/s²)
  r = distance from center of central body (m)
  a = semi-major axis of the orbit (m)

For a circular orbit (r = a):  v = sqrt(μ/r)
For escape velocity (a → ∞):  v = sqrt(2μ/r)
```

### Gravitational Parameters (μ = GM) for The Dawning

| Body | μ (m³/s²) | Radius (m) | Notes |
|---|---|---|---|
| **Sun (Sol analog)** | 1.327 × 10²⁰ | 6.96 × 10⁸ | Central star |
| **Earth analog** | 3.986 × 10¹⁴ | 6.371 × 10⁶ | Standard habitable |
| **Moon analog** | 4.905 × 10¹² | 1.737 × 10⁶ | Large moon |
| **Mars analog** | 4.283 × 10¹³ | 3.390 × 10⁶ | Small rocky |
| **Jupiter analog** | 1.267 × 10¹⁷ | 6.991 × 10⁷ | Gas giant |
| **Small asteroid** | ~1.0 × 10⁶ | ~500 | Procgen varies |

For procedural planets: μ = G × density × (4/3)π × radius³
G = 6.674 × 10⁻¹¹ m³/(kg·s²)

### Kepler's Laws in Code

```cpp
// Orbital elements → position and velocity
struct OrbitalElements {
    double a;           // Semi-major axis (m)
    double e;           // Eccentricity (0=circle, 0-1=ellipse, 1=parabola, >1=hyperbola)
    double i;           // Inclination (radians, 0=equatorial)
    double omega;       // Longitude of ascending node (radians)
    double w;           // Argument of periapsis (radians)
    double trueAnomaly; // Current position in orbit (radians, 0=periapsis)
    double mu;          // Gravitational parameter of central body
};

// Orbital period (Kepler's 3rd law)
double OrbitalPeriod(double a, double mu) {
    return 2.0 * PI * sqrt(a * a * a / mu);
}

// Velocity at distance r in orbit with semi-major axis a
double OrbitalVelocity(double r, double a, double mu) {
    return sqrt(mu * (2.0 / r - 1.0 / a));  // Vis-viva
}

// Circular orbit velocity
double CircularVelocity(double r, double mu) {
    return sqrt(mu / r);
}

// Escape velocity at distance r
double EscapeVelocity(double r, double mu) {
    return sqrt(2.0 * mu / r);
}

// Specific orbital energy (negative = bound, positive = escape)
double SpecificEnergy(double a, double mu) {
    return -mu / (2.0 * a);
}

// Position in orbit from true anomaly
double OrbitRadius(double a, double e, double trueAnomaly) {
    return a * (1.0 - e * e) / (1.0 + e * cos(trueAnomaly));
}

// Sphere of influence radius (Hill sphere approximation)
double SOIRadius(double a_orbit, double m_body, double m_parent) {
    return a_orbit * pow(m_body / m_parent, 2.0 / 5.0);
}
```

---

## PART 2: HOHMANN TRANSFER (Minimum Energy Transfer)

```cpp
struct HohmannTransfer {
    double dv1;         // Delta-v at departure (m/s)
    double dv2;         // Delta-v at arrival (m/s)
    double dvTotal;     // Total delta-v required
    double transferTime;// Transfer time (seconds)
    double a_transfer;  // Semi-major axis of transfer ellipse
};

HohmannTransfer ComputeHohmann(double r1, double r2, double mu)
{
    HohmannTransfer h;

    // Semi-major axis of transfer ellipse
    h.a_transfer = (r1 + r2) / 2.0;

    // Velocities on initial and final circular orbits
    double v1_circular = sqrt(mu / r1);
    double v2_circular = sqrt(mu / r2);

    // Velocities on transfer ellipse at periapsis and apoapsis
    double v_transfer_periapsis = sqrt(mu * (2.0/r1 - 1.0/h.a_transfer));
    double v_transfer_apoapsis  = sqrt(mu * (2.0/r2 - 1.0/h.a_transfer));

    // Delta-v at each burn
    h.dv1 = abs(v_transfer_periapsis - v1_circular);
    h.dv2 = abs(v2_circular - v_transfer_apoapsis);
    h.dvTotal = h.dv1 + h.dv2;

    // Transfer time = half the period of the transfer ellipse
    h.transferTime = PI * sqrt(h.a_transfer * h.a_transfer * h.a_transfer / mu);

    return h;
}
```

### Example Values (for UI display calibration)

| Transfer | r1 (km) | r2 (km) | Δv1 (km/s) | Δv2 (km/s) | Time |
|---|---|---|---|---|---|
| LEO → GEO | 6,571 | 42,164 | 2.46 | 1.47 | 5.26 hrs |
| Earth → Mars | 149.6M | 227.9M | 2.94 | 2.65 | 258.8 days |
| Earth → Venus | 149.6M | 108.2M | 2.49 | 2.71 | 146.1 days |
| Earth → Jupiter | 149.6M | 778.6M | 8.79 | 5.64 | 2.73 years |

---

## PART 3: PATCHED CONICS (Multi-Body Trajectories)

Break interplanetary travel into three two-body problems:
1. **Departure**: Hyperbolic escape from origin planet
2. **Cruise**: Elliptical transfer around sun (Hohmann or other)
3. **Arrival**: Hyperbolic capture at destination planet

```cpp
// Hyperbolic excess velocity (v_infinity)
// This is the velocity relative to the planet at the edge of its SOI
double HyperbolicExcessVelocity(double v_helio_required, double v_planet_orbital) {
    // v_infinity² = v_helio_required² - v_planet_orbital²  (for departure)
    // Actually: vector difference, but for Hohmann (tangential burns):
    return abs(v_helio_required - v_planet_orbital);
}

// Delta-v to escape from parking orbit onto hyperbolic trajectory
double EscapeDeltaV(double v_infinity, double r_parking, double mu_planet) {
    double v_parking = sqrt(mu_planet / r_parking);  // Circular velocity
    double v_escape_point = sqrt(v_infinity * v_infinity + 2.0 * mu_planet / r_parking);
    return v_escape_point - v_parking;
}

// Delta-v to capture into orbit from hyperbolic approach
double CaptureDeltaV(double v_infinity, double r_target_orbit, double mu_planet) {
    double v_orbit = sqrt(mu_planet / r_target_orbit);
    double v_approach = sqrt(v_infinity * v_infinity + 2.0 * mu_planet / r_target_orbit);
    return v_approach - v_orbit;
}
```

### SOI Radii for Gameplay

```
For "Earth": SOI ≈ 924,000 km (about 145 Earth radii)
For "Mars":  SOI ≈ 577,000 km
For "Jupiter": SOI ≈ 48,200,000 km
For "Moon":  SOI ≈ 66,000 km (relative to Earth)

Rule of thumb: SOI ≈ orbital_radius × (planet_mass / star_mass)^(2/5)

In-game: entering a planet's SOI switches the gravity reference frame.
The ship's velocity is transformed from heliocentric to planetocentric.
This is the "patch" in patched conics.
```

---

## PART 4: ON-RAILS vs N-BODY SIMULATION

### On-Rails (Kepler Propagation) — Used for distant objects

```cpp
// Propagate position along a Kepler orbit analytically
// No numerical integration needed — exact solution at any time
Vec3d KeplerPosition(const OrbitalElements& orbit, double time) {
    // 1. Compute mean anomaly from time
    double n = sqrt(orbit.mu / (orbit.a * orbit.a * orbit.a)); // Mean motion
    double M = n * time; // Mean anomaly

    // 2. Solve Kepler's equation: M = E - e*sin(E)  (iterative)
    double E = SolveKeplersEquation(M, orbit.e);

    // 3. Convert eccentric anomaly to true anomaly
    double cosf = (cos(E) - orbit.e) / (1.0 - orbit.e * cos(E));
    double sinf = sqrt(1.0 - orbit.e * orbit.e) * sin(E) / (1.0 - orbit.e * cos(E));
    double f = atan2(sinf, cosf);

    // 4. Compute radius
    double r = orbit.a * (1.0 - orbit.e * cos(E));

    // 5. Position in orbital plane
    double x = r * cos(f);
    double y = r * sin(f);

    // 6. Rotate by orbital elements (i, omega, w) to 3D
    // (rotation matrix from orbital plane to inertial frame)
    return RotateToInertial(x, y, 0, orbit.i, orbit.omega, orbit.w);
}

// Newton-Raphson solver for Kepler's equation
double SolveKeplersEquation(double M, double e, int maxIter = 20) {
    double E = M; // Initial guess
    for (int i = 0; i < maxIter; i++) {
        double dE = (E - e * sin(E) - M) / (1.0 - e * cos(E));
        E -= dE;
        if (abs(dE) < 1e-12) break;
    }
    return E;
}
```

### N-Body (Numerical Integration) — Used for player ship and nearby objects

```cpp
// Verlet integration for N-body (symplectic = energy-conserving)
void NBodyStep(std::vector<Body>& bodies, double dt) {
    // Kick-Drift-Kick (leapfrog)
    
    // Half-kick: update velocities by half step
    for (auto& b : bodies) {
        Vec3d accel = ComputeGravity(b, bodies);
        b.velocity += accel * (dt * 0.5);
    }
    
    // Drift: update positions by full step
    for (auto& b : bodies)
        b.position += b.velocity * dt;
    
    // Half-kick: update velocities by another half step
    for (auto& b : bodies) {
        Vec3d accel = ComputeGravity(b, bodies);
        b.velocity += accel * (dt * 0.5);
    }
}

Vec3d ComputeGravity(const Body& target, const std::vector<Body>& bodies) {
    Vec3d totalAccel = {0, 0, 0};
    for (const auto& other : bodies) {
        if (&other == &target) continue;
        Vec3d r = other.position - target.position;
        double dist = r.Length();
        if (dist < 1.0) continue; // Softening
        double accelMag = G * other.mass / (dist * dist);
        totalAccel += r.Normalized() * accelMag;
    }
    return totalAccel;
}

// Physics timestep: fixed at 1/60s for N-body near player
// On-rails objects: updated analytically, no timestep needed
// Transition: when player enters SOI, switch that body from on-rails to N-body
```

---

## PART 5: GAMEPLAY INTEGRATION

### Maneuver Node System (like KSP)

```cpp
struct ManeuverNode {
    double timeOfBurn;        // Game time to execute burn
    Vec3d deltaV;             // Delta-v vector (prograde, normal, radial)
    double burnDuration;      // Estimated burn time (from thrust/mass)
    OrbitalElements resultingOrbit;  // Predicted orbit after burn
};

// Predict orbit from maneuver
OrbitalElements PredictPostBurnOrbit(const OrbitalElements& current,
                                        const ManeuverNode& node, double mu) {
    // 1. Get position and velocity at burn time
    Vec3d pos = KeplerPosition(current, node.timeOfBurn);
    Vec3d vel = KeplerVelocity(current, node.timeOfBurn);
    
    // 2. Apply delta-v
    vel += node.deltaV;
    
    // 3. Convert state vector back to orbital elements
    return StateVectorToElements(pos, vel, mu);
}
```

### Warp Drive / FTL (Lore-Consistent)
```
The Dawning uses "Foldspace" (our subspace analog):
- Foldspace compression: 1000× effective speed (1 AU in ~8 minutes)
- Entry/exit: requires minimum distance from gravity wells
  Minimum distance = 2 × SOI radius of nearest body
- Fuel: Helion (our dilithium analog) consumed proportional to mass × distance
- Interdiction: enemies can create gravity anomalies that force exit
- Navigation: compute great-circle route on galactic plane, avoid hazards

In-game: player plots course on star map → ship auto-warps
Between systems: loading screen with procedural generation
Within system: real-time traversal with time acceleration options
```

---

## PART 6: CURRICULUM STEP UPDATES

**Step 331-340 (Galaxy generation)**: Each star has μ computed from mass.
Planet orbits use Kepler elements. Display orbital lines using KeplerPosition() at 360 sample points.

**Step 341-345 (Orbit rendering)**: Draw orbit ellipse as line strip.
Color-code by velocity (blue=slow near apoapsis, red=fast near periapsis).
Vis-viva equation provides velocity at each sample point.

**Step 346-350 (Ship orbit insertion)**: When ship velocity < escape velocity at current radius,
automatically compute and display current orbital elements. Show periapsis/apoapsis markers.

**Step 351-360 (Maneuver planning)**: Maneuver node UI on orbit line.
Drag prograde/normal/radial handles to set Δv. Show predicted orbit in real-time.
Display Δv remaining from current fuel. Hohmann transfer helper: auto-compute optimal transfer.

**Step 361-370 (SOI transitions)**: Patched conics implementation.
Detect when ship crosses SOI boundary. Transform velocity to new reference frame.
Smoothly transition orbit display. Gravity assist trajectories show velocity gain.

**Step 371-380 (Time acceleration)**: 1×, 10×, 100×, 1000×, 10000× warp.
On-rails objects advance analytically (instant). Player ship: integrate with larger dt.
Auto-slow to 1× on SOI transitions, approaching bodies, or combat.

**Step 1301-1340 (Physics depth)**: Replace simple gravity with proper N-body for
all objects within player's SOI. Lagrange points emerge naturally from 3-body physics.
Tidal forces on ship near massive bodies.
