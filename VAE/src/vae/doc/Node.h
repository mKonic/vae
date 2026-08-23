#pragma once

#include "vae/doc/PropBag.h"
#include "vae/layout/LayoutTypes.h"

#include <optional>
#include <vector>

namespace vae::doc {

    enum class NodeKind : u8 {
        Frame,      // a box: the workhorse, and what auto-layout containers are
        Text,
        Image,
        Vector,     // imported SVG path data
        Instance,   // an instance of a Component
        Component,  // a reusable definition; its subtree is the master
        Screen,     // a top-level screen of the app
    };

    const char* NodeKindName(NodeKind kind);

    // What a screen is, which decides what navigating to it does. A page replaces what is on
    // screen; everything else is presented over it and the screen underneath stays where it was.
    enum class ScreenKind : u8 {
        Page,       // replaces the current screen and pushes it onto the back stack
        Modal,      // over the current screen, blocking it, dismissed by its scrim or Escape
        Alert,      // a modal that is asking something: it does not dismiss itself
        Sheet,      // a modal that arrives from an edge
        Popover,    // anchored to whatever opened it, dismissed by clicking outside
        Count
    };

    const char* ScreenKindName(ScreenKind kind);
    std::optional<ScreenKind> ScreenKindFromName(std::string_view name);
    // Presented over the screen below rather than replacing it.
    inline bool IsOverlayKind(ScreenKind kind) { return kind != ScreenKind::Page; }
    // Blocks input to whatever is underneath until it closes.
    inline bool BlocksBelow(ScreenKind kind) {
        return kind == ScreenKind::Modal || kind == ScreenKind::Alert || kind == ScreenKind::Sheet;
    }
    // Goes away when the user clicks off it or presses Escape. An alert is a question: it stays.
    inline bool DismissesItself(ScreenKind kind) { return kind != ScreenKind::Alert; }
    std::optional<NodeKind> NodeKindFromName(std::string_view name);

    struct Node {
        Uuid id;
        NodeKind kind = NodeKind::Frame;
        std::string name;

        Uuid parent = Uuid::Invalid();
        std::vector<Uuid> children;

        layout::LayoutStyle layout{};
        PropBag props;

        bool visible = true;
        bool locked = false;

        // Component authoring: the one frame inside a component that an instance's own children
        // land in. A Card without this is a picture of a card — the whole point of a container is
        // that you put something in it, and an instance otherwise has no way to hold anything.
        // Its own children stay as the placeholder shown when an instance supplies nothing.
        bool slot = false;

        // Instance only: which component, and what this instance changed about it. Overrides are
        // keyed by the id of the node INSIDE the component, so they survive the component's
        // children being reordered.
        Uuid componentId = Uuid::Invalid();
        std::map<Uuid, PropBag> overrides;

        // Whole-node equality, which is how "is this still the stock widget?" is answered on
        // save: a component whose subtree matches the one the binary would build is written to the
        // file as a reference instead of 40 nodes of copy.
        bool operator==(const Node&) const = default;

        bool IsInstance() const { return kind == NodeKind::Instance; }
        bool IsComponent() const { return kind == NodeKind::Component; }
    };

}
