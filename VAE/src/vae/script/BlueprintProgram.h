#pragma once

#include "vae/doc/Blueprint.h"

#include <string>
#include <string_view>
#include <vector>

namespace vae::script {

    // A blueprint, checked and indexed, ready to run.
    //
    // Compiling is not translation here — the interpreter walks the blueprint itself — it is the pass
    // that answers everything the interpreter would otherwise have to ask a thousand times a
    // second, and that says no to a blueprint that cannot mean anything. Unreal draws that line in the
    // same place and calls it Compile, and the reason is the same: a wire that would be a type
    // error is worth a red node in the editor, not a surprise at run time.
    //
    // The program owns a COPY of the blueprint. It has to: it outlives the edit that produced it (the
    // document is free to change while an app is playing), and the whole point of a hot reload is
    // that the running copy and the copy being edited are two things.
    class BlueprintProgram {
    public:
        struct Diagnostic {
            u32 node = 0;                   // 0 when it is about the blueprint rather than one node
            bool error = true;              // false for a warning: it runs, but not as drawn
            std::string message;
        };

        // Compiles `blueprint` for a component of this name. False when anything is an error; the
        // diagnostics say what, and every one of them names the node it is about so the editor can
        // put a marker on it.
        bool Compile(const doc::Blueprint& blueprint, std::string component);

        bool Ok() const { return m_Ok; }
        const std::vector<Diagnostic>& Diagnostics() const { return m_Diagnostics; }
        std::size_t ErrorCount() const;
        const doc::Blueprint& Blueprint() const { return m_Blueprint; }
        const std::string& Component() const { return m_Component; }

        // The event nodes of one type, in the order they were drawn. More than one On Mount is a
        // legal thing to draw and runs both — Unreal refuses it, but only because a Blueprint event
        // is a function that can be overridden, and nothing here is.
        std::vector<u32> Entries(std::string_view type) const;
        // The event nodes of this type whose binding matches — which widget, which timer, which
        // tag. An empty binding on the node means "any", which is what On Signal with no name is.
        std::vector<u32> EntriesFor(std::string_view type, std::string_view target) const;
        bool HasEntry(std::string_view type) const;

        // Whether anything in this blueprint can be woken by a timer. The runtime asks so that an app
        // with a delay in it keeps being drawn while it waits.
        bool UsesTimers() const { return m_UsesTimers; }

    private:
        void Check();
        // Follows data wires backwards looking for the node it started from. A pure node that
        // feeds itself would evaluate for ever, and the only place to catch that is here — at run
        // time it is a stack overflow with no line number.
        bool Cycles(u32 node, std::vector<u32>& path, std::vector<u32>& done);

        doc::Blueprint m_Blueprint;
        std::string m_Component;
        std::vector<Diagnostic> m_Diagnostics;
        bool m_Ok = false;
        bool m_UsesTimers = false;
    };

}
