// =============================================================================
// tests/test_terrain_chunk.cpp — cube-sphere mapping + chunk mesh generation
// =============================================================================
// CPU geometry for chunked-LOD terrain: the tangent-warp cube-sphere mapping and
// one displaced patch. The load-bearing test is the PRECISION one — that a
// per-chunk double origin keeps float positions cm-accurate at Earth radius where
// a naive full-magnitude narrow loses ~1 m.
// =============================================================================

#include "test_framework.h"
#include "terrain/cube_sphere.h"
#include "terrain/chunk_mesh.h"
#include "core/planet_height.h"

#include <cmath>

namespace
{

const terrain::CubeFace kFaces[6] = {
    terrain::CubeFace::PosX, terrain::CubeFace::NegX,
    terrain::CubeFace::PosY, terrain::CubeFace::NegY,
    terrain::CubeFace::PosZ, terrain::CubeFace::NegZ,
};

double Len(const core::Vec3d& v) { return v.Length(); }

} // namespace

TEST_CASE(CubeSphere_DirectionsAreUnit)
{
    for (auto face : kFaces)
        for (int i = 0; i <= 8; ++i)
            for (int j = 0; j <= 8; ++j)
            {
                double u = -1.0 + 2.0 * i / 8.0;
                double v = -1.0 + 2.0 * j / 8.0;
                core::Vec3d d = terrain::FaceToDirection(face, u, v);
                CHECK_APPROX_EPS(Len(d), 1.0, 1e-9);
            }
}

TEST_CASE(CubeSphere_FaceCentresAreAxes)
{
    for (auto face : kFaces)
    {
        core::Vec3d c = terrain::FaceToDirection(face, 0.0, 0.0);
        core::Vec3d a = terrain::FaceAxis(face);
        CHECK_APPROX_EPS(c.x, a.x, 1e-9);
        CHECK_APPROX_EPS(c.y, a.y, 1e-9);
        CHECK_APPROX_EPS(c.z, a.z, 1e-9);
    }
}

TEST_CASE(CubeSphere_FacesCoverAllSixAxisBuckets)
{
    // Each face centre must fall in a distinct (axis, sign) bucket — otherwise the
    // 6 faces would not tile the whole sphere.
    bool bucket[6] = { false, false, false, false, false, false };
    for (auto face : kFaces)
    {
        core::Vec3d a = terrain::FaceAxis(face);
        int axis = (std::fabs(a.x) > std::fabs(a.y) && std::fabs(a.x) > std::fabs(a.z)) ? 0
                 : (std::fabs(a.y) > std::fabs(a.z)) ? 1 : 2;
        double s = (axis == 0) ? a.x : (axis == 1) ? a.y : a.z;
        bucket[axis * 2 + (s > 0 ? 0 : 1)] = true;
    }
    for (int b = 0; b < 6; ++b) CHECK(bucket[b]);
}

TEST_CASE(ChunkMesh_VertexAndIndexCounts)
{
    terrain::ChunkParams p;
    p.gridN = 33;
    terrain::ChunkMesh m = terrain::GenerateChunk(p);
    CHECK_EQ(m.vertices.size(), static_cast<size_t>(33 * 33));
    CHECK_EQ(m.indices.size(),  static_cast<size_t>(32 * 32 * 6));
}

TEST_CASE(ChunkMesh_GridSizeIsClampedToIndexRange)
{
    terrain::ChunkParams tooSmall;
    tooSmall.gridN = -100;
    tooSmall.amplitudeMeters = 0.0;
    terrain::ChunkMesh small = terrain::GenerateChunk(tooSmall);
    CHECK_EQ(small.vertices.size(), static_cast<size_t>(4));
    CHECK_EQ(small.indices.size(), static_cast<size_t>(6));

    terrain::ChunkParams tooLarge;
    tooLarge.gridN = terrain::kMaxChunkGridN + 1;
    tooLarge.amplitudeMeters = 0.0;
    terrain::ChunkMesh large = terrain::GenerateChunk(tooLarge);
    const size_t maxN = static_cast<size_t>(terrain::kMaxChunkGridN);
    CHECK_EQ(large.vertices.size(), maxN * maxN);
    CHECK_EQ(large.indices.size(), (maxN - 1) * (maxN - 1) * 6);

    uint16_t largestIndex = 0;
    for (uint16_t index : large.indices)
        largestIndex = (std::max)(largestIndex, index);
    CHECK_EQ(largestIndex, UINT16_MAX);
}

TEST_CASE(ChunkMesh_DisplacementMatchesSharedHeightField)
{
    // Every vertex's radial distance from the planet centre must equal
    // planetRadius + amplitude*PlanetHeight(dir) — i.e. the mesh displaces by the
    // exact field the shader tints. Recompute the direction per vertex and check.
    terrain::ChunkParams p;
    p.face = terrain::CubeFace::PosZ;
    p.u0 = 0.10; p.u1 = 0.20; p.v0 = -0.05; p.v1 = 0.05;
    p.gridN = 17;
    terrain::ChunkMesh m = terrain::GenerateChunk(p);

    const int N = p.gridN;
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i)
        {
            double u = p.u0 + (p.u1 - p.u0) * i / (N - 1);
            double v = p.v0 + (p.v1 - p.v0) * j / (N - 1);
            core::Vec3d dir = terrain::FaceToDirection(p.face, u, v);
            double expectedR = p.planetRadius + terrain::SampleHeightMeters(dir, p);

            // Recover the world position from origin + local residual.
            const core::Vec3f& lp = m.vertices[static_cast<size_t>(j) * N + i].position;
            core::Vec3d world{ m.origin.x + lp.x, m.origin.y + lp.y, m.origin.z + lp.z };
            CHECK_APPROX_EPS(Len(world), expectedR, 1.0); // within 1 m over a 6.4e6 m radius
        }
}

TEST_CASE(ChunkMesh_DoubleOriginKeepsCentimetrePrecision)
{
    // THE precision crux. On a small (~1 km) chunk at Earth radius, recovering a
    // vertex world position from the double origin + narrowed float residual is
    // cm-accurate, whereas narrowing the full ~6.4e6 m position to float loses ~1 m.
    terrain::ChunkParams p;
    p.face = terrain::CubeFace::PosZ;
    p.u0 = 0.20000; p.u1 = 0.20016;   // ~1 km patch
    p.v0 = 0.10000; p.v1 = 0.10016;
    p.gridN = 9;
    terrain::ChunkMesh m = terrain::GenerateChunk(p);

    const int N = p.gridN;
    double worstDouble = 0.0, worstNaive = 0.0;
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i)
        {
            double u = p.u0 + (p.u1 - p.u0) * i / (N - 1);
            double v = p.v0 + (p.v1 - p.v0) * j / (N - 1);
            core::Vec3d dir = terrain::FaceToDirection(p.face, u, v);
            core::Vec3d trueWp = terrain::SurfacePoint(dir, p); // full-precision double

            // Double-origin path (what GenerateChunk does).
            const core::Vec3f& lp = m.vertices[static_cast<size_t>(j) * N + i].position;
            core::Vec3d recovered{ m.origin.x + lp.x, m.origin.y + lp.y, m.origin.z + lp.z };
            worstDouble = (std::max)(worstDouble, Len(recovered - trueWp));

            // Naive path: narrow the full world position to float and back.
            core::Vec3d naive{ static_cast<double>(static_cast<float>(trueWp.x)),
                               static_cast<double>(static_cast<float>(trueWp.y)),
                               static_cast<double>(static_cast<float>(trueWp.z)) };
            worstNaive = (std::max)(worstNaive, Len(naive - trueWp));
        }

    CHECK(worstDouble < 0.01);  // per-chunk origin: sub-centimetre
    CHECK(worstNaive  > 0.1);   // naive full-magnitude float: decimetres-to-metres
}

TEST_CASE(ChunkMesh_ResidualIsChunkSizedNotPlanetSized)
{
    // The stored float positions are chunk-local, so their magnitude is bounded by
    // the chunk extent (km here), never the planet radius (~6.4e6 m).
    terrain::ChunkParams p;
    p.face = terrain::CubeFace::PosZ;
    p.u0 = 0.20; p.u1 = 0.22; p.v0 = 0.10; p.v1 = 0.12; // ~128 km patch
    p.gridN = 17;
    terrain::ChunkMesh m = terrain::GenerateChunk(p);
    for (const auto& vtx : m.vertices)
    {
        float mag = vtx.position.Length();
        CHECK(mag < 2.0e5f); // < 200 km, i.e. chunk-sized, not 6.4e6 m
    }
}

TEST_CASE(ChunkMesh_NormalsAreUnitAndOutward)
{
    terrain::ChunkParams p;
    p.face = terrain::CubeFace::PosZ;
    p.u0 = 0.10; p.u1 = 0.20; p.v0 = -0.05; p.v1 = 0.05;
    p.gridN = 17;
    terrain::ChunkMesh m = terrain::GenerateChunk(p);

    const int N = p.gridN;
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i)
        {
            double u = p.u0 + (p.u1 - p.u0) * i / (N - 1);
            double v = p.v0 + (p.v1 - p.v0) * j / (N - 1);
            core::Vec3d dir = terrain::FaceToDirection(p.face, u, v);
            const core::Vec3f& n = m.vertices[static_cast<size_t>(j) * N + i].normal;
            CHECK_APPROX_EPS(n.Length(), 1.0f, 1e-3f);
            // Outward: positive dot with the radial direction (terrain slopes are
            // shallow relative to the planet, so this always holds).
            float radialDot = n.x * static_cast<float>(dir.x)
                            + n.y * static_cast<float>(dir.y)
                            + n.z * static_cast<float>(dir.z);
            CHECK(radialDot > 0.0f);
        }
}
