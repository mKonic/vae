#pragma once

// Umbrella header for the reactive graph. See design/architecture.md §8 (bindings) — document
// properties, widget state and paint invalidation are all edges in this one graph.

#include "vae/rx/Node.h"
#include "vae/rx/Signal.h"
#include "vae/rx/Computed.h"
#include "vae/rx/Effect.h"
