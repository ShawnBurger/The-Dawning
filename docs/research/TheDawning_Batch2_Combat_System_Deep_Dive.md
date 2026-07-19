# THE DAWNING — Combat System Deep Dive
# Batch 2, Topic 4: Weapon Balance, TTK, Damage Model, Armor/Shields,
#                    Targeting, Projectile Ballistics, Combat Feel
# Sources: Bungie GDC talks (Halo/Destiny weapon feel), Star Citizen component damage,
#          The Expanse ship combat physics, real-world ballistics

---

## PART 1: DAMAGE MODEL (Component-Based, Star Citizen Inspired)

Ships don't have a single HP pool — they have individual components that can be damaged.

```cpp
struct ShipComponent {
    std::string name;         // "Port Engine", "Shield Generator", "Cockpit"
    float health;
    float maxHealth;
    float armorRating;        // Damage reduction (flat subtraction)
    float armorPenetrationThreshold; // Damage below this is fully absorbed
    bool  destroyed;
    bool  critical;           // Destroying this = ship disabled/destroyed
    Vec3f position;           // Position on ship hull (for hit detection)
    Vec3f size;               // Bounding box size
    
    // Effects when damaged
    float performanceAt50Pct; // 0.5 = 50% performance at 50% health
    bool  causesFireWhenHit;
    bool  explosiveOnDestroy;
};

// Example: Medium Fighter Components
//   Cockpit:         HP=100, Armor=5,  Critical=YES (pilot dies)
//   Port Engine:     HP=200, Armor=10, Critical=NO  (lose thrust on that side)
//   Starboard Engine:HP=200, Armor=10, Critical=NO
//   Shield Generator:HP=150, Armor=5,  Critical=NO  (lose shields)
//   Reactor:         HP=300, Armor=15, Critical=YES (ship explodes)
//   Fuel Tank:       HP=100, Armor=5,  Critical=NO  (fire + fuel leak)
//   Weapon Mount L:  HP=80,  Armor=5,  Critical=NO  (lose weapon)
//   Weapon Mount R:  HP=80,  Armor=5,  Critical=NO
//   Hull (general):  HP=500, Armor=8,  Critical=YES (structural failure)
```

### Hit Detection Pipeline

```
1. Projectile traces ray (hitscan) or sweeps sphere (physical projectile)
2. First test: ship bounding sphere (fast reject)
3. Second test: ship OBB (oriented bounding box)
4. Third test: component bounding boxes within ship
5. Identify which component was hit
6. Apply damage to that specific component
7. If component destroyed: apply gameplay effect
```

---

## PART 2: WEAPON TYPES AND BALANCE

### Weapon Categories

| Weapon | Type | DPS | Range | Projectile Speed | Energy Cost | Role |
|---|---|---|---|---|---|---|
| **Pulse Laser** | Hitscan | 80 | 2000m | Instant | 10/shot | Precision |
| **Arc Lance** | Beam | 120 | 1500m | Instant | 30/s | Sustained |
| **Autocannon** | Kinetic | 150 | 1000m | 800 m/s | 0 (ammo) | DPS king |
| **Railgun** | Kinetic | 250 | 4000m | 2000 m/s | 50/shot | Sniper |
| **Fusion Torpedo** | Guided | 500/hit | 5000m | 200 m/s | 100/shot | Alpha |
| **Scatter Cannon** | Kinetic | 200@100m | 300m | 600 m/s | 0 (ammo) | CQC |
| **EMP Pulse** | Energy | 0 (disables) | 500m | Instant (AoE) | 200 | Utility |
| **Mining Laser** | Beam | 40 | 300m | Instant | 15/s | Non-combat |

### TTK (Time To Kill) Targets

```
Design philosophy: combat should last 15-60 seconds for fighters.
Longer for larger ships. This creates tension without being frustrating.

Fighter vs Fighter (equal skill):
  With shields up: 20-30 seconds
  Shields down: 8-15 seconds
  Critical hit (reactor/cockpit): instant kill if unshielded

Fighter vs Corvette:
  Fighter attacking: 60-120 seconds (chip away at components)
  Corvette defending: 10-20 seconds (one good burst kills fighter)

Corvette vs Corvette:
  30-60 seconds per engagement
  
Capital vs Capital:
  5-15 minutes (fleet battles are strategic, not twitch)

These TTK values determine all weapon DPS values:
  Target TTK × average DPS needed = total hull+shield HP
```

### Shield Mechanics

```cpp
struct ShieldState {
    float currentHP;          // Current shield points
    float maxHP;              // Maximum shield points
    float regenRate;          // Points per second regeneration
    float regenDelay;         // Seconds after last hit before regen starts
    float timeSinceLastHit;
    float resistKinetic;      // 0-1 damage reduction (0.3 = 30% reduction)
    float resistEnergy;       // Energy weapons vs shields
    float resistExplosive;    // Torpedoes/missiles vs shields
    bool  overloaded;         // True when HP = 0 (shields down!)
    float overloadRecoveryTime; // Seconds to restart after overload
};

// Shield recharge curve:
// 0-2 seconds after hit: NO regen (regenDelay)
// 2+ seconds: linear regen at regenRate
// When overloaded (HP=0): 5-second full blackout, then 50% regen rate for 10s

// Fighter shields:  500HP, regen 25/s, delay 2s
// Corvette shields: 3000HP, regen 80/s, delay 3s
// Capital shields:  25000HP, regen 200/s, delay 5s
```

---

## PART 3: PROJECTILE BALLISTICS

### Physical Projectiles (Not Hitscan)

```cpp
struct Projectile {
    Vec3d position;
    Vec3d velocity;           // Initial = gun muzzle velocity + ship velocity
    float lifetime;           // Max flight time before despawn
    float damage;
    float armorPenetration;   // Ignores this much armor rating
    float splashRadius;       // 0 = direct hit only, >0 = AoE
    float trackingStrength;   // 0 = dumbfire, 1.0 = perfect tracking (missiles)
    uint32_t targetEntity;    // For guided weapons
    uint32_t ownerEntity;     // Who fired this (for score attribution)
};

// Leading the target (essential for non-hitscan weapons)
Vec3d ComputeLeadPosition(Vec3d targetPos, Vec3d targetVel,
                              Vec3d shooterPos, float projectileSpeed)
{
    // Time for projectile to reach target's current position
    Vec3d toTarget = targetPos - shooterPos;
    float distance = toTarget.Length();
    float timeToTarget = distance / projectileSpeed;
    
    // Where the target will be at that time
    Vec3d predictedPos = targetPos + targetVel * timeToTarget;
    
    // Iterative refinement (2 iterations is usually sufficient)
    for (int i = 0; i < 2; i++) {
        toTarget = predictedPos - shooterPos;
        distance = toTarget.Length();
        timeToTarget = distance / projectileSpeed;
        predictedPos = targetPos + targetVel * timeToTarget;
    }
    
    return predictedPos;
}
```

### Missile/Torpedo Guidance

```cpp
// Proportional navigation: the real-world guidance law
Vec3d ProportionalNavigation(Vec3d missilePos, Vec3d missileVel,
                                 Vec3d targetPos, Vec3d targetVel,
                                 float navGain)  // 3-5 typical
{
    Vec3d relPos = targetPos - missilePos;
    Vec3d relVel = targetVel - missileVel;
    float closingSpeed = -relVel.Dot(relPos.Normalized());
    
    // Line-of-sight rotation rate
    Vec3d losRate = relPos.Cross(relVel) / relPos.Dot(relPos);
    
    // Commanded acceleration perpendicular to velocity
    Vec3d accelCmd = missileVel.Cross(losRate) * navGain * closingSpeed;
    
    return accelCmd;
}

// Countermeasures:
// Chaff/flares: -50% tracking for 3 seconds (missile loses lock probability)
// ECM jamming: -30% tracking continuous while active
// Hard maneuver: if target acceleration > missile max turn, missile overshoots
```

---

## PART 4: COMBAT FEEL

### Screen Effects During Combat

```
Hit received (damage to player ship):
  Screen shake: 0.02m amplitude, 0.3s duration, scales with damage
  Red vignette flash: 0.1s, intensity scales with damage/maxHP
  Sparks/debris particles from hit point
  Hull groan audio
  Camera micro-punch (slight push in hit direction)

Shield hit (absorbs damage):
  Blue/cyan flash on shield mesh
  Electric crackle particles along shield surface
  Shield hum audio pitch shifts up
  No screen shake (shields absorb impact)

Shield overload (shields go down):
  Full-screen blue flash 0.2s
  Electric discharge particles surrounding ship
  "Shield failure" alarm audio
  HUD shield indicator turns red/flashing

Critical hit (component destroyed):
  Explosion at component location
  Screen shake 0.05m, 0.5s
  Sparks + fire from damaged area
  System failure notification on HUD
  If engine: ship pulls in that direction (asymmetric thrust)
  If weapon: weapon goes offline, HUD weapon indicator greys out
```

### Target Lead Indicator (TLI)

```
For non-hitscan weapons, display a leading reticle showing where to aim:
  Position: ComputeLeadPosition() projected to screen space
  Shape: circle with pip, sized inversely to distance
  Color: green when in optimal range, yellow approaching max range, red beyond
  Pulsing: pulse when target is within weapons cone of fire
  
Accuracy cone:
  Each weapon has a spread angle (0.5° for railgun, 5° for scatter cannon)
  Display as a translucent cone projected on HUD
  Hits within cone: random point within cone radius at target distance
```

---

## PART 5: DAMAGE NUMBERS AND FEEDBACK

```
Damage numbers: float up from hit point, color-coded
  White: normal damage
  Yellow: critical component hit (2× damage)
  Orange: armor penetration (ignoring armor)
  Blue: shield damage
  Grey: absorbed by armor (0 damage)
  Size: proportional to damage amount

Kill notification:
  "SHIP DESTROYED" text + explosion
  Bounty earned notification (credits + XP)
  Kill feed in upper-right corner (for multiplayer)
```

---

## PART 6: CURRICULUM STEP UPDATES

**Step 471-480 (Combat foundation)**: Hitscan ray for basic laser. Damage application to ship HP. Shield check before hull damage.

**Step 481-490 (Weapons)**: Weapon types with DPS/range/projectile speed from table. Physical projectile simulation (position + velocity × dt).

**Step 491-500 (Targeting)**: Target lock-on system. Lead indicator computation. Missile guidance (proportional navigation).

**Step 501-510 (Component damage)**: Ship component list with individual HP. Hit detection identifies component. Destroyed components affect gameplay.

**Step 511-520 (Shields)**: Shield HP with regen delay. Energy/kinetic/explosive resistance types. Overload state with recovery time.

**Step 521-530 (Combat feel)**: Screen shake, vignette, particles on hit. Shield flash effects. Sound cues (impact, alarm, explosion).

**Step 531-540 (Encounters)**: Combat encounter spawning. Enemy AI engages with weapons. Wave system with escalating difficulty.

**Step 1531-1630 (AI depth)**: Enemy combat AI uses cover, flanking, target prioritization. Wingman AI coordinates with player. Fleet tactics for capital ship battles.
