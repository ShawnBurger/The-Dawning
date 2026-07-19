# THE DAWNING — Crafting & Progression Deep Dive
# Batch 4, Topic 2: Skill Trees, XP Curves, Crafting Recipes,
#                    Blueprint Discovery, Power Curve, Endgame Loop

---

## PART 1: EXPERIENCE AND LEVELING

### XP Curve (Logarithmic Growth)

```cpp
// XP required for each level
// Each level requires ~15% more XP than the previous
int XPForLevel(int level) {
    // Level 1: 1000 XP, Level 10: ~4000 XP, Level 50: ~100,000 XP
    return (int)(1000.0 * pow(1.15, level - 1));
}

// Total XP from level 1 to target level
int TotalXPForLevel(int targetLevel) {
    int total = 0;
    for (int i = 1; i < targetLevel; i++)
        total += XPForLevel(i);
    return total;
}

// Time to level (at expected earning rate):
// Level 1-10:    ~2 hours (tutorial + early missions)
// Level 10-20:   ~5 hours (opening up the galaxy)
// Level 20-30:   ~10 hours (mid-game)
// Level 30-40:   ~15 hours (late-game)
// Level 40-50:   ~20 hours (endgame grind)
// Total to max:  ~52 hours
```

### XP Sources

| Source | XP Amount | Frequency | Notes |
|---|---|---|---|
| **Kill (fighter)** | 50-200 | Common | Scale with enemy level |
| **Kill (capital)** | 500-2000 | Rare | Boss-level encounters |
| **Mission complete** | 300-3000 | Per mission | Scales with difficulty |
| **Discovery (planet)** | 100-500 | Per planet | First discovery bonus |
| **Discovery (system)** | 200-1000 | Per system | Exploration reward |
| **Trade profit** | 10% of profit | Per trade | Encourages trading |
| **Crafting success** | 50-500 | Per craft | Higher tier = more XP |
| **Scan (creature)** | 25-100 | Per species | Science XP |
| **Scan (anomaly)** | 100-500 | Per anomaly | Rare finds |

---

## PART 2: SKILL TREE

### Skill Disciplines (6 Trees)

```
Each tree has 20 skills, 4 tiers of 5 skills each.
Tier 1: costs 1 skill point each (levels 1-10)
Tier 2: costs 2 skill points each (levels 10-20, requires 3 Tier 1 skills)
Tier 3: costs 3 skill points each (levels 20-35, requires 3 Tier 2 skills)
Tier 4: costs 5 skill points each (levels 35-50, requires 3 Tier 3 skills, capstone)

Skill points earned: 1 per level = 49 total at level 50
Can fully max 1 tree (20 pts) + most of a second (20 pts) + basics of third (9 pts)
This forces specialization — no "master of everything"

COMBAT TREE:
  T1: Weapon Handling (+5% DPS), Quick Reload (-15% reload), Armor Piercing,
      Evasive Maneuvers (+10% dodge), Target Lock Speed (+20%)
  T2: Dual Weapons (fire two weapon groups), Shield Overcharge (+25% max),
      Critical Systems (2× damage to components), Missile Expert (+30% tracking),
      Hull Reinforcement (+15% HP)
  T3: Ace Pilot (time-slow during dodge), Broadside Master (capital weapon access),
      EMP Specialist (disable enemy systems), Torpedo Mastery (50% more damage),
      Fleet Commander (buff nearby allies)
  T4: Legendary Gunner (15% crit chance), Unbreakable (+50% hull, -20% speed)

ENGINEERING TREE:
  T1: Basic Repair (self-repair in combat), Fuel Efficiency (-10% consumption),
      Overclocking (+10% engine output), Shield Tuning (+15% regen),
      Scrap Recycling (salvage 20% more)
  T2: Advanced Repair (repair components mid-combat), Power Routing (+15% to chosen system),
      Emergency Shields (instant 50% shield), Thruster Mastery (+20% maneuverability),
      Module Crafting (craft Tier 2 modules)
  T3: Master Engineer (all systems +10%), Jury Rig (temp fix destroyed components),
      Reactor Overclock (+30% power, risk of overload), Advanced Crafting (Tier 3 modules),
      Drone Deploy (repair drone follows ship)
  T4: Legendary Engineer (ship never fully disabled), Master Crafter (Tier 4 modules)

TRADE TREE:
  T1-T4: Haggling, Cargo Expansion, Route Finder, Smuggling, Market Prediction,
         Contract Negotiation, Rare Goods Access, Trade Empire...

EXPLORATION TREE:
  T1-T4: Extended Sensors, Fuel Scoop, Survey Speed, Anomaly Detection,
         Deep Space Navigation, Planetary Landing Expert, Xenobiology Scanner...

DIPLOMACY TREE:
  T1-T4: Persuasion, Intimidation, Faction Relations, Alliance Broker,
         Peace Negotiation, Spy Network, Double Agent...

SCIENCE TREE:
  T1-T4: Advanced Scanning, Material Analysis, Reverse Engineering,
         Alien Tech Research, Experimental Weapons, Genetic Modification...
```

---

## PART 3: CRAFTING SYSTEM

### Recipe Structure

```cpp
struct CraftingRecipe {
    std::string outputItem;
    int outputCount = 1;
    int outputQualityBase = 1;    // 1-5 quality, modified by skill
    
    struct Ingredient {
        std::string itemId;
        int count;
    };
    std::vector<Ingredient> ingredients;
    
    std::string requiredStation;  // "workbench","forge","lab","shipyard"
    std::string requiredSkill;    // "engineering","science","trade"
    int requiredSkillLevel = 0;
    float craftTime;              // Seconds
    int tier;                     // 0-4 (matches rarity tiers)
    
    bool discovered = false;      // Must find blueprint first
};

// Quality system:
// Quality 1 (Basic):    100% base stats
// Quality 2 (Standard): 110% base stats
// Quality 3 (Superior): 125% base stats
// Quality 4 (Excellent):145% base stats
// Quality 5 (Masterwork):175% base stats
//
// Quality roll: baseQuality + (skill/25) + random(-1,+1)
// Clamped to 1-5
```

### Blueprint Discovery

```
Blueprints are found through gameplay, not given by default:
  Tier 0 (Common): known from start (basic repairs, ammo, food)
  Tier 1 (Uncommon): purchased from stations or found in wreckage
  Tier 2 (Rare): quest rewards, hidden caches, reverse-engineering
  Tier 3 (Epic): faction reputation rewards, boss drops
  Tier 4 (Legendary): unique quests, ancient ruins, Xenoes technology

Reverse engineering:
  Scan an item with Science skill → 20% + (skill/2)% chance to learn recipe
  Destroys the item on success
  Each item can only be attempted once
```

### Crafting Material Chain

```
RAW → REFINED → COMPONENT → MODULE → SHIP

Raw Materials (mining/salvage):
  Iron Ore → Refined Iron → Metal Plates → Hull Plating
  Silicon → Refined Silicon → Circuit Boards → Electronics
  Helion Crystal → Refined Helion → Helion Core → Helion Drive
  Organic Matter → Bio-Extract → Medicine / Food
  Rare Earth → Refined RE → Neural Processor → AI Core

Each step: costs time + fuel/energy + chance of failure based on skill
Value multiplier per step: Raw(1×) → Refined(2×) → Component(5×) → Module(15×)
This creates profitable crafting paths at every skill level
```

---

## PART 4: POWER CURVE AND ENDGAME

### Player Power by Level

```
Level 1:   Starter ship (10K value), basic weapons, 1000 credits
Level 10:  Light fighter (50K), Tier 1 weapons, 15K credits
Level 20:  Heavy fighter (200K), Tier 2 weapons, 80K credits
Level 30:  Corvette (800K), Tier 2-3 weapons, 300K credits
Level 40:  Frigate (3M), Tier 3 weapons, 1M credits
Level 50:  Custom ship (5-10M), Tier 3-4 weapons, 3M+ credits

DPS growth: ~2× per 10 levels (Level 1: 80 DPS → Level 50: ~2500 DPS)
HP growth: ~2.5× per 10 levels (Level 1: 500 HP → Level 50: ~50000 HP)
Speed growth: ~1.3× per tier (fighters always faster than capitals)
```

### Endgame Loop (Post Level 50)

```
1. Legendary hunts: procedurally generated rare bosses
2. Faction warfare: territory control PvP/PvE campaigns
3. Ancient ruins: randomly spawning dungeons with puzzle+combat
4. Trading empire: own stations, manage supply chains
5. Fleet building: recruit NPC crews, command multiple ships
6. Cosmetic: ship paint schemes, interior decoration, crew uniforms
7. New Game+: restart story with all skills, harder enemies, new dialogue options
8. Seasonal content: limited-time events with unique rewards

Player should always have 3+ goals at any time:
  Short-term: complete current mission, buy next upgrade
  Medium-term: reach next ship tier, complete faction chain
  Long-term: endgame activity, collection completion
```

---

## PART 5: CURRICULUM STEP UPDATES

**Step 601-610 (Economy)**: Already covered in Economy deep dive. Add crafting station as a location type.

**Step 641-650 (Crafting)**: Recipe struct with ingredients, required station, skill check. Crafting UI: ingredient slots, craft button, progress bar, quality result.

**Step 651-660 (Progression)**: XP system with logarithmic curve. Level-up notification. Skill point allocation UI. 6 skill trees with 20 skills each.

**Step 661-670 (Blueprints)**: Blueprint discovery from loot, quests, reverse-engineering. Blueprint collection UI. Recipe unlocks gated by blueprint ownership.

**Step 1901-1950 (Progression depth)**: Prestige/mastery system post-50. Achievement system with rewards. Stat tracking (kills, distance, trades). Leaderboards for multiplayer. Dynamic difficulty scaling based on player power level.
