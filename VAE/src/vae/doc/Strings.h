#pragma once

#include "vae/base/Base.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace vae::doc {

    class Document;

    // What an app says, in one language.
    //
    // Separate from the document on purpose. A translation is data somebody who is not the designer
    // produces, usually after the design is finished and often several times; keeping it in the
    // document would mean every translator editing the file the designer is drawing in, and every
    // language reopening the same merge conflict.
    //
    // A node opts in by carrying `Prop::TextKey`. Its `Prop::Text` stays as the authored text, so
    // the canvas draws something readable while it is being designed and an untranslated key falls
    // back to it rather than to nothing. That fallback is the whole safety of the scheme: a missing
    // translation is a screen in the wrong language, never a blank one.
    class StringTable {
    public:
        // The locale this table is for: "en", "pt-BR", or empty for "the authored text".
        const std::string& Locale() const { return m_Locale; }
        void SetLocale(std::string locale) { m_Locale = std::move(locale); }

        void Set(std::string key, std::string text);
        // Empty when the key is not in this table — the caller falls back to the authored text.
        std::string_view Find(std::string_view key) const;
        bool Has(std::string_view key) const { return !Find(key).empty(); }
        std::size_t Count() const { return m_Strings.size(); }
        const std::map<std::string, std::string>& All() const { return m_Strings; }
        void Clear() { m_Strings.clear(); }

        // `strings/<locale>.json` in a project: a flat object, because a translator opening it in
        // any editor on earth has to be able to see what to change.
        bool Load(const std::filesystem::path& file, std::string* error = nullptr);
        bool Save(const std::filesystem::path& file) const;

        // Every key the document uses, with the authored text as the value — the file a translator
        // is handed. Keys already in the table keep the translation they have, so re-running this
        // after adding a screen adds the new keys and touches nothing else.
        void CollectFrom(const Document& document);

        bool operator==(const StringTable&) const = default;

    private:
        std::string m_Locale;
        std::map<std::string, std::string> m_Strings;
    };

    // The locales a project has files for, by name, in the order a picker should show them.
    std::vector<std::string> LocalesIn(const std::filesystem::path& stringsDir);

    // Where a project's translations live, given the project file.
    std::filesystem::path StringsDirFor(const std::filesystem::path& project);

}
