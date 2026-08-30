#pragma once

#include "vae/script/VaeScriptAPI.h"
#include "vae/svc/Services.h"
#include "vae/ui/UiHost.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace vae::script {

    // A source of script classes: a native .so, or the Lua host. One per language, and a project
    // picks its language once — the two are never mixed inside a project, only inside the engine.
    class Host {
    public:
        virtual ~Host() = default;

        virtual std::string_view Language() const = 0;
        // Binds the table before any class runs. Called again after every reload, because a fresh
        // module has a fresh copy of the pointer.
        virtual void Bind(const VaeScriptAPI& api) = 0;

        virtual bool Load(const std::filesystem::path& path, std::string* error) = 0;
        virtual bool Reload(std::string* error) = 0;
        virtual void Unload() = 0;
        virtual bool Loaded() const = 0;

        virtual const VaeScriptClass* Find(std::string_view component) const = 0;
        virtual std::vector<std::string> Components() const = 0;
        const std::filesystem::path& Path() const { return m_Path; }

    protected:
        std::filesystem::path m_Path;
    };

    // Runs component logic against a live UI.
    //
    // The runtime owns the durable half of every script instance — its state bag and its timers —
    // precisely so a reload can throw the module away and put the screen back exactly as it was. A
    // script that kept its own counter in a C++ member would lose it at the first dlclose.
    class Runtime {
    public:
        Runtime();
        ~Runtime();
        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;

        // What the app can reach outside itself. Optional: a runtime with no services still runs
        // every script, they just cannot save anything or call anything — which is exactly what a
        // unit test wants, and exactly what a designer previewing a screen should get.
        void SetServices(svc::Services* services) { m_Services = services; }
        svc::Services* ServicesOrNull() const { return m_Services; }

        void Attach(ui::UiHost& host, doc::Document& document);
        void Detach();
        bool Attached() const { return m_Host != nullptr; }

        void AddHost(Scope<Host> host);
        void ClearHosts();
        const std::vector<Scope<Host>>& Hosts() const { return m_Hosts; }

        // Mount what is on screen and unmount what is not. Call after the view tree is built.
        void Sync();
        // Timers, queued signals, then on_update for every live instance.
        void Update(f32 dt);
        // Widget actions become script events. Takes the host's queue.
        void Dispatch(const std::vector<ui::Action>& actions);

        // Unmount, reload every module, mount again. State bags and timers are kept, which is the
        // whole point: the screen you were looking at is still the screen you are looking at.
        bool Reload(std::string* error = nullptr);

        std::size_t LiveCount() const { return m_Live.size(); }
        // Whether any live script has asked to be woken again. A timer is the only thing a script
        // owns that fires without anyone touching the app.
        bool HasPendingTimers() const;
        bool IsLive(Uuid instance) const { return m_Live.contains(instance); }
        // The state bag, for tests and for the Studio's inspector.
        const std::map<std::string, doc::Value>* StateOf(Uuid instance) const;

        // --- what a debugger needs -------------------------------------------------------------
        // What is mounted right now, and what each one is. The Studio's Runtime panel reads these
        // every frame while an app is playing; nothing else has any business calling them.
        struct LiveScript {
            Uuid instance;
            std::string component;      // the component the class is bound to
            std::string name;           // what the designer called this copy
            std::size_t timers = 0;
        };
        std::vector<LiveScript> LiveScripts() const;
        // Writes into a live state bag from outside the script. Returns false if nothing is mounted
        // under that id, which is the normal answer one frame after an instance left the screen.
        bool SetState(Uuid instance, const std::string& key, doc::Value value);

        // Every event, as it is delivered. Set by the debugger; costs one empty check per event
        // when nobody is watching.
        using Trace = std::function<void(Uuid instance, const VaeEvent& event)>;
        void SetTrace(Trace trace) { m_Trace = std::move(trace); }

        const VaeScriptAPI& Api() const { return m_Api; }

        // Which runtime an instance belongs to. A script in a .so cannot ask this and has no
        // reason to; a host that lives INSIDE the engine does, because the four C entry points it
        // hands over carry no context of their own and this is the context they need. One line
        // here is what saves the blueprint host from a global, a fixed pool of trampolines, or both.
        static Runtime* Owner(VaeInstance handle) {
            return handle ? Of(handle).runtime : nullptr;
        }

    private:
        struct Timer {
            std::string name;
            f32 remaining = 0.0f;
        };

        struct Record {
            Runtime* runtime = nullptr;
            Uuid instance = Uuid::Invalid();
            Uuid component = Uuid::Invalid();
            std::string componentName;
            std::string instanceName;
            const VaeScriptClass* klass = nullptr;
            // A screen is scripted by name like a component is, but it is not an instance of
            // anything — so the addressing goes through the tree it is the root of, not through an
            // instance id that does not exist.
            bool isScreen = false;
            std::map<std::string, doc::Value> state;
            std::vector<Timer> timers;
            // Storage for strings handed back across the C boundary. A small ring, so two reads in
            // one expression do not tread on each other.
            std::string scratch[4];
            u32 scratchNext = 0;

            const char* Keep(std::string text) {
                std::string& slot = scratch[scratchNext];
                scratchNext = (scratchNext + 1) % 4;
                slot = std::move(text);
                return slot.c_str();
            }
            VaeInstance Handle() { return reinterpret_cast<VaeInstance>(this); }
        };

        struct Signal {
            Uuid from = Uuid::Invalid();
            std::string name;
            f64 number = 0.0;
            std::string text;
        };

        void BuildApi();
        void MountAll();
        void UnmountAll();
        const VaeScriptClass* ClassFor(std::string_view component) const;

        static Record& Of(VaeInstance handle) { return *reinterpret_cast<Record*>(handle); }
        // The view a script means by a node name: "" is the instance's own root.
        static u32 RootViewIn(const ui::ViewTree& tree, Uuid instance);
        ui::ViewTree* TreeOf(const Record& record, u32* rootView) const;
        u32 RootViewOf(const Record& record) const;
        u32 ViewFor(const Record& record, const char* node) const;
        // The instance a node name resolves to, or Invalid when it names something that is not
        // one. What a component property is set on.
        Uuid InstanceUnder(const Record& record, const char* node) const;
        // What a widget's state is keyed on, for the view a name resolved to.
        ui::WidgetId WidgetOf(u32 view) const;
        static ui::WidgetId WidgetIn(const ui::ViewTree& tree, u32 view);
        struct Rows;
        // Rows onto a node, whichever kind of thing it is: a virtualized list reads them through
        // the host, a repeated container out of the tree it is rebuilt from. Empty columns clear.
        void PutRows(Record& record, const char* node, std::vector<std::string> columns,
                     const char* const* cells, u32 total);
        void ScrollTo(Record& record, const char* node, f32 y, bool toEnd);
        // Which instance asked for each live connection, so its messages go to the script that
        // wanted them and stop when that script leaves the screen.
        void PumpSockets();
        // Where a sound named in a script actually is. The asset table first, because a name is
        // what a designer sees; then the app's own folders, so a file that was never imported
        // still plays and a path that escapes the project still does not.
        std::filesystem::path SoundPath(std::string_view asset) const;
        std::map<std::string, Uuid> m_SocketOwners;
        Record* Receiver(const ui::Action& action);
        void Deliver(Record& record, const VaeEvent& event);
        void Fetch(Record& record, svc::Request request, std::string name);

        ui::UiHost*    m_Host = nullptr;
        doc::Document* m_Document = nullptr;
        std::vector<Scope<Host>> m_Hosts;
        std::unordered_map<Uuid, Scope<Record>> m_Live;
        std::vector<Signal> m_Signals;
        VaeScriptAPI m_Api{};
        Trace m_Trace;
        svc::Services* m_Services = nullptr;
    };

}
