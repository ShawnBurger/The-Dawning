// =============================================================================
// terrain/cube_sphere.cpp — see cube_sphere.h
// =============================================================================

#include "cube_sphere.h"

#include <cmath>

namespace terrain
{
namespace
{

core::Vec3d Normalize(const core::Vec3d& p)
{
    double len = p.Length();
    if (len <= 0.0) return { 0.0, 0.0, 1.0 };
    double inv = 1.0 / len;
    return { p.x * inv, p.y * inv, p.z * inv };
}

// Equiangular (tangent) warp of a face coordinate a in [-1,1]: tan(a * pi/4).
// Maps the linear cube grid onto near-uniform arc steps on the sphere.
double Warp(double a)
{
    return std::tan(a * (core::PI * 0.25));
}

} // namespace

core::Vec3d FaceAxis(CubeFace face)
{
    switch (face)
    {
        case CubeFace::PosX: return {  1.0,  0.0,  0.0 };
        case CubeFace::NegX: return { -1.0,  0.0,  0.0 };
        case CubeFace::PosY: return {  0.0,  1.0,  0.0 };
        case CubeFace::NegY: return {  0.0, -1.0,  0.0 };
        case CubeFace::PosZ: return {  0.0,  0.0,  1.0 };
        default:             return {  0.0,  0.0, -1.0 }; // NegZ
    }
}

core::Vec3d FaceToDirection(CubeFace face, double u, double v)
{
    const double a = Warp(u);
    const double b = Warp(v);

    core::Vec3d p;
    switch (face)
    {
        case CubeFace::PosX: p = {  1.0,    b,   -a }; break;
        case CubeFace::NegX: p = { -1.0,    b,    a }; break;
        case CubeFace::PosY: p = {    a,  1.0,   -b }; break;
        case CubeFace::NegY: p = {    a, -1.0,    b }; break;
        case CubeFace::PosZ: p = {    a,    b,  1.0 }; break;
        default:             p = {   -a,    b, -1.0 }; break; // NegZ
    }
    return Normalize(p);
}

} // namespace terrain
