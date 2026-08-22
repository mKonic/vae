#include "ScriptSession.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Platform.h"
#include "vae/base/Log.h"
#include "vae/doc/Serializer.h"
#include "vae/script/LuaHost.h"
#include "vae/script/NativeHost.h"

#include <charconv>
#include <filesystem>
#include <system_error>
#include <sstream>

namespace vae {

    namespace {

        // What a new project's script says. It has to run and do nothing, because the alternative
        // is a first Play that fails on a file the author never wrote.
        constexpr const char* kLuaTemplate = R"(-- Component logic, in Lua.
--
-- vae.component binds a class to a component by name. `self` is one instance of it: `self:state`
-- holds anything that must survive a hot reload, and every node is addressed by the name the
-- designer gave it.

-- vae.component("Counter", {
--     on_mount = function(self) self:show() end,
--
--     on_event = function(self, event)
--         if event.kind == "clicked" and event.source == "Increment" then
--             self:set_state("count", self:state("count") + 1)
--             self:show()
--         end
--     end,
--
--     show = function(self)
--         self:set_text("Label", "text", tostring(math.floor(self:state("count"))))
--     end,
-- })
)";

        constexpr const char* kCppTemplate = R"(// Component logic, in C++.
//
// One header, no engine internals: everything crosses the boundary as a C function table, so this
// file compiles in milliseconds and keeps compiling when the engine changes underneath it.

#include <vae/script/VaeScript.h>

#include <string>

// struct Counter : vae::Script {
//     void OnMount() override { Show(); }
//
//     void OnEvent(const vae::Event& event) override {
//         if (event.Clicked("Increment")) {
//             self.SetState("count", Count() + 1);
//             Show();
//         }
//     }
//
//     double Count() const { return self.State("count"); }
//     void Show() { self["Label"].SetText("text", std::to_string(static_cast<int>(Count()))); }
// };
//
// VAE_SCRIPT(Counter, "Counter")
)";

    }

    void ScriptSession::Attach(Canvas& canvas, EditorState& state) {
        m_Canvas = &canvas;
        m_State = &state;
        if (m_Source.empty()) SetProjectPath({});
    }

    void ScriptSession::SetLanguage(Language language) {
        if (language == m_Language) return;
        Stop();
        m_Language = language;
        m_Built = false;
        // The source path is named after the language, so switching means looking at another file.
        SetProjectPath(m_State && !m_State->Path().empty() ? m_State->Path()
                                                           : std::filesystem::path{});
    }

    void ScriptSession::AdoptLanguageFor(const std::filesystem::path& projectPath) {
        if (projectPath.empty()) return;
        std::filesystem::path base = projectPath;
        base.replace_extension();

        std::error_code ec;
        const bool lua = std::filesystem::exists(std::filesystem::path(base).concat(".lua"), ec);
        const bool cpp = std::filesystem::exists(std::filesystem::path(base).concat(".cpp"), ec);
        // Only when it is unambiguous. A project with both is a project someone is porting, and
        // switching the language out from under them would be the wrong half of the guess.
        if (lua == cpp) return;
        SetLanguage(lua ? Language::Lua : Language::Cpp);
    }

    void ScriptSession::SetProjectPath(const std::filesystem::path& projectPath) {
        // An unsaved project's script would sit beside a document that does not exist yet, so it
        // gets the same placeholder folder the document would — under the projects root, never
        // inside the engine's own directory.
        const std::filesystem::path base =
            projectPath.empty() ? FileSystem::ProjectsRoot() / "Untitled" / "Untitled"
                                : std::filesystem::path(projectPath).replace_extension();
        m_Source = base;
        m_Source += (m_Language == Language::Lua ? ".lua" : ".cpp");
        m_Built = false;
        LoadSource();
    }

    std::filesystem::path ScriptSession::Artifact() const {
        if (m_Language == Language::Lua) return m_Source;
        std::filesystem::path out = m_Source;
        return out.replace_extension(platform::ModuleExtension());
    }

    void ScriptSession::OpenSource(const std::filesystem::path& path) {
        if (path == m_Source) return;
        if (m_Dirty) SaveSource();
        if (m_Playing) Stop();

        const std::string ext = path.extension().string();
        if (ext == ".lua") m_Language = Language::Lua;
        else if (ext == ".cpp") m_Language = Language::Cpp;

        m_Source = path;
        m_Built = false;
        LoadSource();
    }

    void ScriptSession::LoadSource() {
        if (const auto text = FileSystem::ReadText(m_Source)) {
            m_Buffer = *text;
        } else {
            m_Buffer = m_Language == Language::Lua ? kLuaTemplate : kCppTemplate;
        }
        m_Dirty = false;
        m_Output.clear();
        m_Diagnostics.clear();
    }

    bool ScriptSession::HasSource() const {
        std::error_code ec;
        return !m_Source.empty() && std::filesystem::exists(m_Source, ec);
    }

    bool ScriptSession::CreateSource() {
        if (HasSource()) return true;
        if (!SaveSource()) return false;
        VAE_INFO("script: created {}", m_Source.filename().string());
        return true;
    }

    bool ScriptSession::SaveSource() {
        if (!FileSystem::WriteText(m_Source, m_Buffer)) {
            VAE_ERROR("could not write {}", m_Source.string());
            return false;
        }
        m_Dirty = false;
        m_Built = false;
        return true;
    }

    // gcc and Lua both say "where: what". Parsing it is what turns a wall of text into something
    // the editor can point at.
    void ScriptSession::ParseDiagnostics(const std::string& output) {
        m_Diagnostics.clear();

        std::istringstream stream(output);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty()) continue;

            // file:line:col: error: message   (gcc)
            // file:line: message              (lua)
            const std::size_t first = line.find(':');
            if (first == std::string::npos) continue;

            Diagnostic diagnostic;
            diagnostic.file = line.substr(0, first);

            std::size_t cursor = first + 1;
            auto number = [&](int& out) {
                const std::size_t start = cursor;
                while (cursor < line.size() && std::isdigit(static_cast<unsigned char>(line[cursor])))
                    ++cursor;
                if (cursor == start) return false;
                std::from_chars(line.data() + start, line.data() + cursor, out);
                return true;
            };

            if (!number(diagnostic.line)) continue;
            if (cursor < line.size() && line[cursor] == ':') {
                ++cursor;
                number(diagnostic.column);
            }
            if (cursor < line.size() && line[cursor] == ':') ++cursor;

            diagnostic.message = line.substr(std::min(cursor + 1, line.size()));
            diagnostic.error = diagnostic.message.find("warning") == std::string::npos;
            m_Diagnostics.push_back(std::move(diagnostic));
        }
    }

    bool ScriptSession::Build() {
        m_Output.clear();
        m_Diagnostics.clear();

        // Nothing to build is not a failure. A project without a script is an ordinary project —
        // the widget library and declared navigation carry plenty on their own.
        if (!m_Dirty && !HasSource()) {
            m_Built = true;
            return true;
        }
        if (m_Dirty && !SaveSource()) return false;

        if (m_Language == Language::Cpp) {
            m_Built = script::NativeHost::Compile(m_Source, Artifact(), &m_Output);
            ParseDiagnostics(m_Output);
            if (!m_Built) return false;
            VAE_INFO("script: built {}", Artifact().filename().string());
            return true;
        }

        // Lua has no build step, so "build" means "does it load at all" — which is the same
        // question, asked early enough to answer it in the editor rather than at Play.
        script::LuaHost probe;
        probe.Bind(m_Runtime.Api());
        std::string error;
        m_Built = probe.Load(m_Source, &error);
        if (!m_Built) {
            m_Output = error;
            ParseDiagnostics(error);
            return false;
        }
        VAE_INFO("script: {} loads ({} component{})", m_Source.filename().string(),
                 probe.Components().size(), probe.Components().size() == 1 ? "" : "s");
        return true;
    }

    bool ScriptSession::StartHosts() {
        m_Runtime.ClearHosts();

        // No script, no hosts. The runtime still mounts the screen, so navigation and every widget
        // behave exactly as they will in the player.
        if (!HasSource()) return true;

        std::string error;
        if (m_Language == Language::Cpp) {
            auto host = CreateScope<script::NativeHost>();
            host->Bind(m_Runtime.Api());
            if (!host->Load(Artifact(), &error)) {
                m_Output = error;
                VAE_ERROR("script: {}", error);
                return false;
            }
            m_Runtime.AddHost(std::move(host));
            return true;
        }

        auto host = CreateScope<script::LuaHost>();
        host->Bind(m_Runtime.Api());
        if (!host->Load(m_Source, &error)) {
            m_Output = error;
            ParseDiagnostics(error);
            VAE_ERROR("script: {}", error);
            return false;
        }
        m_Runtime.AddHost(std::move(host));
        return true;
    }

    bool ScriptSession::Play() {
        if (m_Playing || !m_Canvas || !m_State) return false;

        // Run builds first, and only refuses if the build refuses — with the compiler's own
        // complaint, which was already logged, rather than a second one about the state of a flag.
        if (!Build()) return false;

        // The document as it stands, before a single script has touched it.
        m_Snapshot = doc::Serializer::ToJson(m_State->Doc(), false);

        // The app gets the same services here it would get in the player: its own folder as a
        // sandbox and a store beside the project. Play is meant to be the app, not a rehearsal of it.
        const std::filesystem::path folder = m_Source.parent_path();
        m_Services.FileSystem().AddRoot(folder);
        m_Services.Store().Open(std::filesystem::path(m_Source).replace_extension(".store.json"));

        m_Runtime.Attach(m_Canvas->Host(), m_State->Doc());
        m_Runtime.SetServices(&m_Services);
        if (!StartHosts()) {
            m_Runtime.Detach();
            return false;
        }

        // Everything that must happen inside one frame, in the order it has to happen in: mount
        // what appeared, deliver what the widgets produced, tick, and only then hold the frozen
        // values — after the scripts have had their turn, so freezing actually wins.
        m_Debugger.Attach(m_Runtime);
        m_Canvas->SetPump([this](f32 dt) {
            m_Runtime.Dispatch(m_Canvas->Host().TakeActions());
            // A navigation lands between the layout that just ran and the paint about to run, so
            // the new screen is laid out here rather than being drawn empty for one frame.
            if (m_Canvas->Host().ApplyNavigation()) m_Canvas->ResyncAfterNavigation();
            m_Runtime.Sync();
            m_Runtime.Update(dt);
            m_Services.Tick(dt);
            m_Debugger.ApplyFrozen(m_Runtime, m_Canvas->Host().Tree());
            m_Debugger.Tick();
        });
        m_Canvas->SetPreview(true);
        m_Playing = true;
        if (HasSource()) VAE_INFO("play: running {} logic", LanguageName());
        else             VAE_INFO("play: running the screen — this project has no script");
        return true;
    }

    void ScriptSession::Stop() {
        if (!m_Playing) return;
        m_Playing = false;

        m_Canvas->SetPreview(false);
        m_Canvas->SetPump({});
        // Stopping means stopping: a request still in flight would deliver into a runtime that has
        // let go of the document.
        m_Services.Net().CancelAll();
        m_Services.Store().Flush();
        m_Debugger.Detach(m_Runtime);
        m_Runtime.Detach();
        m_Runtime.ClearHosts();

        // Put the design back. Restored in place so every observer — the view tree above all —
        // keeps its subscription; replacing the Document object would silently unhook them.
        std::string error;
        if (!doc::Serializer::FromJson(m_Snapshot, m_State->Doc(), &error))
            VAE_ERROR("play: could not restore the document: {}", error);
        m_Canvas->Host().MarkDirty();
        m_Snapshot.clear();
        VAE_INFO("play: stopped");
    }

    bool ScriptSession::HotReload() {
        if (!m_Playing) return Build();
        if (!Build()) return false;

        std::string error;
        if (m_Runtime.Reload(&error)) {
            VAE_INFO("play: reloaded, {} instance(s) kept their state", m_Runtime.LiveCount());
            return true;
        }
        m_Output = error;
        return false;
    }

}
