#pragma once

#include "EditorState.h"

namespace vae {

    // The catalog's exam, not a demo: two of `shadcn`'s own example blocks rebuilt as VAE screens
    // out of library components alone. A block that needs a hand-built widget is a block that says
    // the catalog is still short — which is exactly what building these was for.
    //
    // A sign-in page: a card of fields, two providers, and the small print underneath.
    void BuildLoginBlock(EditorState& state);

    // The other shape every app has: a sidebar, a header, a row of figures, a chart and a table.
    void BuildDashboardBlock(EditorState& state);

}
