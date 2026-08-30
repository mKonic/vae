#include "Example.h"

#include "vae/doc/Blueprint.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"
#include "vae/doc/Document.h"

#include <cmath>
#include <vector>

namespace vae {

    using namespace layout;

    namespace {

        // Set a property straight on the document rather than through EditorState, because the
        // example is authored, not edited: it must not land on the undo stack as fifty steps the
        // user has to walk back past.
        void Set(doc::Document& d, Uuid node, doc::Prop prop, doc::Value value) {
            d.SetProp(node, prop, std::move(value));
        }

        void Token(doc::Document& d, Uuid node, doc::Prop prop, const char* token) {
            d.SetProp(node, prop, doc::TokenRef{ token });
        }

        LayoutStyle& Lay(doc::Document& d, Uuid node) {
            d.Touch(node);
            return d.Find(node)->layout;
        }

    }

    void WriteExampleClick(EditorState& state, const std::filesystem::path& folder) {
        constexpr u32 kRate = 44100;
        constexpr f32 kSeconds = 0.055f;
        const auto frames = static_cast<u32>(kSeconds * kRate);

        // What a click is, as a waveform: a short burst that decays fast. Two tones an octave
        // apart so it reads as a "tick" rather than a beep, and an exponential envelope because a
        // square-edged one ends in an audible pop.
        std::vector<i16> samples(frames);
        for (u32 i = 0; i < frames; ++i) {
            const f64 t = static_cast<f64>(i) / kRate;
            const f64 envelope = std::exp(-t * 90.0);
            const f64 wave = std::sin(2.0 * 3.14159265358979 * 1400.0 * t) * 0.7
                           + std::sin(2.0 * 3.14159265358979 * 2800.0 * t) * 0.3;
            samples[i] = static_cast<i16>(9000.0 * wave * envelope);
        }

        const auto put = [](std::string& out, const char* tag) { out.append(tag, 4); };
        const auto put32 = [](std::string& out, u32 value) {
            for (int i = 0; i < 4; ++i) out += static_cast<char>((value >> (8 * i)) & 0xFF);
        };
        const auto put16 = [](std::string& out, u16 value) {
            for (int i = 0; i < 2; ++i) out += static_cast<char>((value >> (8 * i)) & 0xFF);
        };

        const auto data = static_cast<u32>(samples.size() * 2);
        std::string wav;
        wav.reserve(44 + data);
        put(wav, "RIFF"); put32(wav, 36 + data); put(wav, "WAVE");
        put(wav, "fmt "); put32(wav, 16);
        put16(wav, 1);              // PCM
        put16(wav, 1);              // mono
        put32(wav, kRate);
        put32(wav, kRate * 2);      // bytes per second
        put16(wav, 2);              // block align
        put16(wav, 16);             // bits per sample
        put(wav, "data"); put32(wav, data);
        for (const i16 sample : samples) {
            wav += static_cast<char>(sample & 0xFF);
            wav += static_cast<char>((sample >> 8) & 0xFF);
        }

        const std::filesystem::path file = folder / "assets" / "click.wav";
        if (!FileSystem::WriteText(file, wav)) {
            VAE_WARN("example: could not write {}", file.string());
            return;
        }
        // Registered under a name, not a path, because that is what the script says and what the
        // Assets panel shows.
        state.Doc().AddAsset("click", "assets/click.wav");
    }

    void BuildCounterExample(EditorState& state) {
        state.NewProject();
        doc::Document& d = state.Doc();

        const Uuid screen = state.ActiveScreen();
        {
            LayoutStyle& style = Lay(d, screen);
            style.mode = LayoutMode::Stack;
            style.axis = Axis::Column;
            style.gap = 24.0f;
            style.padding = Edges(48.0f);
            style.align = Align::Center;
            style.justify = Justify::Center;
        }
        Token(d, screen, doc::Prop::Fill, "surface");

        // --- the component ---------------------------------------------------------------------
        // Authored outside any screen, the way the standard library builds its widgets: a component
        // is a definition, and what a screen holds is instances of it.
        const Uuid card = d.CreateNode(doc::NodeKind::Frame, Uuid::Invalid(), "Counter");
        {
            LayoutStyle& style = Lay(d, card);
            style.mode = LayoutMode::Stack;
            style.axis = Axis::Column;
            style.gap = 14.0f;
            style.padding = Edges(28.0f, 24.0f);
            style.align = Align::Center;
            style.width = Size::Px(260.0f);
            style.height = Size::Hug();
        }
        Token(d, card, doc::Prop::Fill, "surfaceAlt");
        Token(d, card, doc::Prop::Stroke, "border");
        Set(d, card, doc::Prop::StrokeWidth, 1.0f);
        Set(d, card, doc::Prop::CornerRadius, 12.0f);

        const Uuid title = d.CreateNode(doc::NodeKind::Text, card, "Title");
        Set(d, title, doc::Prop::Text, std::string("Clicks"));
        Set(d, title, doc::Prop::FontSize, 13.0f);
        Set(d, title, doc::Prop::TextWrap, std::string("none"));
        Token(d, title, doc::Prop::TextColor, "textMuted");

        const Uuid count = d.CreateNode(doc::NodeKind::Text, card, "Count");
        Set(d, count, doc::Prop::Text, std::string("0"));
        Set(d, count, doc::Prop::FontSize, 44.0f);
        Set(d, count, doc::Prop::FontWeight, 600.0f);
        Set(d, count, doc::Prop::TextWrap, std::string("none"));
        Token(d, count, doc::Prop::TextColor, "text");

        // The buttons are the library's, placed by name — so the example is also the proof that a
        // component made of other components works, script and all. Their labels are overrides,
        // which is the only way to change one instance without moving every other Button with it.
        const Uuid master = state.Library().Find("Button");
        const Uuid caption = d.Find(master)->children.front();

        const Uuid button = d.CreateInstance(master, card);
        d.Find(button)->name = "Increment";
        d.Touch(button);
        d.SetOverride(button, caption, doc::Prop::Text, std::string("Add one"));

        const Uuid reset = d.CreateInstance(master, card);
        d.Find(reset)->name = "Reset";
        d.Touch(reset);
        d.SetOverride(reset, caption, doc::Prop::Text, std::string("Reset"));
        // The secondary action is quieter: one override on the instance root, no second component.
        d.SetOverride(reset, master, doc::Prop::Fill, doc::TokenRef{ "surface" });
        d.SetOverride(reset, master, doc::Prop::Stroke, doc::TokenRef{ "border" });
        d.SetOverride(reset, master, doc::Prop::StrokeWidth, 1.0f);
        d.SetOverride(reset, caption, doc::Prop::TextColor, doc::TokenRef{ "textMuted" });

        const Uuid component = d.MakeComponent(card, "Counter");

        // --- two of them on the screen ---------------------------------------------------------
        // Two copies, because one copy proves nothing: the point is that they count separately
        // even though both are the same node in the document.
        for (int i = 0; i < 2; ++i) {
            const Uuid instance = d.CreateInstance(component, screen);
            if (!instance.Valid()) continue;
            d.Find(instance)->name = i == 0 ? "Left" : "Right";
            d.Touch(instance);
        }

        {
            // Side by side, so both are visible in one glance.
            LayoutStyle& style = Lay(d, screen);
            style.axis = Axis::Row;
            style.gap = 32.0f;
        }

        state.ClearSelection();
        state.Commands().Clear();
    }

    // The same counter, with its logic drawn instead of written. Three entry points, because a
    // blueprint has no functions to share the "show the number" chain between them — which is exactly
    // what a small Blueprint looks like, and is worth showing rather than hiding.
    void BuildCounterBlueprint(EditorState& state) {
        BuildCounterExample(state);
        doc::Document& d = state.Doc();

        Uuid component = Uuid::Invalid();
        for (const Uuid root : d.Roots())
            if (const doc::Node* node = d.Find(root); node && node->IsComponent()
                && node->name == "Counter") component = root;
        if (!component.Valid()) return;

        doc::Blueprint blueprint;
        blueprint.SetVariable({ "count", doc::ValueType::Number, 0.0f });

        const auto Node = [&](std::string type, Vec2 at, std::string target = {}) {
            doc::BlueprintNode node;
            node.type = std::move(type);
            node.position = at;
            node.target = std::move(target);
            return blueprint.AddNode(std::move(node));
        };
        const auto Lit = [&](u32 id, const char* pin, doc::Value value) {
            if (doc::BlueprintNode* node = blueprint.Find(id)) node->literals[pin] = std::move(value);
        };
        const auto Wire = [&](u32 from, const char* out, u32 to, const char* in) {
            blueprint.AddLink({ 0, from, out, to, in });
        };
        // Every path ends the same way: put the number on screen. Three copies of one node,
        // because that is what a blueprint without functions honestly is.
        const auto Show = [&](Vec2 at) {
            const u32 id = Node("ui.setText", at);
            Lit(id, "Node", std::string("Count"));
            return id;
        };

        // On Mount: show whatever the count already is.
        const u32 mount = Node("event.mount", { -40.0f, 0.0f });
        const u32 mountGet = Node("var.get", { 300.0f, 110.0f }, "count");
        const u32 mountShow = Show({ 560.0f, 0.0f });
        Wire(mount, "Out", mountShow, "In");
        Wire(mountGet, "Value", mountShow, "Value");

        // Add one: make a noise, add one, show it. The Set's own output carries the new value
        // along, which is why nothing has to read the variable back afterwards.
        const u32 clicked = Node("event.clicked", { -40.0f, 240.0f });
        Lit(clicked, "Node", std::string("Increment"));
        const u32 click = Node("sound.play", { 240.0f, 240.0f });
        Lit(click, "Sound", std::string("click"));
        Lit(click, "Volume", 0.6f);
        const u32 get = Node("var.get", { 240.0f, 430.0f }, "count");
        const u32 add = Node("math.add", { 420.0f, 430.0f });
        Lit(add, "B", 1.0f);
        const u32 set = Node("var.set", { 620.0f, 240.0f }, "count");
        const u32 show = Show({ 860.0f, 240.0f });
        Wire(clicked, "Out", click, "In");
        Wire(click, "Out", set, "In");
        Wire(get, "Value", add, "A");
        Wire(add, "Value", set, "Value");
        Wire(set, "Out", show, "In");
        Wire(set, "Value", show, "Value");

        // Reset: the same shape, with nothing to add.
        const u32 reset = Node("event.clicked", { -40.0f, 620.0f });
        Lit(reset, "Node", std::string("Reset"));
        const u32 resetClick = Node("sound.play", { 240.0f, 620.0f });
        Lit(resetClick, "Sound", std::string("click"));
        Lit(resetClick, "Volume", 0.6f);
        const u32 zero = Node("var.set", { 620.0f, 620.0f }, "count");
        const u32 resetShow = Show({ 860.0f, 620.0f });
        Wire(reset, "Out", resetClick, "In");
        Wire(resetClick, "Out", zero, "In");
        Wire(zero, "Out", resetShow, "In");
        Wire(zero, "Value", resetShow, "Value");

        blueprint.comments.push_back({ 0, "counting", { -80.0f, 190.0f }, { 1080.0f, 340.0f } });
        blueprint.comments.back().id = blueprint.MintId();

        d.SetBlueprint(component, std::move(blueprint));
        state.ClearSelection();
        state.Commands().Clear();
    }

    std::string_view CounterExampleLua() {
        return R"(-- The example project's logic.
--
-- One class, bound to the Counter component by name. Both copies on the screen run it, and each
-- gets its own `self` and its own state bag — which is why they count separately.

vae.component("Counter", {
    on_mount = function(self) self:show() end,

    on_event = function(self, event)
        if event.kind ~= "clicked" then return end

        -- "click" is the name in the Assets panel, not a path: the file is an implementation
        -- detail of importing it, and the name is what the project is written against.
        self:play_sound("click", 0.6)

        if event.source == "Increment" then
            self:set_state("count", self:state("count") + 1)
        elseif event.source == "Reset" then
            self:set_state("count", 0)
        end
        self:show()
    end,

    show = function(self)
        self:set_text("Count", "text", tostring(math.floor(self:state("count"))))
    end,
})
)";
    }

    std::string_view CounterExampleCpp() {
        return R"(// The example project's logic.
//
// One header, no engine internals: everything crosses the boundary as a C function table, so this
// file compiles in milliseconds and keeps compiling when the engine changes underneath it.

#include <vae/script/VaeScript.h>

#include <string>

struct Counter : vae::Script {
    void OnMount() override { Show(); }

    void OnEvent(const vae::Event& event) override {
        if (!event.Is(VAE_EVENT_CLICKED)) return;
        // "click" is the name in the Assets panel, not a path: the file is an implementation
        // detail of importing it, and the name is what the project is written against.
        self.PlaySound("click", 0.6);

        if (event.Clicked("Increment")) { self.SetState("count", Count() + 1); Show(); }
        if (event.Clicked("Reset"))     { self.SetState("count", 0);           Show(); }
    }

    double Count() const { return self.State("count"); }
    void Show() { self["Count"].SetText("text", std::to_string(static_cast<int>(Count()))); }
};

VAE_SCRIPT(Counter, "Counter")
)";
    }

    namespace {

        // A button, placed and captioned in one go. The example is about the screens, not about
        // repeating six lines per control.
        Uuid Button(doc::Document& d, const ui::Library& library, Uuid parent, std::string name,
                    std::string caption, const char* goTo = nullptr) {
            const Uuid master = library.Find("Button");
            const Uuid instance = d.CreateInstance(master, parent);
            d.Find(instance)->name = std::move(name);
            d.Touch(instance);
            d.SetOverride(instance, d.Find(master)->children.front(), doc::Prop::Text,
                          std::move(caption));
            if (goTo) d.SetOverride(instance, master, doc::Prop::GoTo, std::string(goTo));
            return instance;
        }

        Uuid Label(doc::Document& d, Uuid parent, std::string name, std::string text, f32 size,
                   const char* colour) {
            const Uuid id = d.CreateNode(doc::NodeKind::Text, parent, std::move(name));
            Set(d, id, doc::Prop::Text, std::move(text));
            Set(d, id, doc::Prop::FontSize, size);
            Set(d, id, doc::Prop::TextWrap, std::string("none"));
            Token(d, id, doc::Prop::TextColor, colour);
            return id;
        }

        void Column(doc::Document& d, Uuid node, f32 gap, Edges padding,
                    layout::Align align = layout::Align::Start) {
            LayoutStyle& style = Lay(d, node);
            style.mode = LayoutMode::Stack;
            style.axis = Axis::Column;
            style.gap = gap;
            style.padding = padding;
            style.align = align;
        }

    }

    void BuildScreensExample(EditorState& state) {
        state.NewProject();
        doc::Document& d = state.Doc();
        const ui::Library& library = state.Library();

        // --- the list ---------------------------------------------------------------------------
        const Uuid list = state.ActiveScreen();
        d.Find(list)->name = "List";
        Column(d, list, 12.0f, Edges(48.0f, 40.0f));
        Token(d, list, doc::Prop::Fill, "bg");

        Label(d, list, "Title", "Inbox", 28.0f, "text");
        Label(d, list, "Hint", "Pick one. The button says where it goes; no script needed.",
              13.0f, "textMuted");

        // Each row navigates by declaration, and the script only notes which one was picked.
        static const char* kRows[] = { "Design review", "Invoice #4021", "Weekend plans" };
        for (const char* row : kRows)
            Button(d, library, list, row, row, "Detail");

        // --- the detail -------------------------------------------------------------------------
        const Uuid detail = state.AddScreen("Detail", { 1280.0f, 800.0f });
        Column(d, detail, 16.0f, Edges(48.0f, 40.0f));
        Token(d, detail, doc::Prop::Fill, "bg");

        Label(d, detail, "Subject", "—", 28.0f, "text");
        Label(d, detail, "Body", "One screen replaced the other, and Back brings it back.",
              14.0f, "textMuted");
        {
            const Uuid actions = d.CreateNode(doc::NodeKind::Frame, detail, "Actions");
            LayoutStyle& style = Lay(d, actions);
            style.mode = LayoutMode::Stack;
            style.axis = Axis::Row;
            style.gap = 12.0f;
            Button(d, library, actions, "Back", "Back", "back");
            const Uuid remove = Button(d, library, actions, "Delete", "Delete", "Confirm");
            d.SetOverride(remove, library.Find("Button"), doc::Prop::Fill, doc::TokenRef{ "danger" });
        }

        // --- the alert --------------------------------------------------------------------------
        // A screen like any other, except for its kind — which is what makes it block the screen
        // underneath, sit in the middle of it, and refuse to dismiss itself.
        const Uuid confirm = state.AddScreen("Confirm", { 380.0f, 170.0f });
        Set(d, confirm, doc::Prop::ScreenKind, std::string("alert"));
        Column(d, confirm, 14.0f, Edges(24.0f, 20.0f));
        Token(d, confirm, doc::Prop::Fill, "surface");
        Token(d, confirm, doc::Prop::Stroke, "border");
        Set(d, confirm, doc::Prop::StrokeWidth, 1.0f);
        Set(d, confirm, doc::Prop::CornerRadius, 12.0f);
        Set(d, confirm, doc::Prop::ShadowColor, Color{ 0.0f, 0.0f, 0.0f, 0.35f });
        Set(d, confirm, doc::Prop::ShadowBlur, 32.0f);

        Label(d, confirm, "Question", "Delete this item?", 17.0f, "text");
        Label(d, confirm, "Detail", "An alert stays until it is answered.", 13.0f, "textMuted");
        {
            const Uuid choices = d.CreateNode(doc::NodeKind::Frame, confirm, "Choices");
            LayoutStyle& style = Lay(d, choices);
            style.mode = LayoutMode::Stack;
            style.axis = Axis::Row;
            style.gap = 10.0f;
            style.justify = layout::Justify::End;
            style.width = layout::Size::Fill();

            const Uuid cancel = Button(d, library, choices, "Cancel", "Cancel", "back");
            d.SetOverride(cancel, library.Find("Button"), doc::Prop::Fill, doc::TokenRef{ "surfaceAlt" });
            const Uuid confirmed = Button(d, library, choices, "Confirmed", "Delete");
            d.SetOverride(confirmed, library.Find("Button"), doc::Prop::Fill, doc::TokenRef{ "danger" });
        }

        d.SetStartScreen(list);
        state.SetActiveScreen(list);
        state.ClearSelection();
        state.Commands().Clear();
    }

    std::string_view ScreensExampleLua() {
        return R"(-- Three screens, and the relations between them.
--
-- Most of the wiring is not here: each row's "go to Detail" and the Back button are properties on
-- the buttons, because a screen that leads to another screen needs no logic. What is here is the
-- part that does: remembering which row was picked, and what happens when the alert is answered.

vae.component("List", {
    on_event = function(self, event)
        if event.kind == "clicked" then self:set_stored_text("subject", event.source) end
    end,
})

vae.component("Detail", {
    on_mount = function(self)
        self:set_text("Subject", "text", self:stored_text("subject", "Nothing selected"))
    end,
})

vae.component("Confirm", {
    on_event = function(self, event)
        if event.kind == "clicked" and event.source == "Confirmed" then
            self:set_stored_text("subject", "Deleted")
            self:back()   -- closes the alert
            self:back()   -- and leaves the detail behind it
        end
    end,
})
)";
    }

    std::string_view ScreensExampleCpp() {
        return R"(// Three screens, and the relations between them.
//
// Most of the wiring is not here: each row's "go to Detail" and the Back button are properties on
// the buttons, because a screen that leads to another screen needs no logic. What is here is the
// part that does.

#include <vae/script/VaeScript.h>

#include <string>

struct List : vae::Script {
    void OnEvent(const vae::Event& event) override {
        if (event.kind == VAE_EVENT_CLICKED) self.Store("subject", std::string(event.source));
    }
};

VAE_SCRIPT(List, "List")

struct Detail : vae::Script {
    void OnMount() override {
        self["Subject"].SetText("text", self.StoredText("subject", "Nothing selected"));
    }
};

VAE_SCRIPT(Detail, "Detail")

struct Confirm : vae::Script {
    void OnEvent(const vae::Event& event) override {
        if (!event.Clicked("Confirmed")) return;
        self.Store("subject", std::string("Deleted"));
        self.Back();   // closes the alert
        self.Back();   // and leaves the detail behind it
    }
};

VAE_SCRIPT(Confirm, "Confirm")
)";
    }


    void BuildFeedExample(EditorState& state) {
        state.NewProject();
        doc::Document& d = state.Doc();
        const ui::Library& library = state.Library();

        const Uuid screen = state.ActiveScreen();
        d.Find(screen)->name = "Feed";
        Column(d, screen, 16.0f, Edges(48.0f, 40.0f));
        Token(d, screen, doc::Prop::Fill, "bg");

        Label(d, screen, "Title", "Prices", 28.0f, "text");
        Label(d, screen, "Hint",
              "One screen, four states. Which one is true is a property, not three hidden frames.",
              13.0f, "textMuted");

        {
            const Uuid actions = d.CreateNode(doc::NodeKind::Frame, screen, "Actions");
            LayoutStyle& style = Lay(d, actions);
            style.mode = LayoutMode::Stack;
            style.axis = Axis::Row;
            style.gap = 10.0f;
            Button(d, library, actions, "Reload", "Reload");
            Button(d, library, actions, "Listen", "Listen");
        }

        // The four drawings of one panel. `shown` names which is on screen, so a designer can look
        // at the failure without breaking anything and a script can switch to it with one write.
        const Uuid panel = d.CreateNode(doc::NodeKind::Frame, screen, "Panel");
        {
            LayoutStyle& style = Lay(d, panel);
            style.mode = LayoutMode::Stack;
            style.axis = Axis::Column;
            style.width = layout::Size::Fill();
            style.height = layout::Size::Px(360.0f);
        }
        Set(d, panel, doc::Prop::Shown, std::string("Loading"));

        {
            const Uuid loading = d.CreateNode(doc::NodeKind::Frame, panel, "Loading");
            LayoutStyle& style = Lay(d, loading);
            style.mode = LayoutMode::Stack;
            style.axis = Axis::Row;
            style.gap = 10.0f;
            style.align = layout::Align::Center;
            style.width = layout::Size::Fill();
            d.CreateInstance(library.Find("Spinner"), loading);
            Label(d, loading, "Says", "Asking the server…", 14.0f, "textMuted");
        }
        {
            const Uuid failed = d.CreateNode(doc::NodeKind::Frame, panel, "Failed");
            LayoutStyle& style = Lay(d, failed);
            style.mode = LayoutMode::Stack;
            style.axis = Axis::Column;
            style.gap = 8.0f;
            style.width = layout::Size::Fill();
            Label(d, failed, "Says", "The server did not answer.", 15.0f, "danger");
            Label(d, failed, "Why", "—", 13.0f, "textMuted");
        }
        {
            const Uuid empty = d.CreateNode(doc::NodeKind::Frame, panel, "Empty");
            Lay(d, empty).width = layout::Size::Fill();
            Label(d, empty, "Says", "Nothing to show yet.", 15.0f, "textMuted");
        }
        {
            const Uuid content = d.CreateNode(doc::NodeKind::Frame, panel, "Content");
            LayoutStyle& style = Lay(d, content);
            style.mode = LayoutMode::Stack;
            style.axis = Axis::Column;
            style.gap = 10.0f;
            style.width = layout::Size::Fill();
            style.height = layout::Size::Fill();

            // A table the script fills. Nobody built a node per row: the designer styled one, and
            // the widget virtualizes the rest.
            const Uuid rows = d.CreateInstance(library.Find("Table"), content);
            d.Find(rows)->name = "Rows";
            d.Touch(rows);
            Lay(d, rows).width = layout::Size::Fill();
            Lay(d, rows).height = layout::Size::Fill();

            Label(d, content, "Live", "not listening", 12.0f, "textMuted");
        }

        d.SetStartScreen(screen);
        state.SetActiveScreen(screen);
        state.ClearSelection();
        state.Commands().Clear();
    }

    std::string_view FeedExampleLua() {
        return R"(-- A screen that talks to something.
--
-- Run a server for it to talk to and the table fills:
--   python3 -m http.server 8000        (in a folder holding a file called `rows`)
-- Without one, this is a working demonstration of the failure state, which is the state most
-- apps are least prepared for.
--
-- Nothing here builds a node. The four states are one property on the panel, and the rows are
-- handed to the table the designer already styled.

local URL = "http://127.0.0.1:8000/rows"

vae.component("Feed", {
    on_mount = function(self)
        self:set_text("Panel", "shown", "Loading")
        self:get(URL, "rows")
    end,

    on_event = function(self, event)
        if event.kind == "clicked" and event.source == "Reload" then
            self:set_text("Panel", "shown", "Loading")
            self:get(URL, "rows")
        end

        if event.kind == "clicked" and event.source == "Listen" then
            self:socket_open("ws://127.0.0.1:8001/live", "prices")
            self:set_text("Live", "text", "connecting…")
        end

        if event.kind == "http" and event.name == "rows" then
            if event.number < 200 or event.number >= 300 then
                self:set_text("Failed.Why", "text", event.text)
                self:set_text("Panel", "shown", "Failed")
                return
            end

            -- One row per line, one cell per comma. A real app would parse JSON; the point here is
            -- that whatever it parses ends up as rows on a table it did not have to build.
            local rows = {}
            for line in (event.text .. "\n"):gmatch("([^\n]*)\n") do
                if line ~= "" then
                    local cells = {}
                    for cell in (line .. ","):gmatch("([^,]*),") do cells[#cells + 1] = cell end
                    rows[#rows + 1] = cells
                end
            end

            if #rows == 0 then
                self:set_text("Panel", "shown", "Empty")
            else
                self:set_rows("Rows", rows)
                self:set_text("Panel", "shown", "Content")
            end
        end

        if event.kind == "socketOpen" then self:set_text("Live", "text", "listening") end
        if event.kind == "socketMessage" then
            self:set_text("Live", "text", "pushed: " .. event.text)
        end
        if event.kind == "socketClosed" then
            self:set_text("Live", "text", event.number > 0 and "the feed refused" or "not listening")
        end
    end,
})
)";
    }

    std::string_view FeedExampleCpp() {
        return R"(#include <vae/script/VaeScript.h>

#include <string>
#include <vector>

// A screen that talks to something. Run a server for it to talk to and the table fills:
//   python3 -m http.server 8000        (in a folder holding a file called `rows`)
// Without one, this is a working demonstration of the failure state.

namespace {
    const char* kUrl = "http://127.0.0.1:8000/rows";

    std::vector<std::vector<std::string>> Parse(const std::string& body) {
        std::vector<std::vector<std::string>> rows;
        std::size_t at = 0;
        while (at < body.size()) {
            const std::size_t end = std::min(body.find('\n', at), body.size());
            const std::string line = body.substr(at, end - at);
            at = end + 1;
            if (line.empty()) continue;

            std::vector<std::string> cells;
            std::size_t cell = 0;
            while (cell <= line.size()) {
                const std::size_t comma = std::min(line.find(',', cell), line.size());
                cells.push_back(line.substr(cell, comma - cell));
                cell = comma + 1;
            }
            rows.push_back(std::move(cells));
        }
        return rows;
    }
}

struct Feed : vae::Script {
    void OnMount() override { Load(); }

    void Load() {
        self["Panel"].SetText("shown", "Loading");
        self.Get(kUrl, "rows");
    }

    void OnEvent(const vae::Event& event) override {
        if (event.Clicked("Reload")) Load();
        if (event.Clicked("Listen")) {
            self.OpenSocket("ws://127.0.0.1:8001/live", "prices");
            self["Live"].SetText("text", "connecting…");
        }

        if (event.Answered("rows")) {
            if (!event.Ok()) {
                self["Failed.Why"].SetText("text", event.text);
                self["Panel"].SetText("shown", "Failed");
                return;
            }
            const auto rows = Parse(event.text);
            if (rows.empty()) {
                self["Panel"].SetText("shown", "Empty");
            } else {
                self["Rows"].SetRows(rows);
                self["Panel"].SetText("shown", "Content");
            }
        }

        if (event.Is(VAE_EVENT_SOCKET_OPEN)) self["Live"].SetText("text", "listening");
        if (event.Is(VAE_EVENT_SOCKET_MESSAGE))
            self["Live"].SetText("text", std::string("pushed: ") + event.text);
        if (event.Is(VAE_EVENT_SOCKET_CLOSED))
            self["Live"].SetText("text", event.number > 0.0 ? "the feed refused" : "not listening");
    }
};

VAE_SCRIPT(Feed, "Feed")
)";
    }

}
