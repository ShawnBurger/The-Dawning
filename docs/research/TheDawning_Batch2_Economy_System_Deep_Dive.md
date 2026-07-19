# THE DAWNING — Economy & Trade System Deep Dive
# Batch 2, Topic 3: Supply/Demand, Price Discovery, Trade Routes,
#                    Crafting Balance, Resource Sinks/Faucets, Inflation Control
# Sources: EVE Online economic reports, game economy design literature,
#          real-world microeconomics models adapted for virtual worlds

---

## PART 1: SUPPLY AND DEMAND MODEL

### Price Discovery Formula

```cpp
// Every tradeable commodity has a dynamic price based on local supply/demand
struct CommodityMarket {
    std::string commodityId;
    float basePrice;          // Designer-set baseline (credits)
    float currentSupply;      // Units available at this station
    float currentDemand;      // Units wanted by this station's industry/population
    float maxSupply;          // Storage capacity
    float productionRate;     // Units produced per game-hour at this station
    float consumptionRate;    // Units consumed per game-hour
    float lastTradePrice;     // Last transaction price
    float priceVolatility;    // How fast price reacts (0.1=stable, 0.5=volatile)
};

float ComputePrice(const CommodityMarket& market) {
    // Price = basePrice × (demand / supply)^elasticity
    // When supply > demand: price drops below base
    // When demand > supply: price rises above base
    
    float supplyRatio = market.currentDemand / (market.currentSupply + 1.0f);
    float elasticity = 0.5f; // 0.5 = moderate price sensitivity
    float priceMul = powf(supplyRatio, elasticity);
    
    // Clamp price to 10%-500% of base (prevent degenerate values)
    priceMul = std::clamp(priceMul, 0.1f, 5.0f);
    
    // Smooth toward new price (prevent instant jumps)
    float targetPrice = market.basePrice * priceMul;
    float smoothedPrice = Lerp(market.lastTradePrice, targetPrice, market.priceVolatility);
    
    return smoothedPrice;
}
```

### Station Industry Simulation

```
Each station has industries that consume inputs and produce outputs:

Mining Station:
  Produces: Raw Ore (50 units/hr), Crystals (5 units/hr)
  Consumes: Energy Cells (10/hr), Food (20/hr), Equipment (2/hr)
  
Refinery:
  Produces: Refined Metals (30/hr), Alloys (10/hr)
  Consumes: Raw Ore (50/hr), Energy Cells (20/hr), Chemicals (5/hr)

Shipyard:
  Produces: Ship Components (5/hr), Ships (0.1/hr)
  Consumes: Refined Metals (40/hr), Electronics (10/hr), Alloys (15/hr)

Agricultural Station:
  Produces: Food (100/hr), Organic Compounds (20/hr)
  Consumes: Water (50/hr), Energy Cells (10/hr), Seeds (5/hr)

Tech Lab:
  Produces: Electronics (15/hr), Medicine (10/hr), Software (5/hr)
  Consumes: Rare Elements (5/hr), Energy Cells (15/hr)

This creates natural trade routes:
  Mining → Refinery (raw ore)
  Refinery → Shipyard (metals)
  Agriculture → Everyone (food)
  Tech Lab → Shipyard (electronics)
  Everyone → Mining (equipment, food)
```

---

## PART 2: TRADE ROUTE GENERATION

```cpp
struct TradeRoute {
    std::string fromStation;
    std::string toStation;
    std::string commodity;
    float buyPrice;           // Price at source
    float sellPrice;          // Price at destination
    float profitPerUnit;      // sellPrice - buyPrice
    float distance;           // Travel time in minutes
    float profitPerMinute;    // profitPerUnit × cargoCapacity / travelTime
    float risk;               // 0=safe, 1=pirates everywhere
};

// Auto-generate profitable routes for player trade UI
std::vector<TradeRoute> FindProfitableRoutes(
    const std::vector<Station>& stations, float minProfitMargin)
{
    std::vector<TradeRoute> routes;
    
    for (const auto& src : stations) {
        for (const auto& dst : stations) {
            if (&src == &dst) continue;
            
            for (const auto& commodity : src.markets) {
                // Find this commodity at destination
                auto dstMarket = FindMarket(dst, commodity.commodityId);
                if (!dstMarket) continue;
                
                float buyPrice = ComputePrice(commodity);
                float sellPrice = ComputePrice(*dstMarket);
                float margin = (sellPrice - buyPrice) / buyPrice;
                
                if (margin >= minProfitMargin) {
                    TradeRoute route;
                    route.fromStation = src.id;
                    route.toStation = dst.id;
                    route.commodity = commodity.commodityId;
                    route.buyPrice = buyPrice;
                    route.sellPrice = sellPrice;
                    route.profitPerUnit = sellPrice - buyPrice;
                    route.distance = ComputeTravelTime(src.position, dst.position);
                    route.risk = ComputeRouteRisk(src.position, dst.position);
                    routes.push_back(route);
                }
            }
        }
    }
    
    // Sort by profit/minute
    std::sort(routes.begin(), routes.end(), [](const TradeRoute& a, const TradeRoute& b) {
        return a.profitPerMinute > b.profitPerMinute;
    });
    
    return routes;
}
```

---

## PART 3: RESOURCE SINKS AND FAUCETS

### Preventing Inflation (The #1 Economy Killer)

```
FAUCETS (money/resources entering the economy):
  - Mission rewards: ~500-5000 credits per mission
  - Bounty kills: 100-2000 credits per ship destroyed
  - Mining/salvage: resource value varies by rarity
  - Trade profit margins: 5-30% per route
  - Discovery bonuses: first-discovery of planets/systems

SINKS (money/resources leaving the economy):
  - Ship repairs: 5-20% of ship value after combat
  - Fuel costs: continuous drain during travel
  - Ammunition: consumed in combat, must be repurchased
  - Station docking fees: 50-500 credits per dock
  - Insurance premiums: 1-5% of ship value per game-week
  - Ship upgrades: large one-time costs
  - Crafting material consumption: materials destroyed in crafting
  - Tax on transactions: 2-5% sales tax at stations
  - Ship destruction: total loss of ship + cargo (biggest sink)
  - Consumable items: food, medicine, repair kits

BALANCE RULE: sinks must slightly exceed faucets long-term
  Target: player wealth grows ~5% per real-time hour of play
  If growing faster: increase repair costs or add new sinks
  If stagnating: increase mission rewards or reduce tax
```

### Rarity Tiers and Value Scaling

| Tier | Name | Drop Rate | Value Multiplier | Example |
|---|---|---|---|---|
| 0 | **Common** | 60% | 1× | Iron Ore, Water, Food |
| 1 | **Uncommon** | 25% | 3× | Copper, Silicon, Medicine |
| 2 | **Rare** | 10% | 10× | Titanium, Rare Earths, Electronics |
| 3 | **Epic** | 4% | 35× | Helion Crystals, Neural Processors |
| 4 | **Legendary** | 1% | 150× | Ancient Tech, Xenoes Artifacts |

---

## PART 4: CRAFTING ECONOMY

### Recipe Balance Rules

```
Every craftable item must satisfy:
  Output value ≥ Sum(input values) × 1.3 (30% profit incentive)
  Output value ≤ Sum(input values) × 2.5 (prevent infinite money from crafting)
  
Crafting time scales with item tier:
  Common items: 10-30 seconds
  Uncommon: 1-5 minutes
  Rare: 5-30 minutes
  Epic: 1-4 hours (real-time, can queue)
  Legendary: 8-24 hours

Failure/quality system:
  Skill level 0-100 for each crafting discipline
  Success rate: 50% + (skill / 2)% = 50-100%
  Critical success (bonus quality): skill% / 5 = 0-20%
  Failure: materials partially returned (50-80% based on skill)
```

### Ship Module Costs (Credit/Resource Reference)

| Module | Credits | Materials | Tier |
|---|---|---|---|
| Basic Laser | 1,000 | 5 Metal, 2 Electronics | Common |
| Arc Lance Mk1 | 8,000 | 15 Alloy, 5 Crystal, 3 Rare Earth | Uncommon |
| Arc Lance Mk3 | 50,000 | 40 Alloy, 15 Crystal, 8 Neural Proc | Rare |
| Fusion Torpedo Bay | 120,000 | 80 Alloy, 30 Helion, 20 Electronics | Epic |
| Shield Generator Mk1 | 5,000 | 10 Metal, 5 Electronics, 2 Crystal | Common |
| Helion Drive | 200,000 | 50 Helion, 40 Alloy, 20 Rare Earth | Epic |
| Hull Plating (per m²) | 100 | 2 Metal or 1 Alloy | Common |

---

## PART 5: NPC ECONOMIC BEHAVIOR

```cpp
// NPCs participate in the economy as autonomous agents
struct NPCTrader {
    std::string id;
    std::string homeStation;
    float credits;
    float cargoCapacity;
    std::vector<CargoItem> cargo;
    float riskTolerance;      // 0=safe routes only, 1=pirate territory OK
    float greed;              // 0=accepts small margins, 1=only high profit
    
    // AI decision: what to buy and where to sell
    TradeRoute ChooseRoute(const std::vector<TradeRoute>& routes) {
        for (const auto& route : routes) {
            if (route.risk > riskTolerance) continue;
            float minMargin = 0.05f + greed * 0.20f;  // 5-25% minimum
            if (route.profitPerUnit / route.buyPrice >= minMargin)
                return route;
        }
        return {}; // No acceptable route, stay at station
    }
};

// NPC traders stabilize prices:
// - If a commodity is cheap somewhere, NPCs buy it, reducing supply, raising price
// - If expensive somewhere, NPCs sell there, increasing supply, lowering price
// - This creates a natural equilibrium around base prices
// - Player can still exploit temporary imbalances for profit
// - ~100 NPC traders in the galaxy keep the economy moving
```

---

## PART 6: CURRICULUM STEP UPDATES

**Step 601-610 (Economy foundation)**: CommodityMarket struct per station.
Price discovery formula. Base prices from designer table.

**Step 611-620 (Trade system)**: Buy/sell UI at stations. Cargo hold management.
Trade route finder. Profit/loss tracking per voyage.

**Step 621-630 (Station industry)**: Production/consumption cycles per station.
Supply depletes, demand creates shortages. Prices fluctuate over game-time.

**Step 631-640 (NPC traders)**: Autonomous NPC trading ships following profitable routes.
They buy low, sell high, stabilize prices. Player can intercept (piracy) or compete.

**Step 641-650 (Crafting)**: Crafting stations with recipe system. Skill progression.
Material costs from economy. Output items enter economy when sold.

**Step 651-660 (Progression balance)**: Player earning curve: ~1000 credits/hour at start,
~50,000/hour at endgame. Ship prices from 10,000 (starter) to 5,000,000 (capital).
Player should afford next ship tier every 8-12 hours of play.

**Step 661-670 (Sinks)**: Repair costs, fuel costs, insurance, tax. Calibrated to
remove ~80% of earned credits through gameplay sinks (20% net accumulation).
