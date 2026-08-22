#include "vaepch.h"
#include "platform/vulkan/VulkanDevice.h"

#include "platform/vulkan/VulkanCommandList.h"
#include "platform/vulkan/VulkanResources.h"
#include "platform/vulkan/VulkanSwapchain.h"
#include "vae/core/Window.h"

#include <VkBootstrap.h>
#include <GLFW/glfw3.h>

namespace vae::gpu {

    namespace {

        VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT,
            const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                VAE_CORE_ERROR("[vulkan] {}", data->pMessage);
            else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                VAE_CORE_WARN("[vulkan] {}", data->pMessage);
            return VK_FALSE;
        }

    }

    VulkanDevice::VulkanDevice(const DeviceDesc& desc) : m_Window(desc.window) {
        if (!InitVulkan(desc))  return;
        if (!InitAllocator())   return;
        if (!InitFrames())      return;

        if (m_Window) {
            m_Swapchain = CreateScope<VulkanSwapchain>(*this, m_Surface,
                                                       m_Window->Width(), m_Window->Height(), desc.vsync);
            if (!m_Swapchain->Valid()) { VAE_CORE_ERROR("swapchain creation failed"); return; }
        }

        m_CommandList = CreateScope<VulkanCommandList>(*this);
        m_Ok = true;
    }

    VulkanDevice::~VulkanDevice() {
        if (m_Device) vkDeviceWaitIdle(m_Device);
        m_Swapchain.reset();
        DestroyFrames();
        if (m_ImmediateFence) vkDestroyFence(m_Device, m_ImmediateFence, nullptr);
        if (m_ImmediatePool)  vkDestroyCommandPool(m_Device, m_ImmediatePool, nullptr);
        if (m_DescriptorPool) vkDestroyDescriptorPool(m_Device, m_DescriptorPool, nullptr);
        if (m_Allocator) vmaDestroyAllocator(m_Allocator);
        if (m_Device)    vkDestroyDevice(m_Device, nullptr);
        if (m_Surface)   vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
        if (m_Messenger) vkb::destroy_debug_utils_messenger(m_Instance, m_Messenger);
        if (m_Instance)  vkDestroyInstance(m_Instance, nullptr);
        if (Device::Current() == this) Device::SetCurrent(nullptr);
    }

    bool VulkanDevice::InitVulkan(const DeviceDesc& desc) {
#ifdef VAE_DIST
        m_Validation = false;
#else
        m_Validation = desc.enableValidation;
#endif
        vkb::InstanceBuilder builder;
        builder.set_app_name(desc.appName.c_str())
               .set_engine_name("VAE")
               .require_api_version(1, 3, 0);

        if (m_Validation)
            builder.request_validation_layers(true).set_debug_callback(DebugCallback);

        auto result = builder.build();
        if (!result) {
            // Fall back once without validation: a box with no validation layers installed is a
            // normal machine, not a broken one, and it should still run.
            if (m_Validation) {
                VAE_CORE_WARN("instance creation with validation failed ({}); retrying without",
                              result.error().message());
                m_Validation = false;
                vkb::InstanceBuilder plain;
                result = plain.set_app_name(desc.appName.c_str())
                              .set_engine_name("VAE")
                              .require_api_version(1, 3, 0)
                              .build();
            }
            if (!result) {
                VAE_CORE_ERROR("vulkan instance creation failed: {}", result.error().message());
                return false;
            }
        }

        vkb::Instance vkbInstance = result.value();
        m_Instance  = vkbInstance.instance;
        m_Messenger = vkbInstance.debug_messenger;

        if (m_Window) {
            auto* glfwWindow = static_cast<GLFWwindow*>(m_Window->NativeHandle());
            if (glfwCreateWindowSurface(m_Instance, glfwWindow, nullptr, &m_Surface) != VK_SUCCESS) {
                VAE_CORE_ERROR("failed to create a Vulkan surface for the window");
                return false;
            }
        }

        // 1.3 dynamic rendering + synchronization2: no VkRenderPass and no VkFramebuffer objects
        // anywhere in this backend.
        VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        f13.dynamicRendering = VK_TRUE;
        f13.synchronization2 = VK_TRUE;

        // Descriptor indexing is what lets one batch address many textures (the glyph atlas pages
        // and image fills) from a single draw.
        VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        f12.descriptorIndexing = VK_TRUE;
        f12.runtimeDescriptorArray = VK_TRUE;
        f12.descriptorBindingPartiallyBound = VK_TRUE;
        f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

        vkb::PhysicalDeviceSelector selector{ vkbInstance };
        selector.set_minimum_version(1, 3)
                .set_required_features_13(f13)
                .set_required_features_12(f12);

        if (m_Surface) selector.set_surface(m_Surface);
        else           selector.defer_surface_initialization();

        auto physical = selector.select();
        if (!physical) {
            VAE_CORE_ERROR("no Vulkan 1.3 device with dynamic rendering: {}", physical.error().message());
            return false;
        }

        vkb::DeviceBuilder deviceBuilder{ physical.value() };
        auto built = deviceBuilder.build();
        if (!built) {
            VAE_CORE_ERROR("logical device creation failed: {}", built.error().message());
            return false;
        }

        m_Physical       = physical.value().physical_device;
        m_Device         = built.value().device;
        m_GraphicsQueue  = built.value().get_queue(vkb::QueueType::graphics).value();
        m_GraphicsFamily = built.value().get_queue_index(vkb::QueueType::graphics).value();

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_Physical, &props);
        m_Caps.deviceName = props.deviceName;
        m_Caps.discrete   = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        m_Caps.softwareRasterizer = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        m_Caps.maxTextureSize = props.limits.maxImageDimension2D;
        m_Caps.maxSamplersPerBatch = std::min(props.limits.maxPerStageDescriptorSampledImages, 1024u);
        m_Caps.driverInfo = "Vulkan " + std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "."
                          + std::to_string(VK_API_VERSION_MINOR(props.apiVersion));

        VAE_CORE_INFO("GPU: {} ({}{}), {}", m_Caps.deviceName,
                      m_Caps.discrete ? "discrete" : (m_Caps.softwareRasterizer ? "CPU" : "integrated"),
                      m_Validation ? ", validation on" : "", m_Caps.driverInfo);
        return true;
    }

    bool VulkanDevice::InitAllocator() {
        // VMA is compiled with dynamic function loading (vendor-build/src/vk_mem_alloc_impl.cpp),
        // so it needs the two proc-address entry points handed to it explicitly.
        VmaVulkanFunctions fns{};
        fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        fns.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

        VmaAllocatorCreateInfo info{};
        info.physicalDevice   = m_Physical;
        info.device           = m_Device;
        info.instance         = m_Instance;
        info.vulkanApiVersion = VK_API_VERSION_1_3;
        info.pVulkanFunctions = &fns;
        VK_CHECK(vmaCreateAllocator(&info, &m_Allocator));

        const VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         64 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         64 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 },
        };
        VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool.maxSets = 256;
        pool.poolSizeCount = static_cast<u32>(std::size(sizes));
        pool.pPoolSizes = sizes;
        VK_CHECK(vkCreateDescriptorPool(m_Device, &pool, nullptr, &m_DescriptorPool));
        return true;
    }

    bool VulkanDevice::InitFrames() {
        m_Frames.resize(m_Caps.framesInFlight);
        for (auto& frame : m_Frames) {
            VkCommandPoolCreateInfo pool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = m_GraphicsFamily;
            VK_CHECK(vkCreateCommandPool(m_Device, &pool, nullptr, &frame.pool));

            VkCommandBufferAllocateInfo alloc{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            alloc.commandPool = frame.pool;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = 1;
            VK_CHECK(vkAllocateCommandBuffers(m_Device, &alloc, &frame.cmd));

            VkSemaphoreCreateInfo sem{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            VK_CHECK(vkCreateSemaphore(m_Device, &sem, nullptr, &frame.imageAvailable));

            VkFenceCreateInfo fence{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // first frame must not block forever
            VK_CHECK(vkCreateFence(m_Device, &fence, nullptr, &frame.inFlight));
        }

        VkCommandPoolCreateInfo pool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = m_GraphicsFamily;
        VK_CHECK(vkCreateCommandPool(m_Device, &pool, nullptr, &m_ImmediatePool));

        VkFenceCreateInfo fence{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VK_CHECK(vkCreateFence(m_Device, &fence, nullptr, &m_ImmediateFence));
        return true;
    }

    void VulkanDevice::DestroyFrames() {
        for (auto& frame : m_Frames) {
            if (frame.inFlight)       vkDestroyFence(m_Device, frame.inFlight, nullptr);
            if (frame.imageAvailable) vkDestroySemaphore(m_Device, frame.imageAvailable, nullptr);
            if (frame.pool)           vkDestroyCommandPool(m_Device, frame.pool, nullptr);
        }
        m_Frames.clear();
    }

    Swapchain* VulkanDevice::GetSwapchain() { return m_Swapchain.get(); }

    Ref<Buffer>  VulkanDevice::CreateBuffer(const BufferDesc& d)  { return CreateRef<VulkanBuffer>(*this, d); }
    Ref<Texture> VulkanDevice::CreateTexture(const TextureDesc& d){ return CreateRef<VulkanTexture>(*this, d); }
    Ref<Shader>  VulkanDevice::CreateShader(const ShaderDesc& d)  { return CreateRef<VulkanShader>(*this, d); }
    Ref<Pipeline> VulkanDevice::CreatePipeline(const PipelineDesc& d) { return CreateRef<VulkanPipeline>(*this, d); }

    Ref<BindGroup> VulkanDevice::CreateBindGroup(const Ref<Pipeline>& p,
                                                 const std::vector<BindGroupEntry>& e) {
        return CreateRef<VulkanBindGroup>(*this, p, e);
    }

    Ref<RenderTarget> VulkanDevice::CreateRenderTarget(const RenderTargetDesc& d) {
        return CreateRef<VulkanRenderTarget>(*this, d);
    }

    VkCommandBuffer VulkanDevice::CurrentCommandBuffer() const { return m_Frames[m_FrameIndex].cmd; }

    void VulkanDevice::ImmediateSubmit(const std::function<void(VkCommandBuffer)>& fn) {
        VkCommandBufferAllocateInfo alloc{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc.commandPool = m_ImmediatePool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(m_Device, &alloc, &cmd));

        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
        fn(cmd);
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdInfo.commandBuffer = cmd;
        VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;

        VK_CHECK(vkQueueSubmit2(m_GraphicsQueue, 1, &submit, m_ImmediateFence));
        VK_CHECK(vkWaitForFences(m_Device, 1, &m_ImmediateFence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(m_Device, 1, &m_ImmediateFence));
        vkFreeCommandBuffers(m_Device, m_ImmediatePool, 1, &cmd);
    }

    CommandList* VulkanDevice::BeginFrame() {
        if (!m_Ok) return nullptr;
        if (m_Swapchain && !m_Swapchain->Valid()) return nullptr;   // minimized

        Frame& frame = m_Frames[m_FrameIndex];
        VK_CHECK(vkWaitForFences(m_Device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));

        if (m_Swapchain) {
            const VkResult acquired = vkAcquireNextImageKHR(m_Device, m_Swapchain->Raw(), UINT64_MAX,
                                                            frame.imageAvailable, VK_NULL_HANDLE,
                                                            &m_ImageIndex);
            if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
                m_Swapchain->Resize(m_Window->Width(), m_Window->Height());
                return nullptr;                     // skip this frame; the next one uses the new chain
            }
            if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
                VAE_CORE_ERROR("vkAcquireNextImageKHR: {}", VkResultName(acquired));
                return nullptr;
            }
        }

        VK_CHECK(vkResetFences(m_Device, 1, &frame.inFlight));
        VK_CHECK(vkResetCommandBuffer(frame.cmd, 0));

        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(frame.cmd, &begin));

        m_CommandList->Begin(frame.cmd, m_ImageIndex);
        m_FrameOpen = true;
        return m_CommandList.get();
    }

    void VulkanDevice::EndFrame() {
        if (!m_FrameOpen) return;
        m_FrameOpen = false;

        Frame& frame = m_Frames[m_FrameIndex];

        // A frame that drew nothing into the swapchain still has to leave the image presentable,
        // or the present validates as a layout mismatch.
        if (m_Swapchain) {
            const VkImageLayout from = m_CommandList->TouchedSwapchain()
                                     ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                     : VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            b.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstStageMask  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
            b.oldLayout = from;
            b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = m_Swapchain->Image(m_ImageIndex);
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(frame.cmd, &dep);
        }

        VK_CHECK(vkEndCommandBuffer(frame.cmd));
        m_CommandList->End();

        VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdInfo.commandBuffer = frame.cmd;

        VkSemaphoreSubmitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        wait.semaphore = frame.imageAvailable;
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        if (m_Swapchain) signal.semaphore = m_Swapchain->RenderFinished(m_ImageIndex);
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

        VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        if (m_Swapchain) {
            submit.waitSemaphoreInfoCount = 1;
            submit.pWaitSemaphoreInfos = &wait;
            submit.signalSemaphoreInfoCount = 1;
            submit.pSignalSemaphoreInfos = &signal;
        }
        VK_CHECK(vkQueueSubmit2(m_GraphicsQueue, 1, &submit, frame.inFlight));

        if (m_Swapchain) {
            VkSwapchainKHR chain = m_Swapchain->Raw();
            VkSemaphore renderFinished = m_Swapchain->RenderFinished(m_ImageIndex);
            VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &renderFinished;
            present.swapchainCount = 1;
            present.pSwapchains = &chain;
            present.pImageIndices = &m_ImageIndex;

            const VkResult presented = vkQueuePresentKHR(m_GraphicsQueue, &present);
            if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
                m_Swapchain->Resize(m_Window->Width(), m_Window->Height());
            else if (presented != VK_SUCCESS)
                VAE_CORE_ERROR("vkQueuePresentKHR: {}", VkResultName(presented));
        }

        m_FrameIndex = (m_FrameIndex + 1) % static_cast<u32>(m_Frames.size());
    }

    void VulkanDevice::WaitIdle() { if (m_Device) vkDeviceWaitIdle(m_Device); }

    void VulkanDevice::OnWindowResize(u32 width, u32 height) {
        if (m_Swapchain) m_Swapchain->Resize(width, height);
    }

    namespace detail {
        Scope<Device> CreateVulkanDevice(const DeviceDesc& desc) {
            auto device = CreateScope<VulkanDevice>(desc);
            if (!device->Ok()) return nullptr;
            return device;
        }
    }

}
