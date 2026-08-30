#include "vaepch.h"
#include "vae/script/LuaHost.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"

#include <sol/sol.hpp>

#include <algorithm>

namespace vae::script {

    namespace {

        // Every live LuaHost. The four trampolines below are plain C function pointers with no
        // room for a `this`, so they recover the host from the instance's component name.
        std::vector<LuaHost*>& Registry() {
            static std::vector<LuaHost*> hosts;
            return hosts;
        }

        const char* KindName(int kind) {
            switch (static_cast<VaeEventKind>(kind)) {
                case VAE_EVENT_CLICKED:           return "clicked";
                case VAE_EVENT_VALUE_CHANGED:     return "value_changed";
                case VAE_EVENT_TEXT_CHANGED:      return "text_changed";
                case VAE_EVENT_SUBMITTED:         return "submitted";
                case VAE_EVENT_SELECTION_CHANGED: return "selection_changed";
                case VAE_EVENT_OPENED:            return "opened";
                case VAE_EVENT_CLOSED:            return "closed";
                case VAE_EVENT_DISMISSED:         return "dismissed";
                case VAE_EVENT_NAVIGATED:         return "navigated";
                case VAE_EVENT_SCROLLED:          return "scrolled";
                case VAE_EVENT_TIMER:             return "timer";
                case VAE_EVENT_SIGNAL:            return "signal";
                case VAE_EVENT_HTTP:              return "http";
                case VAE_EVENT_SOCKET_OPEN:       return "socketOpen";
                case VAE_EVENT_SOCKET_MESSAGE:    return "socketMessage";
                case VAE_EVENT_SOCKET_CLOSED:     return "socketClosed";
            }
            return "unknown";
        }

    }

    struct LuaHost::State {
        sol::state lua;
        std::map<std::string, sol::table> classes;      // component name → class table
        std::map<VaeInstance, sol::table> objects;      // live instance → its own table
        sol::table base;                                // the methods every class inherits
    };

    LuaHost::LuaHost() { Registry().push_back(this); }

    LuaHost::~LuaHost() {
        Unload();
        std::erase(Registry(), this);
    }

    void LuaHost::Bind(const VaeScriptAPI& api) { m_Api = &api; }

    LuaHost* LuaHost::HostFor(VaeInstance handle) {
        for (LuaHost* host : Registry()) {
            if (!host->m_Loaded || !host->m_Api) continue;
            const char* component = host->m_Api->component_name(handle);
            if (component && host->m_State->classes.contains(component)) return host;
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------------- the Lua side

    void LuaHost::Unload() {
        if (m_State) {
            m_State->objects.clear();
            m_State->classes.clear();
        }
        m_State.reset();
        m_Exposed.clear();
        m_Loaded = false;
    }

    bool LuaHost::Load(const std::filesystem::path& path, std::string* error) {
        Unload();
        m_Path = path;

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            if (error) *error = "no script at " + path.string();
            return false;
        }

        m_State = CreateScope<State>();
        bool ok = false;
        // Every sol handle below lives in this scope and dies before Unload can close the state.
        // Destroying a sol::table after its lua_State is gone unrefs into freed memory.
        {
        sol::state& lua = m_State->lua;
        lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::math, sol::lib::table,
                           sol::lib::os);

        // The handle travels in the instance's own table as light userdata: a script never sees it
        // and never needs to, but every method below starts from it.
        auto handleOf = [](const sol::table& self) -> VaeInstance {
            const sol::optional<void*> raw = self.raw_get<sol::optional<void*>>("__handle");
            return raw ? static_cast<VaeInstance>(*raw) : nullptr;
        };

        sol::table base = lua.create_table();
        m_State->base = base;

        // `this`, not a copy of the table pointer: a host is usually loaded before it is handed to
        // the runtime, and the runtime is what binds the table. Capturing it here would capture a
        // null. Nothing can call these before Bind anyway — every lifecycle hook goes through
        // HostFor, which refuses an unbound host.

        base.set_function("has_node", [this, handleOf](sol::table self, std::string node) {
            return m_Api->has_node(handleOf(self), node.c_str()) != 0;
        });
        base.set_function("number", [this, handleOf](sol::table self, std::string node,
                                                   std::string prop, sol::optional<double> fallback) {
            return m_Api->get_number(handleOf(self), node.c_str(), prop.c_str(),
                                   fallback.value_or(0.0));
        });
        base.set_function("set_number", [this, handleOf](sol::table self, std::string node,
                                                        std::string prop, double value) {
            m_Api->set_number(handleOf(self), node.c_str(), prop.c_str(), value);
        });
        base.set_function("boolean", [this, handleOf](sol::table self, std::string node,
                                                     std::string prop, sol::optional<bool> fallback) {
            return m_Api->get_bool(handleOf(self), node.c_str(), prop.c_str(),
                                 fallback.value_or(false) ? 1 : 0) != 0;
        });
        base.set_function("set_boolean", [this, handleOf](sol::table self, std::string node,
                                                         std::string prop, bool value) {
            m_Api->set_bool(handleOf(self), node.c_str(), prop.c_str(), value ? 1 : 0);
        });
        base.set_function("text", [this, handleOf](sol::table self, std::string node,
                                                  std::string prop,
                                                  sol::optional<std::string> fallback) {
            return std::string(m_Api->get_text(handleOf(self), node.c_str(), prop.c_str(),
                                             fallback ? fallback->c_str() : ""));
        });
        base.set_function("set_text", [this, handleOf](sol::table self, std::string node,
                                                      std::string prop, std::string value) {
            m_Api->set_text(handleOf(self), node.c_str(), prop.c_str(), value.c_str());
        });
        // A knob the component declares, on one instance of it: how a screen talks to what is on
        // it without reaching into a component's own nodes.
        base.set_function("property", [this, handleOf](sol::table self, std::string node,
                                                       std::string name,
                                                       sol::optional<std::string> fallback) {
            return std::string(m_Api->get_property(handleOf(self), node.c_str(), name.c_str(),
                                                   fallback ? fallback->c_str() : ""));
        });
        base.set_function("set_property", [this, handleOf](sol::table self, std::string node,
                                                           std::string name, std::string value) {
            m_Api->set_property(handleOf(self), node.c_str(), name.c_str(), value.c_str());
        });
        base.set_function("set_visible", [this, handleOf](sol::table self, std::string node, bool on) {
            m_Api->set_visible(handleOf(self), node.c_str(), on ? 1 : 0);
        });
        base.set_function("set_enabled", [this, handleOf](sol::table self, std::string node, bool on) {
            m_Api->set_enabled(handleOf(self), node.c_str(), on ? 1 : 0);
        });

        base.set_function("state", [this, handleOf](sol::table self, std::string key,
                                                   sol::optional<double> fallback) {
            return m_Api->state_number(handleOf(self), key.c_str(), fallback.value_or(0.0));
        });
        base.set_function("set_state", [this, handleOf](sol::table self, std::string key, double value) {
            m_Api->set_state_number(handleOf(self), key.c_str(), value);
        });
        base.set_function("state_text", [this, handleOf](sol::table self, std::string key,
                                                        sol::optional<std::string> fallback) {
            return std::string(m_Api->state_text(handleOf(self), key.c_str(),
                                               fallback ? fallback->c_str() : ""));
        });
        base.set_function("set_state_text", [this, handleOf](sol::table self, std::string key,
                                                            std::string value) {
            m_Api->set_state_text(handleOf(self), key.c_str(), value.c_str());
        });
        base.set_function("has_state", [this, handleOf](sol::table self, std::string key) {
            return m_Api->has_state(handleOf(self), key.c_str()) != 0;
        });

        base.set_function("emit", [this, handleOf](sol::table self, std::string name,
                                                  sol::optional<double> number,
                                                  sol::optional<std::string> text) {
            m_Api->emit(handleOf(self), name.c_str(), number.value_or(0.0),
                      text ? text->c_str() : "");
        });
        base.set_function("navigate", [this, handleOf](sol::table self, std::string route) {
            m_Api->navigate(handleOf(self), route.c_str());
        });
        base.set_function("back", [this, handleOf](sol::table self) {
            return m_Api->back(handleOf(self)) != 0;
        });
        base.set_function("toast", [this, handleOf](sol::table self, std::string text,
                                                   sol::optional<double> seconds) {
            m_Api->toast(handleOf(self), text.c_str(), seconds.value_or(3.0));
        });
        base.set_function("after", [this, handleOf](sol::table self, double seconds, std::string name) {
            m_Api->after(handleOf(self), seconds, name.c_str());
        });
        base.set_function("cancel", [this, handleOf](sol::table self, std::string name) {
            m_Api->cancel(handleOf(self), name.c_str());
        });
        base.set_function("time", [this, handleOf](sol::table self) { return m_Api->time(handleOf(self)); });
        base.set_function("log", [this, handleOf](sol::table self, std::string text) {
            m_Api->log(handleOf(self), VAE_LOG_INFO, text.c_str());
        });
        base.set_function("warn", [this, handleOf](sol::table self, std::string text) {
            m_Api->log(handleOf(self), VAE_LOG_WARN, text.c_str());
        });
        base.set_function("fail", [this, handleOf](sol::table self, std::string text) {
            m_Api->log(handleOf(self), VAE_LOG_ERROR, text.c_str());
        });
        base.set_function("component", [this, handleOf](sol::table self) {
            return std::string(m_Api->component_name(handleOf(self)));
        });
        base.set_function("instance_name", [this, handleOf](sol::table self) {
            return std::string(m_Api->instance_name(handleOf(self)));
        });

        // --- services. The same surface the C++ facade exposes, named the way Lua names things.
        base.set_function("stored", [this, handleOf](sol::table self, std::string key,
                                                     sol::optional<double> fallback) {
            return m_Api->store_number(handleOf(self), key.c_str(), fallback.value_or(0.0));
        });
        base.set_function("set_stored", [this, handleOf](sol::table self, std::string key,
                                                         double value) {
            m_Api->set_store_number(handleOf(self), key.c_str(), value);
        });
        base.set_function("stored_text", [this, handleOf](sol::table self, std::string key,
                                                          sol::optional<std::string> fallback) {
            return std::string(m_Api->store_text(handleOf(self), key.c_str(),
                                                 fallback.value_or(std::string{}).c_str()));
        });
        base.set_function("set_stored_text", [this, handleOf](sol::table self, std::string key,
                                                              std::string value) {
            m_Api->set_store_text(handleOf(self), key.c_str(), value.c_str());
        });
        base.set_function("has_stored", [this, handleOf](sol::table self, std::string key) {
            return m_Api->has_stored(handleOf(self), key.c_str()) != 0;
        });
        base.set_function("forget", [this, handleOf](sol::table self, std::string key) {
            m_Api->forget(handleOf(self), key.c_str());
        });

        base.set_function("read_file", [this, handleOf](sol::table self, std::string path) {
            return std::string(m_Api->read_file(handleOf(self), path.c_str()));
        });
        base.set_function("write_file", [this, handleOf](sol::table self, std::string path,
                                                          std::string text) {
            return m_Api->write_file(handleOf(self), path.c_str(), text.c_str()) != 0;
        });
        base.set_function("file_exists", [this, handleOf](sol::table self, std::string path) {
            return m_Api->file_exists(handleOf(self), path.c_str()) != 0;
        });

        base.set_function("get", [this, handleOf](sol::table self, std::string url,
                                                   std::string name) {
            m_Api->http_get(handleOf(self), url.c_str(), name.c_str());
        });
        base.set_function("post", [this, handleOf](sol::table self, std::string url,
                                                    std::string body, std::string name,
                                                    sol::optional<std::string> contentType) {
            m_Api->http_post(handleOf(self), url.c_str(), body.c_str(),
                             contentType.value_or(std::string("application/json")).c_str(),
                             name.c_str());
        });

        base.set_function("clock", [this, handleOf](sol::table self) {
            return m_Api->clock(handleOf(self));
        });
        base.set_function("date", [this, handleOf](sol::table self,
                                                    sol::optional<std::string> format) {
            return std::string(m_Api->date(handleOf(self),
                                           format.value_or(std::string("%Y-%m-%d %H:%M:%S")).c_str()));
        });

        base.set_function("socket_open", [this, handleOf](sol::table self, std::string url,
                                                          std::string name) {
            m_Api->socket_open(handleOf(self), url.c_str(), name.c_str());
        });
        base.set_function("socket_send", [this, handleOf](sol::table self, std::string name,
                                                          std::string text) {
            m_Api->socket_send(handleOf(self), name.c_str(), text.c_str());
        });
        base.set_function("socket_close", [this, handleOf](sol::table self, std::string name) {
            m_Api->socket_close(handleOf(self), name.c_str());
        });
        base.set_function("socket_live", [this, handleOf](sol::table self, std::string name) {
            return m_Api->socket_live(handleOf(self), name.c_str()) != 0;
        });

        // Rows for a list, a table or a repeated container: `{ "a", "b" }` for one column,
        // `{ {"a","b"}, ... }` for several, and `{ {author="Ada", body="hi"}, ... }` when the row
        // template names what it draws. Sequences either way, because that is what a table of rows
        // looks like in Lua and asking for a struct per row would be a shape nobody would type
        // twice — but a record row is exactly what a designed row wants, so both are read.
        base.set_function("set_rows", [this, handleOf](sol::table self, std::string node,
                                                       sol::table rows) {
            const std::size_t count = rows.size();

            // Named columns, if the rows are records. The union across every row, sorted, so a row
            // that leaves a column out is a blank cell rather than a different shape of table.
            std::vector<std::string> names;
            for (std::size_t r = 1; r <= count; ++r) {
                sol::optional<sol::table> row = rows[r];
                if (!row) continue;
                for (const auto& [key, value] : *row) {
                    if (key.get_type() != sol::type::string) continue;
                    std::string name = key.as<std::string>();
                    if (std::find(names.begin(), names.end(), name) == names.end())
                        names.push_back(std::move(name));
                }
            }
            if (!names.empty()) {
                std::sort(names.begin(), names.end());
                std::vector<std::string> owned;
                owned.reserve(count * names.size());
                for (std::size_t r = 1; r <= count; ++r) {
                    sol::optional<sol::table> row = rows[r];
                    for (const std::string& name : names)
                        owned.push_back(row ? row->get_or(name, std::string{}) : std::string{});
                }

                std::vector<const char*> columns;
                columns.reserve(names.size());
                for (const std::string& name : names) columns.push_back(name.c_str());
                std::vector<const char*> flatNamed;
                flatNamed.reserve(owned.size());
                for (const std::string& cell : owned) flatNamed.push_back(cell.c_str());

                m_Api->set_named_rows(handleOf(self), node.c_str(), columns.data(),
                                      static_cast<int>(columns.size()), flatNamed.data(),
                                      static_cast<int>(count));
                return;
            }

            std::vector<std::string> cells;
            std::size_t columns = 1;
            for (std::size_t r = 1; r <= count; ++r)
                if (sol::optional<sol::table> row = rows[r]; row)
                    columns = std::max(columns, row->size());

            cells.reserve(count * columns);
            for (std::size_t r = 1; r <= count; ++r) {
                if (sol::optional<sol::table> row = rows[r]; row) {
                    for (std::size_t c = 1; c <= columns; ++c)
                        cells.push_back(row->get_or(c, std::string{}));
                } else {
                    cells.push_back(rows.get_or(r, std::string{}));
                    for (std::size_t c = 1; c < columns; ++c) cells.emplace_back();
                }
            }

            std::vector<const char*> flat;
            flat.reserve(cells.size());
            for (const std::string& cell : cells) flat.push_back(cell.c_str());
            if (flat.empty()) { m_Api->clear_rows(handleOf(self), node.c_str()); return; }
            m_Api->set_rows(handleOf(self), node.c_str(), flat.data(),
                            static_cast<int>(count), static_cast<int>(columns));
        });
        // Sound. `volume` and `loop` are optional because the overwhelming majority of calls are
        // `self:play_sound("click")` and asking for three arguments to make a noise is a tax.
        base.set_function("play_sound", [this, handleOf](sol::table self, std::string asset,
                                                         sol::optional<double> volume,
                                                         sol::optional<bool> loop) {
            return m_Api->play_sound(handleOf(self), asset.c_str(), volume.value_or(1.0),
                                     loop.value_or(false) ? 1 : 0);
        });
        base.set_function("stop_sound", [this, handleOf](sol::table self,
                                                         unsigned long long voice) {
            m_Api->stop_sound(handleOf(self), voice);
        });
        base.set_function("stop_sounds", [this, handleOf](sol::table self) {
            m_Api->stop_sounds(handleOf(self));
        });
        base.set_function("sound_playing", [this, handleOf](sol::table self,
                                                            unsigned long long voice) {
            return m_Api->sound_playing(handleOf(self), voice) != 0;
        });
        base.set_function("sound_volume", [this, handleOf](sol::table self) {
            return m_Api->sound_volume(handleOf(self));
        });
        base.set_function("set_sound_volume", [this, handleOf](sol::table self, double volume) {
            m_Api->set_sound_volume(handleOf(self), volume);
        });

        base.set_function("clear_rows", [this, handleOf](sol::table self, std::string node) {
            m_Api->clear_rows(handleOf(self), node.c_str());
        });
        base.set_function("row_count", [this, handleOf](sol::table self, std::string node) {
            return m_Api->row_count(handleOf(self), node.c_str());
        });

        // Where a scroller is, and what has the keyboard. Both are facts about the running app
        // rather than the design, which is why neither is a property on a node.
        base.set_function("scroll_to", [this, handleOf](sol::table self, std::string node,
                                                        double y) {
            m_Api->scroll_to(handleOf(self), node.c_str(), y);
        });
        base.set_function("scroll_to_end", [this, handleOf](sol::table self, std::string node) {
            m_Api->scroll_to_end(handleOf(self), node.c_str());
        });
        base.set_function("focus", [this, handleOf](sol::table self, std::string node) {
            m_Api->focus(handleOf(self), node.c_str());
        });

        // vae.component(name, class) — the Lua half of VAE_SCRIPT.
        sol::table vae = lua.create_named_table("vae");
        vae.set_function("component", [this](std::string name, sol::table klass) {
            sol::table meta = m_State->lua.create_table();
            meta["__index"] = m_State->base;
            klass[sol::metatable_key] = meta;
            m_State->classes[name] = klass;
        });

        const auto result = lua.safe_script_file(path.string(), sol::script_pass_on_error);
        if (result.valid()) ok = true;
        else if (error) { const sol::error err = result; *error = err.what(); }
        }

        if (!ok) {
            Unload();
            return false;
        }

        for (const auto& [name, klass] : m_State->classes) {
            VaeScriptClass exposed{};
            exposed.on_mount = &LuaHost::Mount;
            exposed.on_update = &LuaHost::Update;
            exposed.on_event = &LuaHost::Event;
            exposed.on_unmount = &LuaHost::Unmount;
            auto [it, inserted] = m_Exposed.emplace(name, exposed);
            it->second.component = it->first.c_str();   // the map's key outlives the entry
        }

        m_Loaded = true;
        VAE_CORE_INFO("script: loaded {} ({} class{})", path.filename().string(), m_Exposed.size(),
                      m_Exposed.size() == 1 ? "" : "es");
        return true;
    }

    bool LuaHost::Reload(std::string* error) {
        if (m_Path.empty()) return true;
        return Load(m_Path, error);
    }

    const VaeScriptClass* LuaHost::Find(std::string_view component) const {
        auto it = m_Exposed.find(std::string(component));
        return it == m_Exposed.end() ? nullptr : &it->second;
    }

    std::vector<std::string> LuaHost::Components() const {
        std::vector<std::string> names;
        names.reserve(m_Exposed.size());
        for (const auto& [name, klass] : m_Exposed) names.push_back(name);
        return names;
    }

    // ---------------------------------------------------------------------------- trampolines

    namespace {
        void CallLifecycle(const sol::table& object, const char* hook) {
            const sol::optional<sol::protected_function> fn =
                object[hook].get<sol::optional<sol::protected_function>>();
            if (!fn || !fn->valid()) return;
            const sol::protected_function_result result = (*fn)(object);
            if (result.valid()) return;
            const sol::error err = result;
            VAE_CORE_ERROR("script: {} failed: {}", hook, err.what());
        }
    }

    void LuaHost::Mount(VaeInstance handle) {
        LuaHost* host = HostFor(handle);
        if (!host) return;
        const auto klass = host->m_State->classes.find(host->m_Api->component_name(handle));
        if (klass == host->m_State->classes.end()) return;

        // The instance's own table: a script may hang whatever it likes on it, and lose it on a
        // reload. Anything it wants back afterwards goes in the state bag.
        sol::table object = host->m_State->lua.create_table();
        object.raw_set("__handle", static_cast<void*>(handle));
        sol::table meta = host->m_State->lua.create_table();
        meta["__index"] = klass->second;
        object[sol::metatable_key] = meta;
        host->m_State->objects[handle] = object;

        CallLifecycle(object, "on_mount");
    }

    void LuaHost::Update(VaeInstance handle, double dt) {
        LuaHost* host = HostFor(handle);
        if (!host) return;
        auto it = host->m_State->objects.find(handle);
        if (it == host->m_State->objects.end()) return;

        const sol::optional<sol::protected_function> fn =
            it->second["on_update"].get<sol::optional<sol::protected_function>>();
        if (!fn || !fn->valid()) return;
        const sol::protected_function_result result = (*fn)(it->second, dt);
        if (!result.valid()) {
            const sol::error err = result;
            VAE_CORE_ERROR("script: on_update failed: {}", err.what());
        }
    }

    void LuaHost::Event(VaeInstance handle, const VaeEvent* raw) {
        LuaHost* host = HostFor(handle);
        if (!host || !raw) return;
        auto it = host->m_State->objects.find(handle);
        if (it == host->m_State->objects.end()) return;

        // GCC reads sol2's string-literal key as one char longer than it is once the whole
        // traverse_get chain inlines, and reports an out-of-bounds it then never performs. A false
        // positive in a vendored template, silenced where it fires rather than project-wide:
        // -Warray-bounds is worth having everywhere else. -isystem does not reach it, because the
        // diagnostic is attributed to the inlined chain rooted in this function.
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Warray-bounds"
#endif
        const sol::optional<sol::protected_function> fn =
            it->second["on_event"].get<sol::optional<sol::protected_function>>();
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif
        if (!fn || !fn->valid()) return;

        sol::table event = host->m_State->lua.create_table();
        event["kind"]   = KindName(raw->kind);
        event["source"] = raw->source ? raw->source : "";
        event["name"]   = raw->name   ? raw->name   : "";
        event["number"] = raw->number;
        event["text"]   = raw->text   ? raw->text   : "";
        // Which row of which list, when it happened inside a repeated container.
        event["list"]   = raw->list   ? raw->list   : "";
        event["row"]    = raw->row;

        const sol::protected_function_result result = (*fn)(it->second, event);
        if (!result.valid()) {
            const sol::error err = result;
            VAE_CORE_ERROR("script: on_event failed: {}", err.what());
        }
    }

    void LuaHost::Unmount(VaeInstance handle) {
        LuaHost* host = HostFor(handle);
        if (!host) return;
        auto it = host->m_State->objects.find(handle);
        if (it == host->m_State->objects.end()) return;
        CallLifecycle(it->second, "on_unmount");
        host->m_State->objects.erase(it);
    }

}
