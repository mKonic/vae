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
    // cannot drift into capabilities no written script has, and "export this as C++" is a
    // translation rather than a reimplementation. The cost is that a colour kept in a variable is
    // kept as its hex text, because the state bag on that boundary holds numbers and text. A C++
    // script would have had to do exactly the same.
    //
    // Execution is Unreal's: an execution wire pushes, a data wire pulls. Reaching a node runs it;
    // reading one of its inputs asks whatever is wired there for a value, which asks its inputs in
    // turn. A node with no execution pins is an expression and is evaluated where it is read, and
    // memoized for the length of one statement so a value read twice is not computed twice.
    class BlueprintHost final : public Host {
    public:
        BlueprintHost();
        ~BlueprintHost() override;

        std::string_view Language() const override { return "blueprint"; }
        void Bind(const VaeScriptAPI& api) override;

        // The document whose blueprints these are. Borrowed for the length of the call — every
        // blueprint is copied into its program, because the document is free to be edited while an
        // app is playing and a hot reload is exactly the two copies diverging.
        void Adopt(const doc::Document& document);

        // Reads a document off disk and adopts it. This is what an exported app and the Player
        // do; the Studio hands over the document it already has.
        bool Load(const std::filesystem::path& path, std::string* error) override;
        bool Reload(std::string* error) override;
        void Unload() override;
        bool Loaded() const override { return !m_Classes.empty(); }

        const VaeScriptClass* Find(std::string_view component) const override;
        std::vector<std::string> Components() const override;

        // Every diagnostic every blueprint produced, in the order the blueprints were compiled.
        // What the Blueprint panel's error list and the Console both read.
        struct Message {
            std::string component;
            std::string function;       // which canvas: empty for the event graph
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
        // animate the flow and show a value on a pin the way Unreal does while a blueprint runs.
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

        // --- breakpoints --------------------------------------------------------------------------
        // A node the run stops at, the way Unreal's do. The blueprint keeps running the moment
        // Continue is called; until then the app is drawn but nothing in it advances, which is what
        // makes the values on the pins worth reading.
        void SetBreakpoint(std::string_view component, u32 node, bool on);
        bool IsBreakpoint(std::string_view component, u32 node) const;
        void ClearBreakpoints() { m_Breakpoints.clear(); }
        const std::map<std::string, std::vector<u32>>& Breakpoints() const { return m_Breakpoints; }
        // Where the run is stopped, or node 0 when it is not.
        struct Halt {
            bool stopped = false;
            std::string component;
            std::string function;
            u32 node = 0;
        };
        const Halt& Stopped() const { return m_Halt; }
        // Let it go. `step` runs exactly one more node and stops again.
        void Continue();
        void StepOver();
        // Whether a run is suspended at a breakpoint right now. The app is drawn but nothing in it
        // advances until this is false again.
        bool Suspended() const { return m_Halt.stopped; }

    private:
        // One value moving through the blueprint. `double` and not f32 on purpose: a wall clock is
        // 1.7 billion seconds, and an f32 cannot count seconds that high.
        struct Datum {
            doc::PinType type = doc::PinType::Number;
            bool     boolean = false;
            double   number = 0.0;
            std::string text;
            VaeColor colour{ 1, 1, 1, 1 };
            std::vector<std::string> items;                              // a list
            std::vector<std::pair<std::string, std::string>> entries;    // a map

            static Datum OfBool(bool v);
            static Datum OfNumber(double v);
            static Datum OfText(std::string v);
            static Datum OfColour(VaeColor v);
            static Datum OfList(std::vector<std::string> v);
            static Datum OfMap(std::vector<std::pair<std::string, std::string>> v);
            static Datum Of(const doc::Value& value, doc::PinType type);

            bool        AsBool() const;
            double      AsNumber() const;
            std::string AsText() const;
            VaeColor    AsColour() const;
            std::vector<std::string> AsList() const;
            std::vector<std::pair<std::string, std::string>> AsMap() const;
            Datum       As(doc::PinType type) const;
        };

        struct Instance {
            const BlueprintProgram* program = nullptr;
        };

        // One call: the event graph's own outermost frame, or a function's. What a function's
        // parameters and locals live in, and what a Break unwinds to the loop inside.
        struct Frame {
            std::string function;                       // empty on the event graph
            const doc::BlueprintCanvas* canvas = nullptr;
            std::map<std::string, Datum> locals;        // parameters and local variables
            // A pure node's answer, kept for as long as the statement that asked for it is running
            // and no longer. Unreal's rule, and this is the case that settles it: a Set feeding a
            // loop body, and a Get read after the loop, must read what the last iteration wrote.
            std::map<std::string, Datum> memo;
            // What an impure node LEFT on its output pins when it ran. Not answers — results — and
            // they stay until the node runs again, which is what makes wiring one on work.
            std::map<std::string, Datum> results;
            std::map<u32, double> loopIndex;            // which iteration a loop is on
            std::map<u32, Datum> loopElement;           // and what it is looking at
            // A Break is unwinding: every statement it passes on the way out does nothing, and the
            // innermost loop clears it and carries on from its Done.
            bool breaking = false;
        };

        // Everything one dispatch needs, and nothing that outlives it. State that has to survive a
        // hot reload lives in the runtime's own state bag instead — see the '#' keys below.
        // What is still to be done when a breakpoint stops the run in the middle of it.
        //
        // The walk is ordinary C++ recursion, so a breakpoint cannot simply pause where it is — the
        // stack would have to be held across frames, and holding a C++ stack across frames is a
        // coroutine or a thread, and a thread would be mutating the view tree while the editor
        // draws it. So the recursion unwinds, and on the way out every construct writes down the
        // one thing it had left to do. Resuming replays that list, innermost first. It is the same
        // information a call stack holds, in a form that can be put down and picked up.
        struct Pending {
            enum class Kind : u8 {
                Enter,      // run this node, having arrived at `pin`
                Chain,      // carry on from this node's output `pin`
                Sequence,   // the next Then of a Sequence, from `index`
                Loop,       // the next iteration of a loop, from `index`
            };
            Kind kind = Kind::Enter;
            u32 node = 0;
            std::string pin;
            u32 index = 0;
            std::size_t frame = 0;      // which call frame this belongs to
        };

        struct Step {
            VaeInstance handle = nullptr;
            const BlueprintProgram* program = nullptr;
            const VaeEvent* event = nullptr;
            double delta = 0.0;
            std::vector<Frame> frames;
            u32 steps = 0;
            u32 depth = 0;
            bool stopped = false;
            // Set when a breakpoint halted this run. Everything below it on the C++ stack unwinds
            // without doing anything more, writing its continuation into `pending` as it goes.
            bool halted = false;
            std::vector<Pending> pending;
            // The node this run is being picked up at, which must not stop it a second time.
            u32 resumeAt = 0;
            // Whether the halt happened BEFORE the node did anything. A Sequence, a loop and a
            // call all write their own continuation on the way out, and the walk must not also
            // write "enter this node again" — that would run the whole construct twice.
            bool haltAtEntry = false;

            Frame& Top() { return frames.back(); }
            const Frame& Top() const { return frames.back(); }
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
        void Fire(VaeInstance handle, const BlueprintProgram& program,
                  const std::vector<u32>& entries, const VaeEvent* event, double delta);

        // Follows an execution wire out of `pin` and keeps going. Iterative along a straight run,
        // so a hundred statements in a row cost one frame of C++ stack rather than a hundred.
        void Continue(Step& step, u32 node, std::string_view pin);
        // Runs one node, having arrived at `entry`, and returns the output execution pin to carry
        // on from — or an empty name to stop here.
        std::string Run(Step& step, const doc::BlueprintNode& node, std::string_view entry);
        // Every loop is the same shape; `from` is which turn to start at, which is where a
        // breakpoint left off when one stopped a loop in the middle.
        void RunLoop(Step& step, const doc::BlueprintNode& node, u32 from);
        // Runs a function or a custom event, and hands back what its Return left. `into` collects
        // the return values by pin name.
        void Invoke(Step& step, const doc::BlueprintFunction& function,
                    const std::map<std::string, Datum>& arguments,
                    std::map<std::string, Datum>* into);

        // Reads an input pin: what is wired into it, or the literal it holds.
        Datum Read(Step& step, const doc::BlueprintNode& node, const doc::PinSpec& pin);
        Datum ReadNamed(Step& step, const doc::BlueprintNode& node, std::string_view pin);
        // What an output pin says. A pure node works it out here; an impure one left it in the
        // results when it ran, and an event node's outputs are the event being delivered.
        Datum Value(Step& step, const doc::BlueprintNode& node, std::string_view pin);
        Datum Evaluate(Step& step, const doc::BlueprintNode& node, std::string_view pin);

        // A variable, wherever it lives: the frame when the name is a parameter or a local of the
        // function being run, and the instance's state bag otherwise.
        Datum ReadVariable(Step& step, std::string_view name);
        void  WriteVariable(Step& step, std::string_view name, const Datum& value);
        const doc::BlueprintVariable* VariableSpec(const Step& step, std::string_view name) const;
        bool  IsLocal(const Step& step, std::string_view name) const;

        // State the blueprint keeps for itself, under keys no variable can collide with, in the
        // same bag a variable lives in — so a hot reload keeps a Do Once done and a Delay pending.
        static std::string InternalKey(std::string_view what, u32 node);
        void Record(Step& step, const doc::BlueprintNode& node, std::string_view pin,
                    const Datum& value);
        void Say(VaeInstance handle, int level, const std::string& text) const;
        // The canvas a node id is on, anywhere in the blueprint. What a Delay's resume needs, since
        // the timer that wakes it carries a node and nothing else.
        static const doc::BlueprintCanvas* CanvasOf(const doc::Blueprint& blueprint, u32 node,
                                                    std::string* function);

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

        std::map<std::string, std::vector<u32>> m_Breakpoints;
        Halt m_Halt;
        bool m_Stepping = false;            // let exactly one more node run
        // The run that a breakpoint stopped, kept whole so it can be picked up again.
        std::unique_ptr<Step> m_Suspended;
        // Runs the pending list of a suspended step until it is empty or it halts again.
        void Resume(Step& step);
        // Whether this node should stop the run, and if so, records where.
        bool ShouldHalt(Step& step, const doc::BlueprintNode& node);
    };

}
