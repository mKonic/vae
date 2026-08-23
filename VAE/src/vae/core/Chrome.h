#pragma once

#include "vae/core/Layer.h"

namespace vae {

    namespace gpu { class CommandList; }

    // Editor chrome, behind an interface the core can drive without naming what implements it.
    //
    // This exists for the linker. A static archive only pulls in the object files something
    // references, so an `Application` that constructed an `app::ImGuiLayer` directly pulled that
    // object into every binary — and with it all of Dear ImGui, into apps that never draw a panel.
    // Measured on an exported app: 6.1 MB with ImGui riding along, and the app cannot open a
    // single ImGui window. Now the editor installs a factory and everything else installs nothing,
    // so a shipped app contains no editor toolkit at all rather than merely not using it.
    //
    // Split into three calls rather than one because the phases are genuinely different: Begin
    // opens the frame, Finish closes it on the CPU, and Draw records it into a command list that
    // is already inside a render pass.
    class ChromeLayer : public Layer {
    public:
        using Layer::Layer;

        virtual void Begin() = 0;
        virtual void Finish() = 0;
        virtual void Draw(gpu::CommandList& cmd) = 0;
        virtual void SetBlockEvents(bool block) = 0;
        virtual bool Ready() const = 0;
    };

    using ChromeFactory = Scope<ChromeLayer> (*)();

}
