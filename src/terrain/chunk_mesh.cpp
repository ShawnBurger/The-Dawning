// =============================================================================
// terrain/chunk_mesh.cpp — see chunk_mesh.h
// =============================================================================

#include "chunk_mesh.h"

#include "core/planet_height.h"

namespace terrain
{
namespace
{

core::Vec3d CrossD(const core::Vec3d& a, const core::Vec3d& b)
{
    return { a.y * b.z - a.z * b.y,
             a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x };
}

core::Vec3f ToF(const core::Vec3d& v)
{
    return { static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z) };
}

double Lerp(double a, double b, double t) { return a + (b - a) * t; }

} // namespace

double SampleHeightMeters(const core::Vec3d& dir, const ChunkParams& p)
{
    // Narrow the unit direction to float for the height eval so the CPU matches the
    // GPU shader (which runs the same float32 field). The direction magnitude is 1,
    // so this narrowing is exact to float precision and costs no world-scale error.
    const core::Vec3f d = ToF(dir);
    const float h = core::PlanetHeight(d, p.type, p.seed, p.seaLevel, p.coastWidth);
    return p.amplitudeMeters * static_cast<double>(h);
}

core::Vec3d SurfacePoint(const core::Vec3d& dir, const ChunkParams& p)
{
    const double r = p.planetRadius + SampleHeightMeters(dir, p);
    return { dir.x * r, dir.y * r, dir.z * r };
}

ChunkMesh GenerateChunk(const ChunkParams& p)
{
    ChunkMesh mesh;
    const int N = (p.gridN < 2) ? 2 : p.gridN;

    // Chunk origin: the surface point at the patch centre, in double. Every vertex
    // is stored relative to this, so the narrowed floats are chunk-sized, not
    // planet-sized (the precision crux — see chunk_mesh.h).
    const double uc = 0.5 * (p.u0 + p.u1);
    const double vc = 0.5 * (p.v0 + p.v1);
    mesh.origin = SurfacePoint(FaceToDirection(p.face, uc, vc), p);

    // A gradient step ~ one grid cell, for the finite-difference surface normal.
    const double du = (p.u1 - p.u0) / (N - 1);
    const double dv = (p.v1 - p.v0) / (N - 1);

    mesh.vertices.resize(static_cast<size_t>(N) * N);
    for (int j = 0; j < N; ++j)
    {
        const double v = Lerp(p.v0, p.v1, static_cast<double>(j) / (N - 1));
        for (int i = 0; i < N; ++i)
        {
            const double u = Lerp(p.u0, p.u1, static_cast<double>(i) / (N - 1));
            const core::Vec3d dir = FaceToDirection(p.face, u, v);
            const core::Vec3d wp  = SurfacePoint(dir, p);

            // Surface gradient from two nearby directions (works at edges too, and
            // captures the displacement slope, not just the grid facet).
            const core::Vec3d pu = SurfacePoint(FaceToDirection(p.face, u + du, v), p);
            const core::Vec3d pv = SurfacePoint(FaceToDirection(p.face, u, v + dv), p);
            core::Vec3d n = CrossD(pu - wp, pv - wp);
            // Orient outward (dot with the radial direction).
            if (n.Dot(dir) < 0.0) n = { -n.x, -n.y, -n.z };
            const double nlen = n.Length();
            const core::Vec3f normal = (nlen > 0.0)
                ? core::Vec3f{ static_cast<float>(n.x / nlen),
                               static_cast<float>(n.y / nlen),
                               static_cast<float>(n.z / nlen) }
                : ToF(dir);

            ChunkVertex vert;
            vert.position = ToF(wp - mesh.origin); // small residual, full float precision
            vert.normal   = normal;
            vert.uv       = { static_cast<float>(i) / (N - 1),
                              static_cast<float>(j) / (N - 1) };
            mesh.vertices[static_cast<size_t>(j) * N + i] = vert;
        }
    }

    // Two triangles per quad, CW winding for the LH / CW-front convention.
    mesh.indices.reserve(static_cast<size_t>(N - 1) * (N - 1) * 6);
    for (int j = 0; j < N - 1; ++j)
    {
        for (int i = 0; i < N - 1; ++i)
        {
            const uint16_t tl = static_cast<uint16_t>(j * N + i);
            const uint16_t tr = static_cast<uint16_t>(j * N + i + 1);
            const uint16_t bl = static_cast<uint16_t>((j + 1) * N + i);
            const uint16_t br = static_cast<uint16_t>((j + 1) * N + i + 1);
            mesh.indices.push_back(tl);
            mesh.indices.push_back(br);
            mesh.indices.push_back(bl);
            mesh.indices.push_back(tl);
            mesh.indices.push_back(tr);
            mesh.indices.push_back(br);
        }
    }

    return mesh;
}

} // namespace terrain
