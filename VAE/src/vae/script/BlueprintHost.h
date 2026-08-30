#pragma once

#include "vae/doc/Document.h"
#include "vae/script/BlueprintProgram.h"
#include "vae/script/Runtime.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vae::script {

    // Runs blueprints. The third language, beside Lua and a native .so, and the only one that lives
    // inside the engine rather than behind a file the engine loads.
    //
    // It goes through the same `VaeScriptAPI` table a compiled script does, and that is a decision
    // rather than an accident: a blueprint node may only do what a script can do, so the vocabulary
    // cannot drift into capabilities no written script has, and "convert this blueprint to C++" is a
    // translation rather than a reimplementation. The cost is that a colour kept in a variable is
    // kept as its hex text, because the state bag on that boundary holds numbers and text — which
    // is exactly what a C++ script would have had to do too.
    //
    // Execution is Unreal's: an execution wire pushes, a data wire pulls. Reaching a node runs it;
    // reading one of its inputs asks whatever is wired there for a value, which asks its inputs in
    // turn. A node with no execution pins is an expression and is evaluated where it is read, and
    // memoized for the length of one step so a value read twice is not computed twice.
    class BlueprintHost final : public Host {
    public:
        BlueprintHost();
        ~BlueprintHost() override;

        std::string_view Language() const override { return "blueprint"; }
        void Bind(const VaeScriptAPI& api) override;

        // The document whose blueprints these are. Borrowed for the length of the call — every blueprint
        // is copied into its program, because the document is free to be edited while an app is
        // playing and a hot reload is exactly the two copies diverging.
        void Adopt(const doc::Document& document);

        // Reads a document off disk and adopts it. This is what an exported app and the Player
        // do; the Studio hands over the document it already has.
        bool Load(const std::filesystem::path& path, std::string* error) override;
        bool Reload(std::string* error) override;
        void Unload() override;
        bool Loaded() const override { return !m_Classes.empty(); }

        const VaeScriptClass* Find(std::string_view component) const override;
        std::vector<std::string> Components() const override;

        // Every diagnostic every blueprint produced, in the order the blueprints were compiled. What the
        // Blueprint panel's error list and the Console both read.
        struct Message {
            std::string component;
            u32 node = 0;
            bool error = true;
            std::string message;
        };
        const std::vector<Message>& Messages() const { return m_Messages; }
        std::size_t ErrorCount() const;
        const BlueprintProgram* ProgramFor(std::string_view component) const;

        // --- what a debugger watches ------------------------------------------------------------
        // Off by default and free when off. On, every execution wire that carried a step and every
        // value that crossed a data wire is recorded for one frame, which is what lets the editor
        // animate the flow and show a value on a pin the way Unreal does while a Blueprint runs.
        void SetWatching(bool watching) { m_Watching = watching; }
        bool Watching() const { return m_Watching; }
        struct Flow { std::string component; u32 link = 0; };
        // Takes what has been recorded and clears it: the editor draws each step once.
        std::vector<Flow> TakeFlow();
        // The last value seen on an output pin, by component, node and pin.
        struct Watched { std::string text; u32 step = 0; };
        const std::map<std::string, Watched>& Values() const { return m_Values; }
        static std::string WatchKey(std::string_view component, u32 node, std::string_view pin);
        void ClearWatch();

    private:
        // One value moving through the blueprint. `double` and not f32 on purpose: a wall clock is
        // 1.7 billion seconds, and an f32 cannot count seconds that high.
        struct Datum {
            doc::PinType type = doc::PinType::Number;
            bool     boolean = false;
            double   number = 0.0;
            std::string text;
            VaeColor colour{ 1, 1, 1, 1 };

            // Named OfBool rather than Bool because the members below are called boolean,
            // number, text and colour, and a factory sharing a name with a member is a name
            // lookup that finds the wrong one in half the bodies here.
            static Datum OfBool(bool v);
            static Datum OfNumber(double v);
            static Datum OfText(std::string v);
            static Datum OfColour(VaeColor v);
            static Datum Of(const doc::Value& value, doc::PinType type);

            bool        AsBool() const;
            double      AsNumber() const;
            std::string AsText() const;
            VaeColor    AsColour() const;
            Datum  As(doc::PinType type) const;
        };

        struct Instance {
            const BlueprintProgram* program = nullptr;
        };

        // Everything one dispatch needs, and nothing that outlives it. State that has to survive a
        // hot reload lives in the runtime's own state bag instead — see the '#' keys below.
        struct Step {
            VaeInstance handle = nullptr;
            const BlueprintProgram* program = nullptr;
            const VaeEvent* event = nullptr;
            double delta = 0.0;
            // A pure node's answer, kept for as long as the node that asked for it is running
            // and no longer. Unreal's rule, and this is the case that settles it: a Set feeding a
            // loop body, and a Get read after the loop, must read what the last iteration wrote —
            // so a value worked out inside the loop cannot still be sitting here afterwards. It
            // is still one evaluation per node that asks, so one pure node feeding two pins of
            // the same node is worked out once.
            std::map<std::string, Datum> memo;
            // What an impure node LEFT on its output pins when it ran. These are not answers to a
            // question, they are results — a Set's value, a Play Sound's voice — and they stay
            // until the node runs again, which is what makes wiring one into the next node work.
            std::map<std::string, Datum> results;
            // Which iteration a For Loop is on, for the loops currently running.
            std::map<u32, double> loopIndex;
            u32 steps = 0;
            u32 depth = 0;
            bool stopped = false;
        };

        // A class per component, kept in a stable place: `VaeScriptClass::component` points into
        // the name below, and the runtime keeps that pointer for as long as the class is mounted.
        struct Class {
            std::string component;
            BlueprintProgram program;
            VaeScriptClass klass{};
        };

        // The four C entry points, once for every class there is. They recover their context from
        // the instance — which runtime owns it, and which component it is — so one set of four
        // static functions serves every blueprint in the process, with no globals and no ceiling on
        // how many blueprints a project may have.
        static void MountThunk(VaeInstance handle);
        static void UpdateThunk(VaeInstance handle, double dt);
        static void EventThunk(VaeInstance handle, const VaeEvent* event);
        static void UnmountThunk(VaeInstance handle);
        static BlueprintHost* For(VaeInstance handle);

        void Mount(VaeInstance handle);
        void Update(VaeInstance handle, double dt);
        void Event(VaeInstance handle, const VaeEvent* event);
        void Unmount(VaeInstance handle);

        const BlueprintProgram* ProgramOf(VaeInstance handle) const;
        void Fire(VaeInstance handle, const BlueprintProgram& program, const std::vector<u32>& entries,
                  const VaeEvent* event, double delta);

        // Follows an execution wire out of `pin` and keeps going. Iterative along a straight run,
        // so a hundred statements in a row cost one frame of C++ stack rather than a hundred.
        void Continue(Step& step, u32 node, std::string_view pin);
        // Runs one node, having arrived at `entry`, and returns the output execution pin to carry
        // on from — or an empty name to stop here.
        std::string Run(Step& step, const doc::BlueprintNode& node, std::string_view entry);

        // Reads an input pin: what is wired into it, or the literal it holds.
        Datum Read(Step& step, const doc::BlueprintNode& node, const doc::PinSpec& pin);
        Datum ReadNamed(Step& step, const doc::BlueprintNode& node, std::string_view pin);
        // What an output pin says. A pure node works it out here; an impure one left it in the
        // memo when it ran, and an event node's outputs are the event that is being delivered.
        Datum Value(Step& step, const doc::BlueprintNode& node, std::string_view pin);
        Datum Evaluate(Step& step, const doc::BlueprintNode& node, std::string_view pin);

        // State the blueprint keeps for itself, under keys no variable can collide with, in the same
        // bag a variable lives in — so a hot reload keeps a Do Once done and a Delay pending.
        static std::string InternalKey(std::string_view what, u32 node);
        void Record(Step& step, const doc::BlueprintNode& node, std::string_view pin,
                    const Datum& value);
        void Say(VaeInstance handle, int level, const std::string& text) const;

        VaeScriptAPI m_Api{};
        bool m_Bound = false;
        std::vector<std::unique_ptr<Class>> m_Classes;
        std::unordered_map<VaeInstance, Instance> m_Live;
        std::vector<Message> m_Messages;
        std::filesystem::path m_Loaded;         // set only when Load read one off disk
        const doc::Document* m_Source = nullptr;

        bool m_Watching = false;
        std::vector<Flow> m_Flow;
        std::map<std::string, Watched> m_Values;
        u32 m_WatchStep = 0;
    };

}
