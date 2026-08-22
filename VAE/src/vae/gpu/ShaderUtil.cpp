#include "vaepch.h"
#include "vae/gpu/ShaderUtil.h"

#include "vae/base/FileSystem.h"

namespace vae::gpu {

    Ref<Shader> LoadShader(Device& device, std::string_view name, ShaderStage stage) {
        const auto path = FileSystem::Asset("VAE/assets/shaders/cache") / (std::string(name) + ".spv");
        auto bytes = FileSystem::ReadBinary(path);
        if (!bytes) {
            VAE_CORE_ERROR("shader '{}' not found at {} — run scripts/CompileShaders.sh",
                           name, path.string());
            return nullptr;
        }
        if (bytes->size() % 4 != 0) {
            VAE_CORE_ERROR("shader '{}' is {} bytes, which is not a whole number of SPIR-V words",
                           name, bytes->size());
            return nullptr;
        }

        ShaderDesc desc;
        desc.spirv.resize(bytes->size() / 4);
        std::memcpy(desc.spirv.data(), bytes->data(), bytes->size());
        desc.stage = stage;
        desc.debugName = std::string(name);
        return device.CreateShader(desc);
    }

}
