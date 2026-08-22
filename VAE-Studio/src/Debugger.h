#pragma once

#include "vae/script/Runtime.h"
#include "vae/ui/ViewTree.h"

#include <deque>
#include <string>
#include <vector>

namespace vae {

    // What the Runtime panel shows, minus the drawing.
    //
    // It lives here rather than in the panel because the interesting parts — reading a value the
    // way a script addresses it, writing one back, holding one against a script that keeps changing
    // it — are behaviour, and behaviour that only exists inside an ImGui callback cannot be tested.
    class Debugger {
    public:
        // A pinned value: either a key in a script's state bag, or a property of a node inside the
        // component that script is bound to. Both are addressed by name within one live instance,
        // which is exactly how the script addresses them.
        struct Watch {
            Uuid instance = Uuid::Invalid();
            std::string owner;          // the instance's name, for the row label
            std::string node;           // empty for a state key
            std::string key;            // property name, or state key
            bool state = false;
            bool frozen = false;
            doc::Value hold;            // what to keep writing back while frozen
        };

        struct Traced {
            Uuid instance;
            std::string kind;
            std::string source;
            std::string detail;
            u64 frame = 0;
        };

        static constexpr std::size_t kMaxTraced = 500;

        // Starts recording. Safe to call again; a second run starts clean.
        void Attach(script::Runtime& runtime);
        void Detach(script::Runtime& runtime);

        // --- watches ---------------------------------------------------------------------------
        void Add(Watch watch);
        void Remove(std::size_t index);
        bool Watching(Uuid instance, std::string_view node, std::string_view key) const;
        std::vector<Watch>& Watches() { return m_Watches; }
        const std::vector<Watch>& Watches() const { return m_Watches; }

        // Unset when the instance is gone or the node is not on screen — which is the normal answer
        // one frame after something left the screen, not an error.
        doc::Value Read(const Watch& watch, const script::Runtime& runtime,
                        const ui::ViewTree& tree) const;
        bool Write(const Watch& watch, script::Runtime& runtime, ui::ViewTree& tree,
                   const doc::Value& value);

        // Re-applies every frozen value. Runs after the scripts have had their turn, so a script
        // that writes the property every frame loses the argument — which is the point.
        void ApplyFrozen(script::Runtime& runtime, ui::ViewTree& tree);

        // --- event log -------------------------------------------------------------------------
        const std::deque<Traced>& Log() const { return m_Log; }
        void ClearLog() { m_Log.clear(); }
        bool Paused() const { return m_Paused; }
        void SetPaused(bool paused) { m_Paused = paused; }
        void Tick() { ++m_Frame; }
        u64  Frame() const { return m_Frame; }

    private:
        u32 ViewOf(const Watch& watch, const ui::ViewTree& tree) const;

        std::vector<Watch> m_Watches;
        std::deque<Traced> m_Log;
        bool m_Paused = false;
        u64  m_Frame = 0;
    };

}
