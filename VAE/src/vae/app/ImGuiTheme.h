#pragma once

namespace vae::app {

    // The Studio's look. Kept apart from the layer so a theme edit is a one-file change and never
    // touches the backend wiring.
    void ApplyStudioTheme();

}
