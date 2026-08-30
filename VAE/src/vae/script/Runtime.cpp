#include "vaepch.h"
#include "vae/script/Runtime.h"

#include "vae/base/Log.h"

#include <algorithm>

namespace vae::script {

    namespace {
        // An event with nothing in it. Everything the engine raises starts here, so a field
        // nobody filled in reads as absent rather than as row zero of a list called "".
        VaeEvent BlankEvent() {
            VaeEvent event{};
            event.source = "";
            event.name = "";
            event.text = "";
            event.list = "";
            event.row = -1;
            return event;
        }
    }


    namespace {

        VaeEventKind KindOf(ui::ActionKind kind) {
            switch (kind) {
                case ui::ActionKind::Clicked:          return VAE_EVENT_CLICKED;
                case ui::ActionKind::ValueChanged:     return VAE_EVENT_VALUE_CHANGED;
                case ui::ActionKind::TextChanged:      return VAE_EVENT_TEXT_CHANGED;
                case ui::ActionKind::Submitted:        return VAE_EVENT_SUBMITTED;
                case ui::ActionKind::SelectionChanged: return VAE_EVENT_SELECTION_CHANGED;
                case ui::ActionKind::Opened:           return VAE_EVENT_OPENED;
                case ui::ActionKind::Closed:           return VAE_EVENT_CLOSED;
                case ui::ActionKind::Dismissed:        return VAE_EVENT_DISMISSED;
                case ui::ActionKind::Navigated:        return VAE_EVENT_NAVIGATED;
                case ui::ActionKind::Scrolled:         return VAE_EVENT_SCROLLED;
            }
            return VAE_EVENT_CLICKED;
        }

        f64 NumberOf(const doc::Value& value) {
            if (const f32* number = std::get_if<f32>(&value)) return *number;
            if (const bool* flag = std::get_if<bool>(&value)) return *flag ? 1.0 : 0.0;
            return 0.0;
        }

        std::string TextOf(const doc::Value& value) {
            if (const std::string* text = std::get_if<std::string>(&value)) return *text;
            return {};
        }

    }

    Runtime::Runtime() { BuildApi(); }
    Runtime::~Runtime() { Detach(); }

    void Runtime::Attach(ui::UiHost& host, doc::Document& document) {
        Detach();
        m_Host = &host;
        m_Document = &document;
    }

    void Runtime::Detach() {
        UnmountAll();
        m_Live.clear();
        m_Signals.clear();
        m_Host = nullptr;
        m_Document = nullptr;
    }

    void Runtime::AddHost(Scope<Host> host) {
        if (!host) return;
        host->Bind(m_Api);
        m_Hosts.push_back(std::move(host));
    }

    void Runtime::ClearHosts() {
        UnmountAll();
        m_Live.clear();
        m_Hosts.clear();
    }

    const VaeScriptClass* Runtime::ClassFor(std::string_view component) const {
        for (const auto& host : m_Hosts)
            if (const VaeScriptClass* klass = host->Find(component)) return klass;
        return nullptr;
    }

    const std::map<std::string, doc::Value>* Runtime::StateOf(Uuid instance) const {
        auto it = m_Live.find(instance);
        return it == m_Live.end() ? nullptr : &it->second->state;
    }

    std::vector<Runtime::LiveScript> Runtime::LiveScripts() const {
        std::vector<LiveScript> out;
        out.reserve(m_Live.size());
        for (const auto& [instance, record] : m_Live)
            out.push_back({ instance, record->componentName, record->instanceName,
                            record->timers.size() });
        // Stable order, or the debugger's list reshuffles itself every frame under the reader's
        // cursor — unordered_map iteration order is nobody's idea of a sort.
        std::ranges::sort(out, [](const LiveScript& a, const LiveScript& b) {
            return a.component != b.component ? a.component < b.component : a.name < b.name;
        });
        return out;
    }

    bool Runtime::SetState(Uuid instance, const std::string& key, doc::Value value) {
        auto it = m_Live.find(instance);
        if (it == m_Live.end()) return false;
        it->second->state[key] = std::move(value);
        return true;
    }

    // ---------------------------------------------------------------------------- lifecycle

    void Runtime::Sync() {
        if (!m_Host || !m_Document) return;

        // Everything on screen: the main tree and anything presented over it. A dialog's script has
        // to mount when the dialog opens, and it lives in an overlay tree.
        const std::vector<ui::ViewTree*> trees = m_Host->Trees();

        // What is present, and what each one is scripted as. A screen is named by its own name; an
        // instance by its component's. Painter order puts an instance's root first, so the first
        // view wins and nested parts are skipped.
        struct Present {
            Uuid id;
            std::string name;      // the component or screen name a class binds to
            Uuid component;
            bool isScreen = false;
        };
        std::vector<Present> present;

        for (const ui::ViewTree* tree : trees) {
            if (const doc::Node* root = m_Document->Find(tree->RootId());
                root && root->kind == doc::NodeKind::Screen)
                present.push_back({ root->id, root->name, root->id, true });

            for (u32 i = 0; i < tree->ViewCount(); ++i) {
                const Uuid instance = tree->At(i).instanceId;
                if (!instance.Valid()) continue;
                if (std::ranges::any_of(present, [&](const Present& e) { return e.id == instance; }))
                    continue;
                // A copy of a nested instance has no node of its own; the authored instance it came
                // from does, and that is what says which component is on screen here.
                const doc::Node* node = m_Document->Find(tree->At(i).authoredId);
                if (!node || !node->IsInstance()) continue;
                const doc::Node* master = m_Document->Find(node->componentId);
                if (!master) continue;
                present.push_back({ instance, master->name, node->componentId, false });
            }
        }

        // Gone from the screen means gone, state bag included. A hidden node is still on screen;
        // this only fires when the instance really left the document or the screen changed.
        for (auto it = m_Live.begin(); it != m_Live.end();) {
            const bool stillHere = std::ranges::any_of(present,
                                                       [&](const Present& e) { return e.id == it->first; });
            if (stillHere) { ++it; continue; }
            Record& record = *it->second;
            if (record.klass && record.klass->on_unmount) record.klass->on_unmount(record.Handle());
            it = m_Live.erase(it);
        }

        for (const Present& entry : present) {
            if (m_Live.contains(entry.id)) continue;
            const VaeScriptClass* klass = ClassFor(entry.name);
            if (!klass) continue;

            auto record = CreateScope<Record>();
            record->runtime = this;
            record->instance = entry.id;
            record->component = entry.component;
            record->componentName = entry.name;
            record->isScreen = entry.isScreen;
            record->instanceName = entry.name;
            if (!entry.isScreen)
                for (const ui::ViewTree* tree : trees) {
                    const u32 view = RootViewIn(*tree, entry.id);
                    if (view != ui::ViewTree::kInvalid) {
                        record->instanceName = tree->At(view).name;
                        break;
                    }
                }
            record->klass = klass;

            Record* raw = record.get();
            m_Live.emplace(entry.id, std::move(record));
            if (klass->on_mount) klass->on_mount(raw->Handle());
        }
    }

    void Runtime::MountAll() {
        for (auto& [instance, record] : m_Live) {
            record->klass = ClassFor(record->componentName);
            if (record->klass && record->klass->on_mount) record->klass->on_mount(record->Handle());
        }
    }

    void Runtime::UnmountAll() {
        for (auto& [instance, record] : m_Live)
            if (record->klass && record->klass->on_unmount)
                record->klass->on_unmount(record->Handle());
    }

    bool Runtime::Reload(std::string* error) {
        // The records survive, so every state bag and every pending timer survives with them. That
        // is the whole difference between a reload and a restart.
        UnmountAll();
        for (auto& [instance, record] : m_Live) record->klass = nullptr;

        bool ok = true;
        for (auto& host : m_Hosts) {
            std::string reason;
            if (host->Reload(&reason)) { host->Bind(m_Api); continue; }
            ok = false;
            VAE_CORE_ERROR("script: reloading {} failed: {}", host->Path().string(), reason);
            if (error && error->empty()) *error = reason;
        }

        MountAll();
        Sync();
        return ok;
    }

    // Everything the live connections have said since the last frame, handed to the scripts that
    // opened them. On the main thread, in order, like every other event a script sees.
    std::filesystem::path Runtime::SoundPath(std::string_view asset) const {
        if (!m_Services) return {};

        std::string relative(asset);
        if (m_Document) {
            for (const doc::Document::Asset& known : m_Document->Assets()) {
                if (known.name != asset) continue;
                relative = known.path;
                break;
            }
        }
        // Through Files rather than joined by hand: the app's folders are its sandbox, and a sound
        // named "../../../etc/passwd" should come back empty for the same reason a file read does.
        return m_Services->FileSystem().Resolve(relative);
    }

    void Runtime::PumpSockets() {
        if (!m_Services || m_SocketOwners.empty()) return;

        std::vector<std::string> gone;
        for (const auto& [name, owner] : m_SocketOwners) {
            svc::Socket* socket = m_Services->FindLive(name);
            if (!socket) { gone.push_back(name); continue; }

            const auto found = m_Live.find(owner);
            if (found == m_Live.end()) {
                // The script that wanted this left the screen. Nobody is listening, so the socket
                // is not left holding a connection open for a component that no longer exists.
                gone.push_back(name);
                continue;
            }

            Record& record = *found->second;
            socket->Pump([&](const svc::Socket::Event& incoming) {
                VaeEvent event = BlankEvent();
                event.source = "";
                event.name = name.c_str();
                event.number = 0.0;
                event.text = incoming.text.c_str();
                switch (incoming.kind) {
                    case svc::Socket::Event::Kind::Opened:
                        event.kind = VAE_EVENT_SOCKET_OPEN; break;
                    case svc::Socket::Event::Kind::Message:
                        event.kind = VAE_EVENT_SOCKET_MESSAGE; break;
                    case svc::Socket::Event::Kind::Closed:
                        event.kind = VAE_EVENT_SOCKET_CLOSED; break;
                    case svc::Socket::Event::Kind::Failed:
                        event.kind = VAE_EVENT_SOCKET_CLOSED;
                        event.number = 1.0;
                        break;
                }
                Deliver(record, event);
            });
        }

        for (const std::string& name : gone) {
            m_Services->CloseLive(name);
            m_SocketOwners.erase(name);
        }
    }

    bool Runtime::HasPendingTimers() const {
        for (const auto& [instance, record] : m_Live)
            if (!record->timers.empty()) return true;
        return false;
    }

    void Runtime::Update(f32 dt) {
        if (!m_Host) return;

        PumpSockets();

        // Timers first: a timer that fires this frame should be visible to the update that follows.
        for (auto& [instance, record] : m_Live) {
            if (record->timers.empty()) continue;
            std::vector<std::string> fired;
            for (Timer& timer : record->timers) {
                timer.remaining -= dt;
                if (timer.remaining <= 0.0f) fired.push_back(timer.name);
            }
            if (fired.empty()) continue;
            std::erase_if(record->timers, [](const Timer& t) { return t.remaining <= 0.0f; });
            for (const std::string& name : fired) {
                VaeEvent event = BlankEvent();
                event.kind = VAE_EVENT_TIMER;
                event.source = "";
                event.name = name.c_str();
                event.text = "";
                Deliver(*record, event);
            }
        }

        // Signals are queued rather than delivered inline: a script emitting from inside on_event
        // would otherwise re-enter the very dispatch it is being called from.
        if (!m_Signals.empty()) {
            const std::vector<Signal> pending = std::move(m_Signals);
            m_Signals.clear();
            for (const Signal& signal : pending) {
                for (auto& [instance, record] : m_Live) {
                    if (instance == signal.from) continue;
                    VaeEvent event = BlankEvent();
                    event.kind = VAE_EVENT_SIGNAL;
                    event.source = "";
                    event.name = signal.name.c_str();
                    event.number = signal.number;
                    event.text = signal.text.c_str();
                    Deliver(*record, event);
                }
            }
        }

        for (auto& [instance, record] : m_Live)
            if (record->klass && record->klass->on_update)
                record->klass->on_update(record->Handle(), dt);
    }

    // Which script hears an action. Usually the instance it came from — but when a component is
    // built out of other components, a click on the Button inside it belongs to the component that
    // placed the Button, not to the Button. So an action nobody claims bubbles up the view tree to
    // the nearest live script, which is the rule the web has taught every designer already.
    Runtime::Record* Runtime::Receiver(const ui::Action& action) {
        if (auto it = m_Live.find(action.instance); it != m_Live.end()) return it->second.get();
        if (!m_Host) return nullptr;

        for (ui::ViewTree* tree : m_Host->Trees()) {
            u32 view = tree->ViewOf(ui::WidgetId{ action.source, action.instance });
            if (view == ui::ViewTree::kInvalid) continue;

            while (view != ui::ViewTree::kInvalid) {
                const Uuid instance = tree->At(view).instanceId;
                if (instance.Valid())
                    if (auto it = m_Live.find(instance); it != m_Live.end()) return it->second.get();
                view = tree->At(view).parent;
            }
            // Nothing between the widget and the top claimed it, so it belongs to the screen — the
            // outermost scripted thing there is, and the one a designer means by "this screen".
            if (auto it = m_Live.find(tree->RootId()); it != m_Live.end()) return it->second.get();
            return nullptr;
        }
        return nullptr;
    }

    void Runtime::Dispatch(const std::vector<ui::Action>& actions) {
        for (const ui::Action& action : actions) {
            Record* receiver = Receiver(action);
            if (!receiver) continue;
            Record& record = *receiver;

            const std::string text = TextOf(action.value);
            VaeEvent event = BlankEvent();
            event.kind   = KindOf(action.kind);
            event.source = action.name.c_str();
            event.name   = "";
            event.number = NumberOf(action.value);
            event.text   = text.c_str();

            // Which row of which list, when it happened inside one. Every copy of a repeated
            // container shares one document node, so the name of the thing clicked cannot say
            // which copy it was and the index has to travel beside it.
            std::string list;
            if (m_Host) {
                for (ui::ViewTree* tree : m_Host->Trees()) {
                    const u32 view = tree->ViewOf(ui::WidgetId{ action.source, action.instance });
                    if (view == ui::ViewTree::kInvalid) continue;
                    if (const u32 copy = tree->RowOwner(view); copy != ui::ViewTree::kInvalid) {
                        event.row = tree->At(copy).row;
                        const u32 owner = tree->At(copy).parent;
                        if (owner != ui::ViewTree::kInvalid) list = tree->At(owner).name;
                    }
                    break;
                }
            }
            event.list = list.c_str();
            Deliver(record, event);
        }
    }

    void Runtime::Deliver(Record& record, const VaeEvent& event) {
        if (m_Trace) m_Trace(record.instance, event);
        if (record.klass && record.klass->on_event) record.klass->on_event(record.Handle(), &event);
    }

    // ---------------------------------------------------------------------------- addressing

    u32 Runtime::RootViewIn(const ui::ViewTree& tree, Uuid instance) {
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).instanceId == instance) return i;   // painter order: root first
        return ui::ViewTree::kInvalid;
    }

    // The tree a record lives in, and its root view within it. A screen is the root of its own
    // tree; an instance is a view somewhere inside one.
    ui::ViewTree* Runtime::TreeOf(const Record& record, u32* rootView) const {
        if (!m_Host) return nullptr;
        for (ui::ViewTree* tree : m_Host->Trees()) {
            if (record.isScreen) {
                if (tree->RootId() != record.instance) continue;
                if (rootView) *rootView = tree->Root();
                return tree;
            }
            const u32 view = RootViewIn(*tree, record.instance);
            if (view == ui::ViewTree::kInvalid) continue;
            if (rootView) *rootView = view;
            return tree;
        }
        return nullptr;
    }

    u32 Runtime::RootViewOf(const Record& record) const {
        u32 view = ui::ViewTree::kInvalid;
        TreeOf(record, &view);
        return view;
    }

    // A virtualized list's view of the same rows a repeated container gets. One table, two
    // readers: the widget that draws its own rows asks a cell at a time, the container flattens
    // the whole thing into copies.
    struct Runtime::Rows final : public ui::UiHost::ListDataSource {
        doc::RowTable table;

        u32 Count() const override { return table.Count(); }
        std::string Cell(u32 row, u32 column) const override {
            return std::string(table.Cell(row, column));
        }
    };

    ui::WidgetId Runtime::WidgetOf(u32 view) const {
        const ui::ViewTree::View& node = m_Host->Tree().At(view);
        return ui::WidgetId{ node.sourceId, node.instanceId };
    }

    ui::WidgetId Runtime::WidgetIn(const ui::ViewTree& tree, u32 view) {
        const ui::ViewTree::View& node = tree.At(view);
        return ui::WidgetId{ node.sourceId, node.instanceId };
    }

    void Runtime::PutRows(Record& record, const char* node, std::vector<std::string> columns,
                          const char* const* cells, u32 total) {
        u32 root = ui::ViewTree::kInvalid;
        ui::ViewTree* tree = TreeOf(record, &root);
        const u32 view = ViewFor(record, node);
        if (!tree || view == ui::ViewTree::kInvalid || !m_Host) return;

        const ui::WidgetId widget = WidgetIn(*tree, view);
        if (columns.empty()) {
            tree->ClearRows(widget);
            m_Host->SetDataSource(widget, nullptr);
            m_Host->MarkDirty();
            return;
        }

        doc::RowTable table;
        table.columns = std::move(columns);
        table.cells.reserve(total);
        for (u32 i = 0; i < total; ++i)
            table.cells.emplace_back(cells && cells[i] ? cells[i] : "");

        auto source = CreateRef<Rows>();
        source->table = table;
        m_Host->SetDataSource(widget, std::move(source));
        tree->SetRows(widget, std::move(table));
        // The copies are made when the tree is rebuilt, and the tree is rebuilt because of this.
        m_Host->MarkDirty();
    }

    void Runtime::ScrollTo(Record& record, const char* node, f32 y, bool toEnd) {
        u32 root = ui::ViewTree::kInvalid;
        ui::ViewTree* tree = TreeOf(record, &root);
        const u32 view = ViewFor(record, node);
        if (!tree || view == ui::ViewTree::kInvalid) return;

        // The scroller is the node itself, or the nearest one above it: a script says "the
        // messages", meaning the thing they are inside.
        u32 scroller = ui::ViewTree::kInvalid;
        for (u32 at = view; at != ui::ViewTree::kInvalid; at = tree->At(at).parent) {
            const ui::Role role = tree->At(at).role;
            if (role == ui::Role::Scroll || role == ui::Role::List || role == ui::Role::Table) {
                scroller = at;
                break;
            }
        }
        if (scroller == ui::ViewTree::kInvalid) {
            VAE_CORE_WARN("[{}] '{}' is not in anything that scrolls", record.componentName,
                          node ? node : "");
            return;
        }

        if (toEnd) {
            tree->KeepAtEnd(WidgetIn(*tree, scroller));
            if (m_Host) m_Host->MarkDirty();
            return;
        }
        const f32 limit = std::max(tree->ContentSize(scroller).y
                                   - tree->Bounds(scroller).size.y, 0.0f);
        tree->SetScroll(scroller, { tree->At(scroller).scroll.x, std::clamp(y, 0.0f, limit) });
    }

    namespace {

        // Breadth-first, so the nearest name wins. It matters: the library's Button has a text node
        // called Label inside it, and a component that places a Button next to its own Label must
        // still mean its own.
        u32 FindNamed(const ui::ViewTree& tree, u32 root, std::string_view name) {
            std::vector<u32> queue{ root };
            for (std::size_t at = 0; at < queue.size(); ++at) {
                const u32 view = queue[at];
                if (view != root && tree.At(view).name == name) return view;
                for (const u32 child : tree.At(view).children) queue.push_back(child);
            }
            return ui::ViewTree::kInvalid;
        }

    }

    // A name, resolved inside the component that owns the script. The search covers the instance's
    // whole subtree rather than only the views tagged with its id, because a component composed out
    // of other components places them by name — and "the Button I called Increment" has to keep
    // meaning that whether the designer drew a frame or dropped in the library's Button.
    //
    // A dotted name — "Sidebar.Header.Close" — scopes each step to what the step before it found.
    // A bare name keeps meaning "the nearest one"; a dotted one is how you say which of two
    // identically-named things you meant, which a bare name cannot express at all.
    u32 Runtime::ViewFor(const Record& record, const char* node) const {
        u32 root = ui::ViewTree::kInvalid;
        const ui::ViewTree* found = TreeOf(record, &root);
        if (!found || root == ui::ViewTree::kInvalid || !node || node[0] == '\0') return root;

        const ui::ViewTree& tree = *found;
        std::string_view path{ node };
        u32 at = root;
        while (!path.empty()) {
            const std::size_t dot = path.find('.');
            const std::string_view segment = path.substr(0, dot);
            path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
            // "A..B", ".A" and "A." all mean "A.B" and "A": an empty step names nothing, so it
            // cannot narrow anything either.
            if (segment.empty()) continue;
            at = FindNamed(tree, at, segment);
            if (at == ui::ViewTree::kInvalid) return ui::ViewTree::kInvalid;
        }
        return at;
    }

    // ---------------------------------------------------------------------------- the C table

    namespace {

        std::optional<doc::Prop> PropOf(const char* name) {
            if (!name) return std::nullopt;
            if (auto prop = doc::PropFromName(name)) return prop;
            VAE_CORE_WARN("script: '{}' is not a known property", name);
            return std::nullopt;
        }

    }

    void Runtime::BuildApi() {
        m_Api = {};
        m_Api.abiVersion = VAE_SCRIPT_ABI_VERSION;

        m_Api.has_node = [](VaeInstance handle, const char* node) -> int {
            Record& record = Of(handle);
            return record.runtime->ViewFor(record, node) != ui::ViewTree::kInvalid ? 1 : 0;
        };

        m_Api.get_number = [](VaeInstance handle, const char* node, const char* prop,
                              double fallback) -> double {
            Record& record = Of(handle);
            const auto key = PropOf(prop);
            const u32 view = record.runtime->ViewFor(record, node);
            if (!key || view == ui::ViewTree::kInvalid) return fallback;
            return record.runtime->m_Host->Tree().Number(view, *key, static_cast<f32>(fallback));
        };

        m_Api.set_number = [](VaeInstance handle, const char* node, const char* prop, double value) {
            Record& record = Of(handle);
            const auto key = PropOf(prop);
            const u32 view = record.runtime->ViewFor(record, node);
            if (!key || view == ui::ViewTree::kInvalid) return;
            record.runtime->m_Host->Tree().SetViewProp(view, *key, static_cast<f32>(value));
        };

        m_Api.get_bool = [](VaeInstance handle, const char* node, const char* prop,
                            int fallback) -> int {
            Record& record = Of(handle);
            const auto key = PropOf(prop);
            const u32 view = record.runtime->ViewFor(record, node);
            if (!key || view == ui::ViewTree::kInvalid) return fallback;
            return record.runtime->m_Host->Tree().Flag(view, *key, fallback != 0) ? 1 : 0;
        };

        m_Api.set_bool = [](VaeInstance handle, const char* node, const char* prop, int value) {
            Record& record = Of(handle);
            const auto key = PropOf(prop);
            const u32 view = record.runtime->ViewFor(record, node);
            if (!key || view == ui::ViewTree::kInvalid) return;
            record.runtime->m_Host->Tree().SetViewProp(view, *key, value != 0);
        };

        m_Api.get_text = [](VaeInstance handle, const char* node, const char* prop,
                            const char* fallback) -> const char* {
            Record& record = Of(handle);
            const auto key = PropOf(prop);
            const u32 view = record.runtime->ViewFor(record, node);
            if (!key || view == ui::ViewTree::kInvalid) return fallback ? fallback : "";
            return record.Keep(record.runtime->m_Host->Tree().Str(view, *key,
                                                                 fallback ? fallback : ""));
        };

        m_Api.set_text = [](VaeInstance handle, const char* node, const char* prop,
                            const char* value) {
            Record& record = Of(handle);
            const auto key = PropOf(prop);
            const u32 view = record.runtime->ViewFor(record, node);
            if (!key || view == ui::ViewTree::kInvalid) return;
            record.runtime->m_Host->Tree().SetViewProp(view, *key, std::string(value ? value : ""));
        };

        m_Api.get_color = [](VaeInstance handle, const char* node, const char* prop,
                             VaeColor fallback) -> VaeColor {
            Record& record = Of(handle);
            const auto key = PropOf(prop);
            const u32 view = record.runtime->ViewFor(record, node);
            if (!key || view == ui::ViewTree::kInvalid) return fallback;
            const doc::Value value = record.runtime->m_Host->Tree().ResolvedProp(view, *key);
            if (const Color* colour = std::get_if<Color>(&value))
                return { colour->r, colour->g, colour->b, colour->a };
            return fallback;
        };

        m_Api.set_color = [](VaeInstance handle, const char* node, const char* prop, VaeColor value) {
            Record& record = Of(handle);
            const auto key = PropOf(prop);
            const u32 view = record.runtime->ViewFor(record, node);
            if (!key || view == ui::ViewTree::kInvalid) return;
            record.runtime->m_Host->Tree().SetViewProp(view, *key,
                                                       Color{ value.r, value.g, value.b, value.a });
        };

        m_Api.set_visible = [](VaeInstance handle, const char* node, int visible) {
            Record& record = Of(handle);
            const u32 view = record.runtime->ViewFor(record, node);
            if (view == ui::ViewTree::kInvalid) return;
            record.runtime->m_Host->Tree().SetViewProp(view, doc::Prop::Visible, visible != 0);
        };

        m_Api.set_enabled = [](VaeInstance handle, const char* node, int enabled) {
            Record& record = Of(handle);
            const u32 view = record.runtime->ViewFor(record, node);
            if (view == ui::ViewTree::kInvalid) return;
            record.runtime->m_Host->Tree().SetViewProp(view, doc::Prop::Enabled, enabled != 0);
        };

        // --- state ---------------------------------------------------------------------------

        m_Api.state_number = [](VaeInstance handle, const char* key, double fallback) -> double {
            Record& record = Of(handle);
            auto it = record.state.find(key ? key : "");
            if (it == record.state.end()) return fallback;
            return NumberOf(it->second);
        };

        m_Api.set_state_number = [](VaeInstance handle, const char* key, double value) {
            Of(handle).state[key ? key : ""] = static_cast<f32>(value);
        };

        m_Api.state_text = [](VaeInstance handle, const char* key,
                              const char* fallback) -> const char* {
            Record& record = Of(handle);
            auto it = record.state.find(key ? key : "");
            if (it == record.state.end()) return fallback ? fallback : "";
            return record.Keep(TextOf(it->second));
        };

        m_Api.set_state_text = [](VaeInstance handle, const char* key, const char* value) {
            Of(handle).state[key ? key : ""] = std::string(value ? value : "");
        };

        m_Api.has_state = [](VaeInstance handle, const char* key) -> int {
            return Of(handle).state.contains(key ? key : "") ? 1 : 0;
        };

        // --- the app around it -----------------------------------------------------------------

        m_Api.emit = [](VaeInstance handle, const char* name, double number, const char* text) {
            Record& record = Of(handle);
            record.runtime->m_Signals.push_back({ record.instance, name ? name : "", number,
                                                  text ? text : "" });
        };

        // One verb, two destinations. A screen wins over a route because it is the bigger thing:
        // "go to Settings" means the Settings screen if there is one, and a router's Settings pane
        // only when there is not.
        m_Api.navigate = [](VaeInstance handle, const char* route) {
            Record& record = Of(handle);
            ui::UiHost& host = *record.runtime->m_Host;
            const std::string where = route ? route : "";

            if (host.GoToScreen(where)) return;

            const u32 router = host.Tree().FindRole(host.Tree().Root(), ui::Role::Router);
            if (router == ui::ViewTree::kInvalid) {
                VAE_CORE_WARN("[{}] navigate('{}'): no screen by that name and no router on screen",
                              record.componentName, where);
                return;
            }
            host.Navigate({ host.Tree().At(router).sourceId, host.Tree().At(router).instanceId },
                          where);
        };

        m_Api.back = [](VaeInstance handle) -> int {
            Record& record = Of(handle);
            ui::UiHost& host = *record.runtime->m_Host;
            // The overlay or the screen stack first: that is what "back" means to whoever pressed
            // it. A router's history is the innermost thing and goes last.
            if (host.GoBack()) return 1;

            const u32 router = host.Tree().FindRole(host.Tree().Root(), ui::Role::Router);
            if (router == ui::ViewTree::kInvalid) return 0;
            return host.Back({ host.Tree().At(router).sourceId,
                               host.Tree().At(router).instanceId }) ? 1 : 0;
        };

        // A toast is a widget like any other: the script does not invent one, it shows the one the
        // screen already has. Without a Toast on screen there is nothing to show it in.
        m_Api.toast = [](VaeInstance handle, const char* text, double seconds) {
            Record& record = Of(handle);
            ui::UiHost& host = *record.runtime->m_Host;
            ui::ViewTree& tree = host.Tree();
            const u32 toast = tree.FindRole(tree.Root(), ui::Role::Toast);
            if (toast == ui::ViewTree::kInvalid) {
                VAE_CORE_WARN("script: toast('{}') with no Toast on screen", text ? text : "");
                return;
            }
            const ui::WidgetId id{ tree.At(toast).sourceId, tree.At(toast).instanceId };
            const u32 label = tree.FindRole(toast, ui::Role::Content);
            tree.SetViewProp(label == ui::ViewTree::kInvalid ? toast : label, doc::Prop::Text,
                             std::string(text ? text : ""));
            host.OpenOverlay(id, tree.At(toast).sourceId, false, {}, static_cast<f32>(seconds));
        };

        m_Api.after = [](VaeInstance handle, double seconds, const char* name) {
            Record& record = Of(handle);
            const std::string key = name ? name : "";
            // One pending timer per name: asking again restarts it rather than stacking a second.
            for (Timer& timer : record.timers)
                if (timer.name == key) { timer.remaining = static_cast<f32>(seconds); return; }
            record.timers.push_back({ key, static_cast<f32>(seconds) });
        };

        m_Api.cancel = [](VaeInstance handle, const char* name) {
            Record& record = Of(handle);
            const std::string key = name ? name : "";
            std::erase_if(record.timers, [&](const Timer& t) { return t.name == key; });
        };

        m_Api.time = [](VaeInstance handle) -> double {
            return Of(handle).runtime->m_Host->Time();
        };

        m_Api.log = [](VaeInstance handle, int level, const char* text) {
            Record& record = Of(handle);
            const std::string& who = record.componentName;
            const char* body = text ? text : "";
            switch (level) {
                case VAE_LOG_TRACE: VAE_CORE_TRACE("[{}] {}", who, body); break;
                case VAE_LOG_WARN:  VAE_CORE_WARN ("[{}] {}", who, body); break;
                case VAE_LOG_ERROR: VAE_CORE_ERROR("[{}] {}", who, body); break;
                default:            VAE_CORE_INFO ("[{}] {}", who, body); break;
            }
        };

        m_Api.component_name = [](VaeInstance handle) -> const char* {
            return Of(handle).componentName.c_str();
        };

        m_Api.instance_name = [](VaeInstance handle) -> const char* {
            return Of(handle).instanceName.c_str();
        };

        // --- services ------------------------------------------------------------------------
        // Every one of these answers harmlessly when there are no services: a script written for an
        // app must still run in a preview, and "the store is empty" beats a crash.

        m_Api.store_number = [](VaeInstance handle, const char* key, double fallback) -> double {
            svc::Services* services = Of(handle).runtime->m_Services;
            if (!services || !key) return fallback;
            const doc::Value value = services->Store().Get(key);
            return doc::TypeOf(value) == doc::ValueType::Number ? std::get<f32>(value) : fallback;
        };

        m_Api.set_store_number = [](VaeInstance handle, const char* key, double value) {
            svc::Services* services = Of(handle).runtime->m_Services;
            if (services && key) services->Store().Set(key, static_cast<f32>(value));
        };

        m_Api.store_text = [](VaeInstance handle, const char* key,
                              const char* fallback) -> const char* {
            Record& record = Of(handle);
            svc::Services* services = record.runtime->m_Services;
            if (!services || !key) return fallback ? fallback : "";
            const doc::Value value = services->Store().Get(key);
            if (doc::TypeOf(value) != doc::ValueType::Text) return fallback ? fallback : "";
            return record.Keep(std::get<std::string>(value));
        };

        m_Api.set_store_text = [](VaeInstance handle, const char* key, const char* value) {
            svc::Services* services = Of(handle).runtime->m_Services;
            if (services && key) services->Store().Set(key, std::string(value ? value : ""));
        };

        m_Api.has_stored = [](VaeInstance handle, const char* key) -> int {
            svc::Services* services = Of(handle).runtime->m_Services;
            return services && key && services->Store().Has(key) ? 1 : 0;
        };

        m_Api.forget = [](VaeInstance handle, const char* key) {
            svc::Services* services = Of(handle).runtime->m_Services;
            if (services && key) services->Store().Remove(key);
        };

        m_Api.read_file = [](VaeInstance handle, const char* path) -> const char* {
            Record& record = Of(handle);
            svc::Services* services = record.runtime->m_Services;
            if (!services || !path) return "";
            const auto text = services->FileSystem().Read(path);
            return text ? record.Keep(*text) : "";
        };

        m_Api.write_file = [](VaeInstance handle, const char* path, const char* text) -> int {
            svc::Services* services = Of(handle).runtime->m_Services;
            if (!services || !path) return 0;
            return services->FileSystem().Write(path, text ? text : "") ? 1 : 0;
        };

        m_Api.file_exists = [](VaeInstance handle, const char* path) -> int {
            svc::Services* services = Of(handle).runtime->m_Services;
            return services && path && services->FileSystem().Exists(path) ? 1 : 0;
        };

        m_Api.http_get = [](VaeInstance handle, const char* url, const char* name) {
            Of(handle).runtime->Fetch(Of(handle), { "GET", url ? url : "" }, name ? name : "");
        };

        m_Api.http_post = [](VaeInstance handle, const char* url, const char* body,
                             const char* contentType, const char* name) {
            svc::Request request{ "POST", url ? url : "" };
            request.body = body ? body : "";
            if (contentType && contentType[0]) request.contentType = contentType;
            Of(handle).runtime->Fetch(Of(handle), std::move(request), name ? name : "");
        };

        // A live connection, owned by the services and remembered against the instance that asked
        // for it — the same reason a request is held by id: the instance can leave the screen
        // before the server says anything, and delivering to an unmounted script is a crash.
        m_Api.socket_open = [](VaeInstance handle, const char* url, const char* name) {
            Record& record = Of(handle);
            Runtime& runtime = *record.runtime;
            if (!runtime.m_Services) {
                VAE_CORE_WARN("[{}] no services: the socket went nowhere", record.componentName);
                return;
            }
            const std::string tag = name && name[0] ? name : "socket";
            runtime.m_SocketOwners[tag] = record.instance;

            std::string error;
            if (!runtime.m_Services->Live(tag).Open(url ? url : "", &error)) {
                VaeEvent event = BlankEvent();
                event.kind = VAE_EVENT_SOCKET_CLOSED;
                event.source = "";
                event.name = tag.c_str();
                event.number = 1.0;
                event.text = error.c_str();
                runtime.Deliver(record, event);
                runtime.m_Services->CloseLive(tag);
                runtime.m_SocketOwners.erase(tag);
            }
        };

        m_Api.socket_send = [](VaeInstance handle, const char* name, const char* text) {
            Runtime& runtime = *Of(handle).runtime;
            if (!runtime.m_Services) return;
            if (svc::Socket* socket = runtime.m_Services->FindLive(name ? name : "socket"))
                socket->Send(text ? text : "");
        };

        m_Api.socket_close = [](VaeInstance handle, const char* name) {
            Runtime& runtime = *Of(handle).runtime;
            if (!runtime.m_Services) return;
            const std::string tag = name && name[0] ? name : "socket";
            runtime.m_Services->CloseLive(tag);
            runtime.m_SocketOwners.erase(tag);
        };

        m_Api.socket_live = [](VaeInstance handle, const char* name) -> int {
            Runtime& runtime = *Of(handle).runtime;
            if (!runtime.m_Services) return 0;
            const svc::Socket* socket = runtime.m_Services->FindLive(name ? name : "socket");
            return socket && socket->Connected() ? 1 : 0;
        };

        // Sound. Named the way a designer names it, because "click" is what the Assets panel shows
        // and `assets/ui/click-01.wav` is not what anyone would type.
        m_Api.play_sound = [](VaeInstance handle, const char* asset, double volume,
                              int loop) -> unsigned long long {
            Record& record = Of(handle);
            Runtime& runtime = *record.runtime;
            if (!runtime.m_Services || !asset || !asset[0]) return 0;

            const std::filesystem::path file = runtime.SoundPath(asset);
            if (file.empty()) {
                VAE_CORE_WARN("[{}] no sound called '{}'", record.componentName, asset);
                return 0;
            }
            return runtime.m_Services->Sound().Play(file, static_cast<f32>(volume), loop != 0);
        };

        m_Api.stop_sound = [](VaeInstance handle, unsigned long long voice) {
            Runtime& runtime = *Of(handle).runtime;
            if (runtime.m_Services) runtime.m_Services->Sound().Stop(voice);
        };

        m_Api.stop_sounds = [](VaeInstance handle) {
            Runtime& runtime = *Of(handle).runtime;
            if (runtime.m_Services) runtime.m_Services->Sound().StopAll();
        };

        m_Api.sound_playing = [](VaeInstance handle, unsigned long long voice) -> int {
            Runtime& runtime = *Of(handle).runtime;
            return runtime.m_Services && runtime.m_Services->Sound().Playing(voice) ? 1 : 0;
        };

        m_Api.sound_volume = [](VaeInstance handle) -> double {
            Runtime& runtime = *Of(handle).runtime;
            return runtime.m_Services ? runtime.m_Services->Sound().MasterVolume() : 0.0;
        };

        m_Api.set_sound_volume = [](VaeInstance handle, double volume) {
            Runtime& runtime = *Of(handle).runtime;
            if (runtime.m_Services)
                runtime.m_Services->Sound().SetMasterVolume(static_cast<f32>(volume));
        };

        m_Api.clock = [](VaeInstance handle) -> double {
            svc::Services* services = Of(handle).runtime->m_Services;
            return services ? services->Now() : 0.0;
        };

        m_Api.date = [](VaeInstance handle, const char* format) -> const char* {
            Record& record = Of(handle);
            svc::Services* services = record.runtime->m_Services;
            if (!services) return "";
            return record.Keep(services->Date(format && format[0] ? format
                                                                  : "%Y-%m-%d %H:%M:%S"));
        };

        // Rows for a virtualized list or table. The seam already existed for C++ callers; without
        // this a script could style the template and never put anything in it.
        m_Api.set_rows = [](VaeInstance handle, const char* node, const char* const* cells,
                            int rows, int columns) {
            Record& record = Of(handle);
            const u32 width = static_cast<u32>(std::max(columns, 1));
            // Unnamed columns are numbered, so a positional caller and a named one produce the
            // same table and a template can still bind to "0" if it wants to.
            std::vector<std::string> names;
            names.reserve(width);
            for (u32 i = 0; i < width; ++i) names.push_back(std::to_string(i));
            record.runtime->PutRows(record, node, std::move(names), cells,
                                    static_cast<u32>(std::max(rows, 0)) * width);
        };

        m_Api.set_named_rows = [](VaeInstance handle, const char* node, const char* const* columns,
                                  int columnCount, const char* const* cells, int rows) {
            Record& record = Of(handle);
            std::vector<std::string> names;
            names.reserve(static_cast<std::size_t>(std::max(columnCount, 0)));
            for (int i = 0; i < std::max(columnCount, 0); ++i)
                names.emplace_back(columns && columns[i] ? columns[i] : "");
            const u32 total = static_cast<u32>(std::max(rows, 0))
                            * static_cast<u32>(names.size());
            record.runtime->PutRows(record, node, std::move(names), cells, total);
        };

        m_Api.clear_rows = [](VaeInstance handle, const char* node) {
            Record& record = Of(handle);
            record.runtime->PutRows(record, node, {}, nullptr, 0);
        };

        m_Api.row_count = [](VaeInstance handle, const char* node) -> int {
            Record& record = Of(handle);
            u32 root = ui::ViewTree::kInvalid;
            ui::ViewTree* tree = record.runtime->TreeOf(record, &root);
            const u32 view = record.runtime->ViewFor(record, node);
            if (!tree || view == ui::ViewTree::kInvalid) return 0;
            if (const doc::RowTable* table = tree->RowsOf(record.runtime->WidgetIn(*tree, view)))
                return static_cast<int>(table->Count());
            return 0;
        };

        // Where a scroller is. Clamped by the widget, so "further than there is" is the end and
        // not an empty view of nothing.
        m_Api.scroll_to = [](VaeInstance handle, const char* node, double y) {
            Record& record = Of(handle);
            record.runtime->ScrollTo(record, node, static_cast<f32>(y), false);
        };
        m_Api.scroll_to_end = [](VaeInstance handle, const char* node) {
            Record& record = Of(handle);
            record.runtime->ScrollTo(record, node, 0.0f, true);
        };

        m_Api.focus = [](VaeInstance handle, const char* node) {
            Record& record = Of(handle);
            u32 root = ui::ViewTree::kInvalid;
            ui::ViewTree* tree = record.runtime->TreeOf(record, &root);
            const u32 view = record.runtime->ViewFor(record, node);
            if (!tree || view == ui::ViewTree::kInvalid || !record.runtime->m_Host) return;
            // Only into the tree events are going to: focusing a node underneath an open dialog
            // would give the keyboard to something the user cannot see.
            if (tree != &record.runtime->m_Host->ActiveTree()) {
                VAE_CORE_WARN("[{}] cannot focus '{}': something is presented over it",
                              record.componentName, node ? node : "");
                return;
            }
            record.runtime->m_Host->Focus(tree->BehaviorOwner(view));
        };
    }

    // A request whose answer comes back to one instance, by name. Held as an id rather than a
    // captured pointer: the instance can leave the screen before the network answers, and delivering
    // an event to a script that has already been unmounted is a use-after-free.
    void Runtime::Fetch(Record& record, svc::Request request, std::string name) {
        if (!m_Services) {
            VAE_CORE_WARN("[{}] no services: {} went nowhere", record.componentName, request.url);
            return;
        }

        const Uuid instance = record.instance;
        m_Services->Net().Send(std::move(request),
                               [this, instance, name = std::move(name)](const svc::Response& answer) {
            const auto it = m_Live.find(instance);
            if (it == m_Live.end()) return;      // it left while the request was in flight

            VaeEvent event = BlankEvent();
            event.kind = VAE_EVENT_HTTP;
            event.source = "";
            event.name = name.c_str();
            event.number = static_cast<double>(answer.status);
            event.text = answer.status == 0 ? answer.error.c_str() : answer.body.c_str();
            Deliver(*it->second, event);
        });
    }

}
