#include "vaepch.h"
#include "platform/vulkan/VulkanResources.h"

#include "platform/vulkan/VulkanDevice.h"

namespace vae::gpu {

    namespace {

        VkBufferUsageFlags UsageFlags(BufferUsage u) {
            switch (u) {
                case BufferUsage::Vertex:  return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                case BufferUsage::Index:   return VK_BUFFER_USAGE_INDEX_BUFFER_BIT  | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                case BufferUsage::Uniform: return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT| VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                case BufferUsage::Storage: return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT| VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                case BufferUsage::Staging: return VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            }
            return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        }

    }

    // ---------------------------------------------------------------- Buffer

    VulkanBuffer::VulkanBuffer(VulkanDevice& device, const BufferDesc& desc)
        : m_Device(device), m_Desc(desc) {
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size  = desc.size;
        info.usage = UsageFlags(desc.usage);
        if (desc.memory == MemoryKind::DeviceLocal) info.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        if (desc.memory == MemoryKind::HostVisible) {
            // Persistently mapped and sequentially written: this is the per-frame instance buffer
            // path, and re-mapping it every frame would be pure overhead.
            alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                        | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }

        VmaAllocationInfo allocInfo{};
        VK_CHECK(vmaCreateBuffer(m_Device.Allocator(), &info, &alloc, &m_Buffer, &m_Allocation, &allocInfo));
        m_Mapped = allocInfo.pMappedData;
    }

    VulkanBuffer::~VulkanBuffer() {
        if (m_Buffer) vmaDestroyBuffer(m_Device.Allocator(), m_Buffer, m_Allocation);
    }

    void VulkanBuffer::Upload(const void* data, u64 size, u64 offset) {
        VAE_CORE_ASSERT(offset + size <= m_Desc.size, "buffer upload overruns the allocation");

        if (m_Mapped) {
            std::memcpy(static_cast<u8*>(m_Mapped) + offset, data, size);
            return;
        }

        // Device-local: stage and copy.
        BufferDesc stagingDesc{ size, BufferUsage::Staging, MemoryKind::HostVisible, "staging" };
        VulkanBuffer staging(m_Device, stagingDesc);
        std::memcpy(staging.Map(), data, size);

        m_Device.ImmediateSubmit([&](VkCommandBuffer cmd) {
            VkBufferCopy copy{ 0, offset, size };
            vkCmdCopyBuffer(cmd, staging.Raw(), m_Buffer, 1, &copy);
        });
    }

    // ---------------------------------------------------------------- Texture

    VulkanTexture::VulkanTexture(VulkanDevice& device, const TextureDesc& desc)
        : m_Device(device), m_Desc(desc) {
        const bool depth  = IsDepthFormat(desc.format);
        const bool target = desc.usage != TextureUsage::Sampled;

        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType   = VK_IMAGE_TYPE_2D;
        info.format      = ToVk(desc.format);
        info.extent      = { desc.width, desc.height, 1 };
        info.mipLevels   = 1;
        info.arrayLayers = 1;
        info.samples     = VK_SAMPLE_COUNT_1_BIT;
        info.tiling      = VK_IMAGE_TILING_OPTIMAL;
        info.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (target) info.usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VK_CHECK(vmaCreateImage(m_Device.Allocator(), &info, &alloc, &m_Image, &m_Allocation, nullptr));

        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image    = m_Image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format   = info.format;
        view.subresourceRange = { static_cast<VkImageAspectFlags>(depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                        : VK_IMAGE_ASPECT_COLOR_BIT),
                                  0, 1, 0, 1 };
        VK_CHECK(vkCreateImageView(m_Device.Raw(), &view, nullptr, &m_View));

        if (!depth) CreateSampler();
    }

    VulkanTexture::VulkanTexture(VulkanDevice& device, const TextureDesc& desc,
                                 VkImage image, VkImageView view)
        : m_Device(device), m_Desc(desc), m_Image(image), m_View(view), m_Owned(false) {
        if (!IsDepthFormat(desc.format)) CreateSampler();
    }

    VulkanTexture::~VulkanTexture() {
        if (m_Sampler) vkDestroySampler(m_Device.Raw(), m_Sampler, nullptr);
        if (!m_Owned) return;
        if (m_View)  vkDestroyImageView(m_Device.Raw(), m_View, nullptr);
        if (m_Image) vmaDestroyImage(m_Device.Allocator(), m_Image, m_Allocation);
    }

    void VulkanTexture::CreateSampler() {
        VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        info.magFilter    = ToVk(m_Desc.magFilter);
        info.minFilter    = ToVk(m_Desc.minFilter);
        info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = info.addressModeV = info.addressModeW = ToVk(m_Desc.addressMode);
        info.borderColor  = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        info.maxLod       = VK_LOD_CLAMP_NONE;
        VK_CHECK(vkCreateSampler(m_Device.Raw(), &info, nullptr, &m_Sampler));
    }

    void VulkanTexture::Upload(const void* pixels, u64 size) {
        UploadRegion(pixels, 0, 0, m_Desc.width, m_Desc.height);
        (void)size;
    }

    void VulkanTexture::UploadRegion(const void* pixels, u32 x, u32 y, u32 w, u32 h) {
        const u64 bytes = u64(w) * h * FormatSize(m_Desc.format);
        BufferDesc stagingDesc{ bytes, BufferUsage::Staging, MemoryKind::HostVisible, "tex-staging" };
        VulkanBuffer staging(m_Device, stagingDesc);
        std::memcpy(staging.Map(), pixels, bytes);

        m_Device.ImmediateSubmit([&](VkCommandBuffer cmd) {
            auto barrier = [&](VkImageLayout from, VkImageLayout to,
                               VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                               VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
                VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
                b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
                b.oldLayout = from; b.newLayout = to;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = m_Image;
                b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &b;
                vkCmdPipelineBarrier2(cmd, &dep);
            };

            barrier(m_Layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageOffset = { static_cast<i32>(x), static_cast<i32>(y), 0 };
            copy.imageExtent = { w, h, 1 };
            vkCmdCopyBufferToImage(cmd, staging.Raw(), m_Image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        });

        m_Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // ---------------------------------------------------------------- Shader

    VulkanShader::VulkanShader(VulkanDevice& device, const ShaderDesc& desc)
        : m_Device(device), m_Stage(desc.stage), m_EntryPoint(desc.entryPoint) {
        VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        info.codeSize = desc.spirv.size() * sizeof(u32);
        info.pCode    = desc.spirv.data();
        VK_CHECK(vkCreateShaderModule(m_Device.Raw(), &info, nullptr, &m_Module));
    }

    VulkanShader::~VulkanShader() {
        if (m_Module) vkDestroyShaderModule(m_Device.Raw(), m_Module, nullptr);
    }

    // ---------------------------------------------------------------- Pipeline

    VulkanPipeline::VulkanPipeline(VulkanDevice& device, const PipelineDesc& desc)
        : m_Device(device), m_Desc(desc) {

        if (!desc.bindings.empty()) {
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            bindings.reserve(desc.bindings.size());
            for (const auto& b : desc.bindings)
                bindings.push_back({ b.binding, ToVk(b.type), b.count, ToVkStageFlags(b.stages), nullptr });

            VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            info.bindingCount = static_cast<u32>(bindings.size());
            info.pBindings    = bindings.data();
            VK_CHECK(vkCreateDescriptorSetLayout(m_Device.Raw(), &info, nullptr, &m_SetLayout));
        }

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        push.size       = desc.pushConstantSize;

        VkPipelineLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout.setLayoutCount = m_SetLayout ? 1u : 0u;
        layout.pSetLayouts    = m_SetLayout ? &m_SetLayout : nullptr;
        layout.pushConstantRangeCount = desc.pushConstantSize ? 1u : 0u;
        layout.pPushConstantRanges    = desc.pushConstantSize ? &push : nullptr;
        VK_CHECK(vkCreatePipelineLayout(m_Device.Raw(), &layout, nullptr, &m_Layout));

        auto* vs = static_cast<VulkanShader*>(desc.vertex.get());
        auto* fs = static_cast<VulkanShader*>(desc.fragment.get());
        VAE_CORE_ASSERT(vs && fs, "a graphics pipeline needs both a vertex and a fragment shader");

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs->Module();
        stages[0].pName = vs->EntryPoint().c_str();
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs->Module();
        stages[1].pName = fs->EntryPoint().c_str();

        std::vector<VkVertexInputBindingDescription> vertexBindings;
        std::vector<VkVertexInputAttributeDescription> vertexAttrs;
        for (u32 i = 0; i < desc.vertexBuffers.size(); ++i) {
            const auto& vb = desc.vertexBuffers[i];
            vertexBindings.push_back({ i, vb.stride,
                                       vb.stepMode == VertexStepMode::Instance
                                           ? VK_VERTEX_INPUT_RATE_INSTANCE
                                           : VK_VERTEX_INPUT_RATE_VERTEX });
            for (const auto& a : vb.attributes)
                vertexAttrs.push_back({ a.location, i, ToVk(a.format), a.offset });
        }

        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount   = static_cast<u32>(vertexBindings.size());
        vertexInput.pVertexBindingDescriptions      = vertexBindings.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<u32>(vertexAttrs.size());
        vertexInput.pVertexAttributeDescriptions    = vertexAttrs.data();

        VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = ToVk(desc.topology);

        VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewport.viewportCount = 1;
        viewport.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_NONE;     // UI geometry is 2D and single-sided
        raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable  = desc.depthTest  ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

        const VkPipelineColorBlendAttachmentState blend = BlendState(desc.blend);
        VkPipelineColorBlendStateCreateInfo blendState{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blendState.attachmentCount = 1;
        blendState.pAttachments    = &blend;

        const VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates    = dynamics;

        // Dynamic rendering: no VkRenderPass, no VkFramebuffer. The attachment formats declared
        // here must match the target the pipeline is used against, or validation fires at draw time.
        const VkFormat colorFormat = ToVk(desc.colorFormat);
        VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount    = 1;
        rendering.pColorAttachmentFormats = &colorFormat;
        rendering.depthAttachmentFormat   = ToVk(desc.depthFormat);

        VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blendState;
        info.pDynamicState = &dynamic;
        info.layout = m_Layout;
        VK_CHECK(vkCreateGraphicsPipelines(m_Device.Raw(), VK_NULL_HANDLE, 1, &info, nullptr, &m_Pipeline));
    }

    VulkanPipeline::~VulkanPipeline() {
        if (m_Pipeline)  vkDestroyPipeline(m_Device.Raw(), m_Pipeline, nullptr);
        if (m_Layout)    vkDestroyPipelineLayout(m_Device.Raw(), m_Layout, nullptr);
        if (m_SetLayout) vkDestroyDescriptorSetLayout(m_Device.Raw(), m_SetLayout, nullptr);
    }

    // ---------------------------------------------------------------- BindGroup

    VulkanBindGroup::VulkanBindGroup(VulkanDevice& device, const Ref<Pipeline>& pipeline,
                                     const std::vector<BindGroupEntry>& entries)
        : m_Device(device) {
        auto* vkPipeline = static_cast<VulkanPipeline*>(pipeline.get());
        VAE_CORE_ASSERT(vkPipeline->HasBindings(), "pipeline declares no bindings");
        m_PipelineLayout = vkPipeline->Layout();

        VkDescriptorSetLayout layout = vkPipeline->SetLayout();
        VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc.descriptorPool     = m_Device.DescriptorPool();
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &layout;
        VK_CHECK(vkAllocateDescriptorSets(m_Device.Raw(), &alloc, &m_Set));

        std::vector<VkWriteDescriptorSet> writes;
        std::vector<VkDescriptorBufferInfo> bufferInfos;
        std::vector<std::vector<VkDescriptorImageInfo>> imageInfos;
        bufferInfos.reserve(entries.size());
        imageInfos.reserve(entries.size());

        for (const auto& e : entries) {
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = m_Set;
            w.dstBinding = e.binding;
            w.descriptorType = ToVk(e.type);

            if (e.type == BindingType::SampledTexture) {
                auto& infos = imageInfos.emplace_back();
                for (const auto& t : e.textures) {
                    auto* vt = static_cast<VulkanTexture*>(t.get());
                    infos.push_back({ vt->Sampler(), vt->View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
                }
                w.descriptorCount = static_cast<u32>(infos.size());
                w.pImageInfo = infos.data();
            } else {
                auto* vb = static_cast<VulkanBuffer*>(e.buffer.get());
                bufferInfos.push_back({ vb->Raw(), e.bufferOffset,
                                        e.bufferRange ? e.bufferRange : VK_WHOLE_SIZE });
                w.descriptorCount = 1;
                w.pBufferInfo = &bufferInfos.back();
            }
            writes.push_back(w);
        }

        vkUpdateDescriptorSets(m_Device.Raw(), static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    }

    VulkanBindGroup::~VulkanBindGroup() {
        // The pool is created with FREE_DESCRIPTOR_SET so bind groups can come and go as the
        // glyph atlas grows and its descriptor is rewritten.
        if (m_Set) vkFreeDescriptorSets(m_Device.Raw(), m_Device.DescriptorPool(), 1, &m_Set);
    }

    // ---------------------------------------------------------------- RenderTarget

    VulkanRenderTarget::VulkanRenderTarget(VulkanDevice& device, const RenderTargetDesc& desc)
        : m_Device(device), m_Desc(desc) { Build(); }

    VulkanRenderTarget::~VulkanRenderTarget() { Destroy(); }

    void VulkanRenderTarget::Build() {
        TextureDesc color{};
        color.width = m_Desc.width;
        color.height = m_Desc.height;
        color.format = m_Desc.colorFormat;
        color.usage = TextureUsage::RenderTarget;
        color.debugName = m_Desc.debugName;
        m_Color = CreateRef<VulkanTexture>(m_Device, color);

        if (m_Desc.depthFormat == Format::Undefined) return;

        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = ToVk(m_Desc.depthFormat);
        info.extent = { m_Desc.width, m_Desc.height, 1 };
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VK_CHECK(vmaCreateImage(m_Device.Allocator(), &info, &alloc, &m_DepthImage, &m_DepthAllocation, nullptr));

        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = m_DepthImage;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = info.format;
        view.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        VK_CHECK(vkCreateImageView(m_Device.Raw(), &view, nullptr, &m_DepthView));
    }

    void VulkanRenderTarget::Destroy() {
        m_Color.reset();
        if (m_DepthView)  vkDestroyImageView(m_Device.Raw(), m_DepthView, nullptr);
        if (m_DepthImage) vmaDestroyImage(m_Device.Allocator(), m_DepthImage, m_DepthAllocation);
        m_DepthView = VK_NULL_HANDLE;
        m_DepthImage = VK_NULL_HANDLE;
    }

    void VulkanRenderTarget::Resize(u32 width, u32 height) {
        if (width == m_Desc.width && height == m_Desc.height) return;
        if (width == 0 || height == 0) return;
        m_Device.WaitIdle();
        Destroy();
        m_Desc.width = width;
        m_Desc.height = height;
        Build();
    }

    VkImageView VulkanRenderTarget::ColorView() const { return m_Color->View(); }

}
