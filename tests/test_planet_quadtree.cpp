// =============================================================================
// tests/test_planet_quadtree.cpp — camera-adaptive LOD leaf selection
// =============================================================================
// The LOD "brain": subdivide toward the camera by screen-space error. Tests the
// load-bearing properties — deeper near the camera, coarse when far, the leaves
// tile every face with no gaps/overlaps, bounded, deterministic, capped by level.
// =============================================================================

#include "test_framework.h"
#include "terrain/planet_quadtree.h"
#include "terrain/cube_sphere.h"

#include <cmath>

namespace
{
terrain::QuadtreeConfig MoonCfg()
{
    terrain::QuadtreeConfig c;
    c.planetRadius = 1.7374e6;
    c.amplitude    = 8000.0;
    return c;
}
double Len(const core::Vec3d& v) { return v.Length(); }
} // namespace

TEST_CASE(Quadtree_GeometricErrorDecreasesWithLevel)
{
    terrain::QuadtreeConfig c = MoonCfg();
    for (int L = 0; L < 14; ++L)
        CHECK(terrain::NodeGeometricError(c, L + 1) < terrain::NodeGeometricError(c, L));
}

TEST_CASE(Quadtree_VeryFarCameraGivesOneLeafPerFace)
{
    terrain::QuadtreeConfig c = MoonCfg();
    // 10000 planet radii out: even the level-0 face error is sub-pixel.
    core::Vec3d cam{ 0.0, 0.0, 10000.0 * c.planetRadius };
    std::vector<terrain::QuadPatch> leaves;
    terrain::SelectQuadtreeLOD(c, cam, leaves);
    CHECK_EQ(leaves.size(), static_cast<size_t>(6));
    for (const auto& p : leaves) CHECK_EQ(p.level, 0);
}

TEST_CASE(Quadtree_NearCameraSubdividesDeeperThanFarSide)
{
    terrain::QuadtreeConfig c = MoonCfg();
    // Camera 2 km above a point on the +Z face; the near side must reach a far
    // higher level than the antipodal (-Z) side.
    const core::Vec3d dir = terrain::FaceToDirection(terrain::CubeFace::PosZ, 0.2, 0.1);
    const core::Vec3d cam{ dir.x * (c.planetRadius + 2000.0),
                           dir.y * (c.planetRadius + 2000.0),
                           dir.z * (c.planetRadius + 2000.0) };
    std::vector<terrain::QuadPatch> leaves;
    terrain::SelectQuadtreeLOD(c, cam, leaves);

    int nearMax = 0, farMax = 0;
    for (const auto& p : leaves)
    {
        core::Vec3d cdir = terrain::FaceToDirection(p.face, 0.5 * (p.u0 + p.u1),
                                                    0.5 * (p.v0 + p.v1));
        double d = cdir.x * dir.x + cdir.y * dir.y + cdir.z * dir.z; // cos(angle)
        if (d > 0.99)  nearMax = (p.level > nearMax) ? p.level : nearMax;
        if (d < -0.5)  farMax  = (p.level > farMax)  ? p.level : farMax;
    }
    CHECK(nearMax >= 8);          // deep subdivision under the camera
    CHECK(nearMax > farMax + 3);  // and much deeper than the far side
}

TEST_CASE(Quadtree_LeavesTileEachFaceExactly)
{
    terrain::QuadtreeConfig c = MoonCfg();
    const core::Vec3d dir = terrain::FaceToDirection(terrain::CubeFace::PosZ, 0.2, 0.1);
    const core::Vec3d cam{ dir.x * (c.planetRadius + 50000.0),
                           dir.y * (c.planetRadius + 50000.0),
                           dir.z * (c.planetRadius + 50000.0) };
    std::vector<terrain::QuadPatch> leaves;
    terrain::SelectQuadtreeLOD(c, cam, leaves);

    // Each face's leaves must partition [-1,1]^2 (area 4) with no gaps or overlaps;
    // subdivision splits into 4 equal quarters, so the area sum is conserved.
    for (int f = 0; f < 6; ++f)
    {
        double area = 0.0;
        for (const auto& p : leaves)
            if (static_cast<int>(p.face) == f)
                area += (p.u1 - p.u0) * (p.v1 - p.v0);
        CHECK_APPROX_EPS(area, 4.0, 1e-9);
    }
}

TEST_CASE(Quadtree_DeterministicAndBounded)
{
    terrain::QuadtreeConfig c = MoonCfg();
    const core::Vec3d dir = terrain::FaceToDirection(terrain::CubeFace::PosZ, 0.2, 0.1);
    const core::Vec3d cam{ dir.x * (c.planetRadius + 8000.0),
                           dir.y * (c.planetRadius + 8000.0),
                           dir.z * (c.planetRadius + 8000.0) };
    std::vector<terrain::QuadPatch> a, b;
    terrain::SelectQuadtreeLOD(c, cam, a);
    terrain::SelectQuadtreeLOD(c, cam, b);
    CHECK_EQ(a.size(), b.size());
    bool same = (a.size() == b.size());
    for (size_t i = 0; same && i < a.size(); ++i)
        same = (a[i].face == b[i].face && a[i].level == b[i].level &&
                a[i].u0 == b[i].u0 && a[i].v0 == b[i].v0);
    CHECK(same);
    CHECK(a.size() <= static_cast<size_t>(c.maxLeaves));
    for (const auto& p : a) CHECK(p.level <= c.maxLevel);
}

TEST_CASE(Quadtree_LeafCapIsHardAndCoverageIsPreserved)
{
    terrain::QuadtreeConfig c = MoonCfg();
    c.pixelError = 0.0001;
    c.maxLevel = 20;
    c.maxLeaves = 31; // not every cap is reachable because a split adds 3 leaves

    std::vector<terrain::QuadPatch> leaves;
    terrain::SelectQuadtreeLOD(c, { 0.0, 0.0, c.planetRadius + 10.0 }, leaves);
    CHECK(leaves.size() <= static_cast<size_t>(c.maxLeaves));
    CHECK(leaves.size() >= static_cast<size_t>(terrain::kMinQuadtreeLeaves));

    for (int f = 0; f < 6; ++f)
    {
        double area = 0.0;
        for (const auto& p : leaves)
            if (static_cast<int>(p.face) == f)
                area += (p.u1 - p.u0) * (p.v1 - p.v0);
        CHECK_APPROX_EPS(area, 4.0, 1e-9);
    }
}

TEST_CASE(Quadtree_MalformedBoundsAreSanitized)
{
    terrain::QuadtreeConfig c = MoonCfg();
    c.maxLevel = 1000;
    c.maxLeaves = 1;
    c.pixelError = 0.0001;
    c.tanHalfFovY = 0.0;

    std::vector<terrain::QuadPatch> leaves;
    terrain::SelectQuadtreeLOD(c, { NAN, 0.0, 0.0 }, leaves);
    CHECK_EQ(leaves.size(), static_cast<size_t>(terrain::kMinQuadtreeLeaves));
    for (const auto& p : leaves)
        CHECK(p.level >= 0 && p.level <= terrain::kMaxQuadtreeLevel);

    CHECK(std::isfinite(terrain::NodeGeometricError(c, -1000)));
    CHECK(std::isfinite(terrain::NodeGeometricError(c, 1000)));

    c.maxLeaves = 4096;
    terrain::SelectQuadtreeLOD(c, { NAN, 0.0, 0.0 }, leaves);
    CHECK_EQ(leaves.size(), static_cast<size_t>(terrain::kMinQuadtreeLeaves));
}

TEST_CASE(Quadtree_ExtremeFiniteInputsRemainDeterministic)
{
    terrain::QuadtreeConfig c = MoonCfg();
    c.planetRadius = (std::numeric_limits<double>::max)();
    c.amplitude = (std::numeric_limits<double>::max)();
    c.viewportHeight = (std::numeric_limits<double>::max)();
    c.tanHalfFovY = (std::numeric_limits<double>::min)();
    c.maxLevel = 2;
    c.maxLeaves = 24;

    CHECK(std::isfinite(terrain::NodeGeometricError(c, 0)));
    std::vector<terrain::QuadPatch> first;
    std::vector<terrain::QuadPatch> second;
    terrain::SelectQuadtreeLOD(c, { 0.0, 0.0, 0.0 }, first);
    terrain::SelectQuadtreeLOD(c, { 0.0, 0.0, 0.0 }, second);
    CHECK_EQ(first.size(), second.size());
    CHECK(first.size() <= static_cast<size_t>(c.maxLeaves));
    for (size_t i = 0; i < first.size(); ++i)
    {
        CHECK_EQ(static_cast<int>(first[i].face), static_cast<int>(second[i].face));
        CHECK_EQ(first[i].level, second[i].level);
        CHECK(first[i].u0 == second[i].u0);
        CHECK(first[i].v0 == second[i].v0);
    }
}
