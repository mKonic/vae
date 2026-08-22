#pragma once

// Precompiled header shared by BOTH engine libraries. It must stay free of Vulkan, GLFW and
// ImGui headers — VAE-Core is built without those include dirs and would fail to compile the PCH.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "vae/base/Base.h"
#include "vae/base/Log.h"
#include "vae/base/Assert.h"
