// =============================================================================
// sim/reference_frame.cpp - Galactic world position + hierarchical frame graph
// =============================================================================

#include "reference_frame.h"

#include <cmath>
#include <cstdlib>

namespace sim
{

namespace
{

// Carry a single axis' offset into its sector so the offset lands in
// [0, kSectorSize). floor(off/kSectorSize) is exact while |off|/kSectorSize <
// 2^53, so the carry is exact for any realistic drift. The final nudge repairs
// the rare case where floor()*kSectorSize rounds the offset a hair outside the
// half-open interval - without it a value sitting exactly on kSectorSize could
// survive as a non-canonical offset.
void CanonicalizeAxis(int64_t& sector, double& off)
{
    if (!std::isfinite(off))
        return;  // leave non-finite input untouched; IsCanonical() reports it

    const double q = std::floor(off / kSectorSize);
    const int64_t carry = static_cast<int64_t>(q);
    sector += carry;
    off -= static_cast<double>(carry) * kSectorSize;

    if (off < 0.0)              { off += kSectorSize; sector -= 1; }
    else if (off >= kSectorSize){ off -= kSectorSize; sector += 1; }
}

bool AxisCanonical(double off)
{
    return std::isfinite(off) && off >= 0.0 && off < kSectorSize;
}

} // namespace

// -----------------------------------------------------------------------------
// WorldPos
// -----------------------------------------------------------------------------

WorldPos WorldPos::FromOffset(const Vec3d& offsetFromOrigin)
{
    return Canonicalize(WorldPos{ 0, 0, 0, offsetFromOrigin });
}

bool ValidSector(const WorldPos& p)
{
    return std::llabs(p.sx) <= kMaxSectorCoord
        && std::llabs(p.sy) <= kMaxSectorCoord
        && std::llabs(p.sz) <= kMaxSectorCoord;
}

WorldPos Canonicalize(const WorldPos& p)
{
    WorldPos r = p;
    CanonicalizeAxis(r.sx, r.offset.x);
    CanonicalizeAxis(r.sy, r.offset.y);
    CanonicalizeAxis(r.sz, r.offset.z);
    return r;
}

bool IsCanonical(const WorldPos& p)
{
    return AxisCanonical(p.offset.x)
        && AxisCanonical(p.offset.y)
        && AxisCanonical(p.offset.z);
}

WorldPos Translate(const WorldPos& p, const Vec3d& displacement)
{
    WorldPos r = p;
    r.offset += displacement;
    return Canonicalize(r);
}

Vec3d Separation(const WorldPos& from, const WorldPos& to)
{
    // Sector delta stays in int64 - EXACT and overflow-safe while both operands
    // are within kMaxSectorCoord (static_assert in the header guarantees the
    // difference fits). For nearby bodies these deltas are zero, so the large
    // term below is zero and only the small, precise offset difference survives.
    const int64_t dsx = to.sx - from.sx;
    const int64_t dsy = to.sy - from.sy;
    const int64_t dsz = to.sz - from.sz;

    // delta = dsector*kSectorSize + (to.offset - from.offset).
    // The offset difference is computed FIRST and independently, so when the
    // sector delta is zero it is the entire result and carries full precision.
    return Vec3d{
        static_cast<double>(dsx) * kSectorSize + (to.offset.x - from.offset.x),
        static_cast<double>(dsy) * kSectorSize + (to.offset.y - from.offset.y),
        static_cast<double>(dsz) * kSectorSize + (to.offset.z - from.offset.z),
    };
}

Vec3f ToCameraRelative(const WorldPos& body, const WorldPos& camera)
{
    // Subtract in double (sector-aware), THEN narrow. The narrowing happens only
    // on the small residual, never on an absolute position - RULE 1.
    return Separation(camera, body).ToFloat();
}

// -----------------------------------------------------------------------------
// FrameGraph
// -----------------------------------------------------------------------------

FrameId FrameGraph::CreateFrame(FrameId parent, const WorldPos& originInGlobal,
                                const Vec3d& velocityInGlobal)
{
    Frame f;
    f.parent   = parent;
    f.origin   = Canonicalize(originInGlobal);
    f.velocity = velocityInGlobal;
    m_frames.push_back(f);
    return static_cast<FrameId>(m_frames.size() - 1);
}

const Frame& FrameGraph::GetFrame(FrameId id) const
{
    // Precondition: id is valid. Callers hold ids returned by CreateFrame.
    return m_frames[id];
}

WorldPos FrameGraph::ResolveWorldPos(const Body& body) const
{
    // Global position = frame origin (a precise WorldPos) + small local offset.
    // Because the origin is a WorldPos, a body in a 1-ly-distant frame resolves
    // to (distant sector, small offset) - the interstellar fix carried through.
    return Translate(GetFrame(body.frame).origin, body.localPos);
}

Vec3d FrameGraph::ResolveWorldVel(const Body& body) const
{
    // Translation-only frames: world velocity is the frame's velocity plus the
    // body's local velocity. No cancellation risk - velocities are small.
    return GetFrame(body.frame).velocity + body.localVel;
}

FrameId FrameGraph::NearestCommonAncestor(FrameId a, FrameId b) const
{
    // Walk a's chain, mark depths; then walk b's chain until it meets a marked
    // frame. Small graphs, so an O(depth^2) meet is fine and allocation-free.
    for (FrameId x = a; x != kInvalidFrame; x = ValidId(x) ? m_frames[x].parent : kInvalidFrame)
    {
        for (FrameId y = b; y != kInvalidFrame; y = ValidId(y) ? m_frames[y].parent : kInvalidFrame)
        {
            if (x == y)
                return x;
        }
    }
    return kInvalidFrame;
}

Vec3d FrameGraph::ExpressInFrame(const Body& body, FrameId frameId) const
{
    // MOST PRECISE PATH: a body already in the target frame returns its local
    // offset directly, so the (possibly 1-ly-distant) frame origin never enters
    // the arithmetic. This is what makes a same-frame separation exact at any
    // galactic scale.
    if (body.frame == frameId)
        return body.localPos;

    // Otherwise resolve to a global WorldPos and difference against the frame
    // origin, sector-aware. Precise when the frame is near the body (the usual
    // case for a nearest common ancestor).
    return Separation(GetFrame(frameId).origin, ResolveWorldPos(body));
}

Vec3d FrameGraph::SeparationBetween(const Body& a, const Body& b) const
{
    const FrameId common = NearestCommonAncestor(a.frame, b.frame);
    return ExpressInFrame(b, common) - ExpressInFrame(a, common);
}

Vec3d FrameGraph::RelativeVelocity(const Body& a, const Body& b) const
{
    // Under translation-only frames the common-frame velocity of a body is its
    // world velocity minus the common frame's world velocity; the common-frame
    // term cancels in the difference, leaving worldVel(b) - worldVel(a). Written
    // explicitly so the frame-invariance is visible rather than assumed.
    const FrameId common = NearestCommonAncestor(a.frame, b.frame);
    const Vec3d commonVel = (common != kInvalidFrame) ? GetFrame(common).velocity : Vec3d{};
    const Vec3d va = ResolveWorldVel(a) - commonVel;
    const Vec3d vb = ResolveWorldVel(b) - commonVel;
    return vb - va;
}

Vec3d FrameGraph::RebaseFrame(FrameId frameId, const WorldPos& newOrigin)
{
    const WorldPos oldOrigin = GetFrame(frameId).origin;
    const WorldPos canonNew  = Canonicalize(newOrigin);

    // Correction for in-frame bodies: body.world = old + localOld = new + localNew
    // => localNew = localOld + (old - new) = localOld + Separation(new, old).
    const Vec3d correction = Separation(canonNew, oldOrigin);

    m_frames[frameId].origin = canonNew;
    return correction;
}

Body FrameGraph::Reparent(const Body& body, FrameId newFrame) const
{
    const WorldPos world = ResolveWorldPos(body);

    Body r = body;
    r.frame = newFrame;
    // New local offset = world - newFrameOrigin (sector-aware). SUBTRACT, so the
    // world position is preserved exactly for representable cases.
    r.localPos = Separation(GetFrame(newFrame).origin, world);
    // Preserve world velocity: localVel = worldVel - newFrame velocity.
    r.localVel = ResolveWorldVel(body) - GetFrame(newFrame).velocity;
    return r;
}

} // namespace sim
