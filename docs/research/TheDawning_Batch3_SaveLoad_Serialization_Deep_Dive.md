# THE DAWNING — Save/Load & Serialization Deep Dive
# Batch 3, Topic 4: Binary Formats, Versioning, World State,
#                    Chunk Streaming, Compression, Integrity

---

## PART 1: SAVE FILE ARCHITECTURE

### File Format

```
TheDawning Save File (.tdsave)

Header (64 bytes):
  [0-3]   Magic: "TDSV" (0x54445356)
  [4-7]   Version: uint32 (format version, increment on breaking changes)
  [8-11]  Flags: uint32 (compressed, encrypted, quicksave, autosave)
  [12-15] Checksum: uint32 (CRC32 of everything after header)
  [16-23] Timestamp: int64 (Unix timestamp of save)
  [24-27] PlayTime: float (total play time in hours)
  [28-31] ChunkCount: uint32 (number of data chunks)
  [32-63] Reserved (future use, zero-filled)

Chunks (variable size, repeated ChunkCount times):
  [0-3]   ChunkType: uint32 (enum: Player, Ships, World, Quests, etc.)
  [4-7]   ChunkSize: uint32 (byte size of this chunk's data)
  [8-11]  ChunkVersion: uint32 (version of this chunk's format)
  [12..]  ChunkData: byte[ChunkSize] (the actual data)

Chunk types:
  0x0001: PLAYER_STATE    (position, health, inventory, stats)
  0x0002: SHIP_STATE      (player ship components, cargo, fuel)
  0x0003: GALAXY_STATE    (discovered systems, faction standings)
  0x0004: QUEST_STATE     (active/completed quests, quest variables)
  0x0005: ECONOMY_STATE   (station inventories, prices, NPC traders)
  0x0006: NPC_STATE       (NPC positions, memories, opinions)
  0x0007: WORLD_EVENTS    (active events, timers, triggers)
  0x0008: SETTINGS        (difficulty, preferences — NOT keybinds)
  0x0009: SCREENSHOT      (128×72 thumbnail for save slot display)
```

### Version Migration

```cpp
// Every chunk has its own version number
// When loading, check version and migrate if needed

void LoadPlayerChunk(BinaryReader& reader, PlayerState& state, uint32_t chunkVersion) {
    // Always read in order fields were added
    state.position = reader.ReadVec3d();
    state.health = reader.ReadFloat();
    state.maxHealth = reader.ReadFloat();
    state.credits = reader.ReadFloat();
    
    if (chunkVersion >= 2) {
        // Added in version 2: reputation system
        state.reputation = reader.ReadFloat();
    } else {
        state.reputation = 0.0f;  // Default for old saves
    }
    
    if (chunkVersion >= 3) {
        // Added in version 3: skill system
        state.skillPoints = reader.ReadInt32();
        int skillCount = reader.ReadInt32();
        for (int i = 0; i < skillCount; i++) {
            state.skills.push_back(reader.ReadSkill());
        }
    }
    // Old saves with version < 3 just get empty skill list
}

// RULE: Never remove or reorder fields. Only append.
// RULE: Always provide defaults for missing fields.
// RULE: Increment chunk version when adding fields.
// RULE: Keep migration code for ALL previous versions (players have old saves).
```

---

## PART 2: SERIALIZATION PATTERNS

### Binary Writer/Reader

```cpp
class BinaryWriter {
    std::vector<uint8_t> buffer;
    
public:
    void WriteUint8(uint8_t v) { buffer.push_back(v); }
    void WriteUint16(uint16_t v) { Write(&v, 2); }
    void WriteUint32(uint32_t v) { Write(&v, 4); }
    void WriteInt32(int32_t v) { Write(&v, 4); }
    void WriteFloat(float v) { Write(&v, 4); }
    void WriteDouble(double v) { Write(&v, 8); }
    void WriteVec3f(const Vec3f& v) { WriteFloat(v.x); WriteFloat(v.y); WriteFloat(v.z); }
    void WriteVec3d(const Vec3d& v) { WriteDouble(v.x); WriteDouble(v.y); WriteDouble(v.z); }
    
    void WriteString(const std::string& s) {
        WriteUint16((uint16_t)s.size());  // Length prefix
        Write(s.data(), s.size());
    }
    
    template<typename T>
    void WriteVector(const std::vector<T>& vec, void(*writeFunc)(BinaryWriter&, const T&)) {
        WriteUint32((uint32_t)vec.size());
        for (const auto& item : vec) writeFunc(*this, item);
    }
    
private:
    void Write(const void* data, size_t size) {
        auto ptr = reinterpret_cast<const uint8_t*>(data);
        buffer.insert(buffer.end(), ptr, ptr + size);
    }
};

// Endianness: always little-endian (x86-64 native)
// For cross-platform: add byte-swap on big-endian architectures
```

### Compression

```
Use LZ4 for save file compression:
  - Extremely fast decompression (>4 GB/s)
  - Good compression ratio for game data (~2-4× reduction)
  - Decompression is faster than disk read (net speedup!)
  
Compression strategy:
  - Compress each chunk independently (allows partial reading)
  - Header is NEVER compressed (need to read it to find chunks)
  - Screenshot chunk uses JPEG compression instead of LZ4
  
Typical save file sizes:
  Raw: 2-10 MB (depends on world complexity)
  Compressed: 500KB - 3MB
  Target load time: <1 second (LZ4 decompression + deserialization)
```

---

## PART 3: WHAT TO SAVE (AND WHAT NOT TO)

### Save (Player-Specific State)
```
✅ Player position, orientation, velocity
✅ Player health, shields, fuel, ammo
✅ Player inventory and cargo
✅ Player credits and stats
✅ Ship component health and upgrade state
✅ Quest progress (active quests, completed quests, variables)
✅ Discovered systems and planets
✅ Faction standings / reputation
✅ NPC opinions of player (from emergent AI)
✅ Active gossip about player
✅ Game time (in-game clock)
✅ Difficulty settings
```

### Don't Save (Regenerate from Seed)
```
❌ Planet terrain (regenerate from seed)
❌ Star positions (generated from galaxy seed)
❌ NPC patrol routes (regenerate from station + faction)
❌ Station layouts (generated from seed)
❌ Creature species definitions (generated from biome seed)
❌ Commodity base prices (from designer table)
❌ Weapon/item definitions (from data files, not save)
❌ Texture/mesh data (assets, not state)
❌ Audio state (restart music from game state)
```

### Save Efficiently (Delta from Default)
```
⚡ Station inventories: save only stations the player has visited
   (unvisited stations regenerate from seed + elapsed time)
⚡ NPC states: save only NPCs the player has interacted with
   (others regenerate from initial conditions + time)
⚡ Economy: save only commodities with non-default prices
   (default prices computed from supply/demand formula)
⚡ This typically reduces save size by 80% vs saving everything
```

---

## PART 4: AUTOSAVE AND QUICKSAVE

```
Autosave triggers:
  - Every 5 minutes of real time
  - On entering a new star system
  - On docking at a station
  - Before major story events
  - On quitting to menu (always autosave on exit)

Autosave slots: 3 rotating (oldest overwritten)
Quicksave: 1 slot, bound to F5 by default
Manual save: 10 slots, player-named

Save flow:
  1. Pause game logic (freeze world for consistency)
  2. Serialize all chunks to memory buffer
  3. Compress each chunk with LZ4
  4. Write to temp file (.tdsave.tmp)
  5. Rename .tmp to .tdsave (atomic on most filesystems)
  6. Resume game logic
  
  Total time budget: <200ms (should be imperceptible)
  If >200ms: serialize in background thread, but must snapshot
  world state atomically before background write begins.

Load flow:
  1. Read and verify header (magic, version, checksum)
  2. Decompress each chunk
  3. Deserialize in chunk order
  4. Regenerate non-saved data from seeds + game time
  5. Resume game
  
  Target load time: <2 seconds from menu to gameplay
```

---

## PART 5: DATA INTEGRITY

```cpp
// CRC32 checksum to detect corruption
uint32_t ComputeCRC32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

// On load: recompute CRC and compare with stored value
// If mismatch: "Save file appears corrupted. Load anyway? (May cause issues)"
// Never silently load corrupted data.

// Backup: keep previous save when overwriting
// Before writing slot3.tdsave: rename to slot3.tdsave.bak
// If write fails: restore from .bak
// If both corrupt: inform player, don't crash
```

---

## PART 6: CURRICULUM STEP UPDATES

**Step 671-680 (Save foundation)**: BinaryWriter/BinaryReader. Header with magic/version/CRC. Single chunk: player position + health.

**Step 681-690 (Full save)**: All chunk types. Ship state, quest state, economy state. LZ4 compression per chunk.

**Step 691-700 (Version migration)**: Chunk versioning. Migration code for adding new fields. Backwards compatibility test.

**Step 701-710 (Autosave)**: Autosave timer and triggers. Rotating 3-slot system. Quicksave on F5. Background serialization with world snapshot.

**Step 711-720 (Save UI)**: Save slot list with timestamps, play time, thumbnail screenshots. Load confirmation dialog. "Corrupted save" warning.

**Step 721-730 (Integrity)**: CRC32 validation. Backup file creation before overwrite. Graceful error handling on corrupt data.
