#include "ScriptSession.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Platform.h"
#include "vae/base/Log.h"
#include "vae/doc/Serializer.h"
#include "vae/script/BlueprintHost.h"
#include "vae/script/LuaHost.h"
#include "vae/script/NativeHost.h"
#include "vae/ui/Library.h"

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

        // What the Script panel says for a project whose logic is drawn. There is no file, so
        // there is nothing to edit here — and saying so is better than an empty editor that looks
        // like a project which lost its script.
        constexpr const char* kGraphNote = R"(-- This project's logic is drawn, not written.
--
-- Open the Blueprint panel. Each screen and each component has its own blueprint: events down the left,
-- what happens along the white wires, values along the coloured ones.
--
-- Nothing in a blueprint is invented — every node is one call in vae/script/VaeScript.h, which is why
-- "Export as C++" produces a script you could have written by hand.
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
        std::filesystem::path base = doc::Project::IsProjectFile(projectPath)
            ? projectPath.parent_path() / projectPath.parent_path().filename()
            : std::filesystem::path(projectPath).replace_extension();

        // A drawn project has no script file to read the language off, so the project index is
        // asked first — it is the only place "this one is drawn" can be written down. A file
        // beside it still wins for the two written languages, because a `.cpp` on disk is a
        // stronger statement than a line in a settings file that may be out of date.
        std::error_code ec;
        const bool lua = std::filesystem::exists(std::filesystem::path(base).concat(".lua"), ec);
        const bool cpp = std::filesystem::exists(std::filesystem::path(base).concat(".cpp"), ec);
        if (lua != cpp) { SetLanguage(lua ? Language::Lua : Language::Cpp); return; }

        if (doc::Project::IsProjectFile(projectPath)) {
            doc::Project settings;
            if (doc::Project::Load(projectPath, settings) && settings.scriptLanguage == "blueprint") {
                SetLanguage(Language::Blueprint);
                return;
            }
        }
        // Nothing said so, but the document might: a folder holding blueprints and no script is a
        // drawn project whatever its index forgot to say.
        if (!lua && !cpp && m_State && m_State->Doc().HasBlueprints()) SetLanguage(Language::Blueprint);
    }

    void ScriptSession::SetProjectPath(const std::filesystem::path& projectPath) {
        // An unsaved project's script would sit beside a document that does not exist yet, so it
        // gets the same placeholder folder the document would — under the projects root, never
        // inside the engine's own directory.
        // A project's script is named after the project, not after the file that indexes it: the
        // index is always called project.vae, so stripping its extension would name every script in
        // every project "project". A single-document project still answers off its own name.
        const std::filesystem::path base =
            projectPath.empty()
                ? FileSystem::ProjectsRoot() / "Untitled" / "Untitled"
            : doc::Project::IsProjectFile(projectPath)
                ? projectPath.parent_path() / projectPath.parent_path().filename()
                : std::filesystem::path(projectPath).replace_extension();
        m_Source = base;
        // A drawn project's logic is inside its documents, so there is no file to name. The base
        // path is kept anyway, because it is what the store and the app's sandbox folder are
        // worked out from, and those are the same wherever the logic is written.
        if (m_Language == Language::Lua)      m_Source += ".lua";
        else if (m_Language == Language::Cpp) m_Source += ".cpp";
        m_Built = false;
        LoadSource();
    }

    std::filesystem::path ScriptSession::Artifact() const {
        // A drawn project's logic ships inside the documents, which the app already carries. There
        // is nothing else to build and nothing else to copy.
        if (m_Language == Language::Blueprint) return {};
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
        if (m_Language == Language::Blueprint) {
            m_Buffer = kGraphNote;
        } else if (const auto text = FileSystem::ReadText(m_Source)) {
            m_Buffer = *text;
        } else {
            m_Buffer = m_Language == Language::Lua ? kLuaTemplate : kCppTemplate;
        }
        m_Dirty = false;
        m_Output.clear();
        m_Diagnostics.clear();
    }

    bool ScriptSession::HasSource() const {
        // For a drawn project the question is whether anything has been drawn, which is a fact
        // about the document rather than about the folder it is saved in.
        if (m_Language == Language::Blueprint) return m_State && m_State->Doc().HasBlueprints();
        std::error_code ec;
        return !m_Source.empty() && std::filesystem::exists(m_Source, ec);
    }

    bool ScriptSession::CreateSource() {
        // There is no file to write: a blueprint is made by drawing one in the Blueprint panel, and it is
        // saved with the screen it drives.
        if (m_Language == Language::Blueprint) return true;
        if (HasSource()) return true;
        if (!SaveSource()) return false;
        VAE_INFO("script: created {}", m_Source.filename().string());
        return true;
    }

    bool ScriptSession::SaveSource() {
        if (m_Language == Language::Blueprint) { m_Dirty = false; return true; }
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
        if (m_Language != Language::Blueprint && !m_Dirty && !HasSource()) {
            m_Built = true;
            return true;
        }
        if (m_Dirty && !SaveSource()) return false;

        if (m_Language == Language::Blueprint) {
            m_Built = BuildBlueprints();
            return m_Built;
        }

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

    // Compiling every blueprint in the project, which for a drawn project is what "build" means: no
    // file is produced, the answer is the diagnostics. Done against a host of its own so the one
    // that is running is not disturbed by a check.
    bool ScriptSession::BuildBlueprints() {
        if (!m_State) return false;

        script::BlueprintHost probe;
        probe.Bind(m_Runtime.Api());
        probe.Adopt(m_State->Doc());

        std::string output;
        m_Diagnostics.clear();
        for (const script::BlueprintHost::Message& message : probe.Messages()) {
            output += message.component + ": " + message.message + "\n";
            // Built in one piece rather than field by field: at -O2 GCC cannot see that the
            // default member initialisers ran and warns about reading `error` uninitialised.
            m_Diagnostics.push_back({ message.component, static_cast<int>(message.node), 0,
                                      message.error, message.message });
        }
        m_Output = output;
        if (probe.ErrorCount() > 0) return false;

        const std::size_t blueprints = probe.Components().size();
        VAE_INFO("blueprint: {} blueprint{} compile", blueprints, blueprints == 1 ? "" : "s");
        return true;
    }

    bool ScriptSession::StartHosts() {
        m_Runtime.ClearHosts();
        m_Blueprints = nullptr;

        if (m_Language == Language::Blueprint) {
            if (!m_State) return false;
            auto host = CreateScope<script::BlueprintHost>();
            host->Bind(m_Runtime.Api());
            host->Adopt(m_State->Doc());
            if (host->ErrorCount() > 0) {
                m_Output = host->Messages().front().component + ": "
                         + host->Messages().front().message;
                VAE_ERROR("blueprint: {}", m_Output);
                return false;
            }
            m_Blueprints = host.get();
            m_Runtime.AddHost(std::move(host));
            return true;
        }

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

        // The rows the designer typed are a drawing aid, and the app is about to say what its
        // real ones are. Off before the first frame runs, so nobody sees invented names in a
        // running app.
        m_Canvas->ShowSampleRows(false);

        // The document as it stands, before a single script has touched it.
        // keepIds: this is restored in place so that every observer keeps its subscription, and an
        // observer that survives is holding an id. A file drops the ids nothing references; this
        // cannot.
        m_Snapshot = doc::Serializer::ToXml(m_State->Doc(), false, &ui::StandardLibrary(), true);

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
        m_Canvas->ShowSampleRows(true);
        // Stopping means stopping: a request still in flight would deliver into a runtime that has
        // let go of the document.
        m_Services.Net().CancelAll();
        m_Services.Store().Flush();
        m_Debugger.Detach(m_Runtime);
        m_Runtime.Detach();
        m_Runtime.ClearHosts();
        m_Blueprints = nullptr;

        // Put the design back. Restored in place so every observer — the view tree above all —
        // keeps its subscription; replacing the Document object would silently unhook them.
        std::string error;
        if (!doc::Serializer::FromXml(m_Snapshot, m_State->Doc(), &error, &ui::StandardLibrary()))
            VAE_ERROR("play: could not restore the document: {}", error);
        m_Canvas->Host().MarkDirty();
        m_Snapshot.clear();
        VAE_INFO("play: stopped");
    }

    bool ScriptSession::HotReload() {
        if (!m_Playing) return Build();
        if (!Build()) return false;

        // A blueprint host holds a copy of every blueprint it compiled, so it has to be handed the
        // document again before the reload — otherwise the reload recompiles the blueprint as it was
        // when Play started, which is the one thing a reload is for changing.
        if (m_Blueprints && m_State) m_Blueprints->Adopt(m_State->Doc());

        std::string error;
        if (m_Runtime.Reload(&error)) {
            VAE_INFO("play: reloaded, {} instance(s) kept their state", m_Runtime.LiveCount());
            return true;
        }
        m_Output = error;
        return false;
    }

}
