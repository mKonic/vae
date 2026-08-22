#pragma once

#include "Canvas.h"
#include "Debugger.h"
#include "EditorState.h"

#include "vae/script/Runtime.h"
#include "vae/svc/Services.h"

#include <filesystem>
#include <string>
#include <vector>

namespace vae {

    // Everything the Studio needs to run a project's component logic: which language it is written
    // in, where the source lives, how it gets built, and what has to be put back when Play stops.
    //
    // Play is destructive by design — a script's whole job is to change the document — so the
    // document is snapshotted on the way in and restored on the way out. Without that, running the
    // app once would leave the design showing whatever the last frame happened to say.
    class ScriptSession {
    public:
        enum class Language { Lua, Cpp };

        struct Diagnostic {
            std::string file;
            int line = 0;
            int column = 0;
            bool error = true;      // false for a warning
            std::string message;
        };

        void Attach(Canvas& canvas, EditorState& state);

        Language Lang() const { return m_Language; }
        void SetLanguage(Language language);
        // The language a project is written in is which script file it has. Read from disk rather
        // than from the document, because that is where the truth is: a project with a .cpp beside
        // it is a C++ project whatever the Studio was last set to, and opening it in the other
        // language would show an empty editor and claim the project had no logic.
        void AdoptLanguageFor(const std::filesystem::path& projectPath);
        const char* LanguageName() const { return m_Language == Language::Lua ? "Lua" : "C++"; }

        // Follows the project: the script sits beside the document and is named after it.
        void SetProjectPath(const std::filesystem::path& projectPath);
        const std::filesystem::path& SourcePath() const { return m_Source; }

        std::string& Buffer() { return m_Buffer; }
        const std::string& Buffer() const { return m_Buffer; }
        bool Dirty() const { return m_Dirty; }
        void MarkDirty() { m_Dirty = true; }

        // Replace the source outright — the example, and anything else the editor authors for the
        // author. Dirty, so it is written before the next build rather than silently diverging.
        void SetSource(std::string_view source) { m_Buffer = source; m_Dirty = true; m_Built = false; }

        // Show a specific file in the editor. Switching to a .cpp or .lua also switches the
        // project's language, because the language is which file gets built and run.
        void OpenSource(const std::filesystem::path& path);

        // Whether this project has a script at all. Most do not: a screen wired up with declared
        // navigation and the widget library needs no code, and Play has to run it anyway.
        bool HasSource() const;
        // Writes the template out, which is what makes a script exist. Until then the editor is
        // showing an offer, not a file.
        bool CreateSource();

        bool SaveSource();
        // Saves, then builds: a compile for C++, a reload for Lua. Either way what comes back is
        // the language's own diagnostics, with line numbers a script author can act on.
        bool Build();
        bool Built() const { return m_Built; }
        const std::string& Output() const { return m_Output; }
        const std::vector<Diagnostic>& Diagnostics() const { return m_Diagnostics; }

        bool Playing() const { return m_Playing; }
        bool Play();
        void Stop();
        void Toggle() { if (m_Playing) Stop(); else Play(); }

        // Rebuild and swap the running code without stopping. Live state survives it.
        bool HotReload();

        std::size_t LiveInstances() const { return m_Runtime.LiveCount(); }
        // For the Runtime panel, which reads what is mounted and writes into it.
        script::Runtime& Runtime() { return m_Runtime; }
        svc::Services& Services() { return m_Services; }
        Debugger& Debug() { return m_Debugger; }
        const Debugger& Debug() const { return m_Debugger; }

    private:
        void LoadSource();
        void ParseDiagnostics(const std::string& output);
        bool StartHosts();
        std::filesystem::path Artifact() const;

        script::Runtime m_Runtime;
        svc::Services m_Services;
        Debugger m_Debugger;
        Canvas* m_Canvas = nullptr;
        EditorState* m_State = nullptr;

        Language m_Language = Language::Lua;
        std::filesystem::path m_Source;
        std::string m_Buffer;
        std::string m_Output;
        std::vector<Diagnostic> m_Diagnostics;
        std::string m_Snapshot;

        bool m_Dirty = false;
        bool m_Built = false;
        bool m_Playing = false;
    };

}
