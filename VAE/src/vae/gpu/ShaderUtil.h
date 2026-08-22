#pragma once

#include "vae/gpu/Device.h"

#include <string_view>

namespace vae::gpu {

    // Loads a SPIR-V module produced by scripts/CompileShaders.sh. `name` is the source file name
    // ("triangle.vert"), resolved against the engine root so it works from any working directory.
    Ref<Shader> LoadShader(Device& device, std::string_view name, ShaderStage stage);

}
