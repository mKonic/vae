#include "vaepch.h"
#include "vae/script/BlueprintHost.h"

#include "vae/doc/Serializer.h"
#include "vae/doc/ValueText.h"
#include "vae/ui/Library.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <random>

namespace vae::script {

    using namespace vae::doc;

    namespace {

        // How many nodes one event may run through before the blueprint is declared stuck. Unreal's
        // number, and for Unreal's reason: it is far above anything a real blueprint does and far
        // below "the app has hung". A For Loop over ten thousand rows doing ten things each is a
        // hundred thousand steps, and still an order of magnitude clear of this.
        constexpr u32 kStepBudget = 1'000'000;
        // How deeply a blueprint may nest — a Sequence inside a loop inside a branch, and a
        // function that calls a function. This bounds the C++ stack, which a straight run of nodes
        // does not touch because Continue is iterative.
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
        // because the same text a designer types into the Sample field is the one spelling of a
        // table — learned once, used at both ends.
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

        // An index into a collection, from a number that may be anything. Out of range is out of
        // range; the caller decides whether that is empty, a no-op or the end.
        bool InRange(double index, std::size_t size) {
            return index >= 0.0 && static_cast<std::size_t>(index) < size;
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
    BlueprintHost::Datum BlueprintHost::Datum::OfList(std::vector<std::string> v) {
        Datum out; out.type = PinType::List; out.items = std::move(v); return out;
    }
    BlueprintHost::Datum BlueprintHost::Datum::OfMap(
            std::vector<std::pair<std::string, std::string>> v) {
        Datum out; out.type = PinType::Map; out.entries = std::move(v); return out;
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
            case PinType::List: {
                const std::string* text = std::get_if<std::string>(&value);
                return OfList(ParseListText(text ? *text : std::string()));
            }
            case PinType::Map: {
                const std::string* text = std::get_if<std::string>(&value);
                return OfMap(ParseMapText(text ? *text : std::string()));
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
            case PinType::List:   return !items.empty();
            case PinType::Map:    return !entries.empty();
            default: break;
        }
        return false;
    }

    double BlueprintHost::Datum::AsNumber() const {
        switch (type) {
            case PinType::Bool:   return boolean ? 1.0 : 0.0;
            case PinType::Number: return number;
            case PinType::Text:   return TextNumber(text);
            case PinType::List:   return static_cast<double>(items.size());
            case PinType::Map:    return static_cast<double>(entries.size());
            default: break;
        }
        return 0.0;
    }

    std::string BlueprintHost::Datum::AsText() const {
        switch (type) {
            case PinType::Bool:   return boolean ? "true" : "false";
            case PinType::Number: return NumberText(number);
            case PinType::Text:   return text;
            case PinType::List:   return ListText(items);
            case PinType::Map:    return MapText(entries);
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

    std::vector<std::string> BlueprintHost::Datum::AsList() const {
        if (type == PinType::List) return items;
        if (type == PinType::Map) {
            std::vector<std::string> out;
            out.reserve(entries.size());
            for (const auto& [key, value] : entries) out.push_back(key);
            return out;
        }
        // Anything else is read as the text form of a list, which is what it is: that is how a
        // list is stored in the state bag, and reading one back out is the case that matters. A
        // single word with no line breaks in it comes back as a list of one, which is also right.
        return ParseListText(AsText());
    }

    std::vector<std::pair<std::string, std::string>> BlueprintHost::Datum::AsMap() const {
        if (type == PinType::Map) return entries;
        return ParseMapText(AsText());
    }

    BlueprintHost::Datum BlueprintHost::Datum::As(doc::PinType want) const {
        switch (want) {
            case PinType::Bool:   return OfBool(AsBool());
            case PinType::Number: return OfNumber(AsNumber());
            case PinType::Text:   return OfText(AsText());
            case PinType::Colour: return OfColour(AsColour());
            case PinType::List:   return OfList(AsList());
            case PinType::Map:    return OfMap(AsMap());
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
        m_Suspended.reset();
        m_Halt = {};

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
                m_Messages.push_back({ name, d.function, d.node, d.error, d.message });
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
        m_Suspended.reset();
        m_Halt = {};
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

    std::string BlueprintHost::WatchKey(std::string_view component, u32 node,
                                        std::string_view pin) {
        return std::string(component) + "\n" + std::to_string(node) + "\n" + std::string(pin);
    }

    void BlueprintHost::ClearWatch() { m_Flow.clear(); m_Values.clear(); m_WatchStep = 0; }

    // --- breakpoints -----------------------------------------------------------------------------

    void BlueprintHost::SetBreakpoint(std::string_view component, u32 node, bool on) {
        std::vector<u32>& nodes = m_Breakpoints[std::string(component)];
        if (on) {
            if (std::ranges::find(nodes, node) == nodes.end()) nodes.push_back(node);
        } else {
            std::erase(nodes, node);
        }
    }

    bool BlueprintHost::IsBreakpoint(std::string_view component, u32 node) const {
        const auto it = m_Breakpoints.find(std::string(component));
        if (it == m_Breakpoints.end()) return false;
        return std::ranges::find(it->second, node) != it->second.end();
    }

    void BlueprintHost::Continue() {
        m_Stepping = false;
        if (!m_Suspended) { m_Halt = {}; return; }
        m_Halt = {};
        Step step = std::move(*m_Suspended);
        m_Suspended.reset();
        step.halted = false;
        Resume(step);
    }

    void BlueprintHost::StepOver() {
        if (!m_Suspended) { m_Halt = {}; return; }
        m_Stepping = true;
        Continue();
    }

    bool BlueprintHost::ShouldHalt(Step& step, const doc::BlueprintNode& node) {
        // The node a run is being picked up at does not stop it again — otherwise Continue would
        // stop where it just was and nothing would ever move.
        if (step.resumeAt == node.id) { step.resumeAt = 0; return false; }
        // Stepping stops at the very next node whatever it is; otherwise only where one was put.
        const bool wanted = m_Stepping || IsBreakpoint(step.program->Component(), node.id);
        if (!wanted) return false;
        m_Stepping = false;
        step.halted = true;
        step.haltAtEntry = true;
        m_Halt = { true, step.program->Component(), step.Top().function, node.id };
        return true;
    }

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
            const Datum value = Datum::Of(variable.defaultValue, variable.type);
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
        if (m_Suspended && m_Suspended->handle == handle) { m_Suspended.reset(); m_Halt = {}; }
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
                // A Delay is a timer with a name nobody typed. When it comes due, the blueprint
                // picks up at the node that started it rather than at an event — which is what
                // makes it a pause in the middle of a chain instead of a second entry point.
                const std::string prefix = "#delay.";
                if (name.starts_with(prefix)) {
                    const u32 node = static_cast<u32>(std::strtoul(name.c_str() + prefix.size(),
                                                                   nullptr, 10));
                    std::string function;
                    const BlueprintCanvas* canvas =
                        CanvasOf(program->Blueprint(), node, &function);
                    if (!canvas) break;
                    Step step{ handle, program, event, 0.0 };
                    step.frames.push_back({ function, canvas });
                    // What the event was handed when it started waiting. A custom event's
                    // parameters are the only thing a resumed chain cannot work out for itself.
                    const std::string kept = InternalKey("delayargs", node);
                    if (const char* saved = m_Api.state_text(handle, kept.c_str(), ""))
                        for (const auto& [key, value] : ParseMapText(saved))
                            step.frames.back().locals[key] = Datum::OfText(value);
                    Continue(step, node, "Done");
                    if (step.halted) { m_Suspended = std::make_unique<Step>(std::move(step)); }
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
        // While a breakpoint holds the app, nothing else in it advances. That is what makes the
        // values on the pins worth reading: they are the values at the moment it stopped.
        if (m_Halt.stopped) return;
        for (const u32 entry : entries) {
            Step step{ handle, &program, event, delta };
            step.frames.push_back({ std::string(), &program.Blueprint().graph });
            Continue(step, entry, "Out");
            if (step.halted) {
                m_Suspended = std::make_unique<Step>(std::move(step));
                return;
            }
        }
    }

    void BlueprintHost::Resume(Step& step) {
        // The pending list is innermost first, which is the order the work has to come back in.
        std::vector<Pending> queue = std::move(step.pending);
        step.pending.clear();
        while (!queue.empty() && !step.stopped && !step.halted) {
            const Pending item = queue.front();
            queue.erase(queue.begin());
            if (item.frame + 1 < step.frames.size()) step.frames.resize(item.frame + 1);
            if (step.frames.empty()) break;

            switch (item.kind) {
                case Pending::Kind::Enter: {
                    const BlueprintNode* node = step.Top().canvas->Find(item.node);
                    if (!node) break;
                    step.resumeAt = node->id;
                    const std::string next = Run(step, *node, item.pin);
                    if (step.halted && step.haltAtEntry) {
                        step.haltAtEntry = false;
                        step.pending.push_back({ Pending::Kind::Enter, item.node, item.pin, 0,
                                                 item.frame });
                        break;
                    }
                    if (!next.empty() && !step.halted && !step.stopped)
                        Continue(step, item.node, next);
                    break;
                }
                case Pending::Kind::Chain:
                    Continue(step, item.node, item.pin);
                    break;
                case Pending::Kind::Sequence: {
                    const BlueprintNode* node = step.Top().canvas->Find(item.node);
                    if (!node) break;
                    const std::vector<PinSpec> outputs =
                        BlueprintOutputs(step.program->Blueprint(), *node, step.Top().function);
                    for (std::size_t i = item.index; i < outputs.size(); ++i) {
                        Continue(step, item.node, outputs[i].name);
                        if (step.halted) {
                            step.pending.push_back({ Pending::Kind::Sequence, item.node, {},
                                                     static_cast<u32>(i + 1), item.frame });
                            break;
                        }
                        if (step.stopped || step.Top().breaking) break;
                    }
                    break;
                }
                case Pending::Kind::Loop: {
                    const BlueprintNode* node = step.Top().canvas->Find(item.node);
                    if (node) RunLoop(step, *node, item.index);
                    break;
                }
            }

            // Anything the resumed work left behind has to come before what was already waiting.
            if (!step.pending.empty()) {
                queue.insert(queue.begin(), step.pending.begin(), step.pending.end());
                step.pending.clear();
            }
        }
        step.pending = std::move(queue);
        if (step.halted) m_Suspended = std::make_unique<Step>(std::move(step));
    }

    const doc::BlueprintCanvas* BlueprintHost::CanvasOf(const doc::Blueprint& blueprint, u32 node,
                                                        std::string* function) {
        for (const auto& [name, canvas] : blueprint.Canvases())
            if (canvas->Find(node)) {
                if (function) *function = name;
                return canvas;
            }
        return nullptr;
    }

    // --- walking it ------------------------------------------------------------------------------

    void BlueprintHost::Continue(Step& step, u32 node, std::string_view pin) {
        if (step.stopped || step.halted) return;
        if (++step.depth > kMaxDepth) {
            Say(step.handle, VAE_LOG_ERROR,
                "this blueprint nests deeper than " + std::to_string(kMaxDepth) + " levels");
            step.stopped = true;
            --step.depth;
            return;
        }

        const BlueprintLink* link = step.Top().canvas->LinkOutOf(node, pin);
        while (link) {
            if (++step.steps > kStepBudget) {
                // Unreal's infinite-loop guard, and the same message: a blueprint that runs a
                // million nodes for one click is looping, and stopping it with a line in the
                // console is the only outcome that leaves the app usable.
                Say(step.handle, VAE_LOG_ERROR,
                    "blueprint stopped: it ran " + std::to_string(kStepBudget)
                    + " nodes without finishing, which means something loops for ever");
                step.stopped = true;
                break;
            }
            if (m_Watching) m_Flow.push_back({ step.program->Component(), link->id });

            const BlueprintNode* target = step.Top().canvas->Find(link->to);
            if (!target) break;
            const std::size_t frame = step.frames.size() - 1;
            const std::string entry = link->toPin;
            const std::string next = Run(step, *target, entry);
            if (step.halted) {
                // Only when the node did nothing at all. A construct that had already started has
                // written down where it got to, and entering it again would run it twice.
                if (step.haltAtEntry) {
                    step.haltAtEntry = false;
                    step.pending.push_back({ Pending::Kind::Enter, target->id, entry, 0, frame });
                }
                break;
            }
            if (step.stopped || next.empty()) break;
            // A Break is on its way out to the loop that will catch it.
            if (step.Top().breaking) break;
            link = step.Top().canvas->LinkOutOf(target->id, next);
        }
        --step.depth;
    }
    // Every loop is the same shape: work out how many turns there are, run the body for each, and
    // stop early when a Break comes back out. `from` is which turn to start at, which is 0 for a
    // fresh loop and wherever it got to when a breakpoint stopped one.
    void BlueprintHost::RunLoop(Step& step, const doc::BlueprintNode& node, u32 from) {
        const std::size_t frame = step.frames.size() - 1;
        const bool forEach = node.type == "flow.forEach";
        const bool counted = node.type == "flow.forLoop";

        std::vector<std::string> items;
        double first = 0.0, last = -1.0;
        if (forEach) items = ReadNamed(step, node, "List").AsList();
        if (counted) { first = ReadNamed(step, node, "First").AsNumber();
                       last  = ReadNamed(step, node, "Last").AsNumber(); }

        for (u32 turn = from; ; ++turn) {
            if (counted && first + turn > last) break;
            if (forEach && turn >= items.size()) break;
            if (!counted && !forEach) {
                // A While asks again every time round, which is the difference between a loop that
                // ends and one that does not.
                step.Top().memo.clear();
                if (!ReadNamed(step, node, "Condition").AsBool()) break;
                if (++step.steps > kStepBudget) {
                    Say(step.handle, VAE_LOG_ERROR, "blueprint stopped: a While Loop never ends");
                    step.stopped = true;
                    break;
                }
            }
            if (counted) step.Top().loopIndex[node.id] = first + turn;
            if (forEach) {
                step.Top().loopIndex[node.id] = turn;
                step.Top().loopElement[node.id] = Datum::OfText(items[turn]);
            }

            Continue(step, node.id, "Body");
            if (step.halted) {
                step.pending.push_back({ Pending::Kind::Loop, node.id, {}, turn + 1, frame });
                return;
            }
            if (step.stopped) return;
            if (step.Top().breaking) { step.Top().breaking = false; break; }
        }

        step.Top().loopIndex.erase(node.id);
        step.Top().loopElement.erase(node.id);
        Continue(step, node.id, "Done");
        if (step.halted) return;
    }

    std::string BlueprintHost::Run(Step& step, const BlueprintNode& node, std::string_view entry) {
        if (ShouldHalt(step, node)) return {};

        // A statement asks its inputs afresh. Everything worked out for the previous statement is
        // an answer to a question that has already been answered, and keeping it is how a Get
        // after a loop reads the value from before the loop.
        step.Top().memo.clear();

        const VaeScriptAPI& api = m_Api;
        VaeInstance handle = step.handle;
        const std::string& type = node.type;

        const auto Get  = [&](const char* pin) { return ReadNamed(step, node, pin); };
        const auto Node = [&] { return ReadNamed(step, node, "Node").AsText(); };
        const auto Prop = [&] { return ReadNamed(step, node, "Property").AsText(); };
        const auto Idx  = [&](const char* pin) { return Get(pin).AsNumber(); };

        // ---- flow --------------------------------------------------------------------------
        if (type == "flow.branch") return Get("Condition").AsBool() ? "True" : "False";

        if (type == "flow.sequence") {
            const std::size_t frame = step.frames.size() - 1;
            const std::vector<PinSpec> outputs =
                BlueprintOutputs(step.program->Blueprint(), node, step.Top().function);
            for (std::size_t i = 0; i < outputs.size(); ++i) {
                Continue(step, node.id, outputs[i].name);
                if (step.halted) {
                    step.pending.push_back({ Pending::Kind::Sequence, node.id, {},
                                             static_cast<u32>(i + 1), frame });
                    break;
                }
                if (step.stopped || step.Top().breaking) break;
            }
            return {};
        }

        if (type == "flow.forLoop" || type == "flow.while" || type == "flow.forEach") {
            RunLoop(step, node, 0);
            return {};
        }

        if (type == "flow.break") {
            // Unwinds to the nearest loop, which clears it and carries on from its Done.
            step.Top().breaking = true;
            return {};
        }

        if (type == "switch.number" || type == "switch.text") {
            const bool numeric = type == "switch.number";
            const Datum value = Get("Value");
            const std::vector<PinSpec> inputs =
                BlueprintInputs(step.program->Blueprint(), node, step.Top().function);
            u32 which = 0;
            for (const PinSpec& pin : inputs) {
                if (!pin.name.starts_with("Case ")) continue;
                const Datum candidate = ReadNamed(step, node, pin.name);
                const bool hit = numeric ? candidate.AsNumber() == value.AsNumber()
                                         : candidate.AsText() == value.AsText();
                if (hit) return std::string(pin.name);
                ++which;
            }
            (void)which;
            return "Default";
        }

        if (type == "flow.doOnce") {
            const std::string key = InternalKey("once", node.id);
            if (entry == "Reset") { api.set_state_number(handle, key.c_str(), 0.0); return {}; }
            if (api.state_number(handle, key.c_str(), 0.0) != 0.0) return {};
            api.set_state_number(handle, key.c_str(), 1.0);
            return "Out";
        }

        if (type == "flow.doN") {
            const std::string key = InternalKey("don", node.id);
            if (entry == "Reset") { api.set_state_number(handle, key.c_str(), 0.0); return {}; }
            const double done = api.state_number(handle, key.c_str(), 0.0);
            if (done >= Get("N").AsNumber()) return {};
            api.set_state_number(handle, key.c_str(), done + 1.0);
            Record(step, node, "Counter", Datum::OfNumber(done + 1.0));
            return "Out";
        }

        if (type == "flow.flipFlop") {
            const std::string key = InternalKey("flip", node.id);
            const bool isA = api.state_number(handle, key.c_str(), 0.0) == 0.0;
            api.set_state_number(handle, key.c_str(), isA ? 1.0 : 0.0);
            Record(step, node, "Is A", Datum::OfBool(isA));
            return isA ? "A" : "B";
        }

        if (type == "flow.gate") {
            const std::string key = InternalKey("gate", node.id);
            const bool started = api.has_state(handle, key.c_str()) != 0;
            if (!started)
                api.set_state_number(handle, key.c_str(),
                                     Get("Start Closed").AsBool() ? 0.0 : 1.0);
            const bool open = api.state_number(handle, key.c_str(), 0.0) != 0.0;
            if (entry == "Open")   { api.set_state_number(handle, key.c_str(), 1.0); return {}; }
            if (entry == "Close")  { api.set_state_number(handle, key.c_str(), 0.0); return {}; }
            if (entry == "Toggle") {
                api.set_state_number(handle, key.c_str(), open ? 0.0 : 1.0);
                return {};
            }
            return open ? "Out" : std::string();
        }

        if (type == "flow.delay" || type == "flow.retriggerableDelay") {
            const std::string timer = InternalKey("delay", node.id);
            // Retriggerable means the wait starts again from here; an ordinary Delay reached while
            // one is already pending starts a second, which is what Unreal does too.
            if (type == "flow.retriggerableDelay") api.cancel(handle, timer.c_str());
            // What this chain was handed, so the other side of the wait still has it.
            if (!step.Top().locals.empty()) {
                std::vector<std::pair<std::string, std::string>> kept;
                for (const auto& [name, value] : step.Top().locals)
                    kept.emplace_back(name, value.AsText());
                api.set_state_text(handle, InternalKey("delayargs", node.id).c_str(),
                                   MapText(kept).c_str());
            }
            api.after(handle, Get("Seconds").AsNumber(), timer.c_str());
            return {};
        }

        // ---- functions and custom events ---------------------------------------------------
        if (type == "func.call") {
            const BlueprintFunction* called = step.program->Blueprint().FindFunction(node.target);
            if (!called) return "Out";
            std::map<std::string, Datum> arguments;
            for (const BlueprintParam& param : called->params)
                arguments[param.name] = ReadNamed(step, node, param.name).As(param.type);

            std::map<std::string, Datum> results;
            Invoke(step, *called, arguments, &results);
            if (step.halted) {
                // What the caller had left to do, so the return lands back here.
                step.pending.push_back({ Pending::Kind::Chain, node.id, "Out", 0,
                                         step.frames.size() - 1 });
                return {};
            }
            for (const auto& [name, value] : results) Record(step, node, name, value);
            return "Out";
        }

        if (type == "func.return") {
            // Everything it hands back, read here and collected by Invoke from the frame.
            const BlueprintFunction* owner =
                step.program->Blueprint().FindFunction(step.Top().function);
            if (owner)
                for (const BlueprintParam& value : owner->returns)
                    step.Top().results[MemoKey(0, value.name)] =
                        ReadNamed(step, node, value.name).As(value.type);
            return {};
        }

        // ---- variables ---------------------------------------------------------------------
        if (type == "var.set") {
            const BlueprintVariable* variable =
                step.program->Blueprint().FindVariable(node.target, step.Top().function);
            if (!variable) return "Out";
            const Datum value = Get("Value").As(variable->type);
            WriteVariable(step, node.target, value);
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

        // ---- lists and maps ----------------------------------------------------------------------
        // Each of these hands back a new collection on its second output. Wire it into a Set to
        // keep it: a value that two wires share and one of them changes is the bug this avoids.
        if (type.starts_with("list.") || type.starts_with("map.")) {
            const bool isMap = type.starts_with("map.");
            Datum result = isMap ? Get("Map") : Get("List");
            std::vector<std::string>& items = result.items;
            std::vector<std::pair<std::string, std::string>>& entries = result.entries;

            if (type == "list.set") {
                const double at = Idx("Index");
                if (InRange(at, items.size())) items[static_cast<std::size_t>(at)] = Get("Value").AsText();
            } else if (type == "list.add") {
                items.push_back(Get("Value").AsText());
            } else if (type == "list.insert") {
                const double at = std::clamp(Idx("Index"), 0.0, static_cast<double>(items.size()));
                items.insert(items.begin() + static_cast<std::ptrdiff_t>(at), Get("Value").AsText());
            } else if (type == "list.removeAt") {
                const double at = Idx("Index");
                if (InRange(at, items.size()))
                    items.erase(items.begin() + static_cast<std::ptrdiff_t>(at));
            } else if (type == "list.remove") {
                const std::string value = Get("Value").AsText();
                const auto found = std::ranges::find(items, value);
                if (found != items.end()) items.erase(found);
            } else if (type == "list.clear") {
                items.clear();
            } else if (type == "map.set") {
                const std::string key = Get("Key").AsText();
                const std::string value = Get("Value").AsText();
                bool replaced = false;
                for (auto& entry : entries)
                    if (entry.first == key) { entry.second = value; replaced = true; break; }
                if (!replaced) entries.emplace_back(key, value);
            } else if (type == "map.remove") {
                const std::string key = Get("Key").AsText();
                std::erase_if(entries, [&](const auto& e) { return e.first == key; });
            } else if (type == "map.clear") {
                entries.clear();
            } else {
                Say(handle, VAE_LOG_ERROR, "the blueprint node '" + type + "' has nothing behind it");
                return "Out";
            }
            Record(step, node, isMap ? "Map" : "List", result);
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
            api.stop_sound(handle, static_cast<unsigned long long>(Get("Voice").AsNumber()));
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
        if (declared && !declared->pure && declared->category != BlueprintCategory::Event
            && type != "func.entry")
            Say(handle, VAE_LOG_ERROR, "the blueprint node '" + type + "' has nothing behind it");
        return {};
    }

    void BlueprintHost::Invoke(Step& step, const BlueprintFunction& function,
                               const std::map<std::string, Datum>& arguments,
                               std::map<std::string, Datum>* into) {
        if (++step.depth > kMaxDepth) {
            Say(step.handle, VAE_LOG_ERROR, "'" + function.name
                + "' calls itself deeper than " + std::to_string(kMaxDepth) + " levels");
            step.stopped = true;
            --step.depth;
            return;
        }

        Frame frame;
        frame.function = function.name;
        frame.canvas = &function.body;
        // Parameters arrive as values; locals start at their default, every call. A function that
        // remembered the last call would be a variable wearing a function's clothes.
        frame.locals = arguments;
        for (const BlueprintVariable& local : function.locals)
            frame.locals[local.name] = Datum::Of(local.defaultValue, local.type);
        step.frames.push_back(std::move(frame));

        if (function.pure) {
            // A pure function has no execution pins, so there is no chain to walk: its answer is
            // whatever is wired into its Return, pulled the way any other expression is. That is
            // what "pure" means, and it is why one cannot contain a statement.
            if (const BlueprintNode* ret = function.body.FindType("func.return"))
                for (const BlueprintParam& value : function.returns)
                    step.Top().results[MemoKey(0, value.name)] =
                        ReadNamed(step, *ret, value.name).As(value.type);
        } else if (const BlueprintNode* entry = function.body.FindType("func.entry")) {
            Continue(step, entry->id, "Out");
        }

        if (!step.halted) {
            if (into)
                for (const BlueprintParam& value : function.returns) {
                    const auto found = step.Top().results.find(MemoKey(0, value.name));
                    (*into)[value.name] = found == step.Top().results.end()
                                        ? Datum::Of(value.defaultValue, value.type)
                                        : found->second;
                }
            step.frames.pop_back();
        }
        --step.depth;
    }
    // --- variables, wherever they live ------------------------------------------------------------

    bool BlueprintHost::IsLocal(const Step& step, std::string_view name) const {
        if (step.frames.empty() || step.Top().function.empty()) return false;
        const BlueprintFunction* function =
            step.program->Blueprint().FindFunction(step.Top().function);
        if (!function) return false;
        for (const BlueprintVariable& local : function->locals) if (local.name == name) return true;
        for (const BlueprintParam& param : function->params)    if (param.name == name) return true;
        return false;
    }

    const doc::BlueprintVariable* BlueprintHost::VariableSpec(const Step& step,
                                                              std::string_view name) const {
        return step.program->Blueprint().FindVariable(name, step.Top().function);
    }

    BlueprintHost::Datum BlueprintHost::ReadVariable(Step& step, std::string_view name) {
        const BlueprintVariable* spec = VariableSpec(step, name);
        const PinType want = spec ? spec->type : PinType::Text;
        if (IsLocal(step, name)) {
            const auto found = step.Top().locals.find(std::string(name));
            if (found != step.Top().locals.end()) return found->second.As(want);
            return spec ? Datum::Of(spec->defaultValue, want) : Datum::OfNumber(0.0);
        }
        const std::string key(name);
        if (want == PinType::Number)
            return Datum::OfNumber(m_Api.state_number(step.handle, key.c_str(), 0.0));
        // Through Datum::Of rather than OfText().As(): the bag holds the stored form, and for a
        // list or a map the stored form is text that has to be read back as a collection.
        const char* text = m_Api.state_text(step.handle, key.c_str(), "");
        return Datum::Of(doc::Value(std::string(text ? text : "")), want);
    }

    void BlueprintHost::WriteVariable(Step& step, std::string_view name, const Datum& value) {
        if (IsLocal(step, name)) { step.Top().locals[std::string(name)] = value; return; }
        const std::string key(name);
        if (value.type == PinType::Number)
            m_Api.set_state_number(step.handle, key.c_str(), value.number);
        else
            m_Api.set_state_text(step.handle, key.c_str(), value.AsText().c_str());
    }

    // --- reading a value ------------------------------------------------------------------------

    BlueprintHost::Datum BlueprintHost::ReadNamed(Step& step, const BlueprintNode& node,
                                                  std::string_view pin) {
        const std::vector<PinSpec> inputs =
            BlueprintInputs(step.program->Blueprint(), node, step.Top().function);
        if (const PinSpec* found = FindPin(inputs, pin)) return Read(step, node, *found);
        return Datum::OfNumber(0.0);
    }

    BlueprintHost::Datum BlueprintHost::Read(Step& step, const BlueprintNode& node,
                                             const PinSpec& pin) {
        PinType want = pin.type;
        if (want == PinType::Any) {
            const BlueprintVariable* variable = VariableSpec(step, node.target);
            want = variable ? variable->type : PinType::Text;
        }
        // A fixed pin is a design-time fact, so it is never asked of a wire — see PinSpec::fixed.
        if (!pin.fixed)
            if (const BlueprintLink* link = step.Top().canvas->LinkInto(node.id, pin.name)) {
                if (const BlueprintNode* source = step.Top().canvas->Find(link->from)) {
                    if (m_Watching) m_Flow.push_back({ step.program->Component(), link->id });
                    return Value(step, *source, link->fromPin).As(want);
                }
            }
        return Datum::Of(BlueprintLiteral(step.program->Blueprint(), node, pin,
                                          step.Top().function), want);
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

        // A function's Entry hands out what the call was given, which is in the frame.
        if (node.type == "func.entry") {
            const auto found = step.Top().locals.find(std::string(pin));
            if (found != step.Top().locals.end()) return found->second;
            return Datum::OfText("");
        }

        // A loop's index and element exist only while the loop is running, which is exactly when
        // anything can read them.
        if (node.type == "flow.forLoop" || node.type == "flow.forEach") {
            if (pin == "Index") {
                const auto it = step.Top().loopIndex.find(node.id);
                return Datum::OfNumber(it == step.Top().loopIndex.end() ? 0.0 : it->second);
            }
            if (pin == "Element") {
                const auto it = step.Top().loopElement.find(node.id);
                return it == step.Top().loopElement.end() ? Datum::OfText("") : it->second;
            }
        }

        const std::string key = MemoKey(node.id, pin);

        // An impure node's data output is what it left there when it ran. Nothing has run it yet,
        // so there is nothing there — a zero, and never a second call to something with an effect.
        // A call is as pure as the function it calls, which only the blueprint knows.
        if (!IsPureNode(step.program->Blueprint(), node)) {
            const auto it = step.Top().results.find(key);
            return it == step.Top().results.end() ? Datum::OfNumber(0.0) : it->second;
        }

        if (const auto it = step.Top().memo.find(key); it != step.Top().memo.end()) return it->second;

        Datum value = Evaluate(step, node, pin);
        step.Top().memo.emplace(key, value);
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
        if (type == "var.get") return ReadVariable(step, node.target);

        // ---- a pure function --------------------------------------------------------------
        if (type == "func.call") {
            const BlueprintFunction* called = step.program->Blueprint().FindFunction(node.target);
            if (!called) return Datum::OfNumber(0.0);
            std::map<std::string, Datum> arguments;
            for (const BlueprintParam& param : called->params)
                arguments[param.name] = ReadNamed(step, node, param.name).As(param.type);
            std::map<std::string, Datum> results;
            Invoke(step, *called, arguments, &results);
            // Every return value goes in the memo, so reading a second one does not call it twice.
            for (const auto& [name, value] : results)
                step.Top().memo[MemoKey(node.id, name)] = value;
            const auto found = results.find(std::string(pin));
            return found == results.end() ? Datum::OfNumber(0.0) : found->second;
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
        if (type == "sound.playing")
            return Datum::OfBool(api.sound_playing(handle,
                static_cast<unsigned long long>(Num("Voice"))) != 0);

        // ---- lists ------------------------------------------------------------------------------
        if (type == "list.make") {
            std::vector<std::string> items;
            for (const PinSpec& item :
                     BlueprintInputs(step.program->Blueprint(), node, step.Top().function))
                items.push_back(ReadNamed(step, node, item.name).AsText());
            return Datum::OfList(std::move(items));
        }
        if (type.starts_with("list.") && type != "list.split") {
            const std::vector<std::string> items = Get("List").AsList();
            if (type == "list.length") return Datum::OfNumber(static_cast<double>(items.size()));
            if (type == "list.empty")  return Datum::OfBool(items.empty());
            if (type == "list.get") {
                const double at = Num("Index");
                return Datum::OfText(InRange(at, items.size())
                                     ? items[static_cast<std::size_t>(at)] : std::string());
            }
            if (type == "list.contains")
                return Datum::OfBool(std::ranges::find(items, Str("Value")) != items.end());
            if (type == "list.find") {
                const auto found = std::ranges::find(items, Str("Value"));
                return Datum::OfNumber(found == items.end()
                                       ? -1.0 : static_cast<double>(found - items.begin()));
            }
            if (type == "list.append") {
                std::vector<std::string> out = Get("A").AsList();
                const std::vector<std::string> more = Get("B").AsList();
                out.insert(out.end(), more.begin(), more.end());
                return Datum::OfList(std::move(out));
            }
            if (type == "list.reverse") {
                std::vector<std::string> out = items;
                std::ranges::reverse(out);
                return Datum::OfList(std::move(out));
            }
            if (type == "list.sort") {
                std::vector<std::string> out = items;
                if (Get("Numeric").AsBool())
                    std::ranges::sort(out, [](const std::string& a, const std::string& b) {
                        return TextNumber(a) < TextNumber(b);
                    });
                else
                    std::ranges::sort(out);
                return Datum::OfList(std::move(out));
            }
            if (type == "list.slice") {
                const double first = std::max(0.0, Num("First"));
                const double count = std::max(0.0, Num("Count"));
                std::vector<std::string> out;
                for (double i = first; i < first + count && InRange(i, items.size()); i += 1.0)
                    out.push_back(items[static_cast<std::size_t>(i)]);
                return Datum::OfList(std::move(out));
            }
            if (type == "list.join") {
                const std::string separator = Str("Separator");
                std::string out;
                for (std::size_t i = 0; i < items.size(); ++i) {
                    if (i) out += separator;
                    out += items[i];
                }
                return Datum::OfText(std::move(out));
            }
            if (type == "list.rows") {
                // The row-text format, which is what Set Rows takes: the column name, then one
                // row per item. A list of channels becomes a list of channels on screen.
                std::string out = Str("Column");
                for (const std::string& item : items) { out += '\n'; out += item; }
                return Datum::OfText(std::move(out));
            }
        }
        if (type == "list.split") {
            const std::string text = Str("Text");
            const std::string separator = Str("Separator");
            std::vector<std::string> out;
            if (separator.empty()) {
                // An empty separator cuts every character, which is the only other thing anyone
                // could mean by it.
                for (const char c : text) out.emplace_back(1, c);
            } else if (!text.empty()) {
                std::size_t at = 0;
                while (true) {
                    const std::size_t next = text.find(separator, at);
                    out.push_back(text.substr(at, next == std::string::npos
                                                  ? std::string::npos : next - at));
                    if (next == std::string::npos) break;
                    at = next + separator.size();
                }
            }
            return Datum::OfList(std::move(out));
        }

        // ---- maps -------------------------------------------------------------------------------
        if (type.starts_with("map.")) {
            const std::vector<std::pair<std::string, std::string>> entries = Get("Map").AsMap();
            if (type == "map.length") return Datum::OfNumber(static_cast<double>(entries.size()));
            if (type == "map.get") {
                const std::string key = Str("Key");
                for (const auto& entry : entries)
                    if (entry.first == key) return Datum::OfText(entry.second);
                return Datum::OfText(Str("Default"));
            }
            if (type == "map.has") {
                const std::string key = Str("Key");
                for (const auto& entry : entries)
                    if (entry.first == key) return Datum::OfBool(true);
                return Datum::OfBool(false);
            }
            if (type == "map.keys" || type == "map.values") {
                const bool keys = type == "map.keys";
                std::vector<std::string> out;
                out.reserve(entries.size());
                for (const auto& [key, value] : entries) out.push_back(keys ? key : value);
                return Datum::OfList(std::move(out));
            }
        }

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
        if (type == "math.power") return Datum::OfNumber(std::pow(Num("Base"), Num("Exponent")));
        if (type == "math.sqrt") {
            const double v = Num("Value");
            return Datum::OfNumber(v < 0.0 ? 0.0 : std::sqrt(v));
        }
        if (type == "math.sin")   return Datum::OfNumber(std::sin(Num("Radians")));
        if (type == "math.cos")   return Datum::OfNumber(std::cos(Num("Radians")));
        if (type == "math.tan")   return Datum::OfNumber(std::tan(Num("Radians")));
        if (type == "math.asin")  return Datum::OfNumber(std::asin(std::clamp(Num("Value"), -1.0, 1.0)));
        if (type == "math.acos")  return Datum::OfNumber(std::acos(std::clamp(Num("Value"), -1.0, 1.0)));
        if (type == "math.atan2") return Datum::OfNumber(std::atan2(Num("Y"), Num("X")));
        if (type == "math.exp")   return Datum::OfNumber(std::exp(Num("Value")));
        if (type == "math.log") {
            const double v = Num("Value");
            if (v <= 0.0) return Datum::OfNumber(0.0);
            const double base = Num("Base");
            return Datum::OfNumber(base <= 0.0 || base == 1.0 ? std::log(v)
                                                              : std::log(v) / std::log(base));
        }
        if (type == "math.sign") {
            const double v = Num("Value");
            return Datum::OfNumber((v > 0.0) - (v < 0.0));
        }
        if (type == "math.truncate") return Datum::OfNumber(std::trunc(Num("Value")));
        if (type == "math.fraction") {
            const double v = Num("Value");
            return Datum::OfNumber(v - std::trunc(v));
        }
        if (type == "math.lerp") {
            const double a = Num("A"), b = Num("B"), t = Num("Alpha");
            return Datum::OfNumber(a + (b - a) * t);
        }
        if (type == "math.wrap") {
            const double lo = Num("Min"), hi = Num("Max");
            const double span = hi - lo;
            if (span <= 0.0) return Datum::OfNumber(lo);
            double v = std::fmod(Num("Value") - lo, span);
            if (v < 0.0) v += span;
            return Datum::OfNumber(lo + v);
        }
        if (type == "math.degrees")
            return Datum::OfNumber(Num("Radians") * 180.0 / std::numbers::pi);
        if (type == "math.radians")
            return Datum::OfNumber(Num("Degrees") * std::numbers::pi / 180.0);
        if (type == "math.inRange") {
            const double v = Num("Value"), lo = Num("Min"), hi = Num("Max");
            return Datum::OfBool(Get("Inclusive").AsBool() ? (v >= lo && v <= hi)
                                                           : (v > lo && v < hi));
        }
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
        if (type == "compare.nearly")
            return Datum::OfBool(std::abs(Num("A") - Num("B")) <= std::abs(Num("Within")));

        if (type == "logic.and") return Datum::OfBool(Get("A").AsBool() && Get("B").AsBool());
        if (type == "logic.or")  return Datum::OfBool(Get("A").AsBool() || Get("B").AsBool());
        if (type == "logic.not") return Datum::OfBool(!Get("Value").AsBool());

        // ---- text ---------------------------------------------------------------------------------
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
        if (type == "text.substring") {
            const std::string text = Str("Text");
            const double first = std::max(0.0, Num("First"));
            if (!InRange(first, text.size())) return Datum::OfText("");
            const auto at = static_cast<std::size_t>(first);
            const auto count = static_cast<std::size_t>(std::max(0.0, Num("Count")));
            return Datum::OfText(text.substr(at, count));
        }
        if (type == "text.indexOf") {
            const std::size_t found = Str("Text").find(Str("Part"));
            return Datum::OfNumber(found == std::string::npos ? -1.0
                                                              : static_cast<double>(found));
        }
        if (type == "text.replace") {
            std::string text = Str("Text");
            const std::string find = Str("Find");
            if (find.empty()) return Datum::OfText(std::move(text));
            const std::string with = Str("With");
            std::string out;
            std::size_t at = 0;
            while (true) {
                const std::size_t next = text.find(find, at);
                if (next == std::string::npos) { out += text.substr(at); break; }
                out += text.substr(at, next - at);
                out += with;
                at = next + find.size();
            }
            return Datum::OfText(std::move(out));
        }
        if (type == "text.startsWith") return Datum::OfBool(Str("Text").starts_with(Str("Part")));
        if (type == "text.endsWith")   return Datum::OfBool(Str("Text").ends_with(Str("Part")));
        if (type == "text.pad") {
            std::string text = Str("Text");
            const std::string with = Str("With");
            const auto width = static_cast<std::size_t>(std::max(0.0, Num("Width")));
            const bool left = Get("Left").AsBool();
            const char fill = with.empty() ? ' ' : with[0];
            while (text.size() < width) {
                if (left) text.insert(text.begin(), fill);
                else      text += fill;
            }
            return Datum::OfText(std::move(text));
        }
        if (type == "text.repeat") {
            const std::string text = Str("Text");
            const auto times = static_cast<std::size_t>(std::clamp(Num("Times"), 0.0, 100000.0));
            std::string out;
            out.reserve(text.size() * times);
            for (std::size_t i = 0; i < times; ++i) out += text;
            return Datum::OfText(std::move(out));
        }
        if (type == "text.char") {
            const std::string text = Str("Text");
            const double at = Num("Index");
            return Datum::OfText(InRange(at, text.size())
                                 ? std::string(1, text[static_cast<std::size_t>(at)])
                                 : std::string());
        }

        if (type == "select.number")
            return Datum::OfNumber(Get("Condition").AsBool() ? Num("True") : Num("False"));
        if (type == "select.text")
            return Datum::OfText(Get("Condition").AsBool() ? Str("True") : Str("False"));
        if (type == "select.any")
            return Get("Condition").AsBool() ? Get("True") : Get("False");

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
        step.Top().results[MemoKey(node.id, pin)] = value;
        if (m_Watching)
            m_Values[WatchKey(step.program->Component(), node.id, pin)] =
                { value.AsText(), ++m_WatchStep };
    }

    void BlueprintHost::Say(VaeInstance handle, int level, const std::string& text) const {
        if (m_Bound && m_Api.log) m_Api.log(handle, level, text.c_str());
    }

}
