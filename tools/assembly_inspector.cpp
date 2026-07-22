#include "asset/cooked_assembly.h"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: TheDawningAssemblyInspector <asset.tdassembly>\n";
        return 2;
    }

    const std::filesystem::path path = argv[1];
    const asset::CookedAssemblyResult loaded = asset::LoadCookedAssemblyFile(path);
    if (!loaded.Succeeded())
    {
        std::cerr << "load failed ["
                  << asset::CookedAssemblyStatusName(loaded.status)
                  << "]: " << loaded.error << '\n';
        return 1;
    }

    const asset::CookedAssembly& assembly = *loaded.assembly;
    std::cout << "asset_id=" << assembly.assetId << '\n';
    std::cout << "schema_version=" << assembly.schemaVersion << '\n';
    std::cout << "source_manifest_sha256="
              << assembly.sourceManifestSha256.Hex() << '\n';
    std::cout << "provenance=" << assembly.provenance.size() << '\n';
    std::cout << "modules=" << assembly.modules.size() << '\n';
    std::cout << "sockets=" << assembly.sockets.size() << '\n';
    std::cout << "zones=" << assembly.zones.size() << '\n';
    std::cout << "portals=" << assembly.portals.size() << '\n';
    std::cout << "interactions=" << assembly.interactions.size() << '\n';
    std::cout << "moving_parts=" << assembly.movingParts.size() << '\n';
    std::cout << "light_fixtures=" << assembly.lightFixtures.size() << '\n';
    std::cout << "entry_zone=" << assembly.zones[assembly.entryZone].id << '\n';
    return 0;
}
