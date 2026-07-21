#pragma once
// =============================================================================
// sim/sim_serialize.h - versioned, checksummed binary codec for SIM state
// =============================================================================
// SIM STAGE 6, per docs/research/RELATIVISTIC_SIM_ARCHITECTURE.md section 8.
// CPU-only, GPU-free. Includes ONLY core/types.h + reference_frame.h, so it links
// into BOTH TheDawningV3 and TheDawningTests and the tests drive the SHIPPED codec.
//
// It serializes the SIM-owned persistent state - the FrameGraph, and per-body
// dynamical records (frame-bound pose, rigid-body dynamics, gravitational params
// + optional on-rails elements, relativistic momentum/clock) - into a tagged,
// length-prefixed, CRC-32-checksummed container, and restores it. The records are
// SIM-NATIVE plain structs (mirroring the ecs:: components field-for-field) so the
// codec has NO ECS coupling; the ecs<->record mapping (BuildSnapshot/ApplySnapshot)
// is the ship-slice lane's, exactly as every prior sim stage deferred its wiring.
//
// The load-bearing design rule (section 8.1): a Transform.position is meaningless
// without its FrameId, and RigidBody.linearVelocity without its frame - so each is
// stored WITH its frame. Determinism goal is SAME-BINARY (section 8.3): Serialize is
// a pure function of the physical state (field-by-field little-endian via bit_cast,
// no struct padding, no pointer blitting), so load(save(x)) is bit-exact and a
// hash of the bytes is a valid replay digest. Cross-machine float determinism is an
// explicit NON-GOAL (float summation is non-associative under floating-origin
// storage); the file is portable and lossless, the trajectory is not.

#include "core/types.h"
#include "reference_frame.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sim
{

using core::Quatf;
using core::Vec3f;

// --- Per-body records (SIM-NATIVE mirrors of the ecs:: components) -----------
// frame is structurally inseparable from position; velocityFrame from
// linearVelocity. angularVelocity / invMass / invInertiaDiag are REQUIRED so a
// spinning finite-mass body's load->step reproduces live->step.
struct BodyRecord
{
    uint64_t bodyId          = 0;
    FrameId  frame           = kInvalidFrame;   // Transform.position's frame
    Vec3d    position        {};                // frame-relative
    Quatf    rotation        = Quatf::Identity();
    Vec3f    scale           { 1.0f, 1.0f, 1.0f };
    FrameId  velocityFrame   = kInvalidFrame;   // RigidBody.linearVelocity's frame
    Vec3d    linearVelocity  {};
    Vec3f    angularVelocity {};
    double   invMass         = 0.0;
    Vec3f    invInertiaDiag  {};

    bool operator==(const BodyRecord&) const;
};

// GravitationalBody params + optional on-rails OrbitState (elements + primary).
struct GravRecord
{
    uint64_t bodyId   = 0;
    double   mu       = 0.0;
    double   radius   = 0.0;    // primary; NBodyParticle.softening is DERIVED, not stored
    uint8_t  isSource = 1;      // 0/1
    uint8_t  owner    = 0;      // ecs::OrbitOwner { NBodyActive=0, OnRails=1, ForceIntegrated=2 }
    uint8_t  hasRails = 0;      // 0/1; the rails fields below are valid iff 1
    // OrbitState (valid iff hasRails): osculating elements + primary + epoch.
    double   semiMajorAxis    = 0.0;
    double   eccentricity     = 0.0;
    double   inclination      = 0.0;
    double   longitudeAscNode = 0.0;
    double   argPeriapsis     = 0.0;
    double   trueAnomaly      = 0.0;
    double   primaryMu        = 0.0;
    uint64_t primaryBodyId    = 0;
    double   railsEpoch       = 0.0;

    bool operator==(const GravRecord&) const;
};

// Relativity sidecar: momentum (authoritative) + rest mass, and the two clock
// components RAW (never the reconstructed proper time tau).
struct ClockRecord
{
    uint64_t bodyId              = 0;
    Vec3d    momentum            {};
    double   restMass            = 1.0;   // > 0
    double   coordinateTime      = 0.0;
    double   properTimeDeviation = 0.0;   // <= 0

    bool operator==(const ClockRecord&) const;
};

// --- The full sim snapshot ---------------------------------------------------
struct SimSnapshot
{
    // Globals (section 8.1). All load-bearing for deterministic reproduction.
    double   coordinateTimeEpoch = 0.0;          // totalTime; Kepler(t) & the deficit read this
    double   fixedDt             = 0.0;          // step this save was taken at (validated > 0 on load)
    uint64_t simTick             = 0;            // integer fixed steps since t=0 (exact)
    FrameId  masterFrame         = kInvalidFrame;// designated master frame (section 2.3)

    std::vector<Frame>       frames;             // FrameGraph; INDEX ORDER == FrameId
    std::vector<BodyRecord>  bodies;
    std::vector<GravRecord>  gravity;
    std::vector<ClockRecord> clocks;

    bool operator==(const SimSnapshot&) const;   // field-wise; tests only
};

enum class SimLoadStatus : uint32_t
{
    Ok = 0,
    ShortBuffer,        // ran out of bytes (truncation) or below the minimum file size
    BadMagic,           // not a Dawning sim save
    UnsupportedVersion, // formatMajor this build cannot read
    BadChecksum,        // CRC-32 mismatch: bit rot / partial write
    BadLayout,          // header self-inconsistent (fileBytes/headerBytes/reserved)
    BadSectionLength,   // a section length ran past end, a record stream desynced, or a gap/overlap
    UnknownRequired,    // an unknown/unsupported section carried the REQUIRED flag
    MissingRequired,    // EPOK or FRMS absent
    DuplicateSection,   // a singleton section appeared twice
    TooLarge,           // buffer or a count exceeds the configured cap
    InvalidData,        // decoded but a value violates a domain invariant
};

const char* ToString(SimLoadStatus s);

struct SimLoadResult
{
    SimLoadStatus status = SimLoadStatus::BadLayout;
    SimSnapshot   snapshot;   // valid IFF Ok()
    std::string   error;      // human-readable locus, for logs
    bool Ok() const { return status == SimLoadStatus::Ok; }
};

// Canonical form for BIT-IDENTICAL saves: sorts bodies/gravity/clocks by bodyId
// (frames keep index order - their order IS identity) and canonicalizes every
// WorldPos origin. Idempotent.
void CanonicalizeSnapshot(SimSnapshot& snap);

// PURE. Internally canonicalizes a copy, so the output bytes are a function of the
// PHYSICAL STATE, not vector insertion order. Never fails for a well-formed snapshot.
std::vector<uint8_t> Serialize(const SimSnapshot& snap);

// TOTAL. Never throws, never crashes, never reads out of bounds, never returns
// garbage. On any malformed input: status != Ok and an empty snapshot. Ok() gates.
SimLoadResult Deserialize(const uint8_t* data, size_t size);
SimLoadResult Deserialize(std::span<const uint8_t> bytes);

// CRC-32 (IEEE reflected 0xEDB88320) over `size` bytes of `data`, treating the 4
// bytes at `crcFieldOffset` as zero. A null non-empty input returns 0. Exposed
// for checksum tests and safe for callers at trust boundaries.
uint32_t ComputeSimCrc32(const uint8_t* data, size_t size, size_t crcFieldOffset);

} // namespace sim
