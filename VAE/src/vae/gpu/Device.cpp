#include "vaepch.h"
#include "vae/gpu/Device.h"

#include <cstdlib>

namespace vae::gpu {

    namespace detail {
        Scope<Device> CreateVulkanDevice(const DeviceDesc&);
        Scope<Device> CreateD3D12Device(const DeviceDesc&);
        Scope<Device> CreateSoftwareDevice(const DeviceDesc&);
    }

    namespace { Device* s_Current = nullptr; }

    Device* Device::Current() { return s_Current; }
    void Device::SetCurrent(Device* d) { s_Current = d; }

    const char* BackendName(Backend b) {
        switch (b) {
            case Backend::Vulkan:   return "vulkan";
            case Backend::D3D12:    return "d3d12";
            case Backend::Software: return "software";
        }
        return "unknown";
    }

    Backend ParseBackend(std::string_view name) {
        if (name == "vulkan")   return Backend::Vulkan;
        if (name == "d3d12")    return Backend::D3D12;
        if (name == "software") return Backend::Software;
        VAE_CORE_WARN("unknown graphics backend '{}' — using vulkan", name);
        return Backend::Vulkan;
    }

    Scope<Device> CreateDevice(Backend backend, const DeviceDesc& desc) {
        switch (backend) {
            case Backend::Vulkan:   return detail::CreateVulkanDevice(desc);
            case Backend::D3D12:    return detail::CreateD3D12Device(desc);
            case Backend::Software: return detail::CreateSoftwareDevice(desc);
        }
        return nullptr;
    }

    Scope<Device> CreateDeviceWithFallback(const DeviceDesc& desc, std::optional<Backend> requested) {
        Backend backend = Backend::Vulkan;
        bool explicitlyRequested = false;

        if (requested) { backend = *requested; explicitlyRequested = true; }
        else if (const char* env = std::getenv("VAE_GPU")) {
            backend = ParseBackend(env);
            explicitlyRequested = true;
        }

        if (auto device = CreateDevice(backend, desc)) {
            Device::SetCurrent(device.get());
            return device;
        }

        if (backend == Backend::Vulkan) {
            VAE_CORE_ERROR("no usable graphics backend");
            return nullptr;
        }

        // The refusal was already logged with its reason; say plainly what we are doing instead
        // rather than silently coming up on a different backend than was asked for.
        if (explicitlyRequested)
            VAE_CORE_WARN("'{}' is unavailable — falling back to vulkan", BackendName(backend));

        auto device = CreateDevice(Backend::Vulkan, desc);
        Device::SetCurrent(device.get());
        return device;
    }

    u32 FormatSize(Format f) {
        switch (f) {
            case Format::R8_UNORM:       return 1;
            case Format::RG8_UNORM:      return 2;
            case Format::RGBA8_UNORM:
            case Format::RGBA8_SRGB:
            case Format::BGRA8_UNORM:
            case Format::BGRA8_SRGB:     return 4;
            case Format::R16_SFLOAT:     return 2;
            case Format::RG16_SFLOAT:    return 4;
            case Format::RGBA16_SFLOAT:  return 8;
            case Format::R32_SFLOAT:
            case Format::R32_UINT:
            case Format::D32_SFLOAT:     return 4;
            case Format::RG32_SFLOAT:
            case Format::RG32_UINT:      return 8;
            case Format::RGB32_SFLOAT:   return 12;
            case Format::RGBA32_SFLOAT:
            case Format::RGBA32_UINT:    return 16;
            case Format::Undefined:      return 0;
        }
        return 0;
    }

    bool IsDepthFormat(Format f) { return f == Format::D32_SFLOAT; }

}
