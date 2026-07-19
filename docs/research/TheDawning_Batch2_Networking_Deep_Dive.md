# THE DAWNING — Networking & Multiplayer Deep Dive
# Batch 2, Topic 1: Client-Server Architecture, Prediction, Reconciliation,
#                    Entity Interpolation, Lag Compensation, Bandwidth
# Sources: Gabriel Gambetta series, Valve Source Networking, Unity Netcode,
#          Overwatch GDC talk, Wikipedia Netcode, CrystalOrb

---

## PART 1: ARCHITECTURE

### Authoritative Server Model
The server is the single source of truth. Clients send inputs, server simulates,
clients render the result. Prevents cheating at the fundamental level.

```
CLIENT → INPUT (keys, mouse, actions) → SERVER
SERVER → processes inputs → updates world state → SNAPSHOT → CLIENT
CLIENT → interpolates/predicts → RENDER

Round-trip time (RTT): typically 20-200ms
  LAN: 1-5ms
  Same region: 20-50ms
  Cross-continent: 100-200ms
  Intercontinental: 150-300ms
```

### Tick Rates by Game Genre

| Genre | Server Tick | Client Send Rate | Notes |
|---|---|---|---|
| **Competitive FPS** | 128 Hz | 128 Hz | Valorant, CS2 |
| **Standard FPS** | 60-64 Hz | 60 Hz | Overwatch, Halo |
| **Battle Royale** | 20-30 Hz | 30 Hz | Fortnite (server-side budget for 100 players) |
| **Space Sim (combat)** | 30 Hz | 30 Hz | Our target during combat |
| **Space Sim (cruise)** | 10 Hz | 10 Hz | Reduced during non-combat |
| **MMO** | 10-20 Hz | 10 Hz | EVE Online, WoW |
| **Turn-based** | On event | On event | No fixed tick needed |

### The Dawning's Hybrid Approach
```
Zone-based tick rate:
- Combat zone (within 5km of hostiles): 30 Hz server, 30 Hz client
- Station/docking: 20 Hz (lower precision acceptable)
- Cruise/travel: 10 Hz (interpolation handles smooth visuals)
- Warp/FTL: 1 Hz (position updates only, no physics)

Interest management:
- Each client only receives updates for entities within relevance radius
- Combat: 10km radius, all entities
- Station: 500m radius (station interior + nearby ships)
- Space: 50km radius, ships only (no debris/particles networked)
```

---

## PART 2: CLIENT-SIDE PREDICTION

The client simulates player actions immediately without waiting for server confirmation.

```cpp
struct InputCommand {
    uint32_t sequenceNumber;   // Monotonically increasing
    uint16_t inputFlags;       // Bitfield: forward, back, left, right, fire, etc.
    float mouseDeltaX;
    float mouseDeltaY;
    float throttle;
    double timestamp;          // Client time when input was generated
};

// Client stores a circular buffer of recent inputs
static const int INPUT_BUFFER_SIZE = 128;  // ~2 seconds at 60Hz
InputCommand inputBuffer[INPUT_BUFFER_SIZE];
int inputBufferHead = 0;

// Prediction: apply input locally immediately
void ClientPredict(GameState& localState, const InputCommand& cmd) {
    // Run the exact same simulation code the server runs
    SimulatePlayerMovement(localState, cmd);
    // Store predicted state alongside the input
    predictedStates[cmd.sequenceNumber % INPUT_BUFFER_SIZE] = localState;
}
```

### Server Reconciliation

```cpp
void ClientReconcile(GameState& localState, const ServerSnapshot& snapshot) {
    // Server tells us: "At sequence N, your state was X"
    uint32_t lastProcessedSeq = snapshot.lastProcessedInputSequence;
    
    // Set state to server's authoritative state
    localState = snapshot.playerState;
    
    // Re-apply all inputs the server hasn't processed yet
    for (uint32_t seq = lastProcessedSeq + 1; seq <= currentSequence; seq++) {
        int idx = seq % INPUT_BUFFER_SIZE;
        SimulatePlayerMovement(localState, inputBuffer[idx]);
    }
    
    // If predicted state matches reconciled state → no correction needed
    // If they differ → smooth correction over 100ms to avoid visual snap
}

// Smoothing: don't snap to corrected position, blend over time
Vec3d smoothedPosition;
float correctionBlendTime = 0.1f;  // 100ms blend
void SmoothCorrection(Vec3d& displayPos, const Vec3d& correctedPos, float dt) {
    float t = std::min(1.0f, dt / correctionBlendTime);
    displayPos = Lerp(displayPos, correctedPos, t);
}
```

---

## PART 3: ENTITY INTERPOLATION

Other players' positions arrive in snapshots from the server. We interpolate
between snapshots to produce smooth movement, even with low tick rates.

```cpp
// Interpolation buffer: store last N snapshots
struct SnapshotEntry {
    double serverTime;
    Vec3d position;
    Quatd orientation;
    Vec3f velocity;
};

static const int SNAPSHOT_BUFFER_SIZE = 32;
static const double INTERPOLATION_DELAY = 0.1;  // 100ms behind "live"
// This delay means we always have ≥2 snapshots to interpolate between
// (assuming 30Hz tick = 33ms per snapshot, 100ms = ~3 snapshots buffered)

Vec3d InterpolateEntity(const SnapshotEntry* buffer, int count, double renderTime) {
    double targetTime = renderTime - INTERPOLATION_DELAY;
    
    // Find the two snapshots bracketing targetTime
    for (int i = 0; i < count - 1; i++) {
        if (buffer[i].serverTime <= targetTime && buffer[i+1].serverTime >= targetTime) {
            float t = (targetTime - buffer[i].serverTime) 
                    / (buffer[i+1].serverTime - buffer[i].serverTime);
            return Lerp(buffer[i].position, buffer[i+1].position, t);
        }
    }
    
    // If no bracketing snapshots (packet loss), extrapolate from last known
    return buffer[count-1].position + buffer[count-1].velocity * 
           (targetTime - buffer[count-1].serverTime);
}
```

---

## PART 4: BANDWIDTH OPTIMIZATION

### Delta Compression
Only send what changed since last acknowledged snapshot.

```
Full snapshot: ~2KB per entity (position, orientation, velocity, health, state)
Delta snapshot: ~50-200 bytes per entity (only changed fields)
Savings: 90-97% bandwidth reduction

Implementation:
- Server tracks last acknowledged snapshot per client
- XOR current state with last acked state
- Only transmit non-zero fields
- Client reconstructs: lastAcked XOR delta = current
```

### Quantization
Reduce precision of transmitted values to save bytes.

```
Position: 3 × float32 (12 bytes) → 3 × int16 relative to sector (6 bytes)
  Range: ±32767 units at 0.01m resolution = ±327m from sector center
  For larger distances: use sector ID + local offset

Orientation: 4 × float32 (16 bytes) → smallest-3 quaternion (6 bytes)
  Drop largest component, encode 3 remaining as 16-bit normalized
  Reconstruct 4th: w = sqrt(1 - x² - y² - z²)

Velocity: 3 × float32 (12 bytes) → 3 × int16 (6 bytes)
  Range: ±327 m/s at 0.01 m/s resolution (sufficient for most ships)

Health: float32 (4 bytes) → uint8 (1 byte) as percentage 0-255

Total per entity: 12+16+12+4 = 44 bytes → 6+6+6+1 = 19 bytes (57% savings)
```

### Priority System

```cpp
float EntityPriority(const Entity& entity, const Vec3d& clientPos, float clientFov) {
    float distance = (entity.position - clientPos).Length();
    float basePriority = 1.0f;
    
    // Closer = higher priority
    basePriority *= 1000.0f / (distance + 1.0f);
    
    // In combat = higher priority
    if (entity.inCombat) basePriority *= 5.0f;
    
    // Player's target = highest priority
    if (entity.isTargetedByClient) basePriority *= 10.0f;
    
    // Recently changed state = higher priority
    if (entity.stateChangedThisTick) basePriority *= 3.0f;
    
    return basePriority;
}
// High priority entities: update every tick (33ms)
// Medium priority: every 3 ticks (100ms)
// Low priority: every 10 ticks (333ms)
```

### Bandwidth Targets

| Scenario | Upload (client) | Download (client) | Server total |
|---|---|---|---|
| **Solo in space** | 1 KB/s | 2 KB/s | N/A |
| **Station (20 NPCs)** | 2 KB/s | 8 KB/s | 160 KB/s |
| **Combat (8 ships)** | 5 KB/s | 15 KB/s | 120 KB/s |
| **Fleet battle (32)** | 5 KB/s | 40 KB/s | 1.3 MB/s |
| **Max capacity (64)** | 5 KB/s | 60 KB/s | 3.8 MB/s |

---

## PART 5: PACKET STRUCTURE

```cpp
// Packet header (8 bytes)
struct PacketHeader {
    uint16_t protocolId;       // Magic number to identify our protocol
    uint16_t sequenceNumber;   // For ordering and ack
    uint16_t ackBits;          // Bitfield of last 16 received packets
    uint16_t ackSequence;      // Last sequence number we received
};

// Client → Server packet (~64-128 bytes typical)
struct ClientPacket {
    PacketHeader header;
    uint8_t inputCount;        // How many input commands in this packet
    InputCommand inputs[3];    // Redundantly send last 3 inputs (loss protection)
};

// Server → Client packet (~200-2000 bytes typical)
struct ServerPacket {
    PacketHeader header;
    uint32_t serverTick;
    uint32_t lastProcessedInput;  // For reconciliation
    uint16_t entityCount;
    // Followed by: delta-compressed entity states
};

// Transport: UDP with custom reliability layer
// TCP is too slow for real-time game state (head-of-line blocking)
// We implement: sequencing, acking, redundancy on UDP
```

---

## PART 6: ANTI-CHEAT FUNDAMENTALS

```
Server-authoritative validation:
- Server NEVER trusts client-reported position (only inputs)
- Server validates: movement speed ≤ max speed × dt × 1.1 (10% tolerance)
- Server validates: fire rate ≤ weapon fire rate × 1.05
- Server validates: health/ammo/resources only change through server logic
- Server validates: line of sight for hits (server-side hit detection)

Speed hack detection:
- If client claims to move faster than physically possible → reject + flag
- If client sends inputs faster than tick rate → rate limit

Teleport detection:
- If position delta > max_speed * dt * 2.0 → reject, snap to last valid

Aimbot mitigation:
- Server-side hit validation: ray from shooter must intersect target hitbox
- Lag compensation: server rewinds target positions to shooter's perceived time
  (within 200ms window maximum)
```

---

## PART 7: CURRICULUM STEP UPDATES

**Step 731-740 (Network foundation)**: UDP socket wrapper with send/receive queues.
PacketHeader with sequence/ack. Connection handshake (SYN-ACK-style over UDP).

**Step 741-750 (Client-server loop)**: Server tick at 30Hz. Client sends InputCommand packets.
Server processes inputs, advances simulation, sends snapshots.

**Step 751-760 (Prediction)**: Client-side prediction with input buffer.
Reconciliation on server snapshot receipt. Smooth correction blending.

**Step 761-770 (Interpolation)**: Entity interpolation buffer (100ms delay).
Lerp between bracketing snapshots. Extrapolation fallback on packet loss.

**Step 771-780 (Delta compression)**: XOR-based delta encoding. Quantized values.
Priority system for bandwidth allocation. Interest management by distance.

**Step 781-790 (Lag compensation)**: Server-side hit detection with position history buffer.
Rewind target hitboxes to shooter's perceived time. 200ms maximum rewind window.

**Step 791-800 (Voice chat)**: Opus codec at 24kbps. Proximity-based voice (fade with distance).
Push-to-talk and open mic modes. Server mixes and routes voice to relevant clients.

**Step 1631-1730 (Networking depth)**: Encrypted transport (AES-128 for game data, TLS for login).
Replay system (record all inputs + snapshots, deterministic playback).
Server browser / matchmaking. NAT punchthrough for P2P fallback.
Anti-cheat hardening. Load balancing across server instances.
