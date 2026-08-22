#include "vaepch.h"
#include "platform/vulkan/VulkanCommon.h"

namespace vae::gpu {

    const char* VkResultName(VkResult r) {
        switch (r) {
            case VK_SUCCESS:                        return "VK_SUCCESS";
            case VK_NOT_READY:                      return "VK_NOT_READY";
            case VK_TIMEOUT:                        return "VK_TIMEOUT";
            case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
            case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
            case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
            case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
            case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
            case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
            case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
            case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
            case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
            case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
            case VK_ERROR_UNKNOWN:                  return "VK_ERROR_UNKNOWN";
            default:                                return "VK_ERROR_<other>";
        }
    }

    VkFormat ToVk(Format f) {
        switch (f) {
            case Format::R8_UNORM:      return VK_FORMAT_R8_UNORM;
            case Format::RG8_UNORM:     return VK_FORMAT_R8G8_UNORM;
            case Format::RGBA8_UNORM:   return VK_FORMAT_R8G8B8A8_UNORM;
            case Format::RGBA8_SRGB:    return VK_FORMAT_R8G8B8A8_SRGB;
            case Format::BGRA8_UNORM:   return VK_FORMAT_B8G8R8A8_UNORM;
            case Format::BGRA8_SRGB:    return VK_FORMAT_B8G8R8A8_SRGB;
            case Format::R16_SFLOAT:    return VK_FORMAT_R16_SFLOAT;
            case Format::RG16_SFLOAT:   return VK_FORMAT_R16G16_SFLOAT;
            case Format::RGBA16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case Format::R32_SFLOAT:    return VK_FORMAT_R32_SFLOAT;
            case Format::RG32_SFLOAT:   return VK_FORMAT_R32G32_SFLOAT;
            case Format::RGB32_SFLOAT:  return VK_FORMAT_R32G32B32_SFLOAT;
            case Format::RGBA32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case Format::R32_UINT:      return VK_FORMAT_R32_UINT;
            case Format::RG32_UINT:     return VK_FORMAT_R32G32_UINT;
            case Format::RGBA32_UINT:   return VK_FORMAT_R32G32B32A32_UINT;
            case Format::D32_SFLOAT:    return VK_FORMAT_D32_SFLOAT;
            case Format::Undefined:     return VK_FORMAT_UNDEFINED;
        }
        return VK_FORMAT_UNDEFINED;
    }

    Format FromVk(VkFormat f) {
        switch (f) {
            case VK_FORMAT_R8_UNORM:             return Format::R8_UNORM;
            case VK_FORMAT_R8G8_UNORM:           return Format::RG8_UNORM;
            case VK_FORMAT_R8G8B8A8_UNORM:       return Format::RGBA8_UNORM;
            case VK_FORMAT_R8G8B8A8_SRGB:        return Format::RGBA8_SRGB;
            case VK_FORMAT_B8G8R8A8_UNORM:       return Format::BGRA8_UNORM;
            case VK_FORMAT_B8G8R8A8_SRGB:        return Format::BGRA8_SRGB;
            case VK_FORMAT_R16G16B16A16_SFLOAT:  return Format::RGBA16_SFLOAT;
            case VK_FORMAT_D32_SFLOAT:           return Format::D32_SFLOAT;
            default:                             return Format::Undefined;
        }
    }

    VkFilter ToVk(Filter f) { return f == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; }

    VkSamplerAddressMode ToVk(AddressMode m) {
        switch (m) {
            case AddressMode::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case AddressMode::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case AddressMode::ClampToBorder:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        }
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }

    VkPrimitiveTopology ToVk(PrimitiveTopology t) {
        switch (t) {
            case PrimitiveTopology::TriangleList:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case PrimitiveTopology::LineList:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        }
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }

    VkShaderStageFlagBits ToVk(ShaderStage s) {
        switch (s) {
            case ShaderStage::Vertex:   return VK_SHADER_STAGE_VERTEX_BIT;
            case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
            case ShaderStage::Compute:  return VK_SHADER_STAGE_COMPUTE_BIT;
        }
        return VK_SHADER_STAGE_VERTEX_BIT;
    }

    VkShaderStageFlags ToVkStageFlags(u8 stageBits) {
        VkShaderStageFlags flags = 0;
        if (stageBits & StageBit(ShaderStage::Vertex))   flags |= VK_SHADER_STAGE_VERTEX_BIT;
        if (stageBits & StageBit(ShaderStage::Fragment)) flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
        if (stageBits & StageBit(ShaderStage::Compute))  flags |= VK_SHADER_STAGE_COMPUTE_BIT;
        return flags ? flags : VK_SHADER_STAGE_ALL_GRAPHICS;
    }

    VkDescriptorType ToVk(BindingType t) {
        switch (t) {
            case BindingType::UniformBuffer:  return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case BindingType::StorageBuffer:  return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            case BindingType::SampledTexture: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }

    VkPipelineColorBlendAttachmentState BlendState(BlendMode mode) {
        VkPipelineColorBlendAttachmentState s{};
        s.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                         | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        switch (mode) {
            case BlendMode::None:
                s.blendEnable = VK_FALSE;
                break;
            case BlendMode::AlphaStraight:
                s.blendEnable = VK_TRUE;
                s.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                s.colorBlendOp        = VK_BLEND_OP_ADD;
                // Alpha accumulates so an offscreen layer ends up with correct coverage.
                s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                s.alphaBlendOp        = VK_BLEND_OP_ADD;
                break;
            case BlendMode::AlphaPremultiplied:
                s.blendEnable = VK_TRUE;
                s.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                s.colorBlendOp        = VK_BLEND_OP_ADD;
                s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                s.alphaBlendOp        = VK_BLEND_OP_ADD;
                break;
            case BlendMode::Additive:
                s.blendEnable = VK_TRUE;
                s.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                s.colorBlendOp        = VK_BLEND_OP_ADD;
                s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                s.alphaBlendOp        = VK_BLEND_OP_ADD;
                break;
        }
        return s;
    }

}
