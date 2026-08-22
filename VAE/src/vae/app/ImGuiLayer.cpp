#include "vaepch.h"
#include "vae/app/ImGuiLayer.h"

#include "vae/app/ImGuiTheme.h"
#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"
#include "vae/core/Application.h"
#include "platform/vulkan/VulkanDevice.h"
#include "platform/vulkan/VulkanResources.h"
#include "platform/vulkan/VulkanSwapchain.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include <GLFW/glfw3.h>

namespace vae::app {

    namespace {
        void CheckVk(VkResult result) {
            if (result != VK_SUCCESS) VAE_CORE_ERROR("imgui vulkan backend: VkResult {}", (int)result);
        }
    }

    ImGuiLayer::ImGuiLayer() : Layer("ImGui") {}
    ImGuiLayer::~ImGuiLayer() = default;

    void ImGuiLayer::OnAttach() {
        Application& app = Application::Get();
        if (!app.HasDevice()) {
            VAE_CORE_WARN("ImGui layer attached without a device — editor chrome is off");
            return;
        }
        auto* vk = dynamic_cast<gpu::VulkanDevice*>(&app.GetDevice());
        if (!vk) {
            VAE_CORE_WARN("ImGui needs the Vulkan backend; editor chrome is off on {}",
                          (int)app.GetDevice().GetBackend());
            return;
        }
        gpu::VulkanSwapchain* swapchain = vk->SwapchainImpl();
        if (!swapchain) {
            VAE_CORE_WARN("ImGui needs a swapchain — editor chrome is off");
            return;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        // Multi-viewport is deliberately off: it needs the backend to own swapchains, and a panel
        // torn out of the window buys nothing that docking does not already give.
        io.ConfigWindowsMoveFromTitleBarOnly = true;

        // Layout lives beside the project, not in the working directory a launcher happened to
        // start us from.
        static std::string iniPath = (FileSystem::EngineRoot() / "imgui.ini").string();
        io.IniFilename = iniPath.c_str();

        LoadFonts();
        ApplyTheme();

        auto* window = static_cast<GLFWwindow*>(app.GetWindow().NativeHandle());
        ImGui_ImplGlfw_InitForVulkan(window, true);

        const VkFormat colorFormat = swapchain->VkColorFormat();
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;

        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion     = VK_API_VERSION_1_3;
        info.Instance       = vk->Instance();
        info.PhysicalDevice = vk->Physical();
        info.Device         = vk->Raw();
        info.QueueFamily    = vk->GraphicsQueueFamily();
        info.Queue          = vk->GraphicsQueue();
        info.DescriptorPool = vk->DescriptorPool();
        info.MinImageCount  = 2;
        info.ImageCount     = swapchain->ImageCount();
        info.UseDynamicRendering = true;
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.PipelineInfoMain.PipelineRenderingCreateInfo = rendering;
        info.CheckVkResultFn = CheckVk;

        if (!ImGui_ImplVulkan_Init(&info)) {
            VAE_CORE_ERROR("ImGui Vulkan backend failed to initialise");
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            return;
        }

        m_Ready = true;
        VAE_CORE_INFO("editor chrome ready (ImGui {}, docking)", IMGUI_VERSION);
    }

    void ImGuiLayer::OnDetach() {
        if (!m_Ready) return;
        Application::Get().GetDevice().WaitIdle();
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        m_Ready = false;
    }

    void ImGuiLayer::LoadFonts() {
        ImGuiIO& io = ImGui::GetIO();
        const f32 scale = Application::Get().GetWindow().ContentScale();
        const f32 size = 15.0f * (scale > 0.0f ? scale : 1.0f);

        // The engine already knows where its bundled faces are; asking FontDB rather than guessing
        // a system path keeps the editor's type identical to what the canvas renders with.
        const auto uiFont = FileSystem::EngineRoot() / "VAE/assets/fonts/JetBrainsMonoNerdFont-Regular.ttf";
        if (std::filesystem::exists(uiFont)) {
            // Nerd Font glyphs live in the private use areas; without an explicit range ImGui
            // builds Latin only and every icon in the editor is a box.
            static const ImWchar ranges[] = {
                0x0020, 0x00FF,   // Latin
                0x2190, 0x21FF,   // arrows
                0x2500, 0x259F,   // box drawing
                0x25A0, 0x25FF,   // geometric shapes
                0xE000, 0xF8FF,   // private use: the Nerd Font icon block
                0,
            };
            ImFontConfig config;
            config.OversampleH = 2;
            config.OversampleV = 1;
            io.Fonts->AddFontFromFileTTF(uiFont.string().c_str(), size, &config, ranges);
        } else {
            VAE_CORE_WARN("bundled UI font missing at {} — falling back to ImGui's built-in",
                          uiFont.string());
            io.Fonts->AddFontDefault();
        }
    }

    void ImGuiLayer::ApplyTheme() { ApplyStudioTheme(); }

    void ImGuiLayer::Begin() {
        if (!m_Ready) return;
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        m_FrameOpen = true;
    }

    void ImGuiLayer::Finish() {
        if (!m_Ready || !m_FrameOpen) return;
        ImGui::Render();

        // Keep the loop awake for a few frames after input, and for as long as something is being
        // dragged. Anything less loses the tail of a click that arrived in the same batch as the
        // move before it.
        if (m_Awake > 0) --m_Awake;
        if (m_Awake > 0 || ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left))
            Application::Get().RequestFrame();
    }

    void ImGuiLayer::Draw(gpu::CommandList& cmd) {
        if (!m_Ready || !m_FrameOpen) return;
        m_FrameOpen = false;

        auto* vk = static_cast<gpu::VulkanDevice*>(&Application::Get().GetDevice());
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vk->CurrentCommandBuffer());
        (void)cmd;   // the pass is already open; ImGui records straight into the frame's buffer
    }

    void ImGuiLayer::OnEvent(Event& e) {
        if (!m_Ready) return;
        if (e.IsMouse() || e.IsKeyboard()) {
            m_Awake = 3;
            Application::Get().RequestFrame();
        }
        if (!m_BlockEvents) return;
        const ImGuiIO& io = ImGui::GetIO();
        if (e.IsMouse() && io.WantCaptureMouse) e.handled = true;
        if (e.IsKeyboard() && io.WantCaptureKeyboard) e.handled = true;
    }

    u64 ImGuiLayer::TextureHandle(const Ref<gpu::Texture>& texture) {
        auto* vk = std::dynamic_pointer_cast<gpu::VulkanTexture>(texture).get();
        if (!vk) return 0;
        const VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
            vk->Sampler(), vk->View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return reinterpret_cast<u64>(set);
    }

    void ImGuiLayer::ReleaseTextureHandle(u64 handle) {
        if (!handle) return;
        ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(handle));
    }

}
