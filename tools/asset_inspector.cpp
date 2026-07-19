#include "asset/gltf_importer.h"

#include <filesystem>
#include <iomanip>
#include <iostream>

namespace
{

void PrintBounds(const asset::Bounds3f& bounds)
{
    if (!bounds.valid)
    {
        std::cout << "invalid";
        return;
    }
    std::cout << '[' << bounds.min.x << ", " << bounds.min.y << ", " << bounds.min.z
              << "]..[" << bounds.max.x << ", " << bounds.max.y << ", "
              << bounds.max.z << ']';
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: TheDawningAssetInspector <model.gltf|model.glb>\n";
        return 2;
    }

    const std::filesystem::path path(argv[1]);
    const asset::GltfImportResult result = asset::ImportGltfFile(path);
    if (!result.Succeeded())
    {
        std::cerr << "import failed [" << asset::GltfImportStatusName(result.status)
                  << "]: " << result.error << '\n';
        return 1;
    }

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "asset=" << result.model.name << '\n';
    std::cout << "primitives=" << result.model.primitives.size() << '\n';
    std::cout << "vertices=" << result.model.VertexCount() << '\n';
    std::cout << "indices=" << result.model.IndexCount() << '\n';
    std::cout << "triangles=" << result.model.IndexCount() / 3 << '\n';
    std::cout << "materials=" << result.model.materials.size() << '\n';
    std::cout << "textures=" << result.model.textures.size() << '\n';
    std::cout << "images=" << result.model.images.size() << '\n';
    std::cout << "samplers=" << result.model.samplers.size() << '\n';
    std::cout << "bounds=";
    PrintBounds(result.model.bounds);
    std::cout << '\n';
    for (const std::string& warning : result.warnings)
        std::cout << "warning=" << warning << '\n';
    return 0;
}
