#pragma once

#include "vae/core/Layer.h"

namespace vae {

    // `VAE-Studio --selftest` — the editor's manipulation logic checked without a window, a device
    // or a human. It drives the real Canvas through a headless ImGui context rather than calling a
    // parallel copy of the gesture code, because a test that reimplements dragging proves nothing
    // about dragging. Sets the process exit code: non-zero means at least one check failed.
    class SelftestLayer final : public Layer {
    public:
        SelftestLayer() : Layer("Selftest") {}
        void OnAttach() override;
    };

}
