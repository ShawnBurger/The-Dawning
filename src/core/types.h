#pragma once
// =============================================================================
// core/types.h — The Dawning Engine Foundation Types
// =============================================================================
// Every math type the engine uses. Designed for:
//   - Vec3d (double) for world-space positions (interstellar precision)
//   - Vec3f (float) for camera-relative GPU data and local calculations
//   - Mat4x4 (float) for GPU constant buffers — see the layout warning below
//   - Quatf (float) for rotations
//   - Color (float RGBA) for materials and lighting
//
// All types are POD-compatible for direct memcpy to GPU constant buffers.
//
// =============================================================================
// MATRIX LAYOUT — READ THIS BEFORE TOUCHING ANY TRANSFORM CODE
// =============================================================================
// Mat4x4 is ROW-MAJOR STORAGE with ROW-VECTOR SEMANTICS. It is NOT column-major
// and nothing in the engine ever transposes it.
//
//   - Storage is m[row][col]. Data() returns those bytes in row order.
//   - Translation lives at m[3][0..2] (Mat4x4::Translation, ~line 307).
//   - TransformPoint computes v * M, not M * v (~line 434): the input vector is
//     a row vector multiplied on the LEFT.
//   - Consequently composition reads left-to-right: `world * viewProj` applies
//     world first. That is the order renderer.cpp uses.
//
// CORRECTNESS DEPENDS ON AN UNSTATED HLSL INTERACTION. UploadCB memcpys these
// raw row-major bytes straight into a constant buffer. HLSL cbuffers default to
// COLUMN-MAJOR packing, so the GPU reinterprets the upload as the TRANSPOSE of
// what the CPU wrote. basic_vs.hlsl:36 then does mul(worldViewProj, float4(...)),
// i.e. the column-vector form M * v. Those two transposes cancel exactly, which
// is the only reason transforms render correctly.
//
// WARNING: this cancellation is silent and load-bearing. Either of the
// following will break EVERY transform in the engine with no compile error and
// no validation-layer message:
//   - adding `#pragma pack_matrix(row_major)` (or the /Zpr compile flag) to any
//     shader that consumes these matrices — this removes the GPU-side transpose
//     while the CPU still uploads row-major;
//   - "fixing" the shader to mul(v, M) to match the CPU's row-vector convention
//     — this removes the shader-side transpose instead.
// Change one and you must change the other. If you touch either side, verify
// against a translated, rotated, non-uniformly scaled object, not just a cube
// at the origin — a pure rotation about a single axis can mask a transpose.
// =============================================================================

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace core
{

// =============================================================================
// Vec2f
// =============================================================================
struct Vec2f
{
    float x = 0.0f;
    float y = 0.0f;

    Vec2f() = default;
    Vec2f(float x_, float y_) : x(x_), y(y_) {}

    Vec2f operator+(const Vec2f& v) const { return { x + v.x, y + v.y }; }
    Vec2f operator-(const Vec2f& v) const { return { x - v.x, y - v.y }; }
    Vec2f operator*(float s) const { return { x * s, y * s }; }

    float Length() const { return std::sqrt(x * x + y * y); }
};

// =============================================================================
// Vec3f — GPU-bound positions, normals, directions
// =============================================================================
struct Vec3f
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3f() = default;
    Vec3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3f operator+(const Vec3f& v) const { return { x + v.x, y + v.y, z + v.z }; }
    Vec3f operator-(const Vec3f& v) const { return { x - v.x, y - v.y, z - v.z }; }
    Vec3f operator*(float s) const { return { x * s, y * s, z * s }; }
    Vec3f operator/(float s) const { float inv = 1.0f / s; return { x * inv, y * inv, z * inv }; }
    Vec3f operator-() const { return { -x, -y, -z }; }

    Vec3f& operator+=(const Vec3f& v) { x += v.x; y += v.y; z += v.z; return *this; }
    Vec3f& operator-=(const Vec3f& v) { x -= v.x; y -= v.y; z -= v.z; return *this; }
    Vec3f& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }

    float Dot(const Vec3f& v) const { return x * v.x + y * v.y + z * v.z; }

    Vec3f Cross(const Vec3f& v) const
    {
        return { y * v.z - z * v.y,
                 z * v.x - x * v.z,
                 x * v.y - y * v.x };
    }

    float LengthSq() const { return x * x + y * y + z * z; }
    float Length() const { return std::sqrt(LengthSq()); }

    Vec3f Normalized() const
    {
        float len = Length();
        if (len < 1e-8f) return { 0.0f, 0.0f, 0.0f };
        return *this / len;
    }

    static Vec3f Lerp(const Vec3f& a, const Vec3f& b, float t)
    {
        return a + (b - a) * t;
    }
};

inline Vec3f operator*(float s, const Vec3f& v) { return v * s; }

// =============================================================================
// Vec3d — World-space positions (double precision for interstellar distances)
// =============================================================================
struct Vec3d
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    Vec3d() = default;
    Vec3d(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    Vec3d operator+(const Vec3d& v) const { return { x + v.x, y + v.y, z + v.z }; }
    Vec3d operator-(const Vec3d& v) const { return { x - v.x, y - v.y, z - v.z }; }
    Vec3d operator*(double s) const { return { x * s, y * s, z * s }; }
    Vec3d operator/(double s) const { double inv = 1.0 / s; return { x * inv, y * inv, z * inv }; }
    Vec3d operator-() const { return { -x, -y, -z }; }

    Vec3d& operator+=(const Vec3d& v) { x += v.x; y += v.y; z += v.z; return *this; }
    Vec3d& operator-=(const Vec3d& v) { x -= v.x; y -= v.y; z -= v.z; return *this; }
    Vec3d& operator*=(double s) { x *= s; y *= s; z *= s; return *this; }
    Vec3d& operator/=(double s) { double inv = 1.0 / s; x *= inv; y *= inv; z *= inv; return *this; }

    bool operator==(const Vec3d& v) const { return x == v.x && y == v.y && z == v.z; }
    bool operator!=(const Vec3d& v) const { return !(*this == v); }

    double Dot(const Vec3d& v) const { return x * v.x + y * v.y + z * v.z; }

    Vec3d Cross(const Vec3d& v) const
    {
        return { y * v.z - z * v.y,
                 z * v.x - x * v.z,
                 x * v.y - y * v.x };
    }

    double LengthSq() const { return x * x + y * y + z * z; }
    double Length() const { return std::sqrt(LengthSq()); }

    double DistanceSq(const Vec3d& v) const { return (*this - v).LengthSq(); }
    double Distance(const Vec3d& v) const { return (*this - v).Length(); }

    Vec3d Normalized() const
    {
        double len = Length();
        if (len < 1e-15) return { 0.0, 0.0, 0.0 };
        return *this / len;
    }

    static Vec3d Lerp(const Vec3d& a, const Vec3d& b, double t)
    {
        return { a.x + (b.x - a.x) * t,
                 a.y + (b.y - a.y) * t,
                 a.z + (b.z - a.z) * t };
    }

    // Widen from single precision. Safe for offsets and directions; widening an
    // absolute world position that has already been through float does NOT recover
    // the precision it lost.
    static Vec3d FromFloat(const Vec3f& v)
    {
        return { static_cast<double>(v.x),
                 static_cast<double>(v.y),
                 static_cast<double>(v.z) };
    }

    // Narrow to float for GPU upload. CALLER MUST SUBTRACT THE CAMERA POSITION
    // FIRST (Rule 1) - at 1e7 m from the origin, float spacing is about 1 m, so a
    // narrowed absolute position quantises to visible jitter.
    Vec3f ToFloat() const
    {
        return { static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) };
    }
};

inline Vec3d operator*(double s, const Vec3d& v) { return v * s; }

// =============================================================================
// Vec4f — Homogeneous coordinates, shader parameters
// =============================================================================
struct Vec4f
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    Vec4f() = default;
    Vec4f(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4f(const Vec3f& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}

    Vec3f XYZ() const { return { x, y, z }; }
};

// =============================================================================
// Color — RGBA float color
// =============================================================================
struct Color
{
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    Color() = default;
    Color(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}

    Color operator*(float s) const { return { r * s, g * s, b * s, a }; }
    Color operator*(const Color& c) const { return { r * c.r, g * c.g, b * c.b, a * c.a }; }

    static Color White()   { return { 1.0f, 1.0f, 1.0f, 1.0f }; }
    static Color Black()   { return { 0.0f, 0.0f, 0.0f, 1.0f }; }
    static Color Red()     { return { 1.0f, 0.0f, 0.0f, 1.0f }; }
    static Color Green()   { return { 0.0f, 1.0f, 0.0f, 1.0f }; }
    static Color Blue()    { return { 0.0f, 0.0f, 1.0f, 1.0f }; }
    static Color CornflowerBlue() { return { 0.392f, 0.584f, 0.929f, 1.0f }; }
};

// =============================================================================
// Quatf — Rotation quaternion (float)
// =============================================================================
struct Quatf
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    Quatf() = default;
    Quatf(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static Quatf Identity() { return { 0.0f, 0.0f, 0.0f, 1.0f }; }

    static Quatf FromAxisAngle(const Vec3f& axis, float radians)
    {
        float half = radians * 0.5f;
        float s = std::sin(half);
        float c = std::cos(half);
        Vec3f a = axis.Normalized();
        return { a.x * s, a.y * s, a.z * s, c };
    }

    // pitch about X, yaw about Y, roll about Z - matching this engine's LH, Y-up,
    // +Z-forward convention. Composed intrinsically as yaw, then pitch, then roll:
    // because Rotate() evaluates q*v*q^-1, in (a*b) the RIGHT factor applies first.
    //
    // Built by explicit axis-angle composition rather than an expanded closed form.
    // The previous implementation was the Wikipedia ZYX aerospace formula verbatim,
    // whose slots are (roll=X, pitch=Y, yaw=Z), but its parameters were named
    // (pitch, yaw, roll) - so every argument drove the wrong axis: FromEuler(p,0,0)
    // rotated about +Y, (0,y,0) about +Z, and (0,0,r) about +X.
    static Quatf FromEuler(float pitchRad, float yawRad, float rollRad)
    {
        const Quatf qYaw   = FromAxisAngle(Vec3f(0.0f, 1.0f, 0.0f), yawRad);
        const Quatf qPitch = FromAxisAngle(Vec3f(1.0f, 0.0f, 0.0f), pitchRad);
        const Quatf qRoll  = FromAxisAngle(Vec3f(0.0f, 0.0f, 1.0f), rollRad);
        return (qYaw * qPitch * qRoll).Normalized();
    }

    Quatf operator*(const Quatf& q) const
    {
        return {
            w * q.x + x * q.w + y * q.z - z * q.y,
            w * q.y - x * q.z + y * q.w + z * q.x,
            w * q.z + x * q.y - y * q.x + z * q.w,
            w * q.w - x * q.x - y * q.y - z * q.z
        };
    }

    float LengthSq() const { return x * x + y * y + z * z + w * w; }

    Quatf Normalized() const
    {
        float len = std::sqrt(LengthSq());
        if (len < 1e-8f) return Identity();
        float inv = 1.0f / len;
        return { x * inv, y * inv, z * inv, w * inv };
    }

    Quatf Conjugate() const { return { -x, -y, -z, w }; }

    Vec3f Rotate(const Vec3f& v) const
    {
        // q * v * q^-1 optimized
        Vec3f qv(x, y, z);
        Vec3f uv = qv.Cross(v);
        Vec3f uuv = qv.Cross(uv);
        return v + (uv * w + uuv) * 2.0f;
    }

    static Quatf Slerp(const Quatf& a, const Quatf& b, float t)
    {
        float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        Quatf target = b;
        if (dot < 0.0f) { dot = -dot; target = { -b.x, -b.y, -b.z, -b.w }; }

        if (dot > 0.9995f)
        {
            // Linear interpolation for very close quaternions
            return Quatf(
                a.x + t * (target.x - a.x),
                a.y + t * (target.y - a.y),
                a.z + t * (target.z - a.z),
                a.w + t * (target.w - a.w)
            ).Normalized();
        }

        float theta = std::acos(dot);
        float sinTheta = std::sin(theta);
        float wa = std::sin((1.0f - t) * theta) / sinTheta;
        float wb = std::sin(t * theta) / sinTheta;
        return Quatf(
            wa * a.x + wb * target.x,
            wa * a.y + wb * target.y,
            wa * a.z + wb * target.z,
            wa * a.w + wb * target.w
        );
    }
};

// =============================================================================
// Mat4x4 — 4x4 matrix (float, ROW-MAJOR storage, ROW-VECTOR semantics)
// =============================================================================
// Storage is m[row][col]; Data() returns those bytes in row order. Functions
// produce matrices for D3D's row-vector convention (multiply: vector * matrix,
// which is equivalent to matrix^T * vector). The column-major reinterpretation
// happens on the GPU via HLSL's default cbuffer packing — see the MATRIX LAYOUT
// header at the top of this file, which this comment must not contradict.
struct Mat4x4
{
    float m[4][4] = {};

    Mat4x4() { Identity(*this); }

    float& operator()(int row, int col) { return m[row][col]; }
    float  operator()(int row, int col) const { return m[row][col]; }

    // Raw float pointer for GPU constant buffer upload
    const float* Data() const { return &m[0][0]; }

    static void Identity(Mat4x4& out)
    {
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                out.m[i][j] = (i == j) ? 1.0f : 0.0f;
    }

    static Mat4x4 Multiply(const Mat4x4& a, const Mat4x4& b)
    {
        Mat4x4 r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                r.m[i][j] = 0.0f;
                for (int k = 0; k < 4; k++)
                    r.m[i][j] += a.m[i][k] * b.m[k][j];
            }
        return r;
    }

    Mat4x4 operator*(const Mat4x4& other) const { return Multiply(*this, other); }

    static Mat4x4 Translation(float x, float y, float z)
    {
        Mat4x4 r;
        r.m[3][0] = x;
        r.m[3][1] = y;
        r.m[3][2] = z;
        return r;
    }

    static Mat4x4 Translation(const Vec3f& v) { return Translation(v.x, v.y, v.z); }

    static Mat4x4 Scaling(float x, float y, float z)
    {
        Mat4x4 r;
        r.m[0][0] = x;
        r.m[1][1] = y;
        r.m[2][2] = z;
        return r;
    }

    static Mat4x4 Scaling(float uniform) { return Scaling(uniform, uniform, uniform); }

    static Mat4x4 RotationX(float radians)
    {
        float c = std::cos(radians), s = std::sin(radians);
        Mat4x4 r;
        r.m[1][1] = c;  r.m[1][2] = s;
        r.m[2][1] = -s; r.m[2][2] = c;
        return r;
    }

    static Mat4x4 RotationY(float radians)
    {
        float c = std::cos(radians), s = std::sin(radians);
        Mat4x4 r;
        r.m[0][0] = c;  r.m[0][2] = -s;
        r.m[2][0] = s;  r.m[2][2] = c;
        return r;
    }

    static Mat4x4 RotationZ(float radians)
    {
        float c = std::cos(radians), s = std::sin(radians);
        Mat4x4 r;
        r.m[0][0] = c;  r.m[0][1] = s;
        r.m[1][0] = -s; r.m[1][1] = c;
        return r;
    }

    static Mat4x4 FromQuaternion(const Quatf& q)
    {
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

        Mat4x4 r;
        r.m[0][0] = 1.0f - 2.0f * (yy + zz);
        r.m[0][1] = 2.0f * (xy + wz);
        r.m[0][2] = 2.0f * (xz - wy);

        r.m[1][0] = 2.0f * (xy - wz);
        r.m[1][1] = 1.0f - 2.0f * (xx + zz);
        r.m[1][2] = 2.0f * (yz + wx);

        r.m[2][0] = 2.0f * (xz + wy);
        r.m[2][1] = 2.0f * (yz - wx);
        r.m[2][2] = 1.0f - 2.0f * (xx + yy);
        return r;
    }

    // LookAtLH: eye position, target point, up vector
    // Left-handed view matrix (consistent with PerspectiveFovLH)
    // Camera looks down +Z in view space. D3D convention.
    static Mat4x4 LookAt(const Vec3f& eye, const Vec3f& target, const Vec3f& up)
    {
        Vec3f zAxis = (target - eye).Normalized();       // LH: +Z points toward target
        Vec3f xAxis = up.Cross(zAxis).Normalized();
        Vec3f yAxis = zAxis.Cross(xAxis);

        Mat4x4 r;
        r.m[0][0] = xAxis.x; r.m[0][1] = yAxis.x; r.m[0][2] = zAxis.x;
        r.m[1][0] = xAxis.y; r.m[1][1] = yAxis.y; r.m[1][2] = zAxis.y;
        r.m[2][0] = xAxis.z; r.m[2][1] = yAxis.z; r.m[2][2] = zAxis.z;
        r.m[3][0] = -xAxis.Dot(eye);
        r.m[3][1] = -yAxis.Dot(eye);
        r.m[3][2] = -zAxis.Dot(eye);
        return r;
    }

    // Perspective projection: fovY in radians, aspectRatio = width/height
    // D3D convention: Z maps to [0, 1] (not [-1, 1])
    static Mat4x4 PerspectiveFovLH(float fovY, float aspect, float nearZ, float farZ)
    {
        float h = 1.0f / std::tan(fovY * 0.5f);
        float w = h / aspect;
        float q = farZ / (farZ - nearZ);

        Mat4x4 r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r.m[i][j] = 0.0f;

        r.m[0][0] = w;
        r.m[1][1] = h;
        r.m[2][2] = q;
        r.m[2][3] = 1.0f;
        r.m[3][2] = -nearZ * q;
        return r;
    }

    // REVERSED-Z perspective with an INFINITE far plane, LH, Z in [0,1].
    // Maps nearZ -> 1 and z_view -> +inf -> 0. Pairing float depth (D32_FLOAT)
    // with reversed-Z gives near-constant relative precision (dz/z ~ 2^-23), so a
    // single frustum spans a huge near/far ratio without z-fighting — the depth
    // strategy for true-scale astronomical rendering. There is NO far plane, so
    // nothing is far-clipped; the near plane is the sole precision lever.
    // Derivation: take the finite reversed form (near/far swapped) and let far->inf,
    // which sends m[2][2] -> 0 and m[3][2] -> nearZ. Then clip.z = nearZ,
    // clip.w = z_view, so z_ndc = nearZ / z_view.
    static Mat4x4 PerspectiveFovLH_ReverseZ(float fovY, float aspect, float nearZ)
    {
        float h = 1.0f / std::tan(fovY * 0.5f);
        float w = h / aspect;

        Mat4x4 r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r.m[i][j] = 0.0f;

        r.m[0][0] = w;
        r.m[1][1] = h;
        r.m[2][2] = 0.0f;
        r.m[2][3] = 1.0f;
        r.m[3][2] = nearZ;
        return r;
    }

    // REVERSED-Z perspective with a FINITE far plane, LH, Z in [0,1]. Maps
    // nearZ -> 1 and farZ -> 0 (the standard build with near/far swapped in q).
    // Provided for passes that genuinely need a bounded far; the infinite variant
    // above is the default.
    static Mat4x4 PerspectiveFovLH_ReverseZFinite(float fovY, float aspect,
                                                  float nearZ, float farZ)
    {
        float h = 1.0f / std::tan(fovY * 0.5f);
        float w = h / aspect;
        float q = nearZ / (nearZ - farZ); // swapped vs forward-Z's far/(far-near)

        Mat4x4 r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r.m[i][j] = 0.0f;

        r.m[0][0] = w;
        r.m[1][1] = h;
        r.m[2][2] = q;
        r.m[2][3] = 1.0f;
        r.m[3][2] = -farZ * q;
        return r;
    }

    // Orthographic projection
    static Mat4x4 OrthoLH(float width, float height, float nearZ, float farZ)
    {
        Mat4x4 r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r.m[i][j] = 0.0f;

        r.m[0][0] = 2.0f / width;
        r.m[1][1] = 2.0f / height;
        r.m[2][2] = 1.0f / (farZ - nearZ);
        r.m[3][2] = -nearZ / (farZ - nearZ);
        r.m[3][3] = 1.0f;
        return r;
    }

    // Transform a Vec3f as a point (w=1)
    Vec3f TransformPoint(const Vec3f& v) const
    {
        float rx = v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0] + m[3][0];
        float ry = v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1] + m[3][1];
        float rz = v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2] + m[3][2];
        float rw = v.x * m[0][3] + v.y * m[1][3] + v.z * m[2][3] + m[3][3];
        if (std::abs(rw) > 1e-8f) { float inv = 1.0f / rw; rx *= inv; ry *= inv; rz *= inv; }
        return { rx, ry, rz };
    }

    // Transform a Vec3f as a direction (w=0, no translation)
    Vec3f TransformDirection(const Vec3f& v) const
    {
        return {
            v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0],
            v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1],
            v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2]
        };
    }

    // General 4x4 inverse via cofactor expansion. Returns identity and sets
    // outSingular if the determinant is degenerate, so callers cannot silently
    // propagate garbage.
    //
    // Needed for inverse-view-projection reconstruction. Its absence is why
    // path_trace.hlsl hardcoded a 70-degree FOV and why path_tracer.cpp uploads
    // viewProj under a member named for its inverse.
    static Mat4x4 Inverse(const Mat4x4& mat, bool* outSingular = nullptr)
    {
        const float* m = &mat.m[0][0];

        float a2323 = m[10] * m[15] - m[11] * m[14];
        float a1323 = m[9]  * m[15] - m[11] * m[13];
        float a1223 = m[9]  * m[14] - m[10] * m[13];
        float a0323 = m[8]  * m[15] - m[11] * m[12];
        float a0223 = m[8]  * m[14] - m[10] * m[12];
        float a0123 = m[8]  * m[13] - m[9]  * m[12];
        float a2313 = m[6]  * m[15] - m[7]  * m[14];
        float a1313 = m[5]  * m[15] - m[7]  * m[13];
        float a1213 = m[5]  * m[14] - m[6]  * m[13];
        float a2312 = m[6]  * m[11] - m[7]  * m[10];
        float a1312 = m[5]  * m[11] - m[7]  * m[9];
        float a1212 = m[5]  * m[10] - m[6]  * m[9];
        float a0313 = m[4]  * m[15] - m[7]  * m[12];
        float a0213 = m[4]  * m[14] - m[6]  * m[12];
        float a0312 = m[4]  * m[11] - m[7]  * m[8];
        float a0212 = m[4]  * m[10] - m[6]  * m[8];
        float a0113 = m[4]  * m[13] - m[5]  * m[12];
        float a0112 = m[4]  * m[9]  - m[5]  * m[8];

        float det = m[0] * (m[5] * a2323 - m[6] * a1323 + m[7] * a1223)
                  - m[1] * (m[4] * a2323 - m[6] * a0323 + m[7] * a0223)
                  + m[2] * (m[4] * a1323 - m[5] * a0323 + m[7] * a0123)
                  - m[3] * (m[4] * a1223 - m[5] * a0223 + m[6] * a0123);

        if (std::fabs(det) < 1e-20f)
        {
            if (outSingular) *outSingular = true;
            return Mat4x4();
        }
        if (outSingular) *outSingular = false;

        float invDet = 1.0f / det;
        Mat4x4 r;
        float* o = &r.m[0][0];

        o[0]  =  invDet * (m[5] * a2323 - m[6] * a1323 + m[7] * a1223);
        o[1]  = -invDet * (m[1] * a2323 - m[2] * a1323 + m[3] * a1223);
        o[2]  =  invDet * (m[1] * a2313 - m[2] * a1313 + m[3] * a1213);
        o[3]  = -invDet * (m[1] * a2312 - m[2] * a1312 + m[3] * a1212);
        o[4]  = -invDet * (m[4] * a2323 - m[6] * a0323 + m[7] * a0223);
        o[5]  =  invDet * (m[0] * a2323 - m[2] * a0323 + m[3] * a0223);
        o[6]  = -invDet * (m[0] * a2313 - m[2] * a0313 + m[3] * a0213);
        o[7]  =  invDet * (m[0] * a2312 - m[2] * a0312 + m[3] * a0212);
        o[8]  =  invDet * (m[4] * a1323 - m[5] * a0323 + m[7] * a0123);
        o[9]  = -invDet * (m[0] * a1323 - m[1] * a0323 + m[3] * a0123);
        o[10] =  invDet * (m[0] * a1313 - m[1] * a0313 + m[3] * a0113);
        o[11] = -invDet * (m[0] * a1312 - m[1] * a0312 + m[3] * a0112);
        o[12] = -invDet * (m[4] * a1223 - m[5] * a0223 + m[6] * a0123);
        o[13] =  invDet * (m[0] * a1223 - m[1] * a0223 + m[2] * a0123);
        o[14] = -invDet * (m[0] * a1213 - m[1] * a0213 + m[2] * a0113);
        o[15] =  invDet * (m[0] * a1212 - m[1] * a0212 + m[2] * a0112);

        return r;
    }

    // Inverse-transpose of the upper-left 3x3 for correct normal transformation.
    // Handles non-uniform scale correctly. Returns a 4x4 with the result in the
    // upper-left 3x3 and identity elsewhere (safe for shader upload as mat4).
    static Mat4x4 InverseTranspose3x3(const Mat4x4& mat)
    {
        // Extract 3x3
        float a00 = mat.m[0][0], a01 = mat.m[0][1], a02 = mat.m[0][2];
        float a10 = mat.m[1][0], a11 = mat.m[1][1], a12 = mat.m[1][2];
        float a20 = mat.m[2][0], a21 = mat.m[2][1], a22 = mat.m[2][2];

        // Cofactors of the 3x3
        float c00 = a11 * a22 - a12 * a21;
        float c01 = a12 * a20 - a10 * a22;
        float c02 = a10 * a21 - a11 * a20;
        float c10 = a02 * a21 - a01 * a22;
        float c11 = a00 * a22 - a02 * a20;
        float c12 = a01 * a20 - a00 * a21;
        float c20 = a01 * a12 - a02 * a11;
        float c21 = a02 * a10 - a00 * a12;
        float c22 = a00 * a11 - a01 * a10;

        // Determinant
        float det = a00 * c00 + a01 * c01 + a02 * c02;
        if (std::abs(det) < 1e-12f)
        {
            // Singular matrix — return identity as safe fallback
            Mat4x4 identity;
            return identity;
        }

        float invDet = 1.0f / det;

        // The cofactor matrix IS the inverse-transpose (up to scale by 1/det).
        // Cofactor matrix rows become columns of the inverse-transpose.
        Mat4x4 r;
        r.m[0][0] = c00 * invDet; r.m[0][1] = c01 * invDet; r.m[0][2] = c02 * invDet;
        r.m[1][0] = c10 * invDet; r.m[1][1] = c11 * invDet; r.m[1][2] = c12 * invDet;
        r.m[2][0] = c20 * invDet; r.m[2][1] = c21 * invDet; r.m[2][2] = c22 * invDet;
        // Row 3 and column 3 remain identity from default constructor
        return r;
    }
};

// =============================================================================
// Constants
// =============================================================================
constexpr float PI = 3.14159265358979323846f;
constexpr float TWO_PI = 6.28318530717958647692f;
constexpr float HALF_PI = 1.57079632679489661923f;
constexpr float DEG_TO_RAD = PI / 180.0f;
constexpr float RAD_TO_DEG = 180.0f / PI;
constexpr float EPSILON = 1e-6f;

// =============================================================================
// Utility functions
// =============================================================================
inline float Clamp(float v, float lo, float hi) { return (std::max)(lo, (std::min)(hi, v)); }
inline float Saturate(float v) { return Clamp(v, 0.0f, 1.0f); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float Smoothstep(float edge0, float edge1, float x)
{
    float t = Saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

} // namespace core
