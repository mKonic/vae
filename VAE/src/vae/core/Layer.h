#pragma once

#include "vae/base/Base.h"
#include "vae/base/Timestep.h"
#include "vae/core/Event.h"

namespace vae::gpu { class CommandList; }

#include <string>
#include <vector>

namespace vae {

    class Layer {
    public:
        explicit Layer(std::string name = "Layer") : m_Name(std::move(name)) {}
        virtual ~Layer() = default;

        virtual void OnAttach() {}
        virtual void OnDetach() {}
        virtual void OnUpdate(Timestep) {}
        // Offscreen phase: draw into render targets this layer owns.
        virtual void OnRender(gpu::CommandList&) {}
        // Swapchain phase: what ends up on screen.
        virtual void OnUiRender(gpu::CommandList&) {}
        // Editor chrome. Runs BEFORE the swapchain phase so a panel that reserves screen space —
        // a dock space, a viewport window — has already told the layers below where they may draw.
        virtual void OnImGuiRender() {}
        virtual void OnEvent(Event&) {}

        const std::string& Name() const { return m_Name; }

    protected:
        std::string m_Name;
    };

    // Overlays always sit above regular layers, and events walk the stack top-down so the topmost
    // layer sees a click first.
    class LayerStack {
    public:
        ~LayerStack();

        void Push(Scope<Layer> layer);
        void PushOverlay(Scope<Layer> overlay);
        void Clear();

        auto begin()  { return m_Layers.begin(); }
        auto end()    { return m_Layers.end(); }
        auto rbegin() { return m_Layers.rbegin(); }
        auto rend()   { return m_Layers.rend(); }

    private:
        std::vector<Scope<Layer>> m_Layers;
        std::size_t m_InsertIndex = 0;
    };

}
