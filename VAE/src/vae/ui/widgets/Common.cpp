#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

namespace vae::ui::widgets {

    u32 IndexAmongRole(const ViewTree& tree, u32 container, u32 view, Role role) {
        const auto siblings = tree.FindAllRoles(container, role);
        for (u32 i = 0; i < siblings.size(); ++i)
            if (siblings[i] == view) return i;
        return UINT32_MAX;
    }

    u32 AncestorWithRole(const ViewTree& tree, u32 view, Role role) {
        for (u32 i = view; i != ViewTree::kInvalid; i = tree.At(i).parent)
            if (tree.At(i).role == role) return i;
        return ViewTree::kInvalid;
    }

    u32 LabelOf(const ViewTree& tree, u32 view) {
        u32 fallback = ViewTree::kInvalid;
        std::vector<u32> stack{ view };
        while (!stack.empty()) {
            const u32 current = stack.back();
            stack.pop_back();
            const ViewTree::View& node = tree.At(current);
            if (current != view && node.role == Role::Content) continue;  // the menu, not the label
            if (node.kind == doc::NodeKind::Text) {
                if (node.name == "Label") return current;
                if (fallback == ViewTree::kInvalid) fallback = current;
            }
            for (auto it = node.children.rbegin(); it != node.children.rend(); ++it)
                stack.push_back(*it);
        }
        return fallback;
    }

}
