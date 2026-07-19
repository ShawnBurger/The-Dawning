# THE DAWNING — Story & Quest System Deep Dive
# Batch 4, Topic 1: Branching Dialogue, Quest State Machines,
#                    Narrative Variables, Consequence Tracking, Cinematic Triggers
# Sources: Ink/Yarn narrative engines, Witcher 3 quest design postmortem,
#          Obsidian RPG design talks, Bioware dialogue wheel research

---

## PART 1: QUEST STATE MACHINE

### Quest Lifecycle

```cpp
enum class QuestState : uint8_t {
    Unavailable,    // Prerequisites not met
    Available,      // Can be accepted (NPC has quest marker)
    Active,         // Player accepted, working on objectives
    ObjectiveComplete, // All objectives done, need to turn in
    TurnedIn,       // Completed, rewards given
    Failed,         // Failed (timed out, NPC died, wrong choice)
    Abandoned       // Player manually abandoned
};

struct QuestObjective {
    std::string id;
    std::string description;    // "Destroy the pirate base at Sigma-7"
    enum Type { Kill, Deliver, Reach, Interact, Escort, Defend, Collect, Talk, Scan };
    Type type;
    
    std::string targetId;       // Entity/location/item to interact with
    int requiredCount = 1;      // How many (kill 5, collect 3)
    int currentCount = 0;
    bool completed = false;
    bool optional = false;      // Optional objectives affect reward quality
    bool hidden = false;        // Revealed during quest progression
    
    // Tracking
    Vec3d waypointPosition;     // Where to go (for HUD marker)
    std::string waypointLabel;  // "Pirate Base" on HUD
    float timeLimit = 0;        // 0 = no time limit, >0 = seconds
};

struct Quest {
    std::string id;
    std::string title;          // "The Lost Convoy"
    std::string description;    // Full description for quest log
    std::string giver;          // NPC who gives this quest
    std::string giverLocation;  // Where to find them
    
    QuestState state = QuestState::Unavailable;
    
    // Objectives (can be sequential or parallel)
    std::vector<QuestObjective> objectives;
    bool objectivesSequential = false;  // true=must complete in order
    int currentObjectiveIndex = 0;      // For sequential quests
    
    // Prerequisites
    std::vector<std::string> requiredQuests;   // Must be TurnedIn
    std::vector<std::string> requiredFactions;  // Faction standing >= threshold
    float requiredReputation = 0;               // Minimum player reputation
    int requiredLevel = 0;
    
    // Rewards
    int creditReward = 0;
    int xpReward = 0;
    std::vector<std::string> itemRewards;
    float reputationReward = 0;
    std::string factionRewardId;        // Which faction gains standing
    
    // Branching
    std::vector<std::string> choicesMade;  // Player decisions during quest
    std::string branchVariable;            // Narrative variable set by this quest
};
```

### Quest Event Processing

```cpp
void QuestSystem::OnEvent(const GameEvent& event, QuestLog& log) {
    for (auto& quest : log.activeQuests) {
        if (quest.state != QuestState::Active) continue;
        
        for (auto& obj : quest.objectives) {
            if (obj.completed) continue;
            
            switch (obj.type) {
                case QuestObjective::Kill:
                    if (event.type == "entity_killed" && event.targetId == obj.targetId)
                        obj.currentCount++;
                    break;
                case QuestObjective::Reach:
                    if (event.type == "entered_location" && event.locationId == obj.targetId)
                        obj.currentCount = obj.requiredCount;
                    break;
                case QuestObjective::Deliver:
                    if (event.type == "item_given" && event.itemId == obj.targetId 
                        && event.recipientId == quest.giver)
                        obj.currentCount++;
                    break;
                case QuestObjective::Talk:
                    if (event.type == "dialogue_complete" && event.npcId == obj.targetId)
                        obj.completed = true;
                    break;
                // ... etc for each type
            }
            
            if (obj.currentCount >= obj.requiredCount)
                obj.completed = true;
        }
        
        // Check if all required objectives are complete
        bool allDone = true;
        for (const auto& obj : quest.objectives)
            if (!obj.optional && !obj.completed) { allDone = false; break; }
        
        if (allDone)
            quest.state = QuestState::ObjectiveComplete;
    }
}
```

---

## PART 2: BRANCHING DIALOGUE SYSTEM

### Dialogue Node Graph

```cpp
struct DialogueLine {
    std::string speakerId;       // NPC id or "player"
    std::string text;            // The actual dialogue text
    std::string voiceClipId;     // Audio file reference (optional)
    float duration = 3.0f;       // Display time if no voice
    std::string emotion;         // "neutral","angry","sad","happy" → drives animation
    std::string cameraAngle;     // "closeup","over_shoulder","wide" → drives cinematic
};

struct DialogueChoice {
    std::string text;            // What the player sees: "[Threaten] Back off or else."
    std::string tag;             // "threaten","persuade","bribe","honest","lie"
    std::string nextNodeId;      // Which dialogue node this leads to
    
    // Requirements (grey out if not met)
    std::string requiredSkill;   // "persuasion" skill check
    int requiredSkillLevel = 0;
    std::string requiredItem;    // Must have this item
    std::string requiredFaction; // Faction standing check
    float requiredReputation = 0;
    
    // Consequences
    float reputationChange = 0;
    std::string setVariable;     // Set a narrative variable
    std::string variableValue;
    std::string triggerEvent;    // Fire a game event
};

struct DialogueNode {
    std::string id;
    std::vector<DialogueLine> lines;     // NPC speaks these lines in sequence
    std::vector<DialogueChoice> choices; // Player chooses (if empty, auto-advance)
    
    // Conditions for this node to be reachable
    std::string condition;       // Expression: "quest_convoy_complete AND NOT betrayed_rey"
    std::string nextNodeId;      // Auto-advance to this node (if no choices)
};

struct DialogueTree {
    std::string id;
    std::string npcId;
    std::vector<DialogueNode> nodes;
    std::string entryNodeId;     // Where the conversation starts
    
    // Context-sensitive entry: different starting node based on game state
    struct ConditionalEntry {
        std::string condition;   // "quest_active:lost_convoy"
        std::string nodeId;      // Start here if condition is true
    };
    std::vector<ConditionalEntry> conditionalEntries;
};
```

### Narrative Variable System

```cpp
// Global narrative state — persists across the entire game
// Tracks every meaningful player decision
class NarrativeState {
    std::unordered_map<std::string, std::string> variables;
    
public:
    void Set(const std::string& key, const std::string& value) {
        variables[key] = value;
    }
    
    std::string Get(const std::string& key, const std::string& defaultVal = "") const {
        auto it = variables.find(key);
        return it != variables.end() ? it->second : defaultVal;
    }
    
    bool IsSet(const std::string& key) const { return variables.count(key) > 0; }
    
    // Evaluate conditions: "quest_complete:convoy AND reputation > 50 AND NOT betrayed_rey"
    bool Evaluate(const std::string& expression) const;
};

// Examples of narrative variables:
// "saved_colony_7" = "true"              (player choice in a mission)
// "rey_relationship" = "trusted"          (NPC relationship state)
// "faction_xenoes_war" = "active"         (world state)
// "player_killed_civilians" = "3"         (tracked for consequences)
// "main_quest_act" = "2"                  (story progression)
// "sloan_alive" = "false"                 (character fate)
```

---

## PART 3: QUEST DESIGN PATTERNS

### The Dawning's Quest Types

| Type | Duration | Complexity | Example |
|---|---|---|---|
| **Errand** | 5-10 min | 1 objective | "Deliver medical supplies to Station Gamma" |
| **Bounty** | 10-20 min | 1-2 objectives | "Destroy pirate captain Vex and his escorts" |
| **Investigation** | 15-30 min | 3-5 objectives | "Scan anomaly, interview witnesses, find source" |
| **Multi-stage** | 30-60 min | 5-8 objectives | "Infiltrate base, steal plans, escape, deliver" |
| **Story arc** | 2-4 hours | 10+ objectives | Main quest chain with multiple missions |
| **Faction chain** | 5-10 hours | 20+ objectives | Complete faction storyline (reputation gated) |

### Consequence Tiers

```
Tier 1 — Immediate (during quest):
  NPC reacts to your choice in dialogue
  Different enemies spawn based on approach (stealth vs assault)
  Reward quantity changes (honest = less pay, intimidate = more)

Tier 2 — Short-term (next few quests):
  NPC remembers your choice, different greeting next time
  Faction standing shifts open/close quest opportunities
  Alternative quest paths unlock based on previous decisions

Tier 3 — Long-term (act-level):
  Major NPCs live or die based on earlier choices
  Faction allegiances determine which faction's endgame you experience
  Story endings branch based on accumulated Tier 1+2 decisions

Tier 4 — Persistent (forever):
  Reputation echo carries your choices to stations you haven't visited
  NPCs gossip about your decisions (emergent AI integration)
  Some consequences are irreversible (destroyed stations stay destroyed)
```

### The Dawning Campaign Structure

```
ACT 1: AWAKENING (Missions 1-7)
  Tutorial + character establishment
  Player choices: which faction to align with initially
  Key decision: save colony OR pursue pirates (can't do both in time)

ACT 2: CONVERGENCE (Missions 8-15)
  Faction tensions escalate, player drawn into conflict
  3 parallel quest lines (one per major faction)
  Key decision: which faction's intelligence to trust
  
ACT 3: RECKONING (Missions 16-22)
  Full war breaks out, player's earlier choices shape the battlefield
  Missions differ dramatically based on ACT 1+2 decisions
  Key decision: how to end the conflict (3 endings × 2 variants each = 6 endings)

Side content: 40+ optional quests across all acts
  Companion quests: 7 companions × 3 quests each = 21 companion missions
  Faction missions: 6 factions × 5 missions each = 30 faction missions
  Discovery quests: auto-generated from planet exploration
```

---

## PART 4: CINEMATIC SYSTEM

### Scripted Sequences

```cpp
struct CinematicEvent {
    float startTime;            // Seconds from sequence start
    float duration;
    enum Type { CameraMove, CameraShake, DialogueLine, PlayAnimation,
                SpawnEntity, DestroyEntity, PlaySound, FadeScreen,
                SetWeather, TeleportPlayer, DisableInput, EnableInput,
                WaitForCondition, BranchChoice };
    Type type;
    std::string parameters;     // JSON-encoded parameters per type
};

struct CinematicSequence {
    std::string id;
    std::vector<CinematicEvent> events;
    float totalDuration;
    bool skippable = true;       // Player can press ESC to skip
    bool pauseGameplay = true;   // Freeze NPCs during cinematic
    std::string onSkipJumpTo;    // Skip to this point in quest
};

// Camera positions for dialogue:
// Over-shoulder: camera 1.5m behind speaker, 0.3m offset, looking at listener
// Closeup: camera 0.8m from speaker's face, slightly below eye level
// Two-shot: camera 2.5m to the side, both speakers visible
// Establishing: camera 10-20m away, showing environment + characters
// Reaction: quick cut to listener's face during important lines
```

---

## PART 5: CURRICULUM STEP UPDATES

**Step 671-680 (Quest foundation)**: Quest struct with state machine. QuestObjective types. Event-driven objective tracking. HUD waypoint marker.

**Step 681-690 (Dialogue)**: DialogueTree with nodes and choices. Dialogue UI panel (speaker portrait, text, choice buttons). Narrative variable system.

**Step 691-700 (Branching)**: Conditional dialogue entries based on game state. Skill checks gating choices. Consequence tracking (reputation, faction standing).

**Step 701-710 (Campaign)**: 22 main story missions with branching paths. 3-act structure. Key decision points that affect later content.

**Step 711-720 (Cinematics)**: Scripted camera sequences. Dialogue camera angles. Screen fades. Animation triggers during conversations.

**Step 721-730 (Side content)**: Faction quest chains. Companion quests with relationship system. Procedurally triggered discovery quests from exploration.

**Step 1831-1900 (Story depth)**: Full voice acting pipeline integration. Lip sync from audio waveform. Branching quest editor tool. Save/load of all narrative variables. Multiple ending cinematics.
