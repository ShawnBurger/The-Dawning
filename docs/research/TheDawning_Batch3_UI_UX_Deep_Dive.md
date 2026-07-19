# THE DAWNING — UI/UX Framework Deep Dive
# Batch 3, Topic 3: Immediate Mode UI, Widget System, Accessibility,
#                    Input Latency, HUD Design, Menu Architecture
# Sources: Dear ImGui internals, WCAG accessibility guidelines, game UX research

---

## PART 1: UI ARCHITECTURE

### Immediate Mode vs Retained Mode

```
The Dawning uses HYBRID:
  - Game HUD: immediate mode (rebuilt every frame, simple, fast)
  - Menus/panels: retained mode (widget tree persists, handles events)
  - Debug UI: Dear ImGui (pure immediate mode, dev-only)

Immediate mode (HUD):
  Every frame: if (showHealthBar) DrawHealthBar(hp, maxHp, screenPos);
  Pro: No state management, trivial to add/remove elements
  Con: Can't easily handle complex interactions (scrolling lists, text input)
  
Retained mode (menus):
  Widget tree built once, updated when data changes
  Pro: Handles complex interactions, layout, focus management
  Con: More code, state synchronization needed
```

### Render Pipeline

```
UI renders LAST, on top of everything else:
1. 3D scene renders to HDR buffer
2. Post-processing (bloom, tonemap) → LDR buffer
3. UI renders to LDR buffer with alpha blending
4. Present to swap chain

UI rendering: batched quad draws
  - All UI elements are textured quads
  - Single texture atlas (1024×1024) for all UI sprites/icons/fonts
  - Batch all quads with same texture into one draw call
  - Typical UI: 2-5 draw calls per frame
  - Target: <1ms total UI render time
```

### Font Rendering

```
SDF (Signed Distance Field) fonts:
  - Single texture works at all sizes (no blurring when scaled)
  - Sharp edges via alpha test in pixel shader
  - Outline/shadow/glow effects trivially in shader
  - Typical font atlas: 512×512, covers ASCII + extended latin
  
Font sizes for The Dawning:
  Title:      48px (ship names, location headers)
  Heading:    32px (panel titles, section headers)
  Body:       20px (descriptions, dialogue, most text)
  Small:      16px (stats, tooltips, fine print)
  HUD values: 24px (health, speed, ammo — high contrast)
  
Font: monospace for numerical readouts (aligned columns)
      proportional for all other text
```

---

## PART 2: HUD DESIGN

### HUD Layout (1920×1080 Reference)

```
┌─────────────────────────────────────────────────────────────┐
│ [SHIELDS: ████░░] [HULL: ██████░]          [FPS: 144] [NET] │ ← Top bar
│                                                              │
│                                                              │
│ [RADAR]                                              [COMMS] │ ← Sides
│                                                              │
│                    ╔═══════╗                                 │
│                    ║RETICLE║  ← Crosshair + target lead      │
│                    ╚═══════╝                                 │
│                                                              │
│ [TARGET INFO]                                [WEAPON SELECT] │
│                                                              │
│ [SPEED: 450 m/s] [THROTTLE: ██████░░░░]  [FUEL: 72%]      │ ← Bottom bar
│ [MISSION: Patrol Sector 7]                [CREDITS: 12,450] │
└─────────────────────────────────────────────────────────────┘

Center: keep clear for gameplay visibility (only crosshair)
Edges: all information panels
Corners: least-critical information
Left side: radar/minimap (spatial awareness)
Right side: communications/mission log
Bottom: ship status (speed, throttle, fuel)
Top: health/shields (most critical — eye naturally goes up)
```

### HUD Opacity and Scaling

```
Adaptive HUD opacity:
  Combat: 100% opacity (all elements visible)
  Cruise: 70% opacity (fade non-critical elements)
  Exploration: 40% opacity (minimal, mostly crosshair)
  Cinematic: 0% opacity (HUD hidden during cutscenes)
  
Scaling by resolution:
  UI designed at 1920×1080 reference
  Scale factor = min(screenWidth/1920, screenHeight/1080)
  Minimum scale: 0.5 (960×540 still readable)
  Maximum scale: 2.0 (3840×2160 not oversized)
  
Safe zone: 5% margin from screen edges (for TV overscan)
```

---

## PART 3: ACCESSIBILITY

### WCAG-Inspired Standards for Games

```
Visual:
  ✅ Colorblind modes: Protanopia, Deuteranopia, Tritanopia
     - Never use red/green as ONLY differentiator
     - Provide icon shapes alongside colors
     - Enemy: red + triangle, Friendly: blue + circle, Neutral: yellow + diamond
  ✅ Text scaling: 80%, 100%, 120%, 150%, 200%
  ✅ High contrast mode: white text on solid dark backgrounds
  ✅ Minimum text size: 18px at 1080p (12px is unreadable for many)
  ✅ Subtitle size options: Small, Medium, Large, Extra Large
  ✅ Screen reader hints (text descriptions for UI elements)

Audio:
  ✅ Subtitles for all dialogue (on by default)
  ✅ Speaker name labels on subtitles
  ✅ Visual sound indicators (directional pips for off-screen sounds)
  ✅ Separate volume sliders: Master, Music, SFX, Voice, UI
  ✅ Mono audio option (hearing impaired in one ear)

Input:
  ✅ Full key rebinding (every action remappable)
  ✅ Controller support with remappable buttons
  ✅ Toggle vs hold options (sprint, aim, crouch)
  ✅ Adjustable dead zones for analog sticks
  ✅ Mouse sensitivity with separate X/Y
  ✅ One-handed control schemes
  ✅ Auto-aim assist (adjustable strength 0-100%)

Cognitive:
  ✅ Objective markers (toggleable)
  ✅ Quest log with clear current objective
  ✅ Difficulty settings that separately adjust: damage, aim assist, puzzle hints
  ✅ Pause in singleplayer (always, including cutscenes)
  ✅ Tutorial messages can be re-read from menu
```

---

## PART 4: INPUT LATENCY

### Latency Budget (Click-to-Pixel)

```
Target: <50ms total input latency for competitive feel

Breakdown:
  USB polling:          1ms (1000Hz mouse)
  Input processing:     <1ms (read at frame start)
  Game logic:           <5ms (process input → update state)
  Render submission:    <2ms (submit draw calls)
  GPU rendering:        <10ms (produce frame)
  Composition:          1-3ms (OS compositor, can bypass with exclusive fullscreen)
  Display:              4-8ms (60Hz=16ms display time, 144Hz=7ms, 240Hz=4ms)
  Total:                14-30ms at 144fps with exclusive fullscreen

Optimizations:
  - Process input at the VERY start of the frame
  - Use low-latency present mode (DXGI_SWAP_EFFECT_FLIP_DISCARD)
  - Disable V-sync or use variable refresh rate (FreeSync/G-Sync)
  - Reduce render queue depth: max 1 frame queued (not 3)
  - Late-latch input: update camera with latest input AFTER game logic
```

### Input Buffering

```cpp
// Buffer inputs for responsive combat
struct InputBuffer {
    struct Entry {
        uint16_t action;     // Action ID (fire, dodge, interact)
        double timestamp;     // When the input was pressed
    };
    
    static const int BUFFER_SIZE = 8;
    Entry buffer[BUFFER_SIZE];
    int head = 0;
    
    // Buffer window: inputs pressed within 100ms before an action becomes
    // available are still executed (prevents "I pressed it but nothing happened!")
    float bufferWindow = 0.1f; // 100ms
    
    bool HasBufferedAction(uint16_t action, double currentTime) {
        for (int i = 0; i < BUFFER_SIZE; i++) {
            if (buffer[i].action == action &&
                currentTime - buffer[i].timestamp < bufferWindow) {
                buffer[i].action = 0; // Consume
                return true;
            }
        }
        return false;
    }
};
```

---

## PART 5: MENU ARCHITECTURE

```
Main Menu
├── Continue (load last save)
├── New Game
│   ├── Character Creation
│   └── Difficulty Selection
├── Load Game
│   └── Save Slot List
├── Settings
│   ├── Graphics (resolution, quality, vsync, FOV)
│   ├── Audio (volume sliders per bus)
│   ├── Controls (key bindings, sensitivity, controller)
│   ├── Accessibility (colorblind, text size, subtitles)
│   ├── Gameplay (HUD opacity, auto-aim, difficulty)
│   └── Network (name, server region, voice chat)
├── Credits
└── Quit

In-Game Pause Menu
├── Resume
├── Ship Status (components, loadout)
├── Galaxy Map
├── Inventory
├── Quest Log
├── Settings (same as main)
├── Save Game
└── Quit to Menu
```

---

## PART 6: CURRICULUM STEP UPDATES

**Step 801-810 (UI foundation)**: Quad batch renderer for UI. Texture atlas for sprites/fonts. SDF font rendering shader.

**Step 811-820 (HUD)**: Health bar, shield bar, speed indicator, crosshair. Positioned per HUD layout diagram. Adaptive opacity by game state.

**Step 821-830 (Menus)**: Main menu with button navigation. Settings panels with sliders/toggles/dropdowns. Key binding editor.

**Step 831-840 (In-game panels)**: Inventory grid. Trade UI (buy/sell with price display). Quest log with objective tracking.

**Step 841-850 (Accessibility)**: Colorblind modes via color remap shader. Text scaling system. Subtitle system with speaker labels. Input rebinding persistence.

**Step 851-860 (Radar/minimap)**: 2D radar on HUD showing nearby entities. Color-coded by faction. Range ring. North indicator.

**Step 1731-1830 (UI depth)**: Full widget system with focus management. Gamepad navigation (D-pad moves between widgets). Localization system (string tables, RTL support). Tooltip system. Notification queue. Modal dialogs.
