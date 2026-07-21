#include "ftl.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sim
{

namespace
{

bool IsFinite(const Vec3d& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool IsAddressable(const WorldPos& p)
{
    // Avoid llabs(INT64_MIN): compare directly before calling coordinate helpers.
    return p.sx >= -kMaxSectorCoord && p.sx <= kMaxSectorCoord &&
           p.sy >= -kMaxSectorCoord && p.sy <= kMaxSectorCoord &&
           p.sz >= -kMaxSectorCoord && p.sz <= kMaxSectorCoord &&
           IsCanonical(p);
}

bool TryNormalize(const Quatf& q, Quatf& normalized)
{
    const double x = static_cast<double>(q.x);
    const double y = static_cast<double>(q.y);
    const double z = static_cast<double>(q.z);
    const double w = static_cast<double>(q.w);
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(z) || !std::isfinite(w))
        return false;

    const double scale = (std::max)({ std::fabs(x), std::fabs(y),
                                      std::fabs(z), std::fabs(w) });
    if (!(scale > 0.0))
        return false;

    const double sx = x / scale;
    const double sy = y / scale;
    const double sz = z / scale;
    const double sw = w / scale;
    const double scaledLength = std::sqrt(sx * sx + sy * sy + sz * sz + sw * sw);
    if (!(scaledLength > 0.0) || !std::isfinite(scaledLength))
        return false;

    const double lengthSq = scale * scale * scaledLength * scaledLength;
    constexpr double kUnitRoundoff = 8.0 * std::numeric_limits<float>::epsilon();
    if (std::isfinite(lengthSq) && std::fabs(lengthSq - 1.0) <= kUnitRoundoff)
    {
        normalized = q; // preserve ordinary unit-quaternion bits
        return true;
    }

    normalized = Quatf{ static_cast<float>(sx / scaledLength),
                        static_cast<float>(sy / scaledLength),
                        static_cast<float>(sz / scaledLength),
                        static_cast<float>(sw / scaledLength) };
    return std::isfinite(normalized.x) && std::isfinite(normalized.y) &&
           std::isfinite(normalized.z) && std::isfinite(normalized.w);
}

bool TryRotateNormalized(const Quatf& q, const Vec3d& v, Vec3d& rotated)
{
    if (!IsFinite(v))
        return false;

    const double scale = (std::max)({ std::fabs(v.x), std::fabs(v.y),
                                      std::fabs(v.z) });
    Vec3d operand = v;
    // The ordinary path stays bit-compatible. Scaling only protects extreme
    // finite vectors whose intermediate cross products could overflow.
    constexpr double kDirectLimit = 1.0e150;
    if (scale > kDirectLimit)
        operand = v / scale;

    const Vec3d qv{ static_cast<double>(q.x),
                    static_cast<double>(q.y),
                    static_cast<double>(q.z) };
    const double w = static_cast<double>(q.w);
    const Vec3d uv  = qv.Cross(operand);
    const Vec3d uuv = qv.Cross(uv);
    rotated = operand + (uv * w + uuv) * 2.0;
    if (scale > kDirectLimit)
        rotated *= scale;
    return IsFinite(rotated);
}

bool SectorCanAdd(int64_t sector, int64_t carry)
{
    if (carry > 0)
        return sector <= kMaxSectorCoord - carry;
    if (carry < 0)
        return sector >= -kMaxSectorCoord - carry;
    return true;
}

bool AxisCanTranslate(int64_t sector, double offset, double displacement)
{
    const double raw = offset + displacement;
    if (!std::isfinite(raw))
        return false;
    const double carryValue = std::floor(raw / kSectorSize);
    if (carryValue < -2.0 || carryValue > 2.0)
        return false;
    return SectorCanAdd(sector, static_cast<int64_t>(carryValue));
}

bool TryTranslateBounded(const WorldPos& origin, const Vec3d& displacement,
                         WorldPos& translated)
{
    if (!IsAddressable(origin) || !IsFinite(displacement) ||
        std::fabs(displacement.x) > kSectorSize ||
        std::fabs(displacement.y) > kSectorSize ||
        std::fabs(displacement.z) > kSectorSize ||
        !AxisCanTranslate(origin.sx, origin.offset.x, displacement.x) ||
        !AxisCanTranslate(origin.sy, origin.offset.y, displacement.y) ||
        !AxisCanTranslate(origin.sz, origin.offset.z, displacement.z))
        return false;

    translated = Translate(origin, displacement);
    return IsAddressable(translated);
}

bool IsFiniteState(const TeleportState& s)
{
    Quatf ignored;
    return IsAddressable(s.position) && IsAddressable(s.prevPosition) &&
           TryNormalize(s.orientation, ignored) &&
           TryNormalize(s.prevRotation, ignored) &&
           IsFinite(s.linearVelocity) && IsFinite(s.momentum) &&
           IsFinite(s.angularVelocity) && IsFinite(s.forceAccum) &&
           IsFinite(s.torqueAccum);
}

} // namespace

Vec3d RotateVec3d(const Quatf& q, const Vec3d& v)
{
    Quatf normalized;
    Vec3d rotated;
    if (!TryNormalize(q, normalized) || !TryRotateNormalized(normalized, v, rotated))
        return Vec3d{};
    return rotated;
}

bool TryApplyTeleport(const TeleportState& s, const MouthTransform& m,
                      TeleportState& out)
{
    out = s;

    Quatf mouthRotation;
    Quatf sourceOrientation;
    if (!IsFiniteState(s) || !IsAddressable(m.entry) || !IsAddressable(m.exit) ||
        !TryNormalize(m.rotation, mouthRotation) ||
        !TryNormalize(s.orientation, sourceOrientation))
        return false;

    // 2. Position: keep the body's offset FROM the entry mouth (sector-aware, so it
    //    is exact for a nearby body at any galactic distance). The one-sector
    //    capture bound makes the coordinate carry total and forces remote route
    //    motion to be split by the caller.
    const Vec3d offsetFromEntry = Separation(m.entry, s.position);
    if (!IsFinite(offsetFromEntry) ||
        std::fabs(offsetFromEntry.x) > kSectorSize ||
        std::fabs(offsetFromEntry.y) > kSectorSize ||
        std::fabs(offsetFromEntry.z) > kSectorSize)
        return false;

    Vec3d rotatedOffset;
    Vec3d rotatedVelocity;
    Vec3d rotatedMomentum;
    if (!TryRotateNormalized(mouthRotation, offsetFromEntry, rotatedOffset) ||
        !TryRotateNormalized(mouthRotation, s.linearVelocity, rotatedVelocity) ||
        !TryRotateNormalized(mouthRotation, s.momentum, rotatedMomentum))
        return false;

    TeleportState candidate = s;
    if (!TryTranslateBounded(m.exit, rotatedOffset, candidate.position))
        return false;

    // 3. Every authoritative world-frame vector rotates with the mouth.
    candidate.linearVelocity = rotatedVelocity;
    candidate.momentum = rotatedMomentum;

    //    Orientation: prepend the mouth rotation in WORLD space and contain drift.
    //    Near-unit inputs keep their exact bits, including the identity fast path.
    const Quatf composed = mouthRotation * sourceOrientation;
    if (!TryNormalize(composed, candidate.orientation))
        return false;

    //    angularVelocity is body-frame: rotating the body's orientation does not
    //    change the co-rotating spin vector's components, so it is preserved as-is.
    candidate.angularVelocity = s.angularVelocity;

    // 4. Flush staged wrench: it belonged to the pre-jump frame and must not be
    //    consumed on the first post-jump step.
    candidate.forceAccum  = Vec3d{ 0.0, 0.0, 0.0 };
    candidate.torqueAccum = Vec3d{ 0.0, 0.0, 0.0 };

    // 5. prev pose := POST-teleport pose, so render interpolation's delta across the
    //    jump is exactly zero (no source-to-destination smear).
    candidate.prevPosition = candidate.position;
    candidate.prevRotation = candidate.orientation;

    out = candidate;
    return true;
}

TeleportState ApplyTeleport(const TeleportState& s, const MouthTransform& m)
{
    TeleportState out;
    TryApplyTeleport(s, m, out);
    return out;
}

bool TryAdvanceWarpOrigin(const WorldPos& origin, const Vec3d& coordVelocity,
                          double dt, WorldPos& out)
{
    out = origin;
    if (!IsAddressable(origin) || !IsFinite(coordVelocity) ||
        !(dt > 0.0) || !std::isfinite(dt))
        return false;

    const double perAxisLimit = kSectorSize / dt;
    if (std::fabs(coordVelocity.x) > perAxisLimit ||
        std::fabs(coordVelocity.y) > perAxisLimit ||
        std::fabs(coordVelocity.z) > perAxisLimit)
        return false;

    const Vec3d displacement = coordVelocity * dt;
    WorldPos candidate;
    if (!TryTranslateBounded(origin, displacement, candidate))
        return false;

    out = candidate;
    return true;
}

WorldPos AdvanceWarpOrigin(const WorldPos& origin, const Vec3d& coordVelocity, double dt)
{
    WorldPos out;
    TryAdvanceWarpOrigin(origin, coordVelocity, dt, out);
    return out;
}

double WarpProperTime(double dt)
{
    return dt > 0.0 && std::isfinite(dt) ? dt : 0.0;
}

} // namespace sim
