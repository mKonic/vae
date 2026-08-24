#pragma once

#include "vae/gpu/Device.h"

#include <string_view>

namespace vae::gpu {

    // Loads a SPIR-V module produced by scripts/CompileShaders.sh. `name` is the source file name
    // ("triangle.vert"). The engine's own modules are compiled into the library, so an app built
    // against VAE needs nothing on disk to draw; anything else is resolved against the engine root.
    Ref<Shader> LoadShader(Device& device, std::string_view name, ShaderStage stage);

}
