// =============================================================================
// tests/test_math.cpp — core/types.h math invariants
// =============================================================================
// Conventions this file asserts against (see docs/ANALYSIS.md section 2):
//   - Mat4x4 storage is m[row][col] with ROW-VECTOR semantics: v' = v * M.
//     Translation therefore lives in row 3 (m[3][0..2]).
//   - Left-handed throughout. +Z is forward. PerspectiveFovLH maps z to [0, 1].
//
// Nothing here touches D3D12. These are pure CPU header tests.
// =============================================================================

#include "test_framework.h"
#include "core/types.h"
#include "ecs/components.h"   // for ecs::Transform::ToMatrix

namespace
{

// -----------------------------------------------------------------------------
// Local helpers
// -----------------------------------------------------------------------------
core::Vec3f BasisX(const core::Mat4x4& m) { return { m.m[0][0], m.m[1][0], m.m[2][0] }; }
core::Vec3f BasisY(const core::Mat4x4& m) { return { m.m[0][1], m.m[1][1], m.m[2][1] }; }
core::Vec3f BasisZ(const core::Mat4x4& m) { return { m.m[0][2], m.m[1][2], m.m[2][2] }; }

bool QuatApproxEq(const core::Quatf& a, const core::Quatf& b, float eps = 1e-5f)
{
    return std::fabs(a.x - b.x) <= eps
        && std::fabs(a.y - b.y) <= eps
        && std::fabs(a.z - b.z) <= eps
        && std::fabs(a.w - b.w) <= eps;
}

bool MatApproxEq(const core::Mat4x4& a, const core::Mat4x4& b, float eps = 1e-5f)
{
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            if (std::fabs(a.m[i][j] - b.m[i][j]) > eps)
                return false;
    return true;
}

} // namespace

// =============================================================================
// PerspectiveFovLH
// =============================================================================

// The single most common way a projection matrix is silently broken: the
// default Mat4x4 constructor leaves m[3][3] == 1, and a builder that forgets
// to clear it produces a matrix that "looks fine" but has a constant added to
// every w. PerspectiveFovLH zero-fills first (types.h:405-407) — assert it.
TEST_CASE(PerspectiveFovLH_NoIdentityLeak)
{
    const core::Mat4x4 p = core::Mat4x4::PerspectiveFovLH(core::PI * 0.25f, 16.0f / 9.0f, 0.1f, 1000.0f);

    CHECK_EQ(p.m[3][3], 0.0f);   // the classic leak
    CHECK_EQ(p.m[2][3], 1.0f);   // w = z  (left-handed; RH would be -1)

    // Every element outside the five the builder writes must be exactly zero.
    CHECK_EQ(p.m[0][1], 0.0f); CHECK_EQ(p.m[0][2], 0.0f); CHECK_EQ(p.m[0][3], 0.0f);
    CHECK_EQ(p.m[1][0], 0.0f); CHECK_EQ(p.m[1][2], 0.0f); CHECK_EQ(p.m[1][3], 0.0f);
    CHECK_EQ(p.m[2][0], 0.0f); CHECK_EQ(p.m[2][1], 0.0f);
    CHECK_EQ(p.m[3][0], 0.0f); CHECK_EQ(p.m[3][1], 0.0f);
}

// D3D depth convention: z == nearZ maps to 0, z == farZ maps to 1.
// (OpenGL's [-1, 1] mapping would give -1 and +1 here.)
TEST_CASE(PerspectiveFovLH_DepthEndpoints)
{
    const float nearZ = 0.1f;
    const float farZ  = 1000.0f;
    const core::Mat4x4 p = core::Mat4x4::PerspectiveFovLH(core::PI / 3.0f, 16.0f / 9.0f, nearZ, farZ);

    const core::Vec3f atNear = p.TransformPoint({ 0.0f, 0.0f, nearZ });
    const core::Vec3f atFar  = p.TransformPoint({ 0.0f, 0.0f, farZ  });

    CHECK_APPROX(atNear.z, 0.0f);
    CHECK_APPROX(atFar.z,  1.0f);

    // Centre of the frustum stays centred.
    CHECK_APPROX(atNear.x, 0.0f);
    CHECK_APPROX(atNear.y, 0.0f);
    CHECK_APPROX(atFar.x,  0.0f);
    CHECK_APPROX(atFar.y,  0.0f);

    // Depth must be monotonically increasing with distance, and strictly
    // inside [0, 1] for anything between the planes.
    float previous = -1.0f;
    for (float z = nearZ; z <= farZ; z *= 2.0f)
    {
        const float depth = p.TransformPoint({ 0.0f, 0.0f, z }).z;
        CHECK(depth > previous);
        CHECK(depth >= 0.0f && depth <= 1.0f);
        previous = depth;
    }
}

// A point exactly on the frustum edge must land exactly on the NDC edge.
// This is what pins down the fovY / aspect relationship.
TEST_CASE(PerspectiveFovLH_FrustumEdgesMapToNdcEdges)
{
    const float fovY   = core::PI / 3.0f;   // 60 degrees
    const float aspect = 16.0f / 9.0f;
    const float z      = 25.0f;
    const core::Mat4x4 p = core::Mat4x4::PerspectiveFovLH(fovY, aspect, 0.1f, 1000.0f);

    const float halfHeight = z * std::tan(fovY * 0.5f);
    const float halfWidth  = halfHeight * aspect;

    CHECK_APPROX(p.TransformPoint({ halfWidth, 0.0f, z }).x,  1.0f);
    CHECK_APPROX(p.TransformPoint({ -halfWidth, 0.0f, z }).x, -1.0f);
    CHECK_APPROX(p.TransformPoint({ 0.0f, halfHeight, z }).y,  1.0f);
    CHECK_APPROX(p.TransformPoint({ 0.0f, -halfHeight, z }).y, -1.0f);
}

// =============================================================================
// LookAt (left-handed)
// =============================================================================

TEST_CASE(LookAt_BasisIsOrthonormal)
{
    const core::Mat4x4 view = core::Mat4x4::LookAt({ 3.0f, 4.0f, 5.0f },
                                                   { 0.0f, 1.0f, -2.0f },
                                                   { 0.0f, 1.0f, 0.0f });

    const core::Vec3f x = BasisX(view);
    const core::Vec3f y = BasisY(view);
    const core::Vec3f z = BasisZ(view);

    // Unit length
    CHECK_APPROX(x.Length(), 1.0f);
    CHECK_APPROX(y.Length(), 1.0f);
    CHECK_APPROX(z.Length(), 1.0f);

    // Mutually perpendicular
    CHECK_APPROX(x.Dot(y), 0.0f);
    CHECK_APPROX(x.Dot(z), 0.0f);
    CHECK_APPROX(y.Dot(z), 0.0f);

    // Right-handed triple in cross-product terms: x cross y == z.
    const core::Vec3f xy = x.Cross(y);
    CHECK_APPROX(xy.x, z.x);
    CHECK_APPROX(xy.y, z.y);
    CHECK_APPROX(xy.z, z.z);
}

// Left-handed: the camera looks down +Z, so the target must land at positive
// view-space z, at exactly the eye-to-target distance. If LookAt were
// right-handed this would be negative and the whole engine's culling,
// depth test and DXR ray directions would disagree with CLAUDE.md rule 7.
TEST_CASE(LookAt_PlusZPointsTowardTarget)
{
    const core::Vec3f eye    = { 3.0f, 4.0f, 5.0f };
    const core::Vec3f target = { 0.0f, 1.0f, -2.0f };
    const core::Mat4x4 view  = core::Mat4x4::LookAt(eye, target, { 0.0f, 1.0f, 0.0f });

    const float distance = (target - eye).Length();
    const core::Vec3f targetInView = view.TransformPoint(target);

    CHECK(targetInView.z > 0.0f);
    CHECK_APPROX(targetInView.z, distance);
    CHECK_APPROX(targetInView.x, 0.0f);
    CHECK_APPROX(targetInView.y, 0.0f);

    // The eye itself maps to the view-space origin.
    const core::Vec3f eyeInView = view.TransformPoint(eye);
    CHECK_APPROX(eyeInView.x, 0.0f);
    CHECK_APPROX(eyeInView.y, 0.0f);
    CHECK_APPROX(eyeInView.z, 0.0f);
}

// Canonical camera (origin, looking down +Z, +Y up) must be the identity
// rotation: world +X stays view +X (screen right), world +Y stays view +Y.
TEST_CASE(LookAt_CanonicalCameraIsIdentity)
{
    const core::Mat4x4 view = core::Mat4x4::LookAt({ 0.0f, 0.0f, 0.0f },
                                                   { 0.0f, 0.0f, 1.0f },
                                                   { 0.0f, 1.0f, 0.0f });

    CHECK(MatApproxEq(view, core::Mat4x4{}));

    const core::Vec3f p = view.TransformPoint({ 1.0f, 2.0f, 5.0f });
    CHECK_APPROX(p.x, 1.0f);
    CHECK_APPROX(p.y, 2.0f);
    CHECK_APPROX(p.z, 5.0f);
}

// =============================================================================
// Rotation builders
// =============================================================================

// The matrix rotation builders and the quaternion path must agree. This is the
// control for the FromEuler test below: it establishes that RotationX/Y/Z and
// FromAxisAngle/FromQuaternion are all self-consistent, which isolates the
// defect to FromEuler alone.
TEST_CASE(RotationBuilders_MatrixAndQuaternionAgree)
{
    const float angle = 0.7f;

    const core::Mat4x4 rxQuat = core::Mat4x4::FromQuaternion(
        core::Quatf::FromAxisAngle({ 1.0f, 0.0f, 0.0f }, angle));
    const core::Mat4x4 ryQuat = core::Mat4x4::FromQuaternion(
        core::Quatf::FromAxisAngle({ 0.0f, 1.0f, 0.0f }, angle));
    const core::Mat4x4 rzQuat = core::Mat4x4::FromQuaternion(
        core::Quatf::FromAxisAngle({ 0.0f, 0.0f, 1.0f }, angle));

    CHECK(MatApproxEq(core::Mat4x4::RotationX(angle), rxQuat));
    CHECK(MatApproxEq(core::Mat4x4::RotationY(angle), ryQuat));
    CHECK(MatApproxEq(core::Mat4x4::RotationZ(angle), rzQuat));

    // Rotation about an axis leaves that axis fixed.
    CHECK_APPROX(core::Mat4x4::RotationX(angle).TransformDirection({ 1.0f, 0.0f, 0.0f }).x, 1.0f);
    CHECK_APPROX(core::Mat4x4::RotationY(angle).TransformDirection({ 0.0f, 1.0f, 0.0f }).y, 1.0f);
    CHECK_APPROX(core::Mat4x4::RotationZ(angle).TransformDirection({ 0.0f, 0.0f, 1.0f }).z, 1.0f);
}

// -----------------------------------------------------------------------------
// KNOWN FAILING — defect L10, docs/ANALYSIS.md section 4 (LOW)
// -----------------------------------------------------------------------------
// core::Quatf::FromEuler(pitchRad, yawRad, rollRad) at src/core/types.h:193-204
// is the Wikipedia ZYX aerospace formula transcribed verbatim, whose slots are
// (roll = X, pitch = Y, yaw = Z), while the parameters are named
// (pitch, yaw, roll). The parameters are therefore rotated one slot:
//
//     FromEuler(p, 0, 0)  rotates about +Y   (should be +X)
//     FromEuler(0, y, 0)  rotates about +Z   (should be +Y)
//     FromEuler(0, 0, r)  rotates about +X   (should be +Z)
//
// FIXED: FromEuler is now built by explicit axis-angle composition, so each
// parameter drives its intended axis. These are hard assertions and act as the
// regression guard against the closed-form expansion being reintroduced.
//
// It had zero callers, which is why it was never observed — a trap armed for
// whoever first wrote ship orientation code.
// -----------------------------------------------------------------------------
TEST_CASE(Quatf_FromEuler_AxisMapping_L10)
{
    const float angle = 0.6f;
    const core::Vec3f axisX = { 1.0f, 0.0f, 0.0f };
    const core::Vec3f axisY = { 0.0f, 1.0f, 0.0f };
    const core::Vec3f axisZ = { 0.0f, 0.0f, 1.0f };

    // Pitch must rotate about X.
    const core::Quatf pitch = core::Quatf::FromEuler(angle, 0.0f, 0.0f);
    CHECK(QuatApproxEq(pitch, core::Quatf::FromAxisAngle(axisX, angle)));

    // Yaw must rotate about Y.
    const core::Quatf yaw = core::Quatf::FromEuler(0.0f, angle, 0.0f);
    CHECK(QuatApproxEq(yaw, core::Quatf::FromAxisAngle(axisY, angle)));

    // Roll must rotate about Z.
    const core::Quatf roll = core::Quatf::FromEuler(0.0f, 0.0f, angle);
    CHECK(QuatApproxEq(roll, core::Quatf::FromAxisAngle(axisZ, angle)));

    // Same statement expressed geometrically, in case the quaternion sign
    // convention is ever revisited: a pure pitch must leave the X axis fixed
    // and tilt the Y axis toward Z.
    const core::Vec3f pitchedX = pitch.Rotate(axisX);
    CHECK(std::fabs(pitchedX.x - 1.0f) < 1e-5f);
    const core::Vec3f pitchedY = pitch.Rotate(axisY);
    CHECK(pitchedY.z > 0.0f);

    // Composition order: yaw then pitch then roll, applied right-to-left because
    // Rotate() evaluates q*v*q^-1. A combined call must equal the explicit product.
    const core::Quatf combined = core::Quatf::FromEuler(0.3f, 0.4f, 0.5f);
    const core::Quatf explicitProduct =
        (core::Quatf::FromAxisAngle(axisY, 0.4f) *
         core::Quatf::FromAxisAngle(axisX, 0.3f) *
         core::Quatf::FromAxisAngle(axisZ, 0.5f)).Normalized();
    CHECK(QuatApproxEq(combined, explicitProduct));
}

// FromAxisAngle itself is correct — this is the passing counterpart that proves
// the failure above belongs to FromEuler and not to the quaternion type.
TEST_CASE(Quatf_FromAxisAngle_IsCorrect)
{
    const float angle = core::HALF_PI;
    const core::Quatf q = core::Quatf::FromAxisAngle({ 0.0f, 1.0f, 0.0f }, angle);

    CHECK_APPROX(q.x, 0.0f);
    CHECK_APPROX(q.y, std::sin(angle * 0.5f));
    CHECK_APPROX(q.z, 0.0f);
    CHECK_APPROX(q.w, std::cos(angle * 0.5f));
    CHECK_APPROX(q.LengthSq(), 1.0f);

    // 90 degrees about +Y takes +X to -Z under this left-handed convention.
    const core::Vec3f rotated = q.Rotate({ 1.0f, 0.0f, 0.0f });
    CHECK_APPROX(rotated.x, 0.0f);
    CHECK_APPROX(rotated.y, 0.0f);
    CHECK_APPROX(rotated.z, -1.0f);

    // Quatf::Rotate and Mat4x4::FromQuaternion must be the same operation.
    const core::Vec3f viaMatrix = core::Mat4x4::FromQuaternion(q).TransformDirection({ 1.0f, 0.0f, 0.0f });
    CHECK_APPROX(viaMatrix.x, rotated.x);
    CHECK_APPROX(viaMatrix.y, rotated.y);
    CHECK_APPROX(viaMatrix.z, rotated.z);
}

// =============================================================================
// Transform::ToCameraRelativeMatrix composition order
// =============================================================================

// Row-vector convention means `s * r * t` applies scale first, then rotation,
// then translation: v * (S*R*T) == ((v*S)*R)*T. Getting this backwards is the
// classic transform bug — it scales and rotates the translation as well.
TEST_CASE(Transform_ToCameraRelativeMatrix_ScaleThenRotateThenTranslate)
{
    ecs::Transform t;
    t.position = { 10.0f, 0.0f, 0.0f };
    t.rotation = core::Quatf::FromAxisAngle({ 0.0f, 1.0f, 0.0f }, core::HALF_PI);
    t.scale    = { 2.0f, 2.0f, 2.0f };

    const core::Mat4x4 m = t.ToCameraRelativeMatrix({});

    // (1,0,0) --scale 2--> (2,0,0) --yaw 90 (LH)--> (0,0,-2) --translate--> (10,0,-2)
    const core::Vec3f p = m.TransformPoint({ 1.0f, 0.0f, 0.0f });
    CHECK_APPROX(p.x, 10.0f);
    CHECK_APPROX(p.y, 0.0f);
    CHECK_APPROX(p.z, -2.0f);

    // The wrong order (translate, then rotate, then scale) would give
    // (0, 0, -22) — assert we are nowhere near it.
    CHECK(std::fabs(p.z + 22.0f) > 1.0f);
}

// Because translation is applied last, the translation row must be the raw
// position: neither scaled nor rotated. This is the cheapest single assertion
// that pins the composition order.
TEST_CASE(Transform_ToCameraRelativeMatrix_TranslationRowIsUnmodified)
{
    ecs::Transform t;
    t.position = { 3.0f, -7.0f, 11.0f };
    t.rotation = core::Quatf::FromAxisAngle({ 0.3f, 0.5f, 0.81f }, 1.1f);
    t.scale    = { 5.0f, 0.25f, 2.0f };

    const core::Mat4x4 m = t.ToCameraRelativeMatrix({});

    CHECK_APPROX(m.m[3][0], 3.0f);
    CHECK_APPROX(m.m[3][1], -7.0f);
    CHECK_APPROX(m.m[3][2], 11.0f);
    CHECK_APPROX(m.m[3][3], 1.0f);

    // The origin of the object lands exactly on the transform's position.
    const core::Vec3f origin = m.TransformPoint({ 0.0f, 0.0f, 0.0f });
    CHECK_APPROX(origin.x, 3.0f);
    CHECK_APPROX(origin.y, -7.0f);
    CHECK_APPROX(origin.z, 11.0f);
}

TEST_CASE(Transform_ToCameraRelativeMatrix_PreservesPlanetaryOffset)
{
    ecs::Transform t;
    t.position = { 1.0e7 + 0.25, -2.0e7 + 0.5, 3.0e7 - 0.75 };
    const core::Vec3d cameraPosition = { 1.0e7, -2.0e7, 3.0e7 };

    const core::Mat4x4 m = t.ToCameraRelativeMatrix(cameraPosition);

    CHECK_APPROX_EPS(m.m[3][0], 0.25f, 1e-6f);
    CHECK_APPROX_EPS(m.m[3][1], 0.5f, 1e-6f);
    CHECK_APPROX_EPS(m.m[3][2], -0.75f, 1e-6f);
}

TEST_CASE(Transform_ToCameraRelativeMatrix_IdentityTransformIsIdentity)
{
    const ecs::Transform t;
    CHECK(MatApproxEq(t.ToCameraRelativeMatrix({}), core::Mat4x4{}));
}

// Scale must be recoverable from the basis row lengths, independent of rotation.
TEST_CASE(Transform_ToCameraRelativeMatrix_NonUniformScaleGoesToBasisRows)
{
    ecs::Transform t;
    t.rotation = core::Quatf::FromAxisAngle({ 0.0f, 0.0f, 1.0f }, 0.9f);
    t.scale    = { 2.0f, 3.0f, 4.0f };

    const core::Mat4x4 m = t.ToCameraRelativeMatrix({});

    CHECK_APPROX(core::Vec3f(m.m[0][0], m.m[0][1], m.m[0][2]).Length(), 2.0f);
    CHECK_APPROX(core::Vec3f(m.m[1][0], m.m[1][1], m.m[1][2]).Length(), 3.0f);
    CHECK_APPROX(core::Vec3f(m.m[2][0], m.m[2][1], m.m[2][2]).Length(), 4.0f);
}

// =============================================================================
// InverseTranspose3x3 — the normal matrix
// =============================================================================

// For row-vector storage the normal matrix is (A^-1)^T = cofactor(A)/det.
// Under a pure diagonal scale that reduces to the reciprocal scale.
TEST_CASE(InverseTranspose3x3_NonUniformScaleInverts)
{
    const core::Mat4x4 m = core::Mat4x4::Scaling(2.0f, 1.0f, 0.5f);
    const core::Mat4x4 n = core::Mat4x4::InverseTranspose3x3(m);

    CHECK_APPROX(n.m[0][0], 0.5f);
    CHECK_APPROX(n.m[1][1], 1.0f);
    CHECK_APPROX(n.m[2][2], 2.0f);

    // Off-diagonal terms must stay zero.
    CHECK_APPROX(n.m[0][1], 0.0f);
    CHECK_APPROX(n.m[0][2], 0.0f);
    CHECK_APPROX(n.m[1][0], 0.0f);
    CHECK_APPROX(n.m[1][2], 0.0f);
    CHECK_APPROX(n.m[2][0], 0.0f);
    CHECK_APPROX(n.m[2][1], 0.0f);

    // Row/column 3 must remain identity so the result is safe to upload as a mat4.
    CHECK_APPROX(n.m[3][3], 1.0f);
    CHECK_APPROX(n.m[3][0], 0.0f);
    CHECK_APPROX(n.m[3][1], 0.0f);
    CHECK_APPROX(n.m[3][2], 0.0f);
}

// The property that actually matters: a normal stays perpendicular to its
// surface after transformation. Under non-uniform scale, naively transforming
// the normal by the world matrix breaks this — that is exactly defect L5 in
// the DXR closest-hit shader. This test states the invariant on the CPU side.
TEST_CASE(InverseTranspose3x3_PreservesPerpendicularityUnderNonUniformScale)
{
    // A 45-degree surface: normal and tangent both diagonal in XY.
    const core::Vec3f normal  = core::Vec3f( 1.0f,  1.0f, 0.0f).Normalized();
    const core::Vec3f tangent = core::Vec3f( 1.0f, -1.0f, 0.0f).Normalized();
    CHECK_APPROX(normal.Dot(tangent), 0.0f);

    const core::Mat4x4 world = core::Mat4x4::Scaling(2.0f, 1.0f, 0.5f)
                             * core::Mat4x4::RotationY(0.4f);
    const core::Mat4x4 normalMatrix = core::Mat4x4::InverseTranspose3x3(world);

    const core::Vec3f t2 = world.TransformDirection(tangent);
    const core::Vec3f n2 = normalMatrix.TransformDirection(normal).Normalized();

    CHECK_APPROX_EPS(n2.Dot(t2.Normalized()), 0.0f, 1e-5f);

    // Control: the naive transform (world matrix applied to the normal) is
    // measurably wrong here, which is what makes the test above meaningful.
    const core::Vec3f naive = world.TransformDirection(normal).Normalized();
    CHECK(std::fabs(naive.Dot(t2.Normalized())) > 0.1f);
}

// Under uniform scale the normal matrix must be the rotation scaled by 1/s,
// i.e. after normalisation it is exactly the rotation. This is why L5 has
// never been observed at runtime: every entity in main.cpp uses uniform scale.
TEST_CASE(InverseTranspose3x3_UniformScaleReducesToRotation)
{
    const core::Mat4x4 rotation = core::Mat4x4::RotationZ(0.63f);
    const core::Mat4x4 world    = core::Mat4x4::Scaling(3.0f) * rotation;
    const core::Mat4x4 n        = core::Mat4x4::InverseTranspose3x3(world);

    const core::Vec3f probe   = core::Vec3f(0.3f, 0.9f, 0.31f).Normalized();
    const core::Vec3f viaN    = n.TransformDirection(probe).Normalized();
    const core::Vec3f viaRot  = rotation.TransformDirection(probe).Normalized();

    CHECK_APPROX(viaN.x, viaRot.x);
    CHECK_APPROX(viaN.y, viaRot.y);
    CHECK_APPROX(viaN.z, viaRot.z);
}

// Degenerate input must fall back to identity rather than producing inf/NaN
// that would propagate into a constant buffer.
TEST_CASE(InverseTranspose3x3_SingularFallsBackToIdentity)
{
    const core::Mat4x4 flattened = core::Mat4x4::Scaling(1.0f, 1.0f, 0.0f);
    const core::Mat4x4 n = core::Mat4x4::InverseTranspose3x3(flattened);

    CHECK(MatApproxEq(n, core::Mat4x4{}));

    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            CHECK(std::isfinite(n.m[i][j]));
}

// =============================================================================
// Matrix product ordering
// =============================================================================

// Guards the row-vector contract that renderer.cpp:553 depends on when it
// computes `wvp = world * viewProj`. If Multiply ever changed operand order
// every transform in the engine would silently break.
TEST_CASE(Mat4x4_MultiplyIsRowVectorOrdered)
{
    const core::Mat4x4 a = core::Mat4x4::Translation(1.0f, 2.0f, 3.0f);
    const core::Mat4x4 b = core::Mat4x4::Scaling(2.0f);

    const core::Vec3f probe = { 1.0f, 1.0f, 1.0f };

    // v * (A*B) must equal (v * A) * B.
    const core::Vec3f combined  = (a * b).TransformPoint(probe);
    const core::Vec3f sequential = b.TransformPoint(a.TransformPoint(probe));

    CHECK_APPROX(combined.x, sequential.x);
    CHECK_APPROX(combined.y, sequential.y);
    CHECK_APPROX(combined.z, sequential.z);

    // Concretely: translate then scale == (2*(1+1), 2*(1+2), 2*(1+3))
    CHECK_APPROX(combined.x, 4.0f);
    CHECK_APPROX(combined.y, 6.0f);
    CHECK_APPROX(combined.z, 8.0f);
}

// Translation lives in row 3 (row-vector storage), NOT column 3. types.h:8
// claims "column-major, D3D convention", which is wrong; this test asserts
// what the code actually does and what the HLSL side depends on.
TEST_CASE(Mat4x4_TranslationLivesInRowThree)
{
    const core::Mat4x4 t = core::Mat4x4::Translation(7.0f, 8.0f, 9.0f);

    CHECK_EQ(t.m[3][0], 7.0f);
    CHECK_EQ(t.m[3][1], 8.0f);
    CHECK_EQ(t.m[3][2], 9.0f);

    CHECK_EQ(t.m[0][3], 0.0f);
    CHECK_EQ(t.m[1][3], 0.0f);
    CHECK_EQ(t.m[2][3], 0.0f);

    // Directions must ignore translation entirely.
    const core::Vec3f d = t.TransformDirection({ 1.0f, 0.0f, 0.0f });
    CHECK_APPROX(d.x, 1.0f);
    CHECK_APPROX(d.y, 0.0f);
    CHECK_APPROX(d.z, 0.0f);
}

// -----------------------------------------------------------------------------
// Mat4x4::Inverse — new. Its absence is why path_trace.hlsl hardcoded a 70-degree
// FOV and why path_tracer.cpp uploads viewProj under a member named for its
// inverse. Verified by round-trip rather than by comparing against a hand-written
// expected matrix, which would just restate the implementation.
// -----------------------------------------------------------------------------
TEST_CASE(Mat4x4_InverseRoundTrips)
{
    // A deliberately nasty matrix: non-uniform scale, rotation, and translation.
    const core::Mat4x4 m =
        core::Mat4x4::Scaling(2.0f, 0.5f, 3.0f) *
        core::Mat4x4::RotationY(0.7f) *
        core::Mat4x4::RotationX(-0.3f) *
        core::Mat4x4::Translation(10.0f, -4.0f, 7.0f);

    bool singular = true;
    const core::Mat4x4 inv = core::Mat4x4::Inverse(m, &singular);
    CHECK_FALSE(singular);

    const core::Mat4x4 ident = m * inv;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            CHECK_APPROX_EPS(ident.m[r][c], r == c ? 1.0f : 0.0f, 1e-4f);

    // A point pushed through m and back must land where it started.
    const core::Vec3f p = { 3.0f, -2.0f, 5.0f };
    const core::Vec3f round = inv.TransformPoint(m.TransformPoint(p));
    CHECK_APPROX_EPS(round.x, p.x, 1e-3f);
    CHECK_APPROX_EPS(round.y, p.y, 1e-3f);
    CHECK_APPROX_EPS(round.z, p.z, 1e-3f);
}

TEST_CASE(Mat4x4_InverseReportsSingular)
{
    // Collapsing one axis makes the matrix non-invertible.
    core::Mat4x4 degenerate = core::Mat4x4::Scaling(1.0f, 0.0f, 1.0f);
    bool singular = false;
    const core::Mat4x4 inv = core::Mat4x4::Inverse(degenerate, &singular);
    CHECK(singular);
    // Must return identity rather than NaNs, so callers cannot silently propagate
    // garbage through a transform chain.
    CHECK_EQ(inv.m[0][0], 1.0f);
    CHECK_EQ(inv.m[1][1], 1.0f);
}

// -----------------------------------------------------------------------------
// Vec3d — CLAUDE.md Rule 1 requires double-precision world positions, but Vec3d
// lacked Cross, compound assignment, scalar-left multiply and Lerp, so it could
// not actually receive the conversion. These cover the added surface.
// -----------------------------------------------------------------------------
TEST_CASE(Vec3d_CrossMatchesRightHandRule)
{
    const core::Vec3d x = { 1.0, 0.0, 0.0 };
    const core::Vec3d y = { 0.0, 1.0, 0.0 };
    const core::Vec3d z = x.Cross(y);
    CHECK_APPROX(static_cast<float>(z.x), 0.0f);
    CHECK_APPROX(static_cast<float>(z.y), 0.0f);
    CHECK_APPROX(static_cast<float>(z.z), 1.0f);

    // Anti-commutative.
    const core::Vec3d back = y.Cross(x);
    CHECK_APPROX(static_cast<float>(back.z), -1.0f);
}

TEST_CASE(Vec3d_ScalarAndCompoundOperators)
{
    core::Vec3d v = { 1.0, 2.0, 3.0 };
    v *= 2.0;
    CHECK_APPROX(static_cast<float>(v.x), 2.0f);
    CHECK_APPROX(static_cast<float>(v.z), 6.0f);
    v /= 2.0;
    CHECK_APPROX(static_cast<float>(v.z), 3.0f);

    // Scalar on the left must compile and match scalar on the right.
    const core::Vec3d left = 3.0 * v;
    const core::Vec3d right = v * 3.0;
    CHECK(left == right);
}

TEST_CASE(Vec3d_LerpAndDistance)
{
    const core::Vec3d a = { 0.0, 0.0, 0.0 };
    const core::Vec3d b = { 10.0, 0.0, 0.0 };
    const core::Vec3d mid = core::Vec3d::Lerp(a, b, 0.25);
    CHECK_APPROX(static_cast<float>(mid.x), 2.5f);
    CHECK_APPROX(static_cast<float>(a.Distance(b)), 10.0f);
    CHECK_APPROX(static_cast<float>(a.DistanceSq(b)), 100.0f);
}

// The whole point of Rule 1: at planetary distances float cannot represent world
// positions, but the camera-relative difference is small and representable.
TEST_CASE(Vec3d_CameraRelativeSurvivesPlanetaryDistance)
{
    // 1e7 metres from the origin, where float spacing is about 1 metre.
    const core::Vec3d cameraPos = { 1.0e7, 0.0, 0.0 };
    const core::Vec3d objectPos = { 1.0e7 + 0.25, 0.0, 0.0 };

    // Narrowing the absolute positions loses the separation entirely.
    CHECK_EQ(cameraPos.ToFloat().x, objectPos.ToFloat().x);

    // Subtracting first preserves it exactly.
    const core::Vec3f relative = (objectPos - cameraPos).ToFloat();
    CHECK_APPROX_EPS(relative.x, 0.25f, 1e-6f);
}

// =============================================================================
// Shadow-map projection
//
// The directional shadow map is built as LookAt(light) * OrthoLH(...), and the
// pixel shader relies on that product mapping the light frustum onto D3D clip
// space exactly: XY to [-1,1] and Z to [0,1]. Neither OrthoLH nor the light
// matrix had any coverage before shadows started depending on them, and a
// silent error here does not crash - it produces shadows that are subtly
// misplaced, or a shadow map that is entirely lit, which is very hard to
// distinguish from "shadows not implemented yet".
// =============================================================================

TEST_CASE(OrthoLH_MapsExtentsToClipSpace)
{
    const core::Mat4x4 proj = core::Mat4x4::OrthoLH(20.0f, 10.0f, 1.0f, 101.0f);

    // Centre of the near plane sits at clip origin with depth 0.
    const core::Vec3f centreNear = proj.TransformPoint({ 0.0f, 0.0f, 1.0f });
    CHECK_APPROX(centreNear.x, 0.0f);
    CHECK_APPROX(centreNear.y, 0.0f);
    CHECK_APPROX(centreNear.z, 0.0f);

    // Far plane maps to depth 1 - the D3D convention, not OpenGL's [-1,1].
    const core::Vec3f centreFar = proj.TransformPoint({ 0.0f, 0.0f, 101.0f });
    CHECK_APPROX(centreFar.z, 1.0f);

    // Half-width and half-height land on the clip-space edges.
    const core::Vec3f corner = proj.TransformPoint({ 10.0f, 5.0f, 1.0f });
    CHECK_APPROX(corner.x, 1.0f);
    CHECK_APPROX(corner.y, 1.0f);

    const core::Vec3f opposite = proj.TransformPoint({ -10.0f, -5.0f, 1.0f });
    CHECK_APPROX(opposite.x, -1.0f);
    CHECK_APPROX(opposite.y, -1.0f);
}

// Mirrors Renderer::UpdateLightMatrix. The camera-relative origin must land
// inside the light frustum and at a sane depth, because that origin is where
// the camera is - if it fell outside, everything the viewer looks at would be
// outside the shadow map.
TEST_CASE(ShadowLightMatrix_PlacesCameraOriginInsideFrustum)
{
    const core::Vec3f lightDir = core::Vec3f(0.5f, 0.8f, 0.3f).Normalized();
    const float extent      = 24.0f;
    const float depthRange  = 120.0f;

    const core::Vec3f eye = lightDir * (depthRange * 0.5f);
    const core::Mat4x4 view =
        core::Mat4x4::LookAt(eye, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
    const core::Mat4x4 proj =
        core::Mat4x4::OrthoLH(extent * 2.0f, extent * 2.0f, 0.1f, depthRange);
    const core::Mat4x4 lightViewProj = view * proj;

    const core::Vec3f origin = lightViewProj.TransformPoint({ 0.0f, 0.0f, 0.0f });
    CHECK_APPROX_EPS(origin.x, 0.0f, 1e-4f);
    CHECK_APPROX_EPS(origin.y, 0.0f, 1e-4f);
    CHECK(origin.z > 0.0f);
    CHECK(origin.z < 1.0f);

    // A point directly beneath the light is CLOSER to it, so it must have a
    // smaller depth than the origin. This is the comparison the shadow test
    // actually performs; getting the sign backwards inverts every shadow.
    const core::Vec3f nearer = lightViewProj.TransformPoint(lightDir * 5.0f);
    CHECK(nearer.z < origin.z);

    // A point well outside the horizontal extent must fall outside clip space,
    // which is what the sampler's white border then reports as "lit".
    const core::Vec3f outside =
        lightViewProj.TransformPoint({ extent * 3.0f, 0.0f, 0.0f });
    CHECK(std::fabs(outside.x) > 1.0f || std::fabs(outside.y) > 1.0f);
}

// A light pointing straight up is the degenerate case for LookAt: the default
// up vector is parallel to the view direction and the cross product collapses.
// UpdateLightMatrix switches up vectors past 0.99; verify the switched-to one
// actually produces a usable basis rather than NaNs.
TEST_CASE(ShadowLightMatrix_HandlesStraightDownLight)
{
    const core::Vec3f lightDir = { 0.0f, 1.0f, 0.0f };
    const core::Mat4x4 view = core::Mat4x4::LookAt(
        lightDir * 60.0f, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f });
    const core::Mat4x4 proj = core::Mat4x4::OrthoLH(48.0f, 48.0f, 0.1f, 120.0f);
    const core::Vec3f origin = (view * proj).TransformPoint({ 0.0f, 0.0f, 0.0f });

    CHECK(!std::isnan(origin.x));
    CHECK(!std::isnan(origin.y));
    CHECK(!std::isnan(origin.z));
    CHECK_APPROX_EPS(origin.x, 0.0f, 1e-4f);
    CHECK_APPROX_EPS(origin.y, 0.0f, 1e-4f);
    CHECK(origin.z > 0.0f && origin.z < 1.0f);
}
