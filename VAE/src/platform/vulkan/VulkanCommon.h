#pragma once

#include "vae/gpu/Types.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace vae::gpu {

    const char* VkResultName(VkResult r);

    // Every Vulkan call that can fail goes through this. A silent VK_ERROR_DEVICE_LOST that only
    // shows up three frames later as garbage is the worst class of bug in this layer.
    #define VK_CHECK(expr)                                                                     \
        do {                                                                                    \
            const VkResult _vkr = (expr);                                                       \
            if (_vkr != VK_SUCCESS) {                                                           \
                VAE_CORE_ERROR("{} failed: {} @ {}:{}", #expr, ::vae::gpu::VkResultName(_vkr),  \
                               __FILE__, __LINE__);                                             \
                VAE_CORE_ASSERT(false, "vulkan call failed");                                   \
            }                                                                                   \
        } while (0)

    VkFormat            ToVk(Format f);
    Format              FromVk(VkFormat f);
    VkFilter            ToVk(Filter f);
    VkSamplerAddressMode ToVk(AddressMode m);
    VkPrimitiveTopology ToVk(PrimitiveTopology t);
    VkShaderStageFlagBits ToVk(ShaderStage s);
    VkShaderStageFlags  ToVkStageFlags(u8 stageBits);
    VkDescriptorType    ToVk(BindingType t);

    // Fills the blend state for a colour attachment. AlphaStraight is the UI default; premultiplied
    // is what an offscreen layer composites with, because straight alpha double-darkens faded edges.
    VkPipelineColorBlendAttachmentState BlendState(BlendMode mode);

}
