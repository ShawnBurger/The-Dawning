#include "asset/cooked_collision.h"

#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: TheDawningCollisionInspector <asset.tdcollision>\n";
        return 2;
    }
    const asset::CookedCollisionResult result =
        asset::LoadCookedCollisionFile(argv[1]);
    if (!result.Succeeded())
    {
        std::cerr << asset::CookedCollisionStatusName(result.status) << ": "
                  << result.error << '\n';
        return 1;
    }
    const asset::CookedCollision& collision = *result.collision;
    size_t walkable = 0;
    for (const asset::CookedCollisionBox& box : collision.boxes)
    {
        if (asset::HasCollisionSurfaceFlag(
                box.surfaceFlags, asset::CollisionSurfaceFlags::Walkable))
            ++walkable;
    }
    std::cout << "collision_id=" << collision.collisionId << '\n'
              << "boxes=" << collision.boxes.size() << '\n'
              << "walkable=" << walkable << '\n'
              << "payload_sha256=" << collision.payloadSha256.Hex() << '\n';
    return 0;
}
