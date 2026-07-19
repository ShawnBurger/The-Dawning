# THE DAWNING — AI & Behavior Trees Deep Dive
# Batch 3, Topic 2: Behavior Trees, GOAP, Utility AI, Navigation,
#                    Perception, Combat AI, Fleet Tactics
# Sources: Recast/Detour navmesh, GDC AI talks, Halo/Killzone AI postmortems

---

## PART 1: BEHAVIOR TREE ARCHITECTURE

### Node Types

```cpp
enum class BTStatus { Running, Success, Failure };

// Base node
struct BTNode {
    virtual BTStatus Tick(AIContext& ctx, float dt) = 0;
    virtual void Reset() {}
};

// COMPOSITE NODES (have children)

// Sequence: runs children left-to-right, fails on first failure
struct Sequence : BTNode {
    std::vector<BTNode*> children;
    int currentChild = 0;
    
    BTStatus Tick(AIContext& ctx, float dt) override {
        while (currentChild < children.size()) {
            BTStatus s = children[currentChild]->Tick(ctx, dt);
            if (s == BTStatus::Running) return BTStatus::Running;
            if (s == BTStatus::Failure) { currentChild = 0; return BTStatus::Failure; }
            currentChild++;
        }
        currentChild = 0;
        return BTStatus::Success;
    }
};

// Selector: tries children until one succeeds (fallback)
struct Selector : BTNode {
    std::vector<BTNode*> children;
    int currentChild = 0;
    
    BTStatus Tick(AIContext& ctx, float dt) override {
        while (currentChild < children.size()) {
            BTStatus s = children[currentChild]->Tick(ctx, dt);
            if (s == BTStatus::Running) return BTStatus::Running;
            if (s == BTStatus::Success) { currentChild = 0; return BTStatus::Success; }
            currentChild++;
        }
        currentChild = 0;
        return BTStatus::Failure;
    }
};

// DECORATOR NODES (modify single child)
// Inverter: flips Success/Failure
// Repeater: runs child N times
// Cooldown: prevents re-execution for N seconds
// Condition: only runs child if condition is true

// LEAF NODES (actual actions/checks)
// MoveTo, Attack, Flee, Patrol, Wait, PlayAnimation, etc.
```

### Example: Fighter AI Behavior Tree

```
ROOT (Selector — try each behavior, first success wins)
├── [1] CRITICAL (Sequence — emergency behaviors)
│   ├── Condition: health < 20%?
│   └── Selector
│       ├── Sequence: [HasWingman? → CallForHelp → FleeToCover]
│       └── Sequence: [FleeToNearestStation]
│
├── [2] COMBAT (Sequence — fight if enemies present)
│   ├── Condition: enemies detected?
│   ├── SelectTarget (closest + weakest priority)
│   ├── Selector (engagement style)
│   │   ├── Sequence: [range < 500m? → DogfightManeuver → FireWeapons]
│   │   ├── Sequence: [range < 2000m? → ApproachTarget → FireWeapons]
│   │   └── Sequence: [range > 2000m? → ClosingApproach]
│   └── Cooldown(2s): EvaluateThreat (re-check if should disengage)
│
├── [3] PATROL (Sequence — default behavior)
│   ├── Condition: has patrol route?
│   ├── MoveToNextWaypoint
│   └── Wait(5s) at waypoint
│
└── [4] IDLE
    └── OrbitCurrentPosition
```

---

## PART 2: UTILITY AI (For NPC Decision-Making)

Score-based system where each possible action gets a utility score.
The action with the highest score is chosen. More nuanced than behavior trees.

```cpp
struct UtilityAction {
    std::string name;
    float (*ScoreFunc)(const AIContext&);  // Returns 0-1 utility score
    void (*ExecuteFunc)(AIContext&, float dt);
};

// Score functions use response curves:
float AttackScore(const AIContext& ctx) {
    if (!ctx.hasTarget) return 0;
    float healthFactor = ctx.myHealth / ctx.myMaxHealth;           // 0-1
    float threatFactor = 1.0f - ctx.targetThreatLevel;             // Low threat = high score
    float ammFactor = ctx.ammoPercent;                             // Need ammo to attack
    float distFactor = 1.0f - std::min(1.0f, ctx.targetDist / 3000.0f); // Closer = better
    return healthFactor * 0.3f + threatFactor * 0.2f + ammFactor * 0.2f + distFactor * 0.3f;
}

float FleeScore(const AIContext& ctx) {
    float healthPanic = 1.0f - (ctx.myHealth / ctx.myMaxHealth);   // Low health = flee
    healthPanic = healthPanic * healthPanic;                        // Quadratic: panics faster at low HP
    float outnumbered = std::min(1.0f, ctx.enemyCount / 3.0f);    // More enemies = flee
    float shieldDown = ctx.shieldPercent < 0.1f ? 0.5f : 0.0f;    // Bonus if shields gone
    return healthPanic * 0.5f + outnumbered * 0.3f + shieldDown * 0.2f;
}

float TradeScore(const AIContext& ctx) {
    if (ctx.inCombat) return 0;                                     // Never trade in combat
    float profitOpportunity = ctx.bestTradeProfit / 10000.0f;      // Normalize
    float cargoSpace = ctx.cargoFreePercent;                        // Need space
    return std::min(1.0f, profitOpportunity * 0.6f + cargoSpace * 0.4f);
}

// Response curves (shape how inputs map to scores):
// Linear: y = x
// Quadratic: y = x² (slow start, fast end — good for panic)
// Inverse quadratic: y = 1-(1-x)² (fast start, slow end — good for diminishing returns)
// Logistic: y = 1/(1+e^(-10(x-0.5))) (sharp threshold at 0.5)
// Step: y = x > threshold ? 1 : 0 (binary decision)
```

---

## PART 3: PERCEPTION SYSTEM

### What NPCs Can Detect

```cpp
struct PerceptionData {
    // Vision
    float sightRange = 2000.0f;     // Meters (ship sensors)
    float sightAngle = 120.0f;      // Degrees (forward cone)
    float sightUpdateRate = 0.5f;   // Seconds between checks
    
    // Radar/Sensors (360°, longer range)
    float radarRange = 5000.0f;     // Ship sensor range
    float radarUpdateRate = 1.0f;   // Less frequent than vision
    
    // Audio (on-foot only)
    float hearingRange = 50.0f;     // Meters
    
    // Currently perceived entities
    struct PerceivedEntity {
        Entity entity;
        Vec3d lastKnownPosition;
        float lastSeenTime;
        float awareness;            // 0=unaware, 0.5=suspicious, 1.0=fully detected
        bool currentlyVisible;
    };
    std::vector<PerceivedEntity> perceivedEntities;
};

// Awareness ramp: detection isn't instant
// Stealth approach: awareness increases at 0.2/second
// Direct visual: awareness increases at 1.0/second
// Firing weapons: instant awareness = 1.0 for all nearby NPCs
// Out of sight: awareness decreases at 0.1/second
// At awareness 0.5: NPC becomes "suspicious" (investigates)
// At awareness 1.0: NPC enters combat/alert state
```

### Threat Assessment

```cpp
float AssessThreat(const PerceivedEntity& target, const AIContext& self) {
    float threat = 0;
    
    // Ship class threat (bigger ship = bigger threat)
    threat += target.shipClassThreat * 0.3f;  // 0=fighter, 1=capital
    
    // Distance (closer = more threatening)
    float dist = (target.lastKnownPosition - self.position).Length();
    threat += (1.0f - std::min(1.0f, dist / 3000.0f)) * 0.2f;
    
    // Hostile intent (aiming at us, firing)
    if (target.isFiringAtUs) threat += 0.3f;
    else if (target.isHeadingTowardUs) threat += 0.1f;
    
    // Faction hostility
    threat += self.factionHostility[target.factionId] * 0.2f;
    
    return std::clamp(threat, 0.0f, 1.0f);
}
```

---

## PART 4: NAVIGATION

### Space Navigation (No Navmesh Needed)

```
In space, navigation is straightforward:
1. Compute direction to target
2. Accelerate toward target (IFCS handles thruster allocation)
3. Decelerate at halfway point (or computed braking distance)
4. Obstacle avoidance: cast rays in movement direction, steer around

Formations:
  Wing formation: wingmen offset 200m at 45° behind leader
  V formation: staggered diagonal, 150m spacing
  Line abreast: side by side, 300m spacing
  Escort: ring around protected ship, 500m radius
```

### Ground Navigation (Navmesh via Recast/Detour)

```
For on-foot and ground vehicle navigation:

Recast: builds navmesh from level geometry
  Cell size: 0.3m (resolution of walkable areas)
  Cell height: 0.2m (vertical step resolution)
  Agent radius: 0.4m (character collision radius)
  Agent height: 1.8m (character height)
  Max climb: 0.3m (maximum step height)
  Max slope: 45° (steeper = unwalkable)
  
Detour: pathfinding on the navmesh
  A* search on navmesh polygons
  Path smoothing with string-pulling (funnel algorithm)
  Dynamic obstacle avoidance (RVO - reciprocal velocity obstacles)
  
Off-mesh links: ladders, elevators, jump points, teleporters
  Each defined as: start position, end position, traversal cost, bidirectional flag
```

---

## PART 5: COMBAT AI TACTICS

### Dogfight Maneuvers (Ship-to-Ship)

```
1. Lead Pursuit: aim ahead of target's velocity vector
   (the default approach maneuver)

2. Lag Pursuit: aim behind target
   (used to close distance without overshooting)

3. Barrel Roll Defense: roll while pulling up
   (forces attacker to overshoot, reverses positions)

4. Split-S: half loop + roll to reverse direction
   (emergency reversal when outmatched)

5. Boom and Zoom: high-speed pass, fire, extend away, repeat
   (used by heavy fighters against more agile opponents)

6. Jousting: head-on pass, fire, turn for next pass
   (used when both ships are similar in capability)

AI selects maneuver based on:
  Relative position + velocity of target
  Own ship's maneuverability vs target's
  Current health/shield state
  Weapon type (beam vs projectile affects engagement range)
```

### Fleet Tactics (Capital Ship Battles)

```
Roles in fleet combat:
  Flagship: coordinates fleet, provides sensor data
  Escort: protects flagship from fighters/torpedoes
  Striker: attacks enemy capital ships (torpedo runs)
  Screen: fighters that engage enemy fighters
  Support: repair/resupply ships, stay behind front line

Formations update dynamically:
  Aggressive: screen forward, strikers flanking, flagship behind
  Defensive: all ships circle flagship, concentrated fire zone
  Retreat: escorts cover withdrawal, screen delays pursuit
```

---

## PART 6: CURRICULUM STEP UPDATES

**Step 531-540 (AI foundation)**: BTNode base class. Sequence, Selector, Leaf nodes. Fighter AI behavior tree from Part 1.

**Step 541-550 (Perception)**: Vision cone raycast. Radar sphere overlap. Awareness ramp system. Threat assessment scoring.

**Step 551-560 (Navigation)**: Space: direct approach with obstacle avoidance rays. Ground: Recast navmesh generation from station geometry. Detour A* pathfinding.

**Step 561-570 (Combat AI)**: Target selection (threat × distance scoring). Dogfight maneuver library. Weapon range management. Flee/disengage decisions.

**Step 571-580 (Utility AI)**: Utility scoring system for NPC daily decisions. Trade/patrol/combat/rest scoring functions. Response curves for nuanced behavior.

**Step 581-600 (Emergent AI)**: Gossip propagation, memory system, opinion matrix, reputation echo (all from v1 EmergentAISystem). Fleet coordination and formation systems.

**Step 1531-1630 (AI depth)**: GOAP (Goal-Oriented Action Planning) for complex quest NPCs. Hierarchical task networks. Squad-level coordination. Dynamic difficulty adjustment. Enemy learning (adapts to player tactics over time).
