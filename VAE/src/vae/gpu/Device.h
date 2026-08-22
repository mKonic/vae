#pragma once

#include "vae/gpu/CommandList.h"

namespace vae {
    class Window;
}

namespace vae::gpu {

    struct DeviceCaps {
        std::string deviceName;
        std::string driverInfo;
        bool discrete = false;
        bool softwareRasterizer = false;    // true under lavapipe/llvmpipe
        u32 maxTextureSize = 0;
        u32 maxSamplersPerBatch = 0;
        u32 framesInFlight = 2;
    };

    class Swapchain {
    public:
        virtual ~Swapchain() = default;
        virtual u32 Width()  const = 0;
        virtual u32 Height() const = 0;
        virtual Format ColorFormat() const = 0;
        virtual void Resize(u32 width, u32 height) = 0;
    };

    struct DeviceDesc {
        Window* window = nullptr;           // null = headless (no swapchain)
        bool enableValidation = true;       // forced off in Dist
        bool vsync = true;
        std::string appName = "VAE";
    };

    class Device {
    public:
        virtual ~Device() = default;

        virtual const DeviceCaps& Caps() const = 0;
        virtual Backend GetBackend() const = 0;
        virtual Swapchain* GetSwapchain() = 0;

        virtual Ref<Buffer>      CreateBuffer(const BufferDesc&) = 0;
        virtual Ref<Texture>     CreateTexture(const TextureDesc&) = 0;
        virtual Ref<Shader>      CreateShader(const ShaderDesc&) = 0;
        virtual Ref<Pipeline>    CreatePipeline(const PipelineDesc&) = 0;
        virtual Ref<BindGroup>   CreateBindGroup(const Ref<Pipeline>&,
                                                 const std::vector<BindGroupEntry>&) = 0;
        virtual Ref<RenderTarget> CreateRenderTarget(const RenderTargetDesc&) = 0;

        // Returns null when the frame must be skipped (minimized, or the swapchain is being
        // rebuilt). Callers must not record anything in that case.
        virtual CommandList* BeginFrame() = 0;
        virtual void EndFrame() = 0;
        virtual void WaitIdle() = 0;

        virtual void OnWindowResize(u32 width, u32 height) = 0;

        // The active device, for code that should not thread one through every call.
        static Device* Current();
        static void SetCurrent(Device*);
    };

    // Creates a device for `backend`, or returns null after logging exactly why. Selection never
    // silently succeeds on a different backend than asked for — the caller decides whether to
    // fall back, so the UI can say so.
    Scope<Device> CreateDevice(Backend backend, const DeviceDesc& desc);

    // Resolves the requested backend (explicit > VAE_GPU env > Vulkan), creating it, and falling
    // back to Vulkan with a warning if the request cannot be honoured.
    Scope<Device> CreateDeviceWithFallback(const DeviceDesc& desc,
                                           std::optional<Backend> requested = std::nullopt);

}
