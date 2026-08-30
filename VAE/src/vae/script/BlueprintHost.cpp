#include "vaepch.h"
#include "vae/script/BlueprintHost.h"

#include "vae/doc/Serializer.h"
#include "vae/doc/ValueText.h"
#include "vae/ui/Library.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <random>

namespace vae::script {

    using namespace vae::doc;

    namespace {

        // How many nodes one event may run through before the blueprint is declared stuck. Unreal's
        // number, and for Unreal's reason: it is far above anything a real blueprint does and far below
        // "the app has hung". A For Loop over ten thousand rows doing ten things each is a hundred
        // thousand steps, and still an order of magnitude clear of this.
        constexpr u32 kStepBudget = 1'000'000;
        // How deeply a blueprint may nest — a Sequence inside a loop inside a branch. This bounds the
        // C++ stack, which a straight run of nodes does not touch because Continue is iterative.
        constexpr u32 kMaxDepth = 256;

        std::string NumberText(double value) {
            // A whole number reads as a whole number. "3" is what a label should say, and
            // "3.000000" is what every naive conversion says instead.
            if (std::isfinite(value) && value == std::floor(value) && std::abs(value) < 1e15) {
                return std::to_string(static_cast<long long>(value));
            }
            char buffer[40];
            const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
            return error == std::errc() ? std::string(buffer, end) : std::string("0");
        }

        double TextNumber(std::string_view text) {
            const std::string owned(text);
            char* end = nullptr;
            const double value = std::strtod(owned.c_str(), &end);
            return end == owned.c_str() ? 0.0 : value;
        }

        bool TextTruth(std::string_view text) {
            return !(text.empty() || text == "0" || text == "no" || text == "off"
                     || text == "false");
        }

        std::string MemoKey(u32 node, std::string_view pin) {
            return std::to_string(node) + "\n" + std::string(pin);
        }

        std::string Trim(std::string_view text) {
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) return {};
            const auto last = text.find_last_not_of(" \t\r\n");
            return std::string(text.substr(first, last - first + 1));
        }

        const doc::PinSpec* FindPin(const std::vector<doc::PinSpec>& pins, std::string_view name) {
            for (const doc::PinSpec& pin : pins) if (pin.name == name) return &pin;
            return nullptr;
        }

        // A repeated container's rows, as a script hands them over. The blueprint says them as text
        // because a blueprint has no table type — the same text a designer types into the Sample field
        // in the inspector, which is the point: one spelling of a table, learned once.
        void HandRows(const VaeScriptAPI& api, VaeInstance handle, const char* node,
                      std::string_view text) {
            const RowTable table = ParseRowText(text);
            if (table.columns.empty()) { api.clear_rows(handle, node); return; }
            std::vector<const char*> columns;
            columns.reserve(table.columns.size());
            for (const std::string& column : table.columns) columns.push_back(column.c_str());
            std::vector<const char*> cells;
            cells.reserve(table.cells.size());
            for (const std::string& cell : table.cells) cells.push_back(cell.c_str());
            api.set_named_rows(handle, node, columns.data(), static_cast<int>(columns.size()),
                               cells.data(), static_cast<int>(table.Count()));
        }

    }

    // --- values ------------------------------------------------------------------------------

    BlueprintHost::Datum BlueprintHost::Datum::OfBool(bool v) {
        Datum out; out.type = PinType::Bool; out.boolean = v; return out;
    }
    BlueprintHost::Datum BlueprintHost::Datum::OfNumber(double v) {
        Datum out; out.type = PinType::Number; out.number = v; return out;
    }
    BlueprintHost::Datum BlueprintHost::Datum::OfText(std::string v) {
        Datum out; out.type = PinType::Text; out.text = std::move(v); return out;
    }
    BlueprintHost::Datum BlueprintHost::Datum::OfColour(VaeColor v) {
        Datum out; out.type = PinType::Colour; out.colour = v; return out;
    }

    BlueprintHost::Datum BlueprintHost::Datum::Of(const doc::Value& value, doc::PinType type) {
        switch (type) {
            case PinType::Bool:
                return OfBool(std::get_if<bool>(&value) ? std::get<bool>(value) : false);
            case PinType::Number:
                return OfNumber(std::get_if<f32>(&value) ? std::get<f32>(value) : 0.0);
            case PinType::Colour: {
                if (const vae::Color* colour = std::get_if<vae::Color>(&value))
                    return OfColour({ colour->r, colour->g, colour->b, colour->a });
                return OfColour({ 1, 1, 1, 1 });
            }
            default: break;
        }
        const std::string* text = std::get_if<std::string>(&value);
        return OfText(text ? *text : std::string());
    }

    bool BlueprintHost::Datum::AsBool() const {
        switch (type) {
            case PinType::Bool:   return boolean;
            case PinType::Number: return number != 0.0;
            case PinType::Text:   return TextTruth(text);
            case PinType::Colour: return colour.a > 0.0f;
            default: break;
        }
        return false;
    }

    double BlueprintHost::Datum::AsNumber() const {
        switch (type) {
            case PinType::Bool:   return boolean ? 1.0 : 0.0;
            case PinType::Number: return number;
            case PinType::Text:   return TextNumber(text);
            default: break;
        }
        return 0.0;
    }

    std::string BlueprintHost::Datum::AsText() const {
        switch (type) {
            case PinType::Bool:   return boolean ? "true" : "false";
            case PinType::Number: return NumberText(number);
            case PinType::Text:   return text;
            case PinType::Colour: {
                const vae::Color hex{ colour.r, colour.g, colour.b, colour.a };
                return ::vae::doc::text::ColorToHex(hex).value_or("#ffffffff");
            }
            default: break;
        }
        return {};
    }

    VaeColor BlueprintHost::Datum::AsColour() const {
        if (type == PinType::Colour) return colour;
        if (type == PinType::Text)
            if (const auto parsed = ::vae::doc::text::ColorFromHex(text))
                return { parsed->r, parsed->g, parsed->b, parsed->a };
        return { 1, 1, 1, 1 };
    }

    BlueprintHost::Datum BlueprintHost::Datum::As(doc::PinType want) const {
        switch (want) {
            case PinType::Bool:   return OfBool(AsBool());
            case PinType::Number: return OfNumber(AsNumber());
            case PinType::Text:   return OfText(AsText());
            case PinType::Colour: return OfColour(AsColour());
            default: break;
        }
        return *this;
    }

    // --- the host ------------------------------------------------------------------------------

    BlueprintHost::BlueprintHost() = default;
    BlueprintHost::~BlueprintHost() = default;

    void BlueprintHost::Bind(const VaeScriptAPI& api) { m_Api = api; m_Bound = true; }

    void BlueprintHost::Adopt(const doc::Document& document) {
        m_Source = &document;
        m_Classes.clear();
        m_Messages.clear();
        m_Live.clear();

        // Deterministic order, so two runs of the same project report the same diagnostics in the
        // same order. The map is keyed by Uuid, which is not an order anyone can predict.
        std::vector<std::pair<std::string, const Blueprint*>> found;
        for (const auto& [id, blueprint] : document.Blueprints()) {
            const doc::Node* node = document.Find(id);
            if (!node) continue;
            if (node->kind != NodeKind::Component && node->kind != NodeKind::Screen) continue;
            if (node->name.empty()) continue;
            found.emplace_back(node->name, &blueprint);
        }
        std::ranges::sort(found, [](const auto& a, const auto& b) { return a.first < b.first; });

        for (const auto& [name, blueprint] : found) {
            auto entry = std::make_unique<Class>();
            entry->component = name;
            entry->program.Compile(*blueprint, name);
            for (const BlueprintProgram::Diagnostic& d : entry->program.Diagnostics())
                m_Messages.push_back({ name, d.node, d.error, d.message });
            // A blueprint that does not compile is not mounted at all. Half of it running is worse
            // than none of it: the half that ran left the screen in a state nobody drew.
            if (!entry->program.Ok()) continue;
            entry->klass.component  = entry->component.c_str();
            entry->klass.on_mount   = &BlueprintHost::MountThunk;
            entry->klass.on_update  = entry->program.HasEntry("event.update")
                                    ? &BlueprintHost::UpdateThunk : nullptr;
            entry->klass.on_event   = &BlueprintHost::EventThunk;
            entry->klass.on_unmount = &BlueprintHost::UnmountThunk;
            m_Classes.push_back(std::move(entry));
        }
    }

    bool BlueprintHost::Load(const std::filesystem::path& path, std::string* error) {
        Document document;
        std::string why;
        const bool project = Project::IsProjectFile(path)
                          || (std::filesystem::is_directory(path) && !Project::FileIn(path).empty());
        bool read = false;
        if (project) {
            doc::Project settings;
            const std::filesystem::path file =
                std::filesystem::is_directory(path) ? Project::FileIn(path) : path;
            read = Project::LoadDocument(file, document, settings, &why, &ui::StandardLibrary());
        } else {
            read = Serializer::Load(path, document, &why, &ui::StandardLibrary());
        }
        if (!read) {
            if (error) *error = why;
            return false;
        }
        m_Path = path;
        m_Loaded = path;
        Adopt(document);
        // The document was a local: nothing may reach for it later.
        m_Source = nullptr;
        if (ErrorCount() > 0 && error) {
            *error = m_Messages.front().component + ": " + m_Messages.front().message;
            return false;
        }
        return true;
    }

    bool BlueprintHost::Reload(std::string* error) {
        if (!m_Loaded.empty()) return Load(m_Loaded, error);
        if (m_Source) { Adopt(*m_Source); return ErrorCount() == 0; }
        if (error) *error = "there is no blueprint to reload";
        return false;
    }

    void BlueprintHost::Unload() {
        m_Classes.clear();
        m_Live.clear();
        m_Messages.clear();
        m_Source = nullptr;
        m_Loaded.clear();
        ClearWatch();
    }

    const VaeScriptClass* BlueprintHost::Find(std::string_view component) const {
        for (const auto& entry : m_Classes)
            if (entry->component == component) return &entry->klass;
        return nullptr;
    }

    std::vector<std::string> BlueprintHost::Components() const {
        std::vector<std::string> out;
        out.reserve(m_Classes.size());
        for (const auto& entry : m_Classes) out.push_back(entry->component);
        return out;
    }

    std::size_t BlueprintHost::ErrorCount() const {
        return static_cast<std::size_t>(
            std::ranges::count_if(m_Messages, [](const Message& m) { return m.error; }));
    }

    const BlueprintProgram* BlueprintHost::ProgramFor(std::string_view component) const {
        for (const auto& entry : m_Classes)
            if (entry->component == component) return &entry->program;
        return nullptr;
    }

    std::vector<BlueprintHost::Flow> BlueprintHost::TakeFlow() { return std::move(m_Flow); }

    std::string BlueprintHost::WatchKey(std::string_view component, u32 node, std::string_view pin) {
        return std::string(component) + "\n" + std::to_string(node) + "\n" + std::string(pin);
    }

    void BlueprintHost::ClearWatch() { m_Flow.clear(); m_Values.clear(); m_WatchStep = 0; }

    // --- the four entry points ------------------------------------------------------------------

    BlueprintHost* BlueprintHost::For(VaeInstance handle) {
        Runtime* runtime = Runtime::Owner(handle);
        if (!runtime) return nullptr;
        for (const Scope<Host>& host : runtime->Hosts())
            if (auto* blueprint = dynamic_cast<BlueprintHost*>(host.get())) return blueprint;
        return nullptr;
    }

    void BlueprintHost::MountThunk(VaeInstance handle) {
        if (BlueprintHost* host = For(handle)) host->Mount(handle);
    }
    void BlueprintHost::UpdateThunk(VaeInstance handle, double dt) {
        if (BlueprintHost* host = For(handle)) host->Update(handle, dt);
    }
    void BlueprintHost::EventThunk(VaeInstance handle, const VaeEvent* event) {
        if (BlueprintHost* host = For(handle)) host->Event(handle, event);
    }
    void BlueprintHost::UnmountThunk(VaeInstance handle) {
        if (BlueprintHost* host = For(handle)) host->Unmount(handle);
    }

    const BlueprintProgram* BlueprintHost::ProgramOf(VaeInstance handle) const {
        const auto it = m_Live.find(handle);
        return it == m_Live.end() ? nullptr : it->second.program;
    }

    void BlueprintHost::Mount(VaeInstance handle) {
        if (!m_Bound) return;
        const char* component = m_Api.component_name(handle);
        const BlueprintProgram* program = nullptr;
        for (const auto& entry : m_Classes)
            if (entry->component == component) { program = &entry->program; break; }
        if (!program) return;
        m_Live[handle] = { program };

        // A variable starts at its default, once. `has_state` is what makes a hot reload keep the
        // number that is on screen: the bag survives the reload, and re-seeding it would put the
        // counter back to zero every time the blueprint was saved.
        for (const BlueprintVariable& variable : program->Blueprint().variables) {
            if (m_Api.has_state(handle, variable.name.c_str())) continue;
            const Datum value = Datum::Of(variable.defaultValue,
                                                    PinTypeOf(variable.type));
            if (value.type == PinType::Number)
                m_Api.set_state_number(handle, variable.name.c_str(), value.number);
            else
                m_Api.set_state_text(handle, variable.name.c_str(), value.AsText().c_str());
        }

        Fire(handle, *program, program->Entries("event.mount"), nullptr, 0.0);
    }

    void BlueprintHost::Update(VaeInstance handle, double dt) {
        const BlueprintProgram* program = ProgramOf(handle);
        if (!program) return;
        Fire(handle, *program, program->Entries("event.update"), nullptr, dt);
    }

    void BlueprintHost::Unmount(VaeInstance handle) {
        if (const BlueprintProgram* program = ProgramOf(handle))
            Fire(handle, *program, program->Entries("event.unmount"), nullptr, 0.0);
        m_Live.erase(handle);
    }

    void BlueprintHost::Event(VaeInstance handle, const VaeEvent* event) {
        const BlueprintProgram* program = ProgramOf(handle);
        if (!program || !event) return;

        const std::string source = event->source ? event->source : "";
        const std::string name   = event->name   ? event->name   : "";
        const std::string list   = event->list   ? event->list   : "";

        const auto Deliver = [&](std::string_view type, std::string_view target) {
            Fire(handle, *program, program->EntriesFor(type, target), event, 0.0);
        };

        switch (static_cast<VaeEventKind>(event->kind)) {
            case VAE_EVENT_CLICKED:
                // A click inside a list is both: the row it landed in, and the widget it landed
                // on. A blueprint may answer either, and a row template drawn once is why.
                if (event->row >= 0 && !list.empty()) Deliver("event.rowClicked", list);
                Deliver("event.clicked", source);
                break;
            case VAE_EVENT_VALUE_CHANGED:
            case VAE_EVENT_TEXT_CHANGED:      Deliver("event.changed", source); break;
            case VAE_EVENT_SUBMITTED:         Deliver("event.submitted", source); break;
            case VAE_EVENT_SIGNAL:            Deliver("event.signal", name); break;
            case VAE_EVENT_HTTP:              Deliver("event.answer", name); break;
            case VAE_EVENT_SOCKET_OPEN:       Deliver("event.socketOpen", name); break;
            case VAE_EVENT_SOCKET_MESSAGE:    Deliver("event.socketMessage", name); break;
            case VAE_EVENT_SOCKET_CLOSED:     Deliver("event.socketClosed", name); break;
            case VAE_EVENT_OPENED:            Deliver("event.opened", ""); break;
            case VAE_EVENT_CLOSED:            Deliver("event.closed", ""); break;
            case VAE_EVENT_DISMISSED:         Deliver("event.dismissed", ""); break;
            case VAE_EVENT_TIMER: {
                // A Delay is a timer with a name nobody typed. When it comes due, the blueprint picks
                // up at the node that started it rather than at an event — which is what makes it
                // a pause in the middle of a chain instead of a second entry point.
                const std::string prefix = "#delay.";
                if (name.starts_with(prefix)) {
                    const u32 node = static_cast<u32>(std::strtoul(name.c_str() + prefix.size(),
                                                                   nullptr, 10));
                    Step step{ handle, program, event, 0.0 };
                    Continue(step, node, "Done");
                    break;
                }
                Deliver("event.timer", name);
                break;
            }
            default: break;
        }
    }

    void BlueprintHost::Fire(VaeInstance handle, const BlueprintProgram& program,
                         const std::vector<u32>& entries, const VaeEvent* event, double delta) {
        for (const u32 entry : entries) {
            Step step{ handle, &program, event, delta };
            Continue(step, entry, "Out");
        }
    }

    // --- walking it ------------------------------------------------------------------------------

    void BlueprintHost::Continue(Step& step, u32 node, std::string_view pin) {
        if (step.stopped) return;
        if (++step.depth > kMaxDepth) {
            Say(step.handle, VAE_LOG_ERROR,
                "this blueprint nests deeper than " + std::to_string(kMaxDepth) + " levels");
            step.stopped = true;
            return;
        }

        const doc::Blueprint& blueprint = step.program->Blueprint();
        const BlueprintLink* link = blueprint.LinkOutOf(node, pin);
        while (link) {
            if (++step.steps > kStepBudget) {
                // Unreal's infinite-loop guard, and the same message: a blueprint that runs a million
                // nodes for one click is looping, and stopping it with a line in the console is
                // the only outcome that leaves the app usable.
                Say(step.handle, VAE_LOG_ERROR,
                    "blueprint stopped: it ran " + std::to_string(kStepBudget)
                    + " nodes without finishing, which means something loops for ever");
                step.stopped = true;
                break;
            }
            if (m_Watching) m_Flow.push_back({ step.program->Component(), link->id });

            const BlueprintNode* target = blueprint.Find(link->to);
            if (!target) break;
            const std::string next = Run(step, *target, link->toPin);
            if (step.stopped || next.empty()) break;
            link = blueprint.LinkOutOf(target->id, next);
        }
        --step.depth;
    }

    std::string BlueprintHost::Run(Step& step, const BlueprintNode& node, std::string_view entry) {
        // A statement asks its inputs afresh. Everything worked out for the previous statement is
        // an answer to a question that has already been answered, and keeping it is how a Get
        // after a loop reads the value from before the loop.
        step.memo.clear();

        const VaeScriptAPI& api = m_Api;
        VaeInstance handle = step.handle;
        const std::string& type = node.type;

        const auto Get = [&](const char* pin) { return ReadNamed(step, node, pin); };
        const auto Node = [&] { return ReadNamed(step, node, "Node").AsText(); };
        const auto Prop = [&] { return ReadNamed(step, node, "Property").AsText(); };

        // ---- flow --------------------------------------------------------------------------
        if (type == "flow.branch") return Get("Condition").AsBool() ? "True" : "False";

        if (type == "flow.sequence") {
            for (const PinSpec& pin : BlueprintOutputs(step.program->Blueprint(), node)) {
                Continue(step, node.id, pin.name);
                if (step.stopped) break;
            }
            return {};
        }

        if (type == "flow.forLoop") {
            const double first = Get("First").AsNumber();
            const double last  = Get("Last").AsNumber();
            for (double i = first; i <= last; i += 1.0) {
                step.loopIndex[node.id] = i;
                Continue(step, node.id, "Body");
                if (step.stopped) break;
            }
            step.loopIndex.erase(node.id);
            return "Done";
        }

        if (type == "flow.while") {
            while (!step.stopped) {
                // The condition is a fresh question every time round, which is the difference
                // between a loop that ends and one that does not.
                step.memo.clear();
                if (!Get("Condition").AsBool()) break;
                Continue(step, node.id, "Body");
                if (++step.steps > kStepBudget) {
                    Say(handle, VAE_LOG_ERROR, "blueprint stopped: a While Loop never ends");
                    step.stopped = true;
                    break;
                }
            }
            return "Done";
        }

        if (type == "flow.doOnce") {
            const std::string key = InternalKey("once", node.id);
            if (entry == "Reset") { api.set_state_number(handle, key.c_str(), 0.0); return {}; }
            if (api.state_number(handle, key.c_str(), 0.0) != 0.0) return {};
            api.set_state_number(handle, key.c_str(), 1.0);
            return "Out";
        }

        if (type == "flow.delay") {
            const std::string timer = InternalKey("delay", node.id);
            api.after(handle, Get("Seconds").AsNumber(), timer.c_str());
            return {};
        }

        // ---- variables ---------------------------------------------------------------------
        if (type == "var.set") {
            const BlueprintVariable* variable = step.program->Blueprint().FindVariable(node.target);
            if (!variable) return "Out";
            const Datum value = Get("Value").As(PinTypeOf(variable->type));
            if (value.type == PinType::Number)
                api.set_state_number(handle, node.target.c_str(), value.number);
            else
                api.set_state_text(handle, node.target.c_str(), value.AsText().c_str());
            Record(step, node, "Value", value);
            return "Out";
        }

        // ---- the component's own tree --------------------------------------------------------
        if (type == "ui.setText") {
            api.set_text(handle, Node().c_str(), Prop().c_str(), Get("Value").AsText().c_str());
            return "Out";
        }
        if (type == "ui.setNumber") {
            api.set_number(handle, Node().c_str(), Prop().c_str(), Get("Value").AsNumber());
            return "Out";
        }
        if (type == "ui.setBool") {
            api.set_bool(handle, Node().c_str(), Prop().c_str(), Get("Value").AsBool() ? 1 : 0);
            return "Out";
        }
        if (type == "ui.setColour") {
            api.set_color(handle, Node().c_str(), Prop().c_str(), Get("Value").AsColour());
            return "Out";
        }
        if (type == "ui.setProperty") {
            api.set_property(handle, Node().c_str(), Prop().c_str(), Get("Value").AsText().c_str());
            return "Out";
        }
        if (type == "ui.setVisible") {
            api.set_visible(handle, Node().c_str(), Get("Visible").AsBool() ? 1 : 0);
            return "Out";
        }
        if (type == "ui.setEnabled") {
            api.set_enabled(handle, Node().c_str(), Get("Enabled").AsBool() ? 1 : 0);
            return "Out";
        }
        if (type == "ui.focus")       { api.focus(handle, Node().c_str()); return "Out"; }
        if (type == "ui.scrollTo")    {
            api.scroll_to(handle, Node().c_str(), Get("Y").AsNumber());
            return "Out";
        }
        if (type == "ui.scrollToEnd") { api.scroll_to_end(handle, Node().c_str()); return "Out"; }
        if (type == "ui.setRows") {
            HandRows(api, handle, Node().c_str(), Get("Rows").AsText());
            return "Out";
        }
        if (type == "ui.clearRows")   { api.clear_rows(handle, Node().c_str()); return "Out"; }

        // ---- the app around it -----------------------------------------------------------------
        if (type == "app.emit") {
            api.emit(handle, Get("Signal").AsText().c_str(), Get("Number").AsNumber(),
                     Get("Text").AsText().c_str());
            return "Out";
        }
        if (type == "app.navigate") { api.navigate(handle, Get("Route").AsText().c_str()); return "Out"; }
        if (type == "app.back") {
            Record(step, node, "Went", Datum::OfBool(api.back(handle) != 0));
            return "Out";
        }
        if (type == "app.toast") {
            api.toast(handle, Get("Text").AsText().c_str(), Get("Seconds").AsNumber());
            return "Out";
        }
        if (type == "app.after") {
            api.after(handle, Get("Seconds").AsNumber(), Get("Timer").AsText().c_str());
            return "Out";
        }
        if (type == "app.cancel") { api.cancel(handle, Get("Timer").AsText().c_str()); return "Out"; }
        if (type == "app.log") {
            const std::string level = Get("Level").AsText();
            const int which = level == "error" ? VAE_LOG_ERROR
                            : level == "warn"  ? VAE_LOG_WARN
                            : level == "trace" ? VAE_LOG_TRACE : VAE_LOG_INFO;
            api.log(handle, which, Get("Text").AsText().c_str());
            return "Out";
        }

        // ---- services ---------------------------------------------------------------------------
        if (type == "store.setNumber") {
            api.set_store_number(handle, Get("Key").AsText().c_str(), Get("Value").AsNumber());
            return "Out";
        }
        if (type == "store.setText") {
            api.set_store_text(handle, Get("Key").AsText().c_str(), Get("Value").AsText().c_str());
            return "Out";
        }
        if (type == "store.forget") { api.forget(handle, Get("Key").AsText().c_str()); return "Out"; }
        if (type == "file.write") {
            const int wrote = api.write_file(handle, Get("Path").AsText().c_str(),
                                             Get("Text").AsText().c_str());
            Record(step, node, "Wrote", Datum::OfBool(wrote != 0));
            return "Out";
        }
        if (type == "net.get") {
            api.http_get(handle, Get("Url").AsText().c_str(), Get("Tag").AsText().c_str());
            return "Out";
        }
        if (type == "net.post") {
            api.http_post(handle, Get("Url").AsText().c_str(), Get("Body").AsText().c_str(),
                          Get("Content Type").AsText().c_str(), Get("Tag").AsText().c_str());
            return "Out";
        }
        if (type == "socket.open") {
            api.socket_open(handle, Get("Url").AsText().c_str(), Get("Socket").AsText().c_str());
            return "Out";
        }
        if (type == "socket.send") {
            api.socket_send(handle, Get("Socket").AsText().c_str(), Get("Text").AsText().c_str());
            return "Out";
        }
        if (type == "socket.close") {
            api.socket_close(handle, Get("Socket").AsText().c_str());
            return "Out";
        }
        if (type == "sound.play") {
            const unsigned long long voice =
                api.play_sound(handle, Get("Sound").AsText().c_str(), Get("Volume").AsNumber(),
                               Get("Loop").AsBool() ? 1 : 0);
            Record(step, node, "Voice", Datum::OfNumber(static_cast<double>(voice)));
            return "Out";
        }
        if (type == "sound.stop") {
            api.stop_sound(handle,
                           static_cast<unsigned long long>(Get("Voice").AsNumber()));
            return "Out";
        }
        if (type == "sound.stopAll")  { api.stop_sounds(handle); return "Out"; }
        if (type == "sound.setVolume") {
            api.set_sound_volume(handle, Get("Volume").AsNumber());
            return "Out";
        }

        // An event node reached through a wire cannot happen — nothing wires into one — and a pure
        // node reached through a wire cannot either, because it has no execution input. Anything
        // left here is a type the table knows and this switch forgot, which is worth saying.
        const BlueprintNodeType* declared = FindBlueprintNodeType(type);
        if (declared && !declared->pure && declared->category != BlueprintCategory::Event)
            Say(handle, VAE_LOG_ERROR, "the blueprint node '" + type + "' has nothing behind it");
        return {};
    }

    // --- reading a value ------------------------------------------------------------------------

    BlueprintHost::Datum BlueprintHost::ReadNamed(Step& step, const BlueprintNode& node,
                                               std::string_view pin) {
        const std::vector<PinSpec> inputs = BlueprintInputs(step.program->Blueprint(), node);
        if (const PinSpec* found = FindPin(inputs, pin)) return Read(step, node, *found);
        return Datum::OfNumber(0.0);
    }

    BlueprintHost::Datum BlueprintHost::Read(Step& step, const BlueprintNode& node, const PinSpec& pin) {
        PinType want = pin.type;
        if (want == PinType::Any) {
            const BlueprintVariable* variable = step.program->Blueprint().FindVariable(node.target);
            want = variable ? PinTypeOf(variable->type) : PinType::Text;
        }
        // A fixed pin is a design-time fact, so it is never asked of a wire — see PinSpec::fixed.
        if (!pin.fixed)
            if (const BlueprintLink* link = step.program->Blueprint().LinkInto(node.id, pin.name)) {
                if (const BlueprintNode* source = step.program->Blueprint().Find(link->from)) {
                    if (m_Watching) m_Flow.push_back({ step.program->Component(), link->id });
                    return Value(step, *source, link->fromPin).As(want);
                }
            }
        return Datum::Of(BlueprintLiteral(step.program->Blueprint(), node, pin), want);
    }

    BlueprintHost::Datum BlueprintHost::Value(Step& step, const BlueprintNode& node,
                                           std::string_view pin) {
        const BlueprintNodeType* type = FindBlueprintNodeType(node.type);
        if (!type) return Datum::OfNumber(0.0);

        // An event's outputs are the event being delivered. They are facts about this step, not
        // something to work out, which is why they are read here rather than evaluated.
        if (type->category == BlueprintCategory::Event) {
            const VaeEvent* event = step.event;
            if (pin == "Delta")  return Datum::OfNumber(step.delta);
            if (pin == "Row")    return Datum::OfNumber(event ? event->row : -1);
            if (pin == "Status") return Datum::OfNumber(event ? event->number : 0.0);
            if (pin == "Number") return Datum::OfNumber(event ? event->number : 0.0);
            if (pin == "Text" || pin == "Body" || pin == "Reason")
                return Datum::OfText(event && event->text ? event->text : "");
            return Datum::OfNumber(0.0);
        }

        // A For Loop's index is whatever iteration the body is on. It exists only while the loop
        // is running, which is exactly when anything can read it.
        if (node.type == "flow.forLoop" && pin == "Index") {
            const auto it = step.loopIndex.find(node.id);
            return Datum::OfNumber(it == step.loopIndex.end() ? 0.0 : it->second);
        }

        const std::string key = MemoKey(node.id, pin);

        // An impure node's data output is what it left there when it ran. Nothing has run it yet,
        // so there is nothing there — a zero, and never a second call to something with an effect.
        if (!type->pure) {
            const auto it = step.results.find(key);
            return it == step.results.end() ? Datum::OfNumber(0.0) : it->second;
        }

        if (const auto it = step.memo.find(key); it != step.memo.end()) return it->second;

        Datum value = Evaluate(step, node, pin);
        step.memo.emplace(key, value);
        if (m_Watching) {
            m_Values[WatchKey(step.program->Component(), node.id, pin)] =
                { value.AsText(), ++m_WatchStep };
        }
        return value;
    }

    BlueprintHost::Datum BlueprintHost::Evaluate(Step& step, const BlueprintNode& node,
                                              std::string_view pin) {
        const VaeScriptAPI& api = m_Api;
        VaeInstance handle = step.handle;
        const std::string& type = node.type;
        (void)pin;

        const auto Get  = [&](const char* p) { return ReadNamed(step, node, p); };
        const auto Num  = [&](const char* p) { return Get(p).AsNumber(); };
        const auto Str  = [&](const char* p) { return Get(p).AsText(); };
        const auto Node = [&] { return ReadNamed(step, node, "Node").AsText(); };
        const auto Prop = [&] { return ReadNamed(step, node, "Property").AsText(); };

        // ---- variables ---------------------------------------------------------------------
        if (type == "var.get") {
            const BlueprintVariable* variable = step.program->Blueprint().FindVariable(node.target);
            if (!variable) return Datum::OfNumber(0.0);
            const PinType want = PinTypeOf(variable->type);
            if (want == PinType::Number)
                return Datum::OfNumber(api.state_number(handle, node.target.c_str(), 0.0));
            const char* text = api.state_text(handle, node.target.c_str(), "");
            return Datum::OfText(text ? text : "").As(want);
        }

        // ---- the component's own tree --------------------------------------------------------
        if (type == "ui.getText") {
            const char* text = api.get_text(handle, Node().c_str(), Prop().c_str(), "");
            return Datum::OfText(text ? text : "");
        }
        if (type == "ui.getNumber")
            return Datum::OfNumber(api.get_number(handle, Node().c_str(), Prop().c_str(), 0.0));
        if (type == "ui.getBool")
            return Datum::OfBool(api.get_bool(handle, Node().c_str(), Prop().c_str(), 0) != 0);
        if (type == "ui.getColour")
            return Datum::OfColour(api.get_color(handle, Node().c_str(), Prop().c_str(),
                                                    VaeColor{ 1, 1, 1, 1 }));
        if (type == "ui.getProperty") {
            const char* text = api.get_property(handle, Node().c_str(), Prop().c_str(), "");
            return Datum::OfText(text ? text : "");
        }
        if (type == "ui.rowCount")
            return Datum::OfNumber(api.row_count(handle, Node().c_str()));
        if (type == "ui.exists")
            return Datum::OfBool(api.has_node(handle, Node().c_str()) != 0);

        // ---- the app around it -----------------------------------------------------------------
        if (type == "app.time")   return Datum::OfNumber(api.time(handle));
        if (type == "app.clock")  return Datum::OfNumber(api.clock(handle));
        if (type == "app.date") {
            const char* date = api.date(handle, Str("Format").c_str());
            return Datum::OfText(date ? date : "");
        }
        if (type == "app.instanceName") {
            const char* name = api.instance_name(handle);
            return Datum::OfText(name ? name : "");
        }
        if (type == "app.componentName") {
            const char* name = api.component_name(handle);
            return Datum::OfText(name ? name : "");
        }

        // ---- services ---------------------------------------------------------------------------
        if (type == "store.getNumber")
            return Datum::OfNumber(api.store_number(handle, Str("Key").c_str(), Num("Default")));
        if (type == "store.getText") {
            const std::string fallback = Str("Default");
            const char* text = api.store_text(handle, Str("Key").c_str(), fallback.c_str());
            return Datum::OfText(text ? text : "");
        }
        if (type == "store.has")
            return Datum::OfBool(api.has_stored(handle, Str("Key").c_str()) != 0);
        if (type == "file.read") {
            const char* text = api.read_file(handle, Str("Path").c_str());
            return Datum::OfText(text ? text : "");
        }
        if (type == "file.exists")
            return Datum::OfBool(api.file_exists(handle, Str("Path").c_str()) != 0);
        if (type == "socket.live")
            return Datum::OfBool(api.socket_live(handle, Str("Socket").c_str()) != 0);
        if (type == "sound.volume") return Datum::OfNumber(api.sound_volume(handle));

        // ---- arithmetic --------------------------------------------------------------------------
        if (type == "math.add")      return Datum::OfNumber(Num("A") + Num("B"));
        if (type == "math.subtract") return Datum::OfNumber(Num("A") - Num("B"));
        if (type == "math.multiply") return Datum::OfNumber(Num("A") * Num("B"));
        if (type == "math.divide") {
            const double b = Num("B");
            // Zero rather than an infinity: an infinity propagates silently through every later
            // node and comes out as a layout that is nowhere, which is a much longer bug to find.
            return Datum::OfNumber(b == 0.0 ? 0.0 : Num("A") / b);
        }
        if (type == "math.modulo") {
            const double b = Num("B");
            return Datum::OfNumber(b == 0.0 ? 0.0 : std::fmod(Num("A"), b));
        }
        if (type == "math.min")   return Datum::OfNumber(std::min(Num("A"), Num("B")));
        if (type == "math.max")   return Datum::OfNumber(std::max(Num("A"), Num("B")));
        if (type == "math.clamp") {
            const double lo = Num("Min"), hi = Num("Max");
            return Datum::OfNumber(std::clamp(Num("Value"), std::min(lo, hi), std::max(lo, hi)));
        }
        if (type == "math.abs")   return Datum::OfNumber(std::abs(Num("Value")));
        if (type == "math.floor") return Datum::OfNumber(std::floor(Num("Value")));
        if (type == "math.ceil")  return Datum::OfNumber(std::ceil(Num("Value")));
        if (type == "math.round") return Datum::OfNumber(std::round(Num("Value")));
        if (type == "math.random") {
            // One generator for the process, seeded once. A generator per call seeded from the
            // clock returns the same number for every call inside one frame, which is the classic
            // way a random node turns out not to be random at all.
            static std::mt19937 engine{ std::random_device{}() };
            const double lo = Num("Min"), hi = Num("Max");
            if (hi <= lo) return Datum::OfNumber(lo);
            std::uniform_real_distribution<double> spread(lo, hi);
            return Datum::OfNumber(spread(engine));
        }

        if (type == "compare.equal")        return Datum::OfBool(Num("A") == Num("B"));
        if (type == "compare.notEqual")     return Datum::OfBool(Num("A") != Num("B"));
        if (type == "compare.less")         return Datum::OfBool(Num("A") <  Num("B"));
        if (type == "compare.lessEqual")    return Datum::OfBool(Num("A") <= Num("B"));
        if (type == "compare.greater")      return Datum::OfBool(Num("A") >  Num("B"));
        if (type == "compare.greaterEqual") return Datum::OfBool(Num("A") >= Num("B"));

        if (type == "logic.and") return Datum::OfBool(Get("A").AsBool() && Get("B").AsBool());
        if (type == "logic.or")  return Datum::OfBool(Get("A").AsBool() || Get("B").AsBool());
        if (type == "logic.not") return Datum::OfBool(!Get("Value").AsBool());

        if (type == "text.join")   return Datum::OfText(Str("A") + Str("B"));
        if (type == "text.fromNumber") {
            const int decimals = static_cast<int>(std::clamp(Num("Decimals"), 0.0, 12.0));
            if (decimals == 0) return Datum::OfText(NumberText(std::round(Num("Number"))));
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, Num("Number"));
            return Datum::OfText(buffer);
        }
        if (type == "text.toNumber") return Datum::OfNumber(TextNumber(Str("Text")));
        if (type == "text.equal")    return Datum::OfBool(Str("A") == Str("B"));
        if (type == "text.empty")    return Datum::OfBool(Str("Text").empty());
        if (type == "text.length")
            return Datum::OfNumber(static_cast<double>(Str("Text").size()));
        if (type == "text.contains")
            return Datum::OfBool(Str("Text").find(Str("Part")) != std::string::npos);
        if (type == "text.upper" || type == "text.lower") {
            std::string out = Str("Text");
            const bool upper = type == "text.upper";
            for (char& c : out)
                c = static_cast<char>(upper ? std::toupper(static_cast<unsigned char>(c))
                                            : std::tolower(static_cast<unsigned char>(c)));
            return Datum::OfText(std::move(out));
        }
        if (type == "text.trim") return Datum::OfText(Trim(Str("Text")));

        if (type == "select.number")
            return Datum::OfNumber(Get("Condition").AsBool() ? Num("True") : Num("False"));
        if (type == "select.text")
            return Datum::OfText(Get("Condition").AsBool() ? Str("True") : Str("False"));

        if (type == "make.number" || type == "make.text" || type == "make.bool"
            || type == "make.colour")
            return Get("Value");
        if (type == "colour.rgba") {
            const auto Channel = [&](const char* p) {
                return static_cast<float>(std::clamp(Num(p), 0.0, 1.0));
            };
            return Datum::OfColour({ Channel("Red"), Channel("Green"), Channel("Blue"),
                                        Channel("Alpha") });
        }

        Say(handle, VAE_LOG_ERROR, "the blueprint node '" + type + "' has no value behind it");
        return Datum::OfNumber(0.0);
    }

    std::string BlueprintHost::InternalKey(std::string_view what, u32 node) {
        // A '#' so it cannot collide with a variable: a variable is named in the editor, and the
        // editor does not let a name start with one.
        return "#" + std::string(what) + "." + std::to_string(node);
    }

    void BlueprintHost::Record(Step& step, const BlueprintNode& node, std::string_view pin,
                           const Datum& value) {
        step.results[MemoKey(node.id, pin)] = value;
        if (m_Watching)
            m_Values[WatchKey(step.program->Component(), node.id, pin)] =
                { value.AsText(), ++m_WatchStep };
    }

    void BlueprintHost::Say(VaeInstance handle, int level, const std::string& text) const {
        if (m_Bound && m_Api.log) m_Api.log(handle, level, text.c_str());
    }

}
