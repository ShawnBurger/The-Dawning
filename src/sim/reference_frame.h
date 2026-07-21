#pragma once
// =============================================================================
// sim/reference_frame.h - Galactic world position + hierarchical frame graph
// =============================================================================
// SIM STAGE 0 - the load-bearing coordinate architecture, in PURE MATH. No
// render, no physics, no GPU. This header includes ONLY core/types.h and
// <cstdint>/<vector>, exactly as core/shadow_cascades.h does, so it links into
// BOTH TheDawningV3 and TheDawningTests and the unit tests exercise the SHIPPED
// arithmetic rather than a paraphrase of it.
//
// It closes the interstellar precision hole that
// docs/research/RELATIVISTIC_SIM_ARCHITECTURE.md 1.6 / 11 left open, using the
// design that document named as the candidate: integer SECTOR coordinates plus a
// double intra-sector offset, canonicalized so a body never accumulates a huge
// double, and separations computed sector-aware so nearby bodies keep full
// double precision at any galactic distance.
//
// =============================================================================
// WHY A PLAIN Vec3d IS NOT ENOUGH  (the problem this type exists to solve)
// =============================================================================
// double ULP is value * 2^-52 ~= value * 2.22e-16. As an ABSOLUTE world
// coordinate:
//     1 AU  (1.5e11 m):  ULP ~= 3.3e-5 m   - fine
//     1e13 m           :  ULP ~= 2.2e-3 m   - 2 mm, still fine
//     1 ly  (9.5e15 m) :  ULP ~= 2.1 m      - a star's position is only ~2 m
//                                             resolvable
//     1000 ly          :  ULP ~= 2 km
// The killer is NOT the stored jitter - it is CATASTROPHIC CANCELLATION. Two
// ships 0.25 m apart, each stored as a 1-ly-scale absolute Vec3d, know their
// SEPARATION only to ~2 m, because subtracting two 9.5e15-magnitude doubles
// throws away everything below their shared 2 m ULP. That is a total loss of the
// 0.25 m offset - long before either stored position "looks wrong".
//
// =============================================================================
// THE FIX, AND THE SECTOR_SIZE ULP JUSTIFICATION
// =============================================================================
// Split every world coordinate into an integer SECTOR index (int64, exact) and a
// double OFFSET kept canonical in [0, kSectorSize). The offset is therefore
// ALWAYS bounded by one sector, so its worst-case ULP is fixed at
//     kSectorSize * 2^-52
// no matter how far the sector is from the galactic origin. A body 1 ly away is
// stored as (sector ~950, small offset), NOT as a 9.5e15 double - so its offset
// carries sub-micron precision and two nearby bodies keep the full 0.25 m.
//
// kSectorSize = 1e13 m is chosen so that BOTH ends of the trade are comfortable:
//   * Intra-sector precision. Worst-case offset ULP = 1e13 * 2^-52 = 2.2 mm at
//     the FAR EDGE of a sector; sub-micron near the sector origin. "Comfortably
//     double-precise within a sector."  (1e14 would give 2.2 cm; 9.5e15 - a
//     1-ly top-level frame - gives the 2.1 m hole this type removes.)
//   * A solar system fits in one sector. 1e13 m = ~66.8 AU across. Neptune's
//     orbit is 30 AU, the Kuiper belt ~50 AU - so an entire planetary system,
//     and therefore an entire self-contained gravitational problem, lives in ONE
//     sector and is integrated single-frame with no cross-sector cancellation.
//   * floor(offset / kSectorSize) is exact while |offset|/kSectorSize < 2^53, so
//     the sector carry (canonicalization) is exact for any body that drifts a
//     realistic number of sectors.
//
// kSectorSize = 1e13 is an exact integer in double (1e13 < 2^53), so sector
// carries of a few sectors are bit-exact.
//
// =============================================================================
// ADDRESSABLE RANGE (int64 must not overflow silently - CLAUDE.md constraint)
// =============================================================================
// A galaxy is ~1e21 m across => ~1e8 sectors/axis. int64 max is ~9.22e18, so the
// galaxy uses 1e-10 of the range. We cap usable sector coordinates at
// kMaxSectorCoord = 4e18 (=> 4e31 m/axis, ~4e10 galaxy-widths, ~5e4 observable
// universes) purely so the sector DIFFERENCE in Separation cannot overflow:
// |to.s - from.s| <= 2*kMaxSectorCoord = 8e18 < INT64_MAX. A static_assert below
// pins that headroom; ValidSector() lets callers check an individual coordinate.
// =============================================================================

#include "core/types.h"

#include <cstdint>
#include <vector>

namespace sim
{

using core::Vec3d;
using core::Vec3f;

// -----------------------------------------------------------------------------
// Scale constants
// -----------------------------------------------------------------------------

// Metres per sector along each axis. See the header banner for the full ULP
// justification. Exact in double (1e13 < 2^53).
inline constexpr double kSectorSize = 1.0e13;

// Largest sector index (magnitude, per axis) a WorldPos may legally hold. Chosen
// so a sector DELTA between two legal positions cannot overflow int64.
inline constexpr int64_t kMaxSectorCoord = 4'000'000'000'000'000'000LL; // 4e18

// The overflow headroom the cap buys: two legal coords differ by at most
// 2*kMaxSectorCoord, which must stay below INT64_MAX so Separation's subtraction
// is total. Break the cap and this fails at BUILD time.
static_assert(kMaxSectorCoord <= (INT64_MAX / 2),
              "kMaxSectorCoord too large: sector delta could overflow int64");

// -----------------------------------------------------------------------------
// WorldPos - a galactic world position precise at every scale
// -----------------------------------------------------------------------------
// Physical point = (sx, sy, sz) * kSectorSize + offset. The offset is kept
// CANONICAL - each component in [0, kSectorSize) - by carrying overflow into the
// integer sector, so a drifting body never accumulates a large double.
//
// The absolute double  sx*kSectorSize + offset.x  is DELIBERATELY never formed
// by the shipped code: forming it is exactly the catastrophic-cancellation
// mistake the type exists to prevent. Separation() works on the split form.
struct WorldPos
{
    int64_t sx = 0;
    int64_t sy = 0;
    int64_t sz = 0;
    Vec3d   offset{ 0.0, 0.0, 0.0 };  // canonical: each component in [0, kSectorSize)

    WorldPos() = default;
    WorldPos(int64_t sx_, int64_t sy_, int64_t sz_, const Vec3d& offset_)
        : sx(sx_), sy(sy_), sz(sz_), offset(offset_) {}

    // A position given purely as an offset from the galactic origin (sector 0).
    // Canonicalizes, so a large offset is carried into the sector integers.
    static WorldPos FromOffset(const Vec3d& offsetFromOrigin);

    // Bit-exact structural equality. Two canonical WorldPos are equal iff they
    // denote the same physical point.
    bool operator==(const WorldPos& o) const
    {
        return sx == o.sx && sy == o.sy && sz == o.sz && offset == o.offset;
    }
    bool operator!=(const WorldPos& o) const { return !(*this == o); }
};

// True iff every sector coordinate is within the addressable range, so this
// position may participate in Separation without risking int64 overflow.
bool ValidSector(const WorldPos& p);

// Return p with its offset carried into the sector integers so every offset
// component lands in [0, kSectorSize). The physical point is unchanged; the
// sector carry is EXACT for any realistic drift (see header). Idempotent.
WorldPos Canonicalize(const WorldPos& p);

// True iff p is already canonical (every offset component in [0, kSectorSize)
// and finite).
bool IsCanonical(const WorldPos& p);

// p displaced by a Vec3d, re-canonicalized. This is how a body MOVES: its offset
// grows, and when it crosses a sector boundary the excess is carried into the
// integer sector so the offset stays small and precise forever.
WorldPos Translate(const WorldPos& p, const Vec3d& displacement);

// SEPARATION: the vector (to - from), in metres, computed SECTOR-AWARE.
//
//     delta = (double)(to.sector - from.sector) * kSectorSize
//           + (to.offset - from.offset)
//
// For NEARBY bodies the sector delta is 0 (or a tiny integer), so the large term
// vanishes and the result is the exact difference of two small offsets - FULL
// double precision, regardless of how far BOTH bodies are from the galactic
// origin. This is the whole point: precision follows the SEPARATION magnitude,
// not the absolute coordinate.
//
// For genuinely DISTANT bodies (many sectors apart) the (double)delta*kSectorSize
// term is large and legitimately loses sub-metre precision - correct and fine,
// you never need millimetres between things a light-year apart. It DEGRADES
// GRACEFULLY: always finite, never NaN, never a jitter blow-up.
//
// The naive alternative - reconstruct each absolute double then subtract - loses
// the small offset to cancellation even for NEARBY bodies once they are far from
// the origin. That is the mistake this function is written to avoid; the unit
// tests watch the difference.
Vec3d Separation(const WorldPos& from, const WorldPos& to);

// RULE 1 (CLAUDE.md) NARROWING SITE - the ONE place a world position becomes
// float, and it SUBTRACTS FIRST. Returns the camera-relative position of `body`
// as a Vec3f fit for the GPU: the double separation (small once the camera is
// near) narrowed to float. Never narrow an absolute WorldPos - narrow the
// difference. Mirrors Transform::ToCameraRelativeMatrix's contract exactly.
Vec3f ToCameraRelative(const WorldPos& body, const WorldPos& camera);

// =============================================================================
// FrameGraph - hierarchical reference frames for the intra-system case
// =============================================================================
// Frames nest galaxy > sector > star-system > planet. Each frame stores its
// origin as a WorldPos (so a star 1 ly from its sector origin is stored PRECISELY
// as sector+offset, NOT as a 9.5e15 double - this is what carries the interstellar
// fix into the frame layer). A body's state is stored RELATIVE TO ITS FRAME in
// plain small Vec3d, so intra-frame coordinates stay < ~1e13 m and precise.
//
// THE KEY INSIGHT (architecture 1.3): every separation / relative velocity is
// computed BETWEEN TWO BODIES IN A COMMON FRAME, where both coordinates are
// small. Catastrophic cancellation ACROSS frames - not stored jitter - is the
// real killer, so the common-frame subtraction is where correctness lives. When
// two bodies share a frame, their local coordinates are subtracted DIRECTLY, so
// the frame origin (however distant) never enters the arithmetic and the result
// is exact at any galactic scale.
//
// STAGE 0 SCOPE: frames are TRANSLATION-ONLY. Orientation (a Quatd, architecture
// 1.7) is deliberately deferred - the open question in 11 recommends
// translation-only frames, and rotation is not part of the PRECISION problem this
// stage exists to solve (positions cancel; a basis rotation does not). Frame
// linear velocity IS modelled, so relative velocity is correct for moving frames.
// =============================================================================

using FrameId = uint32_t;
inline constexpr FrameId kInvalidFrame = 0xFFFFFFFFu;

struct Frame
{
    FrameId  parent = kInvalidFrame;   // kInvalidFrame for the root
    WorldPos origin;                   // this frame's origin in GLOBAL (root) coords
    Vec3d    velocity{ 0.0, 0.0, 0.0 };// frame's linear velocity in global coords
};

// A body's state, stored relative to its containing frame. localPos stays small
// (< ~1e13 m) because the frame origin absorbs the large offset.
struct Body
{
    FrameId frame = kInvalidFrame;
    Vec3d   localPos{ 0.0, 0.0, 0.0 };
    Vec3d   localVel{ 0.0, 0.0, 0.0 };
};

class FrameGraph
{
public:
    // Create a frame with the given parent (kInvalidFrame for the root) and
    // global-space origin/velocity. Returns kInvalidFrame for an invalid parent,
    // non-finite state, address overflow, or exhausted FrameId space.
    FrameId CreateFrame(FrameId parent, const WorldPos& originInGlobal,
                        const Vec3d& velocityInGlobal = Vec3d{});

    const Frame& GetFrame(FrameId id) const;
    uint32_t     FrameCount() const { return static_cast<uint32_t>(m_frames.size()); }

    // Save/load (SIM STAGE 6). The frame vector IS the graph: a frame's index is
    // its FrameId and a parent always has a strictly lower index (CreateFrame only
    // points at already-created frames). Frames() exposes it for serialization;
    // RebuildFromFrames restores it WITHOUT reordering, so FrameIds are preserved
    // bit-exact. The caller is responsible for having validated the vector (index
    // == id, parent < index, canonical origins) - sim/sim_serialize.cpp does this
    // on load before calling here.
    const std::vector<Frame>& Frames() const { return m_frames; }
    void RebuildFromFrames(std::vector<Frame> frames) { m_frames = std::move(frames); }

    // Compose a body's global WorldPos from its frame origin + small local offset.
    WorldPos ResolveWorldPos(const Body& body) const;

    // Compose a body's global velocity (translation-only: frame vel + local vel).
    Vec3d ResolveWorldVel(const Body& body) const;

    // Nearest common ancestor of two frames (the deepest frame on both parent
    // chains). Returns kInvalidFrame only if the frames share no ancestor.
    FrameId NearestCommonAncestor(FrameId a, FrameId b) const;

    // Express a body's position in frame `frameId`'s local coordinates as a
    // Vec3d. If the body is ALREADY in that frame, returns its local offset
    // DIRECTLY (exact - the frame origin never enters). Otherwise the position is
    // resolved and differenced against the frame origin, sector-aware.
    Vec3d ExpressInFrame(const Body& body, FrameId frameId) const;

    // SEPARATION between two bodies (b - a), computed in their nearest common
    // frame so both coordinates are small before they are subtracted. This is the
    // operation the architecture insists must happen within a common frame.
    Vec3d SeparationBetween(const Body& a, const Body& b) const;

    // RELATIVE VELOCITY of b with respect to a. Frame-invariant under
    // translation-only frames, so it reduces to worldVel(b) - worldVel(a);
    // computed via the common frame for symmetry with SeparationBetween.
    Vec3d RelativeVelocity(const Body& a, const Body& b) const;

    // REBASE: recenter a frame's origin (e.g. onto the active frame near the
    // player). The frame's stored origin is replaced by newOrigin. Returns the
    // correction that must be ADDED to localPos of every body whose .frame ==
    // frameId so its WORLD position is unchanged (use ApplyRebaseToBody). The
    // correction is Separation(newOrigin, oldOrigin) = oldOrigin - newOrigin.
    Vec3d RebaseFrame(FrameId frameId, const WorldPos& newOrigin);

    // Apply a RebaseFrame correction to a body in the rebased frame. Small helper
    // so the sign is defined in exactly one place.
    static void ApplyRebaseToBody(Body& body, const Vec3d& correction)
    {
        body.localPos += correction;
    }

    // REPARENT: move a body to a new parent frame, PRESERVING ITS WORLD POSITION
    // AND WORLD VELOCITY EXACTLY. The new local offset is the sector-aware
    // separation of the body's world position from the new frame origin.
    Body Reparent(const Body& body, FrameId newFrame) const;

private:
    std::vector<Frame> m_frames;

    bool ValidId(FrameId id) const { return id < m_frames.size(); }
};

} // namespace sim
