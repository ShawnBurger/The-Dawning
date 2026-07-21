#include "ftl.h"

namespace sim
{

Vec3d RotateVec3d(const Quatf& q, const Vec3d& v)
{
    // q * v * q^-1, evaluated with the same cross-product optimization as
    // Quatf::Rotate but in double: result = v + 2*( qv x (qv x v) + w*(qv x v) ).
    const Vec3d qv{ static_cast<double>(q.x),
                    static_cast<double>(q.y),
                    static_cast<double>(q.z) };
    const double w = static_cast<double>(q.w);
    const Vec3d uv  = qv.Cross(v);
    const Vec3d uuv = qv.Cross(uv);
    return v + (uv * w + uuv) * 2.0;
}

TeleportState ApplyTeleport(const TeleportState& s, const MouthTransform& m)
{
    TeleportState out = s;

    // 2. Position: keep the body's offset FROM the entry mouth (sector-aware, so it
    //    is exact for a nearby body at any galactic distance), rotate that offset by
    //    the mouth rotation, and place it relative to the exit mouth. WorldPos stays
    //    precise across an arbitrarily long jump - no far-absolute Vec3d is formed.
    const Vec3d offsetFromEntry = Separation(m.entry, s.position);
    const Vec3d rotatedOffset   = RotateVec3d(m.rotation, offsetFromEntry);
    out.position = Translate(m.exit, rotatedOffset);

    // 3. World-frame linearVelocity rotates with the mouth. This is the load-bearing,
    //    easy-to-miss step: even a rotation-only wormhole must rotate it.
    out.linearVelocity = RotateVec3d(m.rotation, s.linearVelocity);

    //    Orientation: prepend the mouth rotation in WORLD space. operator* is a pure
    //    Hamilton product (no renormalization), so an identity mouth leaves the
    //    orientation bit-identical. The mouth rotation is assumed unit.
    out.orientation = m.rotation * s.orientation;

    //    angularVelocity is body-frame: rotating the body's orientation does not
    //    change the co-rotating spin vector's components, so it is preserved as-is.
    out.angularVelocity = s.angularVelocity;

    // 4. Flush staged wrench: it belonged to the pre-jump frame and must not be
    //    consumed on the first post-jump step.
    out.forceAccum  = Vec3d{ 0.0, 0.0, 0.0 };
    out.torqueAccum = Vec3d{ 0.0, 0.0, 0.0 };

    // 5. prev pose := POST-teleport pose, so render interpolation's delta across the
    //    jump is exactly zero (no source-to-destination smear).
    out.prevPosition = out.position;
    out.prevRotation = out.orientation;

    return out;
}

WorldPos AdvanceWarpOrigin(const WorldPos& origin, const Vec3d& coordVelocity, double dt)
{
    // A moving floating origin: displace by coordVelocity*dt and re-canonicalize so
    // the local coordinates stay small. coordVelocity is a bubble COORDINATE speed
    // and may be superluminal; nothing local exceeds c.
    return Translate(origin, coordVelocity * dt);
}

} // namespace sim
