#include "vaepch.h"
#include "vae/gpu/Device.h"

#include "vae/base/Platform.h"

#include <cstdlib>
#include <filesystem>

// The CPU rendering path.
//
// It is NOT a hand-written rasterizer, and deliberately so: Mesa's lavapipe is a software Vulkan
// ICD driven by llvmpipe's multi-threaded LLVM JIT, and it beats the alternatives while costing us
// no code at all. So "software" means "the Vulkan backend, forced onto the CPU ICD" — the whole
// renderer is exercised, on the same code path that runs on a GPU, with no second implementation to
// keep in sync. A hand-written rasterizer only becomes worth writing if VAE ever has to ship
// somewhere with no Vulkan loader at all.

namespace vae::gpu::detail {

    Scope<Device> CreateVulkanDevice(const DeviceDesc&);

    namespace {
        // Where distributions keep ICD manifests. The lavapipe file is named lvp_icd.json on some
        // distributions and lvp_icd.<arch>.json on others, so match the prefix rather than guessing
        // a full filename.
        constexpr const char* kIcdDirs[] = {
            "/usr/share/vulkan/icd.d",
            "/usr/local/share/vulkan/icd.d",
        };

        std::string FindLavapipeIcd() {
            std::error_code ec;
            for (const char* dir : kIcdDirs) {
                if (!std::filesystem::is_directory(dir, ec)) continue;
                for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                    const std::string name = entry.path().filename().string();
                    if (name.starts_with("lvp_icd") && name.ends_with(".json"))
                        return entry.path().string();
                }
            }
            return {};
        }
    }

    Scope<Device> CreateSoftwareDevice(const DeviceDesc& desc) {
        const std::string icdPath = FindLavapipeIcd();
        const char* icd = icdPath.empty() ? nullptr : icdPath.c_str();

        if (!icd) {
            VAE_CORE_ERROR("software rendering needs Mesa's lavapipe ICD, which is not installed "
                           "(Arch: pacman -S vulkan-swrast)");
            return nullptr;
        }

        // Both variables, because which one the loader honours depends on its version:
        // VK_LOADER_DRIVERS_SELECT is the modern filter, VK_ICD_FILENAMES the older override.
        // Must be set before the loader initialises, i.e. before vkCreateInstance below.
        platform::SetEnv("VK_LOADER_DRIVERS_SELECT", "lvp_icd*");
        platform::SetEnv("VK_ICD_FILENAMES", icd);
        VAE_CORE_INFO("software backend: routing Vulkan through lavapipe ({})", icd);

        auto device = CreateVulkanDevice(desc);
        if (device && !device->Caps().softwareRasterizer)
            VAE_CORE_WARN("asked for software rendering but got '{}', which reports as a hardware "
                          "device — the ICD filter did not take", device->Caps().deviceName);
        return device;
    }

}
