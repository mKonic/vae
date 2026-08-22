#include "vaepch.h"
#include "vae/gpu/Device.h"

// Scaffolding, per design/architecture.md §4: this file compiles on every build and the backend is
// selectable from day one, so landing a real D3D12 backend later is filling this file in rather than
// re-plumbing device selection. It refuses loudly instead of pretending.
//
// Note that D3D12 is not needed to ship on Windows — Vulkan runs there too. This exists for
// platform-native distribution, and only when that is actually wanted.

namespace vae::gpu::detail {

    Scope<Device> CreateD3D12Device(const DeviceDesc&) {
#ifdef VAE_PLATFORM_WINDOWS
        VAE_CORE_ERROR("the D3D12 backend is not implemented yet (platform/d3d12/D3D12Device.cpp)");
#else
        VAE_CORE_ERROR("the D3D12 backend does not exist on this platform; it is Windows-only");
#endif
        return nullptr;
    }

}
