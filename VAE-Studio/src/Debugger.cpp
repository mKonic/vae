#include "Debugger.h"

#include <algorithm>
#include <cstdio>

namespace vae {

    namespace {

        const char* KindName(int kind) {
            switch (kind) {
                case VAE_EVENT_CLICKED:           return "clicked";
                case VAE_EVENT_VALUE_CHANGED:     return "value";
                case VAE_EVENT_TEXT_CHANGED:      return "text";
                case VAE_EVENT_SUBMITTED:         return "submitted";
                case VAE_EVENT_SELECTION_CHANGED: return "selection";
                case VAE_EVENT_OPENED:            return "opened";
                case VAE_EVENT_CLOSED:            return "closed";
                case VAE_EVENT_DISMISSED:         return "dismissed";
                case VAE_EVENT_NAVIGATED:         return "navigated";
                case VAE_EVENT_SCROLLED:          return "scrolled";
                case VAE_EVENT_TIMER:             return "timer";
                case VAE_EVENT_SIGNAL:            return "signal";
                case VAE_EVENT_HTTP:              return "http";
                default:                          return "?";
            }
        }

    }

    void Debugger::Attach(script::Runtime& runtime) {
        m_Watches.clear();
        m_Log.clear();
        m_Frame = 0;
        m_Paused = false;

        runtime.SetTrace([this](Uuid instance, const VaeEvent& event) {
            if (m_Paused) return;
            Traced entry;
            entry.instance = instance;
            entry.kind = KindName(event.kind);
            entry.source = event.source ? event.source : "";
            entry.frame = m_Frame;
            if (event.text && event.text[0])      entry.detail = event.text;
            else if (event.name && event.name[0]) entry.detail = event.name;
            else if (event.number != 0.0) {
                char buffer[64];
                std::snprintf(buffer, sizeof buffer, "%g", event.number);
                entry.detail = buffer;
            }
            m_Log.push_back(std::move(entry));
            if (m_Log.size() > kMaxTraced) m_Log.pop_front();
        });
    }

    void Debugger::Detach(script::Runtime& runtime) {
        runtime.SetTrace({});
        m_Watches.clear();
        m_Log.clear();
    }

    void Debugger::Add(Watch watch) {
        if (Watching(watch.instance, watch.node, watch.key)) return;
        m_Watches.push_back(std::move(watch));
    }

    void Debugger::Remove(std::size_t index) {
        if (index < m_Watches.size())
            m_Watches.erase(m_Watches.begin() + static_cast<std::ptrdiff_t>(index));
    }

    bool Debugger::Watching(Uuid instance, std::string_view node, std::string_view key) const {
        return std::ranges::any_of(m_Watches, [&](const Watch& watch) {
            return watch.instance == instance && watch.node == node && watch.key == key;
        });
    }

    // Breadth-first inside the instance, the same rule the script runtime uses to resolve a name —
    // a debugger that resolves names differently from the code it is debugging is a liar.
    u32 Debugger::ViewOf(const Watch& watch, const ui::ViewTree& tree) const {
        u32 root = ui::ViewTree::kInvalid;
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).instanceId == watch.instance) { root = i; break; }
        if (root == ui::ViewTree::kInvalid || watch.node.empty()) return root;

        std::vector<u32> queue{ root };
        for (std::size_t at = 0; at < queue.size(); ++at) {
            const u32 view = queue[at];
            if (view != root && tree.At(view).name == watch.node) return view;
            for (const u32 child : tree.At(view).children) queue.push_back(child);
        }
        return ui::ViewTree::kInvalid;
    }

    doc::Value Debugger::Read(const Watch& watch, const script::Runtime& runtime,
                             const ui::ViewTree& tree) const {
        if (watch.state) {
            const auto* state = runtime.StateOf(watch.instance);
            if (!state) return {};
            const auto it = state->find(watch.key);
            return it == state->end() ? doc::Value{} : it->second;
        }
        const auto prop = doc::PropFromName(watch.key);
        const u32 view = ViewOf(watch, tree);
        if (!prop || view == ui::ViewTree::kInvalid) return {};
        return tree.ResolvedProp(view, *prop);
    }

    bool Debugger::Write(const Watch& watch, script::Runtime& runtime, ui::ViewTree& tree,
                         const doc::Value& value) {
        if (watch.state) return runtime.SetState(watch.instance, watch.key, value);

        const auto prop = doc::PropFromName(watch.key);
        const u32 view = ViewOf(watch, tree);
        if (!prop || view == ui::ViewTree::kInvalid) return false;
        tree.SetViewProp(view, *prop, value);
        return true;
    }

    // A hold is applied to the view, not to the document. Writing the document every frame would
    // rebuild the view tree every frame, and a tree rebuilt between a press and a release is a
    // click that never lands — freezing a label would quietly make the app unclickable.
    void Debugger::ApplyFrozen(script::Runtime& runtime, ui::ViewTree& tree) {
        for (const Watch& watch : m_Watches) {
            if (!watch.frozen || !doc::IsSet(watch.hold)) continue;
            if (watch.state) {
                runtime.SetState(watch.instance, watch.key, watch.hold);
                continue;
            }
            const auto prop = doc::PropFromName(watch.key);
            const u32 view = ViewOf(watch, tree);
            if (prop && view != ui::ViewTree::kInvalid)
                tree.SetViewPropLocal(view, *prop, watch.hold);
        }
    }

}
