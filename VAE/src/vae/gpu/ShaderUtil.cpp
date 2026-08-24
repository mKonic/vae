#include "vaepch.h"
#include "vae/gpu/ShaderUtil.h"

#include "vae/base/FileSystem.h"

#include <vae/Shaders.gen.h>

namespace vae::gpu {

    namespace fs = std::filesystem;

    namespace {

        // The engine's own modules, compiled into it by scripts/EmbedShaders.sh. This is what
        // makes an app standalone: a binary built against VAE carries its pipelines, so it does
        // not have to find the engine's checkout at run time to draw a rectangle.
        std::optional<std::vector<u32>> Embedded(std::string_view name) {
            for (const embedded::Module& module : embedded::kModules) {
                if (module.name != name) continue;
                std::vector<u32> words(module.size / 4);
                std::memcpy(words.data(), module.bytes, module.size);
                return words;
            }
            return std::nullopt;
        }

        // A module nobody embedded — a shader an app added of its own. Still resolved against the
        // engine root, which an app that ships extra shaders is by definition able to provide.
        std::optional<std::vector<u32>> FromDisk(std::string_view name) {
            const fs::path cache = FileSystem::Asset("VAE/assets/shaders/cache");
            if (cache.empty()) {
                VAE_CORE_ERROR("shader '{}' is not built into the engine, and this build has no "
                               "engine root to look in", name);
                return std::nullopt;
            }
            const fs::path path = cache / (std::string(name) + ".spv");
            auto bytes = FileSystem::ReadBinary(path);
            if (!bytes) {
                VAE_CORE_ERROR("shader '{}' is not built into the engine and is not at {} — "
                               "run scripts/CompileShaders.sh", name, path.string());
                return std::nullopt;
            }
            if (bytes->size() % 4 != 0) {
                VAE_CORE_ERROR("shader '{}' is {} bytes, which is not a whole number of SPIR-V words",
                               name, bytes->size());
                return std::nullopt;
            }
            std::vector<u32> words(bytes->size() / 4);
            std::memcpy(words.data(), bytes->data(), bytes->size());
            return words;
        }

    }

    Ref<Shader> LoadShader(Device& device, std::string_view name, ShaderStage stage) {
        auto words = Embedded(name);
        if (!words) words = FromDisk(name);
        if (!words) return nullptr;

        ShaderDesc desc;
        desc.spirv = std::move(*words);
        desc.stage = stage;
        desc.debugName = std::string(name);
        return device.CreateShader(desc);
    }

}
