#include "Test.h"
#include "Tone.h"

#include "vae/base/FileSystem.h"
#include "vae/script/Abi.h"
#include "vae/script/LuaHost.h"
#include "vae/script/NativeHost.h"
#include "vae/svc/Services.h"

#include <httplib.h>
#include "vae/ui/Library.h"
#include "vae/ui/UiHost.h"
#include "vae/ui/Widget.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>
#include <fstream>
#include <string>

using namespace vae;

namespace {

    namespace fs = std::filesystem;

    // A component built by hand, so the test owns every name the script addresses: a button called
    // Increment and a text node called Label. No GPU, no window — the script drives the same view
    // tree a real click drives.
    struct Scripted {
        doc::Document document;
        ui::UiHost host;
        script::Runtime runtime;
        Uuid screen = Uuid::Invalid();
        Uuid component = Uuid::Invalid();
        Uuid instance = Uuid::Invalid();
        Vec2 size{ 400.0f, 300.0f };

        explicit Scripted(std::string componentName = "Counter") {
            component = document.CreateNode(doc::NodeKind::Component, Uuid::Invalid(),
                                            std::move(componentName));
            doc::Node* master = document.Find(component);
            master->layout.mode = layout::LayoutMode::Stack;
            master->layout.axis = layout::Axis::Column;
            master->layout.width = layout::Size::Px(200.0f);
            master->layout.height = layout::Size::Px(80.0f);

            const Uuid button = document.CreateNode(doc::NodeKind::Frame, component, "Increment");
            document.SetProp(button, doc::Prop::Role,
                             std::string(ui::RoleName(ui::Role::Button)));
            doc::Node* buttonNode = document.Find(button);
            buttonNode->layout.width = layout::Size::Px(120.0f);
            buttonNode->layout.height = layout::Size::Px(32.0f);

            const Uuid label = document.CreateNode(doc::NodeKind::Text, component, "Label");
            document.SetProp(label, doc::Prop::Text, std::string("-"));

            screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Screen");
            doc::Node* screenNode = document.Find(screen);
            screenNode->layout.mode = layout::LayoutMode::Absolute;
            screenNode->layout.width = layout::Size::Px(size.x);
            screenNode->layout.height = layout::Size::Px(size.y);

            instance = document.CreateInstance(component, screen);
            document.Find(instance)->layout.offsetStart = { 20.0f, 20.0f };
            document.Touch(instance);

            host.SetDocument(document, screen);
            runtime.Attach(host, document);
        }

        // One editor/player frame, in the order a real one runs: lay out, mount what appeared,
        // turn the widget actions into script events, then tick.
        void Frame(f32 dt = 1.0f / 60.0f) {
            runtime.Dispatch(host.TakeActions());
            host.ApplyNavigation();
            host.Update(size, dt);
            runtime.Sync();
            runtime.Update(dt);
            if (services) services->Tick(dt);
        }

        // Runs frames until the condition holds or the patience runs out. Anything involving the
        // network needs this: a fixed number of frames is a race with a socket.
        bool FrameUntil(const std::function<bool()>& done, int millis = 5000) {
            const auto deadline = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds(millis);
            while (std::chrono::steady_clock::now() < deadline) {
                Frame();
                if (done()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            Frame();
            return done();
        }

        svc::Services* services = nullptr;

        u32 ViewNamed(std::string_view name) const {
            const ui::ViewTree& tree = host.Tree();
            for (u32 i = 0; i < tree.ViewCount(); ++i)
                if (tree.At(i).instanceId == instance && tree.At(i).name == name) return i;
            return ui::ViewTree::kInvalid;
        }

        std::string TextOf(std::string_view name) const {
            const u32 view = ViewNamed(name);
            return view == ui::ViewTree::kInvalid ? std::string{}
                                                  : host.Tree().Str(view, doc::Prop::Text);
        }

        void Click(std::string_view name) {
            const u32 view = ViewNamed(name);
            if (view == ui::ViewTree::kInvalid) return;
            const Vec2 point = host.Tree().Bounds(view).Center();
            host.Dispatch(MakeMouseMoved(point.x, point.y));
            host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                          point.x, point.y, Mod::None));
            host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                          point.x, point.y, Mod::None));
            Frame();
        }
    };

    // The same Counter, but its button is an instance of another component rather than a frame the
    // component owns outright — which is what a designer actually builds, because the library's
    // Button is a component. The script must still see the click.
    struct Composed {
        doc::Document document;
        ui::UiHost host;
        script::Runtime runtime;
        Uuid screen = Uuid::Invalid();
        Uuid counter = Uuid::Invalid();
        Uuid instance = Uuid::Invalid();
        Vec2 size{ 400.0f, 300.0f };

        // A second copy of the same composed component, so a click on one has somewhere wrong to go.
        Uuid second = Uuid::Invalid();

        explicit Composed(int copies = 1) {
            const Uuid button = document.CreateNode(doc::NodeKind::Component, Uuid::Invalid(),
                                                    "Btn");
            doc::Node* buttonNode = document.Find(button);
            buttonNode->layout.width = layout::Size::Px(120.0f);
            buttonNode->layout.height = layout::Size::Px(32.0f);
            document.SetProp(button, doc::Prop::Role, std::string(ui::RoleName(ui::Role::Button)));

            counter = document.CreateNode(doc::NodeKind::Component, Uuid::Invalid(), "Counter");
            doc::Node* master = document.Find(counter);
            master->layout.mode = layout::LayoutMode::Stack;
            master->layout.axis = layout::Axis::Column;
            master->layout.width = layout::Size::Px(200.0f);
            master->layout.height = layout::Size::Px(80.0f);

            const Uuid nested = document.CreateInstance(button, counter);
            document.Find(nested)->name = "Increment";

            const Uuid label = document.CreateNode(doc::NodeKind::Text, counter, "Label");
            document.SetProp(label, doc::Prop::Text, std::string("-"));

            screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Screen");
            doc::Node* screenNode = document.Find(screen);
            screenNode->layout.mode = layout::LayoutMode::Absolute;
            screenNode->layout.width = layout::Size::Px(size.x);
            screenNode->layout.height = layout::Size::Px(size.y);

            instance = document.CreateInstance(counter, screen);
            document.Find(instance)->layout.offsetStart = { 20.0f, 20.0f };
            document.Touch(instance);

            if (copies > 1) {
                second = document.CreateInstance(counter, screen);
                document.Find(second)->layout.offsetStart = { 20.0f, 160.0f };
                document.Touch(second);
            }

            host.SetDocument(document, screen);
            runtime.Attach(host, document);
        }

        void Frame(f32 dt = 1.0f / 60.0f) {
            runtime.Dispatch(host.TakeActions());
            host.ApplyNavigation();
            host.Update(size, dt);
            runtime.Sync();
            runtime.Update(dt);
        }

        u32 ViewNamed(std::string_view name) const { return ViewNamed(name, instance); }

        // Named within one copy: the two Counters use the same names, so the caller says which.
        u32 ViewNamed(std::string_view name, Uuid owner) const {
            const ui::ViewTree& tree = host.Tree();
            u32 root = ui::ViewTree::kInvalid;
            for (u32 i = 0; i < tree.ViewCount(); ++i)
                if (tree.At(i).instanceId == owner) { root = i; break; }
            if (root == ui::ViewTree::kInvalid) return root;

            std::vector<u32> stack{ root };
            while (!stack.empty()) {
                const u32 view = stack.back();
                stack.pop_back();
                if (view != root && tree.At(view).name == name) return view;
                for (auto it = tree.At(view).children.rbegin();
                     it != tree.At(view).children.rend(); ++it)
                    stack.push_back(*it);
            }
            return ui::ViewTree::kInvalid;
        }

        std::string TextOf(std::string_view name, Uuid owner) const {
            const u32 view = ViewNamed(name, owner);
            return view == ui::ViewTree::kInvalid ? std::string{}
                                                  : host.Tree().Str(view, doc::Prop::Text);
        }

        void ClickIn(Uuid owner, std::string_view name) {
            const u32 view = ViewNamed(name, owner);
            if (view == ui::ViewTree::kInvalid) return;
            const Vec2 point = host.Tree().Bounds(view).Center();
            host.Dispatch(MakeMouseMoved(point.x, point.y));
            host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                          point.x, point.y, Mod::None));
            host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                          point.x, point.y, Mod::None));
            Frame();
        }

        std::string TextOf(std::string_view name) const {
            const u32 view = ViewNamed(name);
            return view == ui::ViewTree::kInvalid ? std::string{}
                                                  : host.Tree().Str(view, doc::Prop::Text);
        }

        void Click(std::string_view name) {
            const u32 view = ViewNamed(name);
            if (view == ui::ViewTree::kInvalid) return;
            const Vec2 point = host.Tree().Bounds(view).Center();
            host.Dispatch(MakeMouseMoved(point.x, point.y));
            host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                          point.x, point.y, Mod::None));
            host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                          point.x, point.y, Mod::None));
            Frame();
        }
    };

    fs::path Scratch() {
        const fs::path dir = fs::temp_directory_path() / "vae-script-tests";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }

    fs::path WriteSource(std::string_view name, std::string_view code) {
        const fs::path path = Scratch() / name;
        std::ofstream out(path, std::ios::trunc);
        out << code;
        return path;
    }

    constexpr std::string_view kCounterSource = R"(
#include <vae/script/VaeScript.h>

#include <string>

struct Counter : vae::Script {
    void OnMount() override { Show(); }

    void OnEvent(const vae::Event& event) override {
        if (event.Clicked("Increment")) {
            self.SetState("count", Count() + 1);
            Show();
        }
        if (event.Timer("late")) self.SetStateText("timer", "fired");
    }

    double Count() const { return self.State("count"); }
    void Show() { self["Label"].SetText("text", std::to_string(static_cast<int>(Count()))); }
};

VAE_SCRIPT(Counter, "Counter")

// A second class in the same module, to prove one module can drive several components.
struct Ticker : vae::Script {
    void OnMount() override { self.After(0.05, "late"); }
    void OnUpdate(double dt) override { self.SetState("elapsed", self.State("elapsed") + dt); }
    void OnEvent(const vae::Event& event) override {
        if (event.Timer("late")) self.SetStateText("timer", "fired");
    }
};

VAE_SCRIPT(Ticker, "Ticker")

// Everything a script can reach outside itself, in one component: the durable store, a file, the
// clock, and an answer from the network.
struct Reacher : vae::Script {
    void OnMount() override {
        self.Store("visits", self.Stored("visits") + 1);
        self.Store("who", std::string("cpp"));
        self.WriteFile("note.txt", "written by c++");
        self.SetState("read", self.ReadFile("note.txt") == "written by c++" ? 1.0 : 0.0);
        self.SetState("clock", self.Clock() > 1.0e9 ? 1.0 : 0.0);
        self.SetStateText("today", self.Date("%Y"));
        if (self.HasStored("url")) self.Get(self.StoredText("url").c_str(), "probe");
    }

    void OnEvent(const vae::Event& event) override {
        if (!event.Answered("probe")) return;
        self.SetState("status", event.number);
        self.SetStateText("body", event.text);
        self.SetState("ok", event.Ok() ? 1.0 : 0.0);
    }
};

VAE_SCRIPT(Reacher, "Reacher")

// Sound, from the language that has to shell out to a compiler to say it.
struct Speaker : vae::Script {
    unsigned long long voice = 0;

    void OnMount() override {
        voice = self.PlaySound("chime", 0.5);
        self.SetState("started", voice != 0 ? 1.0 : 0.0);
        self.SetState("playing", self.SoundPlaying(voice) ? 1.0 : 0.0);
        self.SetSoundVolume(0.4);
        self.SetState("volume", self.SoundVolume());
        // A name nobody imported, so the failure has to be a zero rather than a crash.
        self.SetState("missing", self.PlaySound("no-such-sound") == 0 ? 1.0 : 0.0);
    }

    void OnUpdate(double) override {
        self.SetState("still", self.SoundPlaying(voice) ? 1.0 : 0.0);
    }

    void OnEvent(const vae::Event& event) override {
        if (!event.Clicked("Increment")) return;
        self.StopSound(voice);
        self.SetState("stopped", 1.0);
    }
};

VAE_SCRIPT(Speaker, "Speaker")

// Two nodes called Title, in two different places. A bare name cannot say which.
struct Panel : vae::Script {
    void OnMount() override {
        self["Header.Title"].SetText("text", "top");
        self["Footer.Title"].SetText("text", "bottom");
        self["Nowhere.Title"].SetText("text", "unreachable");
    }
};

VAE_SCRIPT(Panel, "Panel")

// A list nobody hand-built a node for: the designer styled one row, the script says what the rows
// say, and the widget virtualizes the rest.
struct Lister : vae::Script {
    void OnMount() override {
        self["Rows"].SetRows(std::vector<std::vector<std::string>>{
            { "Ada", "1815" }, { "Grace", "1906" }, { "Alan", "1912" } });
        self["Count"].SetText("text", std::to_string(self["Rows"].RowCount()));
    }
    void OnEvent(const vae::Event& event) override {
        if (event.Is(VAE_EVENT_CLICKED)) self["Rows"].ClearRows();
    }
};

VAE_SCRIPT(Lister, "Lister")

// A live connection, and what a screen does when it goes away. The interesting half is the
// failure: a feed that cannot be reached has to reach the script, not hang the app.
struct Feed : vae::Script {
    void OnMount() override {
        self["Status"].SetText("text", "connecting");
        self.OpenSocket("ws://127.0.0.1:1/feed", "prices");
    }
    void OnEvent(const vae::Event& event) override {
        if (event.Is(VAE_EVENT_SOCKET_OPEN))    self["Status"].SetText("text", "live");
        if (event.Is(VAE_EVENT_SOCKET_MESSAGE)) self["Status"].SetText("text", event.text);
        if (event.Is(VAE_EVENT_SOCKET_CLOSED))
            self["Status"].SetText("text", event.number > 0.0 ? "failed" : "closed");
    }
};

VAE_SCRIPT(Feed, "Feed")

// A repeated container the designer drew one row of, driven by named columns. Nothing here says
// what a row looks like — that is in the document — and nothing in the document says how many
// there are or what they say, which is here.
struct Roster : vae::Script {
    void OnMount() override {
        vae::Rows rows({ "name", "tint", "online" });
        rows.Add({ "Ada", "#ff8800", "1" });
        rows.Add({ "Grace", "accent", "1" });
        rows.Add({ "Alan", "accent", "" });
        self["People"].SetRows(rows);
        self["Picked"].SetText("text", "none");
        self["Search"].Focus();
    }
    void OnEvent(const vae::Event& event) override {
        // Which row, not which node: every copy is the same node, so the name cannot say.
        if (event.ClickedRow("People")) {
            self["Picked"].SetText("text", std::to_string(event.row));
            self.SetState("picked", event.row);
            self["People"].SetNumber("selectedIndex", event.row);
        }
        if (event.Is(VAE_EVENT_SUBMITTED)) {
            self["Picked"].SetText("text", std::string("typed ") + event.text);
            self["Scroller"].ScrollToEnd();
        }
    }
};

VAE_SCRIPT(Roster, "Roster")
)";

    // A module that is shaped right and answers with the wrong ABI. The engine has to refuse it,
    // because the alternative is reading a differently-shaped table and crashing somewhere else.
    constexpr std::string_view kWrongAbiSource = R"(
#include <vae/script/VaeScriptAPI.h>

extern "C" __attribute__((visibility("default"))) unsigned int vae_script_abi(void) {
    return VAE_SCRIPT_ABI_VERSION + 41u;
}
extern "C" __attribute__((visibility("default"))) void vae_script_register(const VaeScriptAPI*) {}
extern "C" __attribute__((visibility("default")))
const VaeScriptClass* vae_script_classes(int* count) { if (count) *count = 0; return nullptr; }
)";

    // The same component, in the other language. Every assertion below runs against both, because
    // "Lua or C++, chosen once per project" is only true if a project's behaviour does not depend
    // on which one it chose.
    constexpr std::string_view kCounterLua = R"(
vae.component("Counter", {
    on_mount = function(self) self:show() end,

    on_event = function(self, event)
        if event.kind == "clicked" and event.source == "Increment" then
            self:set_state("count", self:state("count") + 1)
            self:show()
        end
        if event.kind == "timer" and event.name == "late" then
            self:set_state_text("timer", "fired")
        end
    end,

    show = function(self)
        self:set_text("Label", "text", tostring(math.floor(self:state("count"))))
    end,
})

vae.component("Panel", {
    on_mount = function(self)
        -- Two nodes called Title, in two different places. A bare name cannot say which.
        self:set_text("Header.Title", "text", "top")
        self:set_text("Footer.Title", "text", "bottom")
        self:set_text("Nowhere.Title", "text", "unreachable")
    end,
})

vae.component("Lister", {
  on_mount = function(self)
    self:set_rows("Rows", { { "Ada", "1815" }, { "Grace", "1906" }, { "Alan", "1912" } })
    self:set_text("Count", "text", tostring(self:row_count("Rows")))
  end,
  on_event = function(self, event)
    if event.kind == "clicked" then self:clear_rows("Rows") end
  end,
})

vae.component("Roster", {
  on_mount = function(self)
    self:set_rows("People", {
        { name = "Ada",   tint = "#ff8800", online = "1" },
        { name = "Grace", tint = "accent",  online = "1" },
        { name = "Alan",  tint = "accent",  online = "" },
    })
    self:set_text("Picked", "text", "none")
    self:focus("Search")
  end,
  on_event = function(self, event)
    if event.kind == "clicked" and event.list == "People" then
      self:set_text("Picked", "text", tostring(event.row))
      self:set_state("picked", event.row)
      self:set_number("People", "selectedIndex", event.row)
    end
    if event.kind == "submitted" then
      self:set_text("Picked", "text", "typed " .. event.text)
      self:scroll_to_end("Scroller")
    end
  end,
})

vae.component("Feed", {
  on_mount = function(self)
    self:set_text("Status", "text", "connecting")
    self:socket_open("ws://127.0.0.1:1/feed", "prices")
  end,
  on_event = function(self, event)
    if event.kind == "socketOpen" then self:set_text("Status", "text", "live") end
    if event.kind == "socketMessage" then self:set_text("Status", "text", event.text) end
    if event.kind == "socketClosed" then
      self:set_text("Status", "text", event.number > 0 and "failed" or "closed")
    end
  end,
})

vae.component("Ticker", {
    on_mount = function(self) self:after(0.05, "late") end,
    on_update = function(self, dt) self:set_state("elapsed", self:state("elapsed") + dt) end,
    on_event = function(self, event)
        if event.kind == "timer" and event.name == "late" then
            self:set_state_text("timer", "fired")
        end
    end,
})

vae.component("Reacher", {
    on_mount = function(self)
        self:set_stored("visits", self:stored("visits") + 1)
        self:set_stored_text("who", "lua")
        self:write_file("note.txt", "written by lua")
        self:set_state("read", self:read_file("note.txt") == "written by lua" and 1 or 0)
        self:set_state("clock", self:clock() > 1e9 and 1 or 0)
        self:set_state_text("today", self:date("%Y"))
        if self:has_stored("url") then self:get(self:stored_text("url"), "probe") end
    end,

    on_event = function(self, event)
        if event.kind ~= "http" or event.name ~= "probe" then return end
        self:set_state("status", event.number)
        self:set_state_text("body", event.text)
        self:set_state("ok", (event.number >= 200 and event.number < 300) and 1 or 0)
    end,
})

vae.component("Speaker", {
    on_mount = function(self)
        self.voice = self:play_sound("chime", 0.5)
        self:set_state("started", self.voice ~= 0 and 1 or 0)
        self:set_state("playing", self:sound_playing(self.voice) and 1 or 0)
        self:set_sound_volume(0.4)
        self:set_state("volume", self:sound_volume())
        self:set_state("missing", self:play_sound("no-such-sound") == 0 and 1 or 0)
    end,

    on_update = function(self)
        self:set_state("still", self:sound_playing(self.voice) and 1 or 0)
    end,

    on_event = function(self, event)
        if event.kind ~= "clicked" or event.source ~= "Increment" then return end
        self:stop_sound(self.voice)
        self:set_state("stopped", 1)
    end,
})
)";

    // Compiled once for the whole file: the compiler is the slow part of this suite, not the tests.
    struct Module {
        fs::path binary;
        std::string diagnostics;
        bool ok = false;

        static const Module& Counter() {
            static Module module = Build("counter.cpp", kCounterSource, "counter.so");
            return module;
        }
        static const Module& WrongAbi() {
            static Module module = Build("wrongabi.cpp", kWrongAbiSource, "wrongabi.so");
            return module;
        }

        static Module Build(std::string_view name, std::string_view code, std::string_view out) {
            Module module;
            module.binary = Scratch() / out;
            module.ok = script::NativeHost::Compile(WriteSource(name, code), module.binary,
                                                    &module.diagnostics);
            return module;
        }
    };

    enum class Lang { Cpp, Lua };

    const char* Name(Lang lang) { return lang == Lang::Cpp ? "c++" : "lua"; }

    Scope<script::Host> MakeHost(Lang lang, std::string* error) {
        if (lang == Lang::Cpp) {
            const Module& module = Module::Counter();
            if (!module.ok) { *error = module.diagnostics; return nullptr; }
            auto host = CreateScope<script::NativeHost>();
            if (!host->Load(module.binary, error)) return nullptr;
            return host;
        }
        auto host = CreateScope<script::LuaHost>();
        if (!host->Load(WriteSource("counter.lua", kCounterLua), error)) return nullptr;
        return host;
    }

}

TEST(script, a_component_script_compiles_against_one_header) {
    const Module& module = Module::Counter();
    // The whole point of the facade: no engine headers, no glm, no spdlog, one include.
    CHECK_MESSAGE(module.ok, module.diagnostics);
    CHECK(fs::exists(module.binary));
}

TEST(script, a_module_built_against_another_abi_is_refused) {
    const Module& module = Module::WrongAbi();
    CHECK_MESSAGE(module.ok, module.diagnostics);

    script::NativeHost host;
    std::string error;
    CHECK(!host.Load(module.binary, &error));
    CHECK(!host.Loaded());
    // The message has to name both versions, or the author has no idea what to rebuild against.
    CHECK(error.find(std::to_string(VAE_SCRIPT_ABI_VERSION + 41u)) != std::string::npos);
    CHECK(error.find(std::to_string(VAE_SCRIPT_ABI_VERSION)) != std::string::npos);
}

TEST(script, a_missing_module_fails_without_taking_the_host_with_it) {
    script::NativeHost host;
    std::string error;
    CHECK(!host.Load(Scratch() / "nothing-here.so", &error));
    CHECK(!error.empty());
    CHECK(!host.Loaded());
    CHECK(host.Find("Counter") == nullptr);
}

TEST(script, a_script_runs_on_mount_and_on_click) {
    for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
        std::string error;
        Scope<script::Host> host = MakeHost(lang, &error);
        CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
        if (!host) continue;
        // Counter, Ticker, Reacher, Speaker, Panel, Lister, Feed and Roster: one module, several
        // components.
        CHECK(host->Components().size() == 8);

        Scripted ui;
        ui.runtime.AddHost(std::move(host));

        ui.Frame();
        CHECK(ui.runtime.LiveCount() == 1);
        CHECK(ui.runtime.IsLive(ui.instance));
        // OnMount wrote through the label the designer named, not through an index.
        CHECK_MESSAGE(ui.TextOf("Label") == "0", std::string(Name(lang)) + ": " + ui.TextOf("Label"));

        ui.Click("Increment");
        CHECK_MESSAGE(ui.TextOf("Label") == "1", std::string(Name(lang)) + ": " + ui.TextOf("Label"));
        ui.Click("Increment");
        ui.Click("Increment");
        CHECK_MESSAGE(ui.TextOf("Label") == "3", std::string(Name(lang)) + ": " + ui.TextOf("Label"));
    }
}

TEST(script, both_languages_produce_the_same_screen) {
    std::vector<std::string> transcripts;
    for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
        std::string error;
        Scope<script::Host> host = MakeHost(lang, &error);
        CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
        if (!host) continue;

        Scripted ui;
        ui.runtime.AddHost(std::move(host));
        ui.Frame();

        // The same script of inputs, and the label read after each one. A project picks its
        // language once; what it picked must not be visible in the result.
        std::string transcript = ui.TextOf("Label");
        for (int i = 0; i < 4; ++i) {
            ui.Click("Increment");
            transcript += "," + ui.TextOf("Label");
        }
        transcripts.push_back(transcript);
    }

    CHECK(transcripts.size() == 2);
    if (transcripts.size() == 2)
        CHECK_MESSAGE(transcripts[0] == transcripts[1],
                      "c++ [" + transcripts[0] + "] vs lua [" + transcripts[1] + "]");
}

TEST(script, hot_reload_keeps_the_state_the_screen_is_showing) {
    for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
        std::string error;
        Scope<script::Host> host = MakeHost(lang, &error);
        CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
        if (!host) continue;

        Scripted ui;
        ui.runtime.AddHost(std::move(host));
        ui.Frame();
        ui.Click("Increment");
        ui.Click("Increment");
        CHECK(ui.TextOf("Label") == "2");

        // The module is thrown away and built again. Everything the script owned is gone with it;
        // the state bag is the engine's, so the count survives and on_mount paints it back.
        CHECK_MESSAGE(ui.runtime.Reload(&error), std::string(Name(lang)) + ": " + error);
        ui.Frame();
        CHECK_MESSAGE(ui.TextOf("Label") == "2", std::string(Name(lang)) + ": " + ui.TextOf("Label"));

        ui.Click("Increment");
        CHECK_MESSAGE(ui.TextOf("Label") == "3", std::string(Name(lang)) + ": " + ui.TextOf("Label"));
    }
}

TEST(script, a_script_stops_when_its_instance_leaves_the_screen) {
    for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
        std::string error;
        Scope<script::Host> host = MakeHost(lang, &error);
        CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
        if (!host) continue;

        Scripted ui;
        ui.runtime.AddHost(std::move(host));
        ui.Frame();
        CHECK(ui.runtime.LiveCount() == 1);

        ui.document.DeleteNode(ui.instance);
        ui.Frame();
        CHECK(ui.runtime.LiveCount() == 0);
        CHECK(!ui.runtime.IsLive(ui.instance));
    }
}

TEST(script, update_and_timers_reach_the_script) {
    for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
        std::string error;
        Scope<script::Host> host = MakeHost(lang, &error);
        CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
        if (!host) continue;

        Scripted ui("Ticker");
        ui.runtime.AddHost(std::move(host));

        ui.Frame();
        for (int i = 0; i < 10; ++i) ui.Frame(1.0f / 60.0f);

        const auto* state = ui.runtime.StateOf(ui.instance);
        CHECK(state != nullptr);
        if (!state) continue;

        // Eleven frames at 1/60, counting the one that mounted it. Asserted as a range rather
        // than an exact sum: the point is that on_update runs every frame with a sensible dt,
        // not that the test can count frames.
        const auto elapsed = state->find("elapsed");
        CHECK(elapsed != state->end());
        if (elapsed != state->end()) {
            const f32 seconds = std::get<f32>(elapsed->second);
            CHECK_MESSAGE(seconds > 0.1f && seconds < 0.25f,
                          std::string(Name(lang)) + ": " + std::to_string(seconds));
        }

        // The timer was set for 50 ms, which is three frames in.
        const auto timer = state->find("timer");
        CHECK(timer != state->end());
        if (timer != state->end())
            CHECK_MESSAGE(std::get<std::string>(timer->second) == "fired", Name(lang));
    }
}

TEST(script, a_component_with_no_class_is_left_alone) {
    for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
        std::string error;
        Scope<script::Host> host = MakeHost(lang, &error);
        CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
        if (!host) continue;

        Scripted ui("Unscripted");
        ui.runtime.AddHost(std::move(host));
        ui.Frame();
        CHECK(ui.runtime.LiveCount() == 0);
        CHECK(ui.TextOf("Label") == "-");
    }
}

TEST(script, a_lua_script_with_a_syntax_error_is_reported_not_swallowed) {
    script::LuaHost host;
    std::string error;
    const auto path = WriteSource("broken.lua", "vae.component('X', { on_mount = function(self)");
    CHECK(!host.Load(path, &error));
    CHECK(!host.Loaded());
    CHECK(!error.empty());
    CHECK(host.Find("X") == nullptr);
}

TEST(script, a_click_on_a_nested_instance_reaches_the_component_that_contains_it) {
    for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
        std::string error;
        Scope<script::Host> host = MakeHost(lang, &error);
        CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
        if (!host) continue;

        Composed ui;
        ui.runtime.AddHost(std::move(host));

        ui.Frame();
        // Only the Counter is scripted; the Btn instance inside it has no class of its own.
        CHECK(ui.runtime.LiveCount() == 1);
        CHECK(ui.runtime.IsLive(ui.instance));
        CHECK_MESSAGE(ui.TextOf("Label") == "0", std::string(Name(lang)) + ": " + ui.TextOf("Label"));

        ui.Click("Increment");
        CHECK_MESSAGE(ui.TextOf("Label") == "1", std::string(Name(lang)) + ": " + ui.TextOf("Label"));
    }
}

TEST(script, two_copies_of_a_composed_component_do_not_share_their_nested_parts) {
    for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
        std::string error;
        Scope<script::Host> host = MakeHost(lang, &error);
        CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
        if (!host) continue;

        Composed ui(2);
        ui.runtime.AddHost(std::move(host));
        ui.Frame();
        CHECK(ui.runtime.LiveCount() == 2);

        // The button lives inside the component definition, so both copies show the same node.
        // Clicking one of them must still only reach the copy it was drawn in.
        ui.ClickIn(ui.second, "Increment");
        CHECK_MESSAGE(ui.TextOf("Label", ui.second) == "1",
                      std::string(Name(lang)) + ": second=" + ui.TextOf("Label", ui.second));
        CHECK_MESSAGE(ui.TextOf("Label", ui.instance) == "0",
                      std::string(Name(lang)) + ": first=" + ui.TextOf("Label", ui.instance));
    }
}

TEST(script, a_dotted_name_says_which_of_two_identical_names_it_means) {
  for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
    std::string error;
    Scope<script::Host> host = MakeHost(lang, &error);
    CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
    if (!host) continue;

    doc::Document document;
    const Uuid panel = document.CreateNode(doc::NodeKind::Component, Uuid::Invalid(), "Panel");
    {
        doc::Node* node = document.Find(panel);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.width = layout::Size::Px(200.0f);
        node->layout.height = layout::Size::Px(120.0f);
    }
    const Uuid header = document.CreateNode(doc::NodeKind::Frame, panel, "Header");
    document.CreateNode(doc::NodeKind::Text, header, "Title");
    const Uuid footer = document.CreateNode(doc::NodeKind::Frame, panel, "Footer");
    document.CreateNode(doc::NodeKind::Text, footer, "Title");

    const Uuid screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Screen");
    {
        doc::Node* node = document.Find(screen);
        node->layout.mode = layout::LayoutMode::Absolute;
        node->layout.width = layout::Size::Px(400.0f);
        node->layout.height = layout::Size::Px(300.0f);
    }
    const Uuid instance = document.CreateInstance(panel, screen);
    document.Touch(instance);

    ui::UiHost ui;
    ui.SetDocument(document, screen);
    script::Runtime runtime;
    runtime.Attach(ui, document);
    runtime.AddHost(std::move(host));

    runtime.Dispatch(ui.TakeActions());
    ui.Update({ 400.0f, 300.0f }, 1.0f / 60.0f);
    runtime.Sync();
    runtime.Update(1.0f / 60.0f);

    // Each step is scoped to what the step before it found, so the two Titles are two things.
    const auto TextUnder = [&](std::string_view parent) {
        const ui::ViewTree& tree = ui.Tree();
        for (u32 i = 0; i < tree.ViewCount(); ++i) {
            if (tree.At(i).name != parent) continue;
            for (const u32 child : tree.At(i).children)
                if (tree.At(child).name == "Title") return tree.Str(child, doc::Prop::Text);
        }
        return std::string{};
    };

    CHECK_MESSAGE(TextUnder("Header") == "top", std::string(Name(lang)) + ": " + TextUnder("Header"));
    CHECK_MESSAGE(TextUnder("Footer") == "bottom", std::string(Name(lang)) + ": " + TextUnder("Footer"));
    // A path whose first step names nothing resolves to nothing, rather than falling back to a
    // breadth-first search that would quietly write to the wrong Title.
    CHECK(TextUnder("Header") != std::string("unreachable"));
    CHECK(TextUnder("Footer") != std::string("unreachable"));
  }
}

// What a page drops into a component's slot is the page's node, showing inside somebody else's
// component. A script addressing it has no way to know that, and should not need to: the path is
// the one the screen reads as, which is through the slot it landed in.
TEST(script, a_script_reaches_what_the_page_put_in_a_components_slot) {
  for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
    std::string error;
    Scope<script::Host> host = MakeHost(lang, &error);
    CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
    if (!host) continue;

    doc::Document document;
    const Uuid panel = document.CreateNode(doc::NodeKind::Component, Uuid::Invalid(), "Panel");
    {
        doc::Node* node = document.Find(panel);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.width = layout::Size::Px(200.0f);
        node->layout.height = layout::Size::Px(120.0f);
    }
    const Uuid header = document.CreateNode(doc::NodeKind::Frame, panel, "Header");
    document.Find(header)->slot = true;
    // The placeholder the component ships with, so a Panel nobody filled still looks like one.
    document.CreateNode(doc::NodeKind::Text, header, "Title");
    const Uuid footer = document.CreateNode(doc::NodeKind::Frame, panel, "Footer");
    document.CreateNode(doc::NodeKind::Text, footer, "Title");

    const Uuid screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Screen");
    {
        doc::Node* node = document.Find(screen);
        node->layout.mode = layout::LayoutMode::Absolute;
        node->layout.width = layout::Size::Px(400.0f);
        node->layout.height = layout::Size::Px(300.0f);
    }
    const Uuid instance = document.CreateInstance(panel, screen);
    document.Touch(instance);
    const Uuid placed = document.CreateNode(doc::NodeKind::Text, instance, "Title");
    document.SetProp(placed, doc::Prop::Text, std::string("mine"));

    ui::UiHost ui;
    ui.SetDocument(document, screen);
    script::Runtime runtime;
    runtime.Attach(ui, document);
    runtime.AddHost(std::move(host));

    runtime.Dispatch(ui.TakeActions());
    ui.Update({ 400.0f, 300.0f }, 1.0f / 60.0f);
    runtime.Sync();
    runtime.Update(1.0f / 60.0f);

    const ui::ViewTree& tree = ui.Tree();
    u32 mine = ui::ViewTree::kInvalid;
    for (u32 i = 0; i < tree.ViewCount(); ++i)
        if (tree.At(i).authoredId == placed) mine = i;

    CHECK(mine != ui::ViewTree::kInvalid);
    if (mine == ui::ViewTree::kInvalid) continue;
    // "Header.Title" found the page's node, not the placeholder it stood in for — which is the
    // only reading that makes the contents of a container scriptable at all.
    CHECK_MESSAGE(tree.Str(mine, doc::Prop::Text) == "top",
                  std::string(Name(lang)) + ": " + tree.Str(mine, doc::Prop::Text));

    // And the component's own node next door is still its own.
    u32 theirs = ui::ViewTree::kInvalid;
    for (u32 i = 0; i < tree.ViewCount(); ++i)
        if (tree.At(i).name == "Footer")
            for (const u32 child : tree.At(i).children) theirs = child;
    CHECK(theirs != ui::ViewTree::kInvalid && tree.Str(theirs, doc::Prop::Text) == "bottom");
  }
}

// The seam for a virtualized list already existed and only C++ could reach it, which meant a
// script could style a row template and never put anything in it.
TEST(script, a_script_fills_a_list_the_designer_only_styled) {
  for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
    std::string error;
    Scope<script::Host> host = MakeHost(lang, &error);
    CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
    if (!host) continue;

    doc::Document document;
    const vae::ui::Library library = vae::ui::BuildStandardLibrary(document);

    const Uuid screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Lister");
    {
        doc::Node* node = document.Find(screen);
        node->layout.mode = layout::LayoutMode::Absolute;
        node->layout.width = layout::Size::Px(400.0f);
        node->layout.height = layout::Size::Px(300.0f);
    }
    const Uuid rows = document.CreateInstance(library.Find("Table"), screen);
    document.Find(rows)->name = "Rows";
    document.Touch(rows);
    const Uuid count = document.CreateNode(doc::NodeKind::Text, screen, "Count");
    document.SetProp(count, doc::Prop::Text, std::string("-"));

    ui::UiHost ui;
    ui.SetDocument(document, screen);
    script::Runtime runtime;
    runtime.Attach(ui, document);
    runtime.AddHost(std::move(host));

    const auto frame = [&] {
        runtime.Dispatch(ui.TakeActions());
        ui.Update({ 400.0f, 300.0f }, 1.0f / 60.0f);
        runtime.Sync();
        runtime.Update(1.0f / 60.0f);
    };
    frame();

    const ui::ViewTree& tree = ui.Tree();
    const auto viewNamed = [&](std::string_view name) {
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).name == name) return i;
        return ui::ViewTree::kInvalid;
    };

    const u32 list = viewNamed("Rows");
    CHECK(list != ui::ViewTree::kInvalid);
    const auto* source = ui.DataSource(ui::WidgetId{ tree.At(list).sourceId,
                                                     tree.At(list).instanceId });
    CHECK_MESSAGE(source != nullptr, std::string(Name(lang)) + ": no rows were handed over");
    if (!source) continue;
    CHECK_EQ(source->Count(), 3u);
    CHECK_MESSAGE(source->Cell(1, 0) == "Grace", std::string(Name(lang)) + ": " + source->Cell(1, 0));
    CHECK(source->Cell(2, 1) == "1912");
    // Read back through the same name the script wrote through, which is the only thing that makes
    // "how many rows are there" answerable without the script keeping its own count.
    CHECK(tree.Str(viewNamed("Count"), doc::Prop::Text) == "3");

    // Cleared means gone, not zero rows of something: the widget falls back to what the document
    // says, which is the placeholder count a designer laid the thing out with.
    runtime.Dispatch({ ui::Action{ ui::ActionKind::Clicked, screen, Uuid::Invalid(),
                                   "Lister", {} } });
    frame();
    CHECK(ui.DataSource(ui::WidgetId{ tree.At(list).sourceId, tree.At(list).instanceId })
          == nullptr);
  }
}

// The whole point of a repeated container: a designer draws one row, a script says what the rows
// are, and neither has to know the other's half. The test proves the seam in both directions —
// the data reaches the drawing, and a click on the drawing reaches the script as a row number.
TEST(script, a_designed_row_is_drawn_once_per_row_the_script_hands_over) {
  for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
    std::string error;
    Scope<script::Host> host = MakeHost(lang, &error);
    CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
    if (!host) continue;

    doc::Document document;
    const vae::ui::Library library = vae::ui::BuildStandardLibrary(document);

    const Uuid screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Roster");
    {
        doc::Node* node = document.Find(screen);
        node->layout.mode = layout::LayoutMode::Absolute;
        node->layout.width = layout::Size::Px(400.0f);
        node->layout.height = layout::Size::Px(300.0f);
    }

    const Uuid scroller = document.CreateNode(doc::NodeKind::Frame, screen, "Scroller");
    {
        doc::Node* node = document.Find(scroller);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.offsetStart = { 10.0f, 10.0f };
        node->layout.width = layout::Size::Px(220.0f);
        node->layout.height = layout::Size::Px(60.0f);
        document.SetProp(scroller, doc::Prop::ClipContent, true);
        document.SetProp(scroller, doc::Prop::Role, std::string(ui::RoleName(ui::Role::Scroll)));
    }
    const Uuid people = document.CreateNode(doc::NodeKind::Frame, scroller, "People");
    {
        doc::Node* node = document.Find(people);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.width = layout::Size::Fill();
        node->layout.height = layout::Size::Hug();
    }
    // One row, drawn once, styled once — including what it looks like when it is the chosen one.
    const Uuid person = document.CreateNode(doc::NodeKind::Frame, people, "Person");
    {
        doc::Node* node = document.Find(person);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Row;
        node->layout.width = layout::Size::Fill();
        node->layout.height = layout::Size::Px(24.0f);
        document.SetProp(person, doc::Prop::Role, std::string(ui::RoleName(ui::Role::Button)));
        document.SetProp(person, doc::Prop::Field, std::string("fill:tint"));
        node->props.Set("selected:fill", Color{ 0.0f, 1.0f, 0.0f, 1.0f });
        document.Touch(person);
    }
    const Uuid personName = document.CreateNode(doc::NodeKind::Text, person, "Name");
    document.SetProp(personName, doc::Prop::Field, std::string("name"));
    document.SetProp(personName, doc::Prop::FontSize, 12.0f);
    const Uuid dot = document.CreateNode(doc::NodeKind::Frame, person, "Dot");
    {
        doc::Node* node = document.Find(dot);
        node->layout.width = layout::Size::Px(8.0f);
        node->layout.height = layout::Size::Px(8.0f);
        document.SetProp(dot, doc::Prop::Visible, true);
        document.SetProp(dot, doc::Prop::Field, std::string("visible:online"));
    }

    const Uuid picked = document.CreateNode(doc::NodeKind::Text, screen, "Picked");
    document.Find(picked)->layout.offsetStart = { 10.0f, 200.0f };
    document.SetProp(picked, doc::Prop::Text, std::string("-"));

    const Uuid search = document.CreateInstance(library.Find("TextInput"), screen);
    document.Find(search)->name = "Search";
    document.Find(search)->layout.offsetStart = { 10.0f, 240.0f };
    document.Touch(search);

    ui::UiHost ui;
    ui.SetDocument(document, screen);
    script::Runtime runtime;
    runtime.Attach(ui, document);
    runtime.AddHost(std::move(host));

    const Vec2 size{ 400.0f, 300.0f };
    const auto frame = [&] {
        runtime.Dispatch(ui.TakeActions());
        ui.Update(size, 1.0f / 60.0f);
        runtime.Sync();
        runtime.Update(1.0f / 60.0f);
    };
    frame();
    frame();

    const auto viewNamed = [&](std::string_view name) {
        const ui::ViewTree& tree = ui.Tree();
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).name == name) return i;
        return ui::ViewTree::kInvalid;
    };
    const auto part = [&](std::string_view name, i32 row) {
        const ui::ViewTree& tree = ui.Tree();
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).name == name && tree.At(i).row == row) return i;
        return ui::ViewTree::kInvalid;
    };
    const std::string who(Name(lang));
    // A missing view is a failure to report, not a crash to debug.
    const auto textAt = [&](u32 view) {
        return view == ui::ViewTree::kInvalid ? std::string("<missing>")
                                              : ui.Tree().Str(view, doc::Prop::Text);
    };

    // Three rows the document never mentioned, and the row the designer drew is gone: it was the
    // template, not one of the answers.
    CHECK_MESSAGE(viewNamed("Person 3") != ui::ViewTree::kInvalid, who + ": no third row");
    CHECK(viewNamed("Person 4") == ui::ViewTree::kInvalid);
    CHECK(viewNamed("Person") == ui::ViewTree::kInvalid);

    // Each part of each copy drew its own row's cell.
    CHECK_MESSAGE(textAt(part("Name", 0)) == "Ada", who + ": " + textAt(part("Name", 0)));
    CHECK(textAt(part("Name", 2)) == "Alan");
    // A hex cell is a colour and a bare one is a token: the same column feeds both.
    // A copy is named for its place; everything inside it keeps the name it was drawn with.
    const u32 firstRow = viewNamed("Person 1");
    const u32 secondRow = viewNamed("Person 2");
    CHECK_MESSAGE(firstRow != ui::ViewTree::kInvalid && secondRow != ui::ViewTree::kInvalid,
                  who + ": the rows are not there");
    if (firstRow == ui::ViewTree::kInvalid || secondRow == ui::ViewTree::kInvalid) continue;
    const doc::Value* first = ui.Tree().At(firstRow).props.Find(doc::Prop::Fill);
    CHECK(first && std::holds_alternative<Color>(*first));
    const doc::Value* second = ui.Tree().At(secondRow).props.Find(doc::Prop::Fill);
    CHECK(second && std::holds_alternative<doc::TokenRef>(*second));
    // And an empty cell is a false: Alan's presence dot is not drawn.
    CHECK(ui.Tree().Flag(part("Dot", 0), doc::Prop::Visible));
    CHECK(!ui.Tree().At(part("Dot", 2)).props.Flag(doc::Prop::Visible, true));

    // The script asked for focus by name and got it, so the app is typeable without a click.
    CHECK_MESSAGE(ui.Focused() != ui::ViewTree::kInvalid, who + ": nothing was focused");

    // A click on the *label* inside the second row. Not the row's own frame: a click lands on
    // whatever is under it, and the answer must still be "row 1 of People".
    {
        const Rect box = ui.Tree().Bounds(part("Name", 1));
        const Vec2 point = box.Center();
        ui.Dispatch(MakeMouseMoved(point.x, point.y));
        ui.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                    point.x, point.y, Mod::None));
        ui.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                    point.x, point.y, Mod::None));
    }
    frame();
    frame();
    CHECK_MESSAGE(textAt(viewNamed("Picked")) == "1", who + ": " + textAt(viewNamed("Picked")));

    // Saying so lights up the row the designer styled for it, and no other.
    CHECK_MESSAGE(ui::HasState(ui.Tree().At(viewNamed("Person 2")).state, ui::StateBit::Selected),
                  who + ": the picked row is not selected");
    CHECK(!ui::HasState(ui.Tree().At(viewNamed("Person 1")).state, ui::StateBit::Selected));

    // Typing into the field and pressing Enter: the script hears it, and the scroller it asks
    // for lands at the end of what is in it now. Clicked into first — picking a row moved the
    // focus the script set on mount, which is what a click on a button is supposed to do.
    {
        const Vec2 point = ui.Tree().Bounds(viewNamed("Search")).Center();
        ui.Dispatch(MakeMouseMoved(point.x, point.y));
        ui.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                    point.x, point.y, Mod::None));
        ui.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                    point.x, point.y, Mod::None));
    }
    frame();
    ui.Dispatch(MakeTextInput('h'));
    ui.Dispatch(MakeTextInput('i'));
    ui.Dispatch(MakeKey(EventType::KeyPressed, Key::Enter, Mod::None));
    frame();
    frame();
    CHECK_MESSAGE(textAt(viewNamed("Picked")) == "typed hi",
                  who + ": " + textAt(viewNamed("Picked")));

    const u32 scrollView = viewNamed("Scroller");
    const f32 limit = ui.Tree().ContentSize(scrollView).y - ui.Tree().Bounds(scrollView).size.y;
    CHECK(limit > 0.0f);
    CHECK_MESSAGE(std::abs(ui.Tree().At(scrollView).scroll.y - limit) < 0.5f,
                  who + ": the scroller did not reach the end");
  }
}

// A feed that cannot be reached has to reach the script. The whole path — the script asks, the
// services connect on a thread, the failure comes back through the pump on the main thread and is
// delivered as an ordinary event — is the same one a message travels, and the failure is the leg
// that is easy to get wrong and impossible to notice.
TEST(script, a_live_connection_reports_itself_to_the_script_that_opened_it) {
  for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
    std::string error;
    Scope<script::Host> host = MakeHost(lang, &error);
    CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
    if (!host) continue;

    doc::Document document;
    const Uuid screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Feed");
    {
        doc::Node* node = document.Find(screen);
        node->layout.mode = layout::LayoutMode::Absolute;
        node->layout.width = layout::Size::Px(320.0f);
        node->layout.height = layout::Size::Px(200.0f);
    }
    const Uuid status = document.CreateNode(doc::NodeKind::Text, screen, "Status");
    document.SetProp(status, doc::Prop::Text, std::string("-"));

    ui::UiHost ui;
    ui.SetDocument(document, screen);
    svc::Services services;
    script::Runtime runtime;
    runtime.Attach(ui, document);
    runtime.SetServices(&services);
    runtime.AddHost(std::move(host));

    const auto frame = [&] {
        runtime.Dispatch(ui.TakeActions());
        ui.Update({ 320.0f, 200.0f }, 1.0f / 60.0f);
        runtime.Sync();
        runtime.Update(1.0f / 60.0f);
        services.Tick(1.0f / 60.0f);
    };
    const auto shown = [&] {
        const ui::ViewTree& tree = ui.Tree();
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).name == "Status") return tree.Str(i, doc::Prop::Text);
        return std::string{};
    };

    frame();
    CHECK_MESSAGE(shown() == "connecting", std::string(Name(lang)) + ": " + shown());

    // Nothing listens on port 1, so the answer is a refusal — and it arrives, rather than the app
    // sitting on a connect that never completes.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (shown() == "connecting" && std::chrono::steady_clock::now() < deadline) {
        frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_MESSAGE(shown() == "failed", std::string(Name(lang)) + ": " + shown());
  }
}

TEST(script, both_languages_reach_the_same_services) {
    // A local server, so the suite never touches the internet.
    httplib::Server server;
    server.Get("/ping", [](const httplib::Request&, httplib::Response& response) {
        response.set_content("pong", "text/plain");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread listening([&] { server.listen_after_bind(); });
    server.wait_until_ready();

    for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
        std::string error;
        Scope<script::Host> host = MakeHost(lang, &error);
        CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
        if (!host) continue;

        const fs::path sandbox = Scratch() / ("services-" + std::string(Name(lang)));
        std::error_code ec;
        fs::remove_all(sandbox, ec);
        fs::create_directories(sandbox, ec);

        svc::Services services;
        services.Store().Open(sandbox / "store.json");
        services.FileSystem().AddRoot(sandbox);
        services.Store().Set("url", std::string("http://127.0.0.1:" + std::to_string(port) + "/ping"));

        Scripted ui("Reacher");
        ui.services = &services;
        ui.runtime.SetServices(&services);
        ui.runtime.AddHost(std::move(host));
        ui.Frame();

        const auto* state = ui.runtime.StateOf(ui.instance);
        CHECK_MESSAGE(state != nullptr, Name(lang));
        if (!state) continue;

        // The store: written by the script, readable by anyone, and written to disk on a tick.
        CHECK_MESSAGE(std::get<f32>(services.Store().Get("visits")) == 1.0f, Name(lang));
        CHECK_MESSAGE(std::get<std::string>(services.Store().Get("who"))
                          == (lang == Lang::Cpp ? "cpp" : "lua"), Name(lang));
        CHECK(fs::exists(sandbox / "store.json"));

        // Files, inside the sandbox and nowhere else.
        CHECK_MESSAGE(std::get<f32>(state->at("read")) == 1.0f, std::string(Name(lang)) + ": file");
        CHECK(fs::exists(sandbox / "note.txt"));

        // The clock, and a date that is at least a year.
        CHECK_MESSAGE(std::get<f32>(state->at("clock")) == 1.0f, std::string(Name(lang)) + ": clock");
        CHECK(std::get<std::string>(state->at("today")).size() == 4);

        // The network: asked for on mount, answered later, delivered as an event to the script.
        const bool answered = ui.FrameUntil([&] {
            const auto* now = ui.runtime.StateOf(ui.instance);
            return now && now->contains("status");
        });
        CHECK_MESSAGE(answered, std::string(Name(lang)) + ": no answer");
        if (answered) {
            const auto* now = ui.runtime.StateOf(ui.instance);
            CHECK_MESSAGE(std::get<f32>(now->at("status")) == 200.0f,
                          std::string(Name(lang)) + ": status "
                              + std::to_string(std::get<f32>(now->at("status"))));
            CHECK_MESSAGE(std::get<std::string>(now->at("body")) == "pong", Name(lang));
            CHECK(std::get<f32>(now->at("ok")) == 1.0f);
        }
    }

    server.stop();
    listening.join();
}

TEST(script, both_languages_play_a_sound_by_the_name_it_was_imported_under) {
    for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
        std::string error;
        Scope<script::Host> host = MakeHost(lang, &error);
        CHECK_MESSAGE(host != nullptr, std::string(Name(lang)) + ": " + error);
        if (!host) continue;

        const fs::path sandbox = Scratch() / ("sound-" + std::string(Name(lang)));
        std::error_code ec;
        fs::remove_all(sandbox, ec);
        tone::WriteTone(sandbox / "sounds" / "chime.wav", 0.4f);

        svc::Services services;
        services.FileSystem().AddRoot(sandbox);
        // Silent, because a suite that needs a sound card is a suite that fails in a container —
        // and because a hand-stepped engine's clock is exact where a real one's is a race.
        CHECK_MESSAGE(services.Sound().OpenSilent(&error), error);

        Scripted ui("Speaker");
        // The name is the whole point: a script says "chime", not "sounds/chime.wav", because the
        // name is what the Assets panel shows and the path is an implementation detail of import.
        ui.document.AddAsset("chime", "sounds/chime.wav");
        ui.services = &services;
        ui.runtime.SetServices(&services);
        ui.runtime.AddHost(std::move(host));
        ui.Frame();

        const auto* state = ui.runtime.StateOf(ui.instance);
        CHECK_MESSAGE(state != nullptr, Name(lang));
        if (!state) continue;

        CHECK_MESSAGE(std::get<f32>(state->at("started")) == 1.0f,
                      std::string(Name(lang)) + ": " + services.Sound().Problem());
        CHECK_MESSAGE(std::get<f32>(state->at("playing")) == 1.0f, Name(lang));
        CHECK_MESSAGE(std::get<f32>(state->at("volume")) == 0.4f, Name(lang));
        // A name nobody imported is a zero, not a crash and not a wrong sound.
        CHECK_MESSAGE(std::get<f32>(state->at("missing")) == 1.0f, Name(lang));
        CHECK_MESSAGE(services.Sound().Voices() == 1, Name(lang));

        // A tenth of the way through a four-tenths sound, the script still hears it.
        services.Sound().Step(0.1f);
        ui.Frame();
        CHECK_MESSAGE(std::get<f32>(ui.runtime.StateOf(ui.instance)->at("still")) == 1.0f,
                      Name(lang));

        // And a click stops it, which is the other half of what a handle is for.
        ui.Click("Increment");
        ui.Frame();
        const auto* after = ui.runtime.StateOf(ui.instance);
        CHECK_MESSAGE(std::get<f32>(after->at("stopped")) == 1.0f, Name(lang));
        CHECK_MESSAGE(std::get<f32>(after->at("still")) == 0.0f, Name(lang));
        CHECK_MESSAGE(services.Sound().Voices() == 0, Name(lang));
    }
}

TEST(script, a_sound_that_reaches_outside_the_project_does_not_play) {
    std::string error;
    Scope<script::Host> host = MakeHost(Lang::Lua, &error);
    CHECK_MESSAGE(host != nullptr, error);
    if (!host) return;

    const fs::path sandbox = Scratch() / "sound-escape";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    tone::WriteTone(sandbox / "inside.wav", 0.2f);
    // A real file, outside the project. A script that can name it can read any wav on the machine,
    // which is the same hole a file read would be and is closed the same way.
    tone::WriteTone(Scratch() / "outside.wav", 0.2f);

    svc::Services services;
    services.FileSystem().AddRoot(sandbox);
    CHECK_MESSAGE(services.Sound().OpenSilent(&error), error);

    Scripted ui("Speaker");
    ui.document.AddAsset("chime", "../outside.wav");
    ui.services = &services;
    ui.runtime.SetServices(&services);
    ui.runtime.AddHost(std::move(host));
    ui.Frame();

    CHECK(std::get<f32>(ui.runtime.StateOf(ui.instance)->at("started")) == 0.0f);
    CHECK(services.Sound().Voices() == 0);
}

TEST(script, a_script_with_no_services_still_runs) {
    // A preview in the Studio has no app around it. A script written for one must not fall over.
    for (const Lang lang : { Lang::Cpp, Lang::Lua }) {
        std::string error;
        Scope<script::Host> host = MakeHost(lang, &error);
        if (!host) continue;

        Scripted ui("Reacher");
        ui.runtime.AddHost(std::move(host));
        ui.Frame();

        CHECK_MESSAGE(ui.runtime.LiveCount() == 1, Name(lang));
        const auto* state = ui.runtime.StateOf(ui.instance);
        CHECK(state != nullptr);
        // Nothing was stored, nothing was read, and nothing crashed.
        if (state) CHECK_MESSAGE(std::get<f32>(state->at("read")) == 0.0f, Name(lang));
    }
}

TEST(script, an_answer_for_an_instance_that_left_the_screen_is_dropped) {
    // The request outlives the thing that asked. Delivering to an unmounted script is a
    // use-after-free with a network-shaped delay on it, which is the worst kind to reproduce.
    httplib::Server server;
    server.Get("/slow", [](const httplib::Request&, httplib::Response& response) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        response.set_content("late", "text/plain");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread listening([&] { server.listen_after_bind(); });
    server.wait_until_ready();

    std::string error;
    Scope<script::Host> host = MakeHost(Lang::Lua, &error);
    CHECK_MESSAGE(host != nullptr, error);
    if (host) {
        const fs::path sandbox = Scratch() / "vanish";
        std::error_code ec;
        fs::create_directories(sandbox, ec);

        svc::Services services;
        services.Store().Open(sandbox / "store.json");
        services.FileSystem().AddRoot(sandbox);
        services.Store().Set("url", std::string("http://127.0.0.1:" + std::to_string(port) + "/slow"));

        Scripted ui("Reacher");
        ui.services = &services;
        ui.runtime.SetServices(&services);
        ui.runtime.AddHost(std::move(host));
        ui.Frame();
        CHECK(ui.runtime.LiveCount() == 1);

        // Gone before the answer arrives.
        ui.document.DeleteNode(ui.instance);
        ui.Frame();
        CHECK(ui.runtime.LiveCount() == 0);

        ui.FrameUntil([&] { return services.Net().Pending() == 0; });
        CHECK(services.Net().Pending() == 0);
    }

    server.stop();
    listening.join();
}

TEST(script, the_editors_api_list_matches_what_lua_actually_binds) {
    // The completion popup offers this list. A name in it that the host does not bind is a lie the
    // editor tells every time somebody types, and nothing else would ever catch it — so the check
    // is not "does the list look right" but "ask a live component whether each one is a function".
    std::string check =
        "vae.component(\"Counter\", { on_mount = function(self)\n"
        "  local missing = {}\n";
    for (const std::string& method : script::LuaSelfMethods())
        check += "  if type(self." + method + ") ~= \"function\" then missing[#missing+1] = \""
               + method + "\" end\n";
    check += "  self:set_text(\"Label\", \"text\", #missing == 0 and \"ok\" "
             "or table.concat(missing, \", \"))\n"
             "end })\n";

    std::string error;
    auto host = CreateScope<script::LuaHost>();
    CHECK_MESSAGE(host->Load(WriteSource("abi-check.lua", check), &error), error);

    Scripted ui;
    ui.runtime.AddHost(std::move(host));
    ui.Frame();
    CHECK_MESSAGE(ui.TextOf("Label") == "ok", "not bound: " + ui.TextOf("Label"));

    // And the list the editor shows is the methods, spelled the way they are written.
    CHECK(script::LuaApi().size() > script::LuaSelfMethods().size());
    bool prefixed = false;
    for (const std::string& name : script::LuaApi())
        if (name == "self:set_text") prefixed = true;
    CHECK(prefixed);
    CHECK(!script::CppApi().empty());
}
