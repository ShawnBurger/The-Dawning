# THE DAWNING — Ship Flight Model Deep Dive
# Batch 1, Topic 4: 6DOF Newtonian Flight, RCS Thrusters, IFCS, Atmospheric Entry
# Sources: Star Citizen flight model docs (RSI), real RCS systems (NASA/Apollo/Shuttle),
#          spacecraft attitude control research, rigid body dynamics

---

## PART 1: NEWTONIAN RIGID BODY FLIGHT

Every ship is a rigid body with mass, center of mass, and moment of inertia tensor.
Thrusters apply forces at specific points on the hull, creating both linear acceleration
AND torque (rotation) depending on where they push relative to the center of mass.

### Core Physics

```cpp
struct ShipPhysicsState {
    Vec3d position;          // World position (double precision for space scale)
    Vec3d velocity;          // Linear velocity (m/s)
    Vec3d angularVelocity;   // Rotational velocity (rad/s, in body frame)
    Quatd orientation;       // Ship orientation (quaternion, double precision)
    
    double mass;             // Total ship mass (kg) — changes with fuel consumption
    Vec3d centerOfMass;      // CoM offset from geometric center (shifts with cargo/fuel)
    Mat3x3 inertiaTensor;    // 3×3 moment of inertia tensor (kg·m²)
    Mat3x3 invInertiaTensor; // Precomputed inverse (for torque → angular acceleration)
};

// Per-frame physics update (symplectic Euler for stability)
void UpdateShipPhysics(ShipPhysicsState& ship, const Vec3d& totalForce,
                          const Vec3d& totalTorque, double dt)
{
    // Linear: F = ma → a = F/m
    Vec3d linearAccel = totalForce / ship.mass;
    ship.velocity += linearAccel * dt;
    ship.position += ship.velocity * dt;
    
    // Angular: τ = Iα → α = I⁻¹τ (in body frame)
    // Must also account for gyroscopic precession: τ_eff = τ - ω × (I·ω)
    Vec3d gyroscopic = ship.angularVelocity.Cross(
        ship.inertiaTensor * ship.angularVelocity);
    Vec3d angularAccel = ship.invInertiaTensor * (totalTorque - gyroscopic);
    ship.angularVelocity += angularAccel * dt;
    
    // Integrate orientation (quaternion)
    Quatd spin(ship.angularVelocity.x * 0.5, 
               ship.angularVelocity.y * 0.5,
               ship.angularVelocity.z * 0.5, 0);
    ship.orientation += spin * ship.orientation * dt;
    ship.orientation = ship.orientation.Normalized();
}
```

### Moment of Inertia for Common Ship Shapes

```
Solid box (L×W×H, mass M):
  Ixx = M/12 × (W² + H²)
  Iyy = M/12 × (L² + H²)
  Izz = M/12 × (L² + W²)

Example: 30m×15m×8m ship, 50000 kg:
  Ixx = 50000/12 × (225 + 64)  = 1,204,167 kg·m²
  Iyy = 50000/12 × (900 + 64)  = 4,016,667 kg·m²
  Izz = 50000/12 × (900 + 225) = 4,687,500 kg·m²
  → Yaw (Izz) and Pitch (Iyy) are hardest to rotate (long axis)
  → Roll (Ixx) is easiest (narrowest cross-section)

Ship scale affects "feel" enormously:
  Fighter (15m, 10t):  snappy rotation, high angular accel
  Frigate (80m, 500t): moderate rotation, feels weighty
  Capital (500m, 50000t): sluggish rotation, needs planning
```

---

## PART 2: THRUSTER SYSTEM (RCS)

### Thruster Placement (per Star Citizen model)

Each thruster has: position on hull, direction it pushes, maximum force (Newtons).
The IFCS (flight computer) determines which thrusters to fire for any desired maneuver.

```cpp
struct Thruster {
    Vec3f position;       // Offset from ship center (meters)
    Vec3f direction;      // Unit vector: direction thrust pushes the ship
    float maxForce;       // Maximum thrust (Newtons)
    float currentThrottle;// 0-1 current output
    float responseTime;   // Seconds to reach full thrust (0.05-0.5)
    float fuelConsumption;// kg/s at full thrust
    bool  damaged;
    std::string group;    // "main", "retro", "rcs_port", "rcs_starboard", etc.
};

// Standard thruster layout for a fighter:
// Main engines (2):     rear-facing, 200,000N each (forward thrust)
// Retro thrusters (2):  front-facing, 50,000N each (braking)
// RCS clusters (8):     4 pairs at corners, 10,000N each, multi-axis
//   Each RCS cluster has 3 nozzles: up/down, left/right, fore/aft
//   Total RCS nozzles: 24 (covering all 6 DOF)
```

### Thruster → Force/Torque Computation

```cpp
void ComputeThrusterContribution(const Thruster& thruster,
                                    const Vec3f& shipCoM,
                                    Vec3f& outForce, Vec3f& outTorque)
{
    // Force: simply thrust direction × throttle × max force
    Vec3f force = thruster.direction * thruster.maxForce * thruster.currentThrottle;
    
    // Torque: cross product of (position relative to CoM) × force
    Vec3f leverArm = thruster.position - shipCoM;
    Vec3f torque = leverArm.Cross(force);
    
    outForce += force;
    outTorque += torque;
}

// Sum all thrusters
void ComputeTotalThrustAndTorque(const std::vector<Thruster>& thrusters,
                                     const Vec3f& shipCoM,
                                     Vec3f& totalForce, Vec3f& totalTorque)
{
    totalForce = {0, 0, 0};
    totalTorque = {0, 0, 0};
    for (const auto& t : thrusters) {
        if (t.damaged || t.currentThrottle <= 0) continue;
        ComputeThrusterContribution(t, shipCoM, totalForce, totalTorque);
    }
}
```

---

## PART 3: IFCS (Intelligent Flight Control System)

The IFCS translates player input into thruster commands.
Without it, the player would need to manually control 24+ thrusters.

### Flight Modes

| Mode | Description | Player Controls | IFCS Does |
|---|---|---|---|
| **Coupled** | Arcade-friendly | Stick = desired velocity direction | Auto-brake, auto-level |
| **Decoupled** | Newtonian | Stick = thrust direction | No auto-brake, drift freely |
| **Cruise** | Long-distance | Throttle = target speed | Maintain heading + speed |
| **Precision** | Docking | Reduced input sensitivity | Ultra-fine thruster control |
| **Combat** | Dogfighting | Stick = rotation, throttle = speed | Speed limiter, auto-aim assist |

### IFCS Controller (PID-based)

```cpp
struct PIDController {
    float kP = 1.0f;    // Proportional gain
    float kI = 0.01f;   // Integral gain
    float kD = 0.5f;    // Derivative gain
    float integral = 0;
    float prevError = 0;
    float outputMin = -1.0f;
    float outputMax = 1.0f;
    
    float Update(float error, float dt) {
        integral += error * dt;
        integral = std::clamp(integral, -10.0f, 10.0f); // Anti-windup
        float derivative = (error - prevError) / dt;
        prevError = error;
        float output = kP * error + kI * integral + kD * derivative;
        return std::clamp(output, outputMin, outputMax);
    }
};

// IFCS has 6 PID controllers: one per axis of motion
// 3 rotational: pitch, yaw, roll
// 3 translational: forward/back, left/right, up/down
struct IFCS {
    PIDController pitchPID{2.0, 0.02, 1.0};   // Tighter for rotation
    PIDController yawPID{2.0, 0.02, 1.0};
    PIDController rollPID{3.0, 0.01, 0.8};    // Roll is fastest
    PIDController forwardPID{1.0, 0.005, 0.5}; // Looser for translation
    PIDController strafePID{1.5, 0.01, 0.6};
    PIDController verticalPID{1.5, 0.01, 0.6};
};
```

### Thruster Allocation (Linear Programming Simplified)

```cpp
// Given desired force and torque, find optimal thruster throttles
// This is a constrained optimization problem. Simplified version:
void AllocateThrusters(std::vector<Thruster>& thrusters,
                          const Vec3f& desiredForce, const Vec3f& desiredTorque,
                          const Vec3f& shipCoM)
{
    // For each thruster, compute its contribution to desired force/torque
    for (auto& t : thrusters) {
        if (t.damaged) { t.currentThrottle = 0; continue; }
        
        Vec3f leverArm = t.position - shipCoM;
        Vec3f torquePerNewton = leverArm.Cross(t.direction);
        
        // Score: how much does this thruster help achieve the goal?
        float forceAlignment = t.direction.Dot(desiredForce.Normalized());
        float torqueAlignment = torquePerNewton.Normalized().Dot(desiredTorque.Normalized());
        
        float score = forceAlignment * desiredForce.Length() * 0.5f
                    + torqueAlignment * desiredTorque.Length() * 0.5f;
        
        t.currentThrottle = std::clamp(score / t.maxForce, 0.0f, 1.0f);
    }
}
```

---

## PART 4: ATMOSPHERIC FLIGHT

When entering atmosphere, aerodynamic forces apply:

```cpp
struct AtmosphericForces {
    float airDensity;       // kg/m³ (sea level Earth = 1.225)
    float dragCoefficient;  // Cd (ship-specific, 0.3-0.8)
    float liftCoefficient;  // Cl (wing area dependent, 0-2.0)
    float crossSectionArea; // m² (frontal area of ship)
};

Vec3f ComputeDrag(const Vec3f& velocity, const AtmosphericForces& atmo)
{
    float speed = velocity.Length();
    if (speed < 0.1f) return {0, 0, 0};
    
    // Drag = 0.5 × ρ × v² × Cd × A
    float dragMagnitude = 0.5f * atmo.airDensity * speed * speed
                        * atmo.dragCoefficient * atmo.crossSectionArea;
    
    // Drag opposes velocity
    return velocity.Normalized() * (-dragMagnitude);
}

Vec3f ComputeLift(const Vec3f& velocity, const Vec3f& shipUp,
                     const AtmosphericForces& atmo)
{
    float speed = velocity.Length();
    if (speed < 1.0f) return {0, 0, 0};
    
    // Angle of attack
    Vec3f velocityDir = velocity.Normalized();
    float aoa = asin(velocityDir.Dot(shipUp));
    
    // Lift = 0.5 × ρ × v² × Cl × A × sin(2α)  (simplified)
    float liftMagnitude = 0.5f * atmo.airDensity * speed * speed
                        * atmo.liftCoefficient * atmo.crossSectionArea
                        * sin(2.0f * aoa);
    
    // Lift perpendicular to velocity, in the plane of velocity and up
    Vec3f liftDir = velocityDir.Cross(shipUp.Cross(velocityDir)).Normalized();
    return liftDir * liftMagnitude;
}

// Reentry heating
float ReentryHeatFlux(float velocity, float airDensity) {
    // Sutton-Graves approximation: q = k × sqrt(ρ/r_nose) × v³
    // Simplified: q ∝ ρ × v³
    return airDensity * velocity * velocity * velocity * 1e-8f;
    // Returns W/m² — compare to heat shield tolerance
    // > 1e6 W/m² = serious structural damage
}
```

---

## PART 5: SHIP CLASS PRESETS

| Class | Mass (kg) | Length (m) | Main Thrust (kN) | RCS Thrust (kN) | Max Accel (G) | Roll Rate (°/s) |
|---|---|---|---|---|---|---|
| **Light Fighter** | 8,000 | 15 | 400 | 10 | 5.0 | 120 |
| **Heavy Fighter** | 15,000 | 22 | 600 | 15 | 4.0 | 80 |
| **Corvette** | 80,000 | 60 | 2,000 | 40 | 2.5 | 30 |
| **Frigate** | 500,000 | 150 | 8,000 | 100 | 1.6 | 10 |
| **Cruiser** | 2,000,000 | 300 | 20,000 | 200 | 1.0 | 4 |
| **Capital Ship** | 20,000,000 | 800 | 100,000 | 500 | 0.5 | 1 |
| **Station** | 500,000,000 | 2000 | 0 | 2,000 | 0 | 0.1 |

### Thruster Failure Behavior
When thrusters are damaged, the IFCS compensates:
- Lost main engine: retro thrusters can provide reduced forward thrust
- Lost RCS cluster: remaining clusters compensate (reduced authority on that axis)
- All port thrusters lost: ship can only turn starboard (asymmetric thrust)
- Total thruster failure: ship drifts on current trajectory (use EVA to repair)

---

## PART 6: CURRICULUM STEP UPDATES

**Step 251-260 (Ship flight)**: Implement ShipPhysicsState with full rigid body dynamics.
Moment of inertia from ship mass and dimensions. Symplectic Euler integration.

**Step 261-270 (Thruster system)**: Define thruster positions per ship class.
ComputeTotalThrustAndTorque sums all contributions. Damaged thrusters contribute nothing.

**Step 271-280 (IFCS)**: 6-axis PID controller. Coupled mode auto-brakes when stick released.
Decoupled mode passes thrust directly. Mode switching with hotkeys.

**Step 281-290 (Thruster allocation)**: Solve for optimal throttle per thruster given desired force/torque.
Simple scoring system initially; upgrade to proper linear programming in depth pass.

**Step 291-300 (Ship handling)**: Tune PID gains per ship class. Fighter = snappy, capital = sluggish.
Test by flying figure-8 course. Measure time to rotate 180° (flip-and-burn maneuver).

**Step 301-310 (Atmospheric entry)**: Drag and lift forces from atmosphere density.
Reentry heating visual (hull glow shader). Speed-dependent atmospheric effects.
IFCS switches to atmospheric mode (lift-based turning, speed limits).

**Step 1341-1380 (Physics depth)**: Full inertia tensor computation from mesh.
Fuel mass changes shift center of mass and inertia. Cargo mass affects handling.
Damaged wing/nacelle changes drag profile asymmetrically.
