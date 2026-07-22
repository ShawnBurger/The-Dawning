#pragma once
// =============================================================================
// render/terrain_height_probe.h — GPU value-agreement probe for the terrain
// height twin (core::PlanetHeight  vs  shaders/planet_noise.hlsli PlanetHeight)
// =============================================================================
// The chunked-LOD terrain mesh displaces on the CPU by core::PlanetHeight; the
// far shaded sphere tints itself with the GPU PlanetHeight in planet_noise.hlsli.
// They are the SAME function written twice, in two languages, and if they diverge
// by one hash constant the near terrain pops/shears against the far sphere as it
// fades in. This probe closes that drift gap the way ibl_sky_agreement closes it
// for the sky twin: it evaluates the SHIPPED GPU PlanetHeight for a fixed set of
// (direction, body-type) queries, reads the results back, and the CPU compares
// them against core::PlanetHeight fed the IDENTICAL inputs, within a measured
// tolerance. NOT a hash tripwire (a re-pinned hash pins agreement in TIME, not
// VALUE — see src/core/sky_radiance.h).
//
// Runs once at startup, in every mode, gated on nothing (before the raster-vs-RT
// branch exists), so it can never be armed on a frame or mode it does not reach —
// the recurring failure the shadow/draw/IBL probes were all rebuilt to remove. A
// failed verdict is FATAL: booting past it means shipping terrain whose near mesh
// is guaranteed to mismatch the far sphere, silently.
// =============================================================================

#include "core/types.h"

#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

namespace render
{

class D3D12Device;

class TerrainHeightProbe
{
public:
    // Create resources, PSO and root signature. No GPU work recorded. Non-fatal
    // failure here disables the probe (RunAndValidate then no-ops to success) so a
    // driver that cannot compile the probe shader does not brick the engine; the
    // smoke harness still asserts the marker's PRESENCE, so an absent probe is a
    // red run there.
    bool Init(ID3D12Device* device);

    // Record + execute the probe on its own command list, read back, compare vs
    // core::PlanetHeight, and log the [SMOKE] markers. Returns false only when the
    // probe ran and the twins DISAGREED (fatal), never when the probe was never
    // built. Idempotent: runs at most once.
    bool RunAndValidate(D3D12Device& device);

private:
    bool CreateRootSignature(ID3D12Device* device);
    bool CreatePipeline(ID3D12Device* device);
    bool CreateResources(ID3D12Device* device);
    void BuildQuerySet();      // fills the direction/param arrays (one generator)
    void UploadConstants();    // writes the query set into the upload CB verbatim
    void Record(D3D12Device& device);
    bool ReadbackAndValidate();

    // 4 body types x 16 directions. 16 dirs cover all 6 cube-face axis buckets
    // plus corners/edges; 4 types exercise every branch of PlanetHeight (Earth's
    // coastline mask, Mars/Moon craters, generic). 64 * 16 B = 1024, already a
    // legal readback footprint offset/pitch with no padding arithmetic.
    static constexpr uint32_t kDirs  = 16;
    static constexpr uint32_t kTypes = 4;
    static constexpr uint32_t kSlots = kDirs * kTypes; // 64

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_target;    // kSlots x 1 RGBA32F
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_readback;  // kSlots * 16 bytes
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_cb;        // upload CB, 2048 B

    // The one generator. The GPU gets these in the CB and the CPU compares against
    // the identical arrays — no second source of inputs.
    core::Vec3f m_dir[kSlots]      = {};
    int         m_type[kSlots]     = {};
    float       m_seed[kSlots]     = {};
    float       m_seaLevel[kSlots] = {};
    float       m_coastWidth[kSlots] = {};

    bool m_ready = false; // Init succeeded and the probe can run
    bool m_ran   = false; // RunAndValidate has already executed
};

} // namespace render
