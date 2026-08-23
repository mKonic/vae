// VaeScript.h — the only header a component script includes.
//
// A thin C++ skin over VaeScriptAPI: nothing here crosses the .so boundary except the C table and
// plain char pointers, so a script and the engine can be built by different compilers, at different
// optimization levels, on different days. Everything is inline, and the whole header costs about
// 30 ms to compile — the engine's own headers cost about 3 s.
//
//   #include <vae/script/VaeScript.h>
//
//   struct Counter : vae::Script {
//       void OnMount() override { Show(); }
//       void OnEvent(const vae::Event& e) override {
//           if (e.Clicked("Increment")) { self.SetState("count", Count() + 1); Show(); }
//       }
//       double Count() const { return self.State("count"); }
//       void Show() { self["Label"].SetText("text", std::to_string((int)Count())); }
//   };
//   VAE_SCRIPT(Counter, "Counter")
#pragma once

#include "vae/script/VaeScriptAPI.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// How a script module hands its three entry points to the engine.
//
// `weak` on the gcc side is not decoration: the three functions below are defined in this header,
// so a script built from more than one translation unit would otherwise be three duplicate-symbol
// errors. MSVC has no weak linkage, and there is no equivalent — a Windows script module is
// therefore one .cpp, which is all NativeHost compiles anyway. See design/windows.md.
#ifdef _MSC_VER
    #define VAE_SCRIPT_EXPORT extern "C" __declspec(dllexport)
#else
    #define VAE_SCRIPT_EXPORT extern "C" __attribute__((visibility("default"), weak))
#endif

namespace vae {

    namespace detail {
        // One per loaded module. The engine hands the table over at load time, before it asks for
        // a single class, so nothing here is ever called through a null.
        inline const VaeScriptAPI* g_Api = nullptr;
        inline std::vector<VaeScriptClass>& Classes() {
            static std::vector<VaeScriptClass> classes;
            return classes;
        }
    }

    using Color = VaeColor;
    using Vec2  = VaeVec2;

    // Rows for a list, a table, or a repeated container. Columns are named because a row template
    // names the part of a row it draws:
    //
    //     vae::Rows rows{ "author", "body", "time" };
    //     rows.Add({ "Ada", "morning", "09:14" });
    //     self["Messages"].SetRows(rows);
    class Rows {
    public:
        Rows() = default;
        Rows(std::initializer_list<const char*> columns) {
            for (const char* column : columns) m_Columns.emplace_back(column);
        }

        // Short rows are padded and long ones are cut, so a row that forgot a column is a blank
        // cell rather than every later row shifted by one.
        Rows& Add(std::initializer_list<std::string> cells) {
            std::size_t at = 0;
            for (const std::string& cell : cells) {
                if (at++ >= m_Columns.size()) break;
                m_Cells.push_back(cell);
            }
            while (at++ < m_Columns.size()) m_Cells.emplace_back();
            return *this;
        }
        void Clear() { m_Cells.clear(); }
        std::size_t Count() const {
            return m_Columns.empty() ? 0 : m_Cells.size() / m_Columns.size();
        }
        const std::vector<std::string>& Columns() const { return m_Columns; }
        const std::vector<std::string>& Cells() const { return m_Cells; }

    private:
        std::vector<std::string> m_Columns;
        std::vector<std::string> m_Cells;
    };

    // A node inside the component, addressed by the name the designer gave it. Names survive a
    // reorder, a restyle and a reload; the index of a child does not.
    class Node {
    public:
        Node(VaeInstance instance, const char* name) : m_Instance(instance), m_Name(name) {}

        bool Exists() const { return detail::g_Api->has_node(m_Instance, m_Name); }

        double Number(const char* prop, double fallback = 0.0) const {
            return detail::g_Api->get_number(m_Instance, m_Name, prop, fallback);
        }
        void SetNumber(const char* prop, double value) {
            detail::g_Api->set_number(m_Instance, m_Name, prop, value);
        }

        bool Bool(const char* prop, bool fallback = false) const {
            return detail::g_Api->get_bool(m_Instance, m_Name, prop, fallback ? 1 : 0) != 0;
        }
        void SetBool(const char* prop, bool value) {
            detail::g_Api->set_bool(m_Instance, m_Name, prop, value ? 1 : 0);
        }

        std::string Text(const char* prop, const char* fallback = "") const {
            return detail::g_Api->get_text(m_Instance, m_Name, prop, fallback);
        }
        void SetText(const char* prop, const std::string& value) {
            detail::g_Api->set_text(m_Instance, m_Name, prop, value.c_str());
        }

        Color Colour(const char* prop, Color fallback = { 0, 0, 0, 0 }) const {
            return detail::g_Api->get_color(m_Instance, m_Name, prop, fallback);
        }
        void SetColour(const char* prop, Color value) {
            detail::g_Api->set_color(m_Instance, m_Name, prop, value);
        }

        void SetVisible(bool visible) { detail::g_Api->set_visible(m_Instance, m_Name, visible ? 1 : 0); }
        void SetEnabled(bool enabled) { detail::g_Api->set_enabled(m_Instance, m_Name, enabled ? 1 : 0); }

        // Rows for a list or a table. The widget virtualizes, so this is a set of strings handed
        // over rather than a node per row — a thousand rows cost the one template that was styled.
        void SetRows(const std::vector<std::vector<std::string>>& rows) {
            std::vector<const char*> flat;
            std::size_t columns = 0;
            for (const auto& row : rows) columns = columns > row.size() ? columns : row.size();
            if (columns == 0) { detail::g_Api->clear_rows(m_Instance, m_Name); return; }
            flat.reserve(rows.size() * columns);
            for (const auto& row : rows)
                for (std::size_t c = 0; c < columns; ++c)
                    flat.push_back(c < row.size() ? row[c].c_str() : "");
            detail::g_Api->set_rows(m_Instance, m_Name, flat.data(),
                                    static_cast<int>(rows.size()), static_cast<int>(columns));
        }
        // One column, for the common case where a list is a list of things and not a table.
        void SetRows(const std::vector<std::string>& rows) {
            std::vector<const char*> flat;
            flat.reserve(rows.size());
            for (const std::string& row : rows) flat.push_back(row.c_str());
            if (flat.empty()) { detail::g_Api->clear_rows(m_Instance, m_Name); return; }
            detail::g_Api->set_rows(m_Instance, m_Name, flat.data(),
                                    static_cast<int>(flat.size()), 1);
        }
        // Named rows: the form a repeated container wants, and the one a table with more than
        // two columns wants too.
        void SetRows(const Rows& rows) {
            if (rows.Columns().empty()) { detail::g_Api->clear_rows(m_Instance, m_Name); return; }
            std::vector<const char*> columns;
            columns.reserve(rows.Columns().size());
            for (const std::string& column : rows.Columns()) columns.push_back(column.c_str());

            std::vector<const char*> cells;
            cells.reserve(rows.Cells().size());
            for (const std::string& cell : rows.Cells()) cells.push_back(cell.c_str());

            detail::g_Api->set_named_rows(m_Instance, m_Name, columns.data(),
                                          static_cast<int>(columns.size()), cells.data(),
                                          static_cast<int>(rows.Count()));
        }
        void ClearRows() { detail::g_Api->clear_rows(m_Instance, m_Name); }
        int RowCount() const { return detail::g_Api->row_count(m_Instance, m_Name); }

        // Where this scroller is. A chat that does not follow the newest message is a chat you
        // have to drag every time somebody speaks.
        void ScrollTo(double y) { detail::g_Api->scroll_to(m_Instance, m_Name, y); }
        void ScrollToEnd() { detail::g_Api->scroll_to_end(m_Instance, m_Name); }
        // Give this node the keyboard. What "and now type" means when the user did not click.
        void Focus() { detail::g_Api->focus(m_Instance, m_Name); }

    private:
        VaeInstance m_Instance;
        const char* m_Name;
    };

    // The script's self: one component instance.
    class Self {
    public:
        Self() = default;
        explicit Self(VaeInstance handle) : m_Handle(handle) {}
        VaeInstance Handle() const { return m_Handle; }

        Node Root() const { return { m_Handle, "" }; }
        Node operator[](const char* node) const { return { m_Handle, node }; }

        // State the engine holds, so a hot reload does not reset the thing on screen. A native
        // script loses every C++ object it owns when its module is unloaded; this survives.
        double State(const char* key, double fallback = 0.0) const {
            return detail::g_Api->state_number(m_Handle, key, fallback);
        }
        void SetState(const char* key, double value) {
            detail::g_Api->set_state_number(m_Handle, key, value);
        }
        // A live connection, by a name of the script's choosing. What arrives comes back as a
        // VAE_EVENT_SOCKET_* tagged with that name.
        void OpenSocket(const char* url, const char* name) {
            detail::g_Api->socket_open(m_Handle, url, name);
        }
        void SendSocket(const char* name, const std::string& text) {
            detail::g_Api->socket_send(m_Handle, name, text.c_str());
        }
        void CloseSocket(const char* name) { detail::g_Api->socket_close(m_Handle, name); }
        bool SocketLive(const char* name) const {
            return detail::g_Api->socket_live(m_Handle, name) != 0;
        }

        // Sound, by the name the asset was imported under. The voice is only worth keeping for
        // something long enough to stop; a click is fire and forget.
        unsigned long long PlaySound(const char* asset, double volume = 1.0, bool loop = false) {
            return detail::g_Api->play_sound(m_Handle, asset, volume, loop ? 1 : 0);
        }
        void StopSound(unsigned long long voice) { detail::g_Api->stop_sound(m_Handle, voice); }
        void StopSounds() { detail::g_Api->stop_sounds(m_Handle); }
        bool SoundPlaying(unsigned long long voice) const {
            return detail::g_Api->sound_playing(m_Handle, voice) != 0;
        }
        double SoundVolume() const { return detail::g_Api->sound_volume(m_Handle); }
        void SetSoundVolume(double volume) { detail::g_Api->set_sound_volume(m_Handle, volume); }

        std::string StateText(const char* key, const char* fallback = "") const {
            return detail::g_Api->state_text(m_Handle, key, fallback);
        }
        void SetStateText(const char* key, const std::string& value) {
            detail::g_Api->set_state_text(m_Handle, key, value.c_str());
        }
        bool HasState(const char* key) const { return detail::g_Api->has_state(m_Handle, key) != 0; }

        void Emit(const char* name, double number = 0.0, const char* text = "") {
            detail::g_Api->emit(m_Handle, name, number, text);
        }
        void Navigate(const char* route) { detail::g_Api->navigate(m_Handle, route); }
        bool Back() { return detail::g_Api->back(m_Handle) != 0; }
        void Toast(const char* text, double seconds = 3.0) {
            detail::g_Api->toast(m_Handle, text, seconds);
        }
        void After(double seconds, const char* name) { detail::g_Api->after(m_Handle, seconds, name); }
        void Cancel(const char* name) { detail::g_Api->cancel(m_Handle, name); }
        double Time() const { return detail::g_Api->time(m_Handle); }

        void Log(VaeLogLevel level, const std::string& text) {
            detail::g_Api->log(m_Handle, static_cast<int>(level), text.c_str());
        }
        void Info(const std::string& text)  { Log(VAE_LOG_INFO, text); }
        void Warn(const std::string& text)  { Log(VAE_LOG_WARN, text); }
        void Error(const std::string& text) { Log(VAE_LOG_ERROR, text); }

        const char* ComponentName() const { return detail::g_Api->component_name(m_Handle); }
        const char* Name() const { return detail::g_Api->instance_name(m_Handle); }

        // --- services ---------------------------------------------------------------------------
        // The app's own durable store, shared by every component in it. Distinct from State above,
        // which belongs to one copy of one component and is not meant to outlive the run.
        double Stored(const char* key, double fallback = 0.0) const {
            return detail::g_Api->store_number(m_Handle, key, fallback);
        }
        void Store(const char* key, double value) {
            detail::g_Api->set_store_number(m_Handle, key, value);
        }
        std::string StoredText(const char* key, const char* fallback = "") const {
            return detail::g_Api->store_text(m_Handle, key, fallback);
        }
        void Store(const char* key, const std::string& value) {
            detail::g_Api->set_store_text(m_Handle, key, value.c_str());
        }
        bool HasStored(const char* key) const {
            return detail::g_Api->has_stored(m_Handle, key) != 0;
        }
        void Forget(const char* key) { detail::g_Api->forget(m_Handle, key); }

        // Files, inside the app's own folders. Anything outside them reads as absent.
        std::string ReadFile(const char* path) const {
            return detail::g_Api->read_file(m_Handle, path);
        }
        bool WriteFile(const char* path, const std::string& text) {
            return detail::g_Api->write_file(m_Handle, path, text.c_str()) != 0;
        }
        bool FileExists(const char* path) const {
            return detail::g_Api->file_exists(m_Handle, path) != 0;
        }

        // The answer arrives later, as an event tagged with `name`, on the thread the script runs
        // on. There is no callback to keep alive and no lock to remember.
        void Get(const char* url, const char* name) {
            detail::g_Api->http_get(m_Handle, url, name);
        }
        void Post(const char* url, const std::string& body, const char* name,
                  const char* contentType = "application/json") {
            detail::g_Api->http_post(m_Handle, url, body.c_str(), contentType, name);
        }

        // Wall-clock, as opposed to Time() above, which is the app's own clock.
        double Clock() const { return detail::g_Api->clock(m_Handle); }
        std::string Date(const char* format = "%Y-%m-%d %H:%M:%S") const {
            return detail::g_Api->date(m_Handle, format);
        }

    private:
        VaeInstance m_Handle = nullptr;
    };

    struct Event {
        VaeEventKind kind = VAE_EVENT_CLICKED;
        const char* source = "";
        const char* name   = "";
        double      number = 0.0;
        const char* text   = "";
        // Set when it happened inside a repeated container: which container, and which copy.
        const char* list   = "";
        int         row    = -1;

        bool From(const char* node) const { return std::strcmp(source, node) == 0; }
        // "A row of this list was clicked" — and `row` says which. The question a list of
        // channels, of people or of anything else asks about every click it gets.
        bool InList(const char* container) const {
            return row >= 0 && std::strcmp(list, container) == 0;
        }
        bool ClickedRow(const char* container) const {
            return kind == VAE_EVENT_CLICKED && InList(container);
        }
        bool Is(VaeEventKind k) const { return kind == k; }
        bool Clicked(const char* node) const { return kind == VAE_EVENT_CLICKED && From(node); }
        bool Changed(const char* node) const {
            return (kind == VAE_EVENT_VALUE_CHANGED || kind == VAE_EVENT_TEXT_CHANGED) && From(node);
        }
        bool Timer(const char* timer) const {
            return kind == VAE_EVENT_TIMER && std::strcmp(name, timer) == 0;
        }
        // An answer from the network, tagged with the name the request was sent with. `number` is
        // the HTTP status — 0 when it never got one, and then `text` is the reason it did not.
        bool Answered(const char* tag) const {
            return kind == VAE_EVENT_HTTP && std::strcmp(name, tag) == 0;
        }
        bool Ok() const { return kind == VAE_EVENT_HTTP && number >= 200.0 && number < 300.0; }
    };

    class Script {
    public:
        virtual ~Script() = default;
        virtual void OnMount() {}
        virtual void OnUpdate(double dt) { (void)dt; }
        virtual void OnEvent(const Event&) {}
        virtual void OnUnmount() {}

        Self self;

        Node operator[](const char* node) const { return self[node]; }
    };

    namespace detail {
        // One C++ object per live instance, created on mount and destroyed on unmount. The engine
        // never sees it: only the four C entry points below cross the boundary.
        template<typename T>
        struct Binding {
            static std::unordered_map<VaeInstance, T*>& Live() {
                static std::unordered_map<VaeInstance, T*> live;
                return live;
            }
            static T* Find(VaeInstance handle) {
                auto it = Live().find(handle);
                return it == Live().end() ? nullptr : it->second;
            }
            static void Mount(VaeInstance handle) {
                T* object = new T();
                object->self = Self(handle);
                Live()[handle] = object;
                object->OnMount();
            }
            static void Update(VaeInstance handle, double dt) {
                if (T* object = Find(handle)) object->OnUpdate(dt);
            }
            static void Dispatch(VaeInstance handle, const VaeEvent* raw) {
                T* object = Find(handle);
                if (!object || !raw) return;
                Event event;
                event.kind   = static_cast<VaeEventKind>(raw->kind);
                event.source = raw->source ? raw->source : "";
                event.name   = raw->name   ? raw->name   : "";
                event.number = raw->number;
                event.text   = raw->text   ? raw->text   : "";
                event.list   = raw->list   ? raw->list   : "";
                event.row    = raw->row;
                object->OnEvent(event);
            }
            static void Unmount(VaeInstance handle) {
                T* object = Find(handle);
                if (!object) return;
                object->OnUnmount();
                Live().erase(handle);
                delete object;
            }
            static bool Register(const char* component) {
                Classes().push_back({ component, &Mount, &Update, &Dispatch, &Unmount });
                return true;
            }
        };
    }

}

// Binds a class to a component, by name. Runs at module load, before the engine asks for anything.
#define VAE_SCRIPT(Type, ComponentName)                                                            \
    namespace {                                                                                    \
        const bool vae_script_registered_##Type =                                                  \
            ::vae::detail::Binding<Type>::Register(ComponentName);                                 \
    }

VAE_SCRIPT_EXPORT unsigned int vae_script_abi(void) { return VAE_SCRIPT_ABI_VERSION; }

VAE_SCRIPT_EXPORT void vae_script_register(const VaeScriptAPI* api) { ::vae::detail::g_Api = api; }

VAE_SCRIPT_EXPORT const VaeScriptClass* vae_script_classes(int* count) {
    auto& classes = ::vae::detail::Classes();
    if (count) *count = static_cast<int>(classes.size());
    return classes.empty() ? nullptr : classes.data();
}
