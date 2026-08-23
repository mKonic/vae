#include "vaepch.h"
#include "vae/doc/Strings.h"

#include "vae/base/FileSystem.h"
#include "vae/doc/Document.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace vae::doc {

    void StringTable::Set(std::string key, std::string text) {
        if (key.empty()) return;
        m_Strings[std::move(key)] = std::move(text);
    }

    std::string_view StringTable::Find(std::string_view key) const {
        const auto it = m_Strings.find(std::string(key));
        return it == m_Strings.end() ? std::string_view{} : std::string_view(it->second);
    }

    // JSON, not the document's own markup: this is the one file in a project a translator opens,
    // and every translation tool on earth reads a flat JSON object.
    bool StringTable::Load(const std::filesystem::path& file, std::string* error) {
        const auto text = FileSystem::ReadText(file);
        if (!text) {
            if (error) *error = "cannot read " + file.string();
            return false;
        }

        nlohmann::json root = nlohmann::json::parse(*text, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            if (error) *error = file.filename().string() + " is not a JSON object of key -> text";
            return false;
        }

        m_Strings.clear();
        for (const auto& [key, value] : root.items())
            if (value.is_string()) m_Strings[key] = value.get<std::string>();

        if (m_Locale.empty()) m_Locale = file.stem().string();
        return true;
    }

    bool StringTable::Save(const std::filesystem::path& file) const {
        nlohmann::json root = nlohmann::json::object();
        for (const auto& [key, text] : m_Strings) root[key] = text;

        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        return FileSystem::WriteText(file, root.dump(2) + "\n");
    }

    void StringTable::CollectFrom(const Document& document) {
        for (Uuid id : document.AllNodes()) {
            const Node* node = document.Find(id);
            if (!node) continue;

            const std::string key = node->props.Text(Prop::TextKey);
            if (key.empty()) continue;
            // Present already means translated already: re-collecting after adding a screen must
            // not overwrite the work somebody did on the keys that were already there.
            if (m_Strings.contains(key)) continue;
            m_Strings[key] = node->props.Text(Prop::Text);
        }
    }

    std::vector<std::string> LocalesIn(const std::filesystem::path& stringsDir) {
        std::vector<std::string> locales;
        std::error_code ec;
        if (!std::filesystem::is_directory(stringsDir, ec)) return locales;

        for (const auto& entry : std::filesystem::directory_iterator(stringsDir, ec))
            if (entry.path().extension() == ".json") locales.push_back(entry.path().stem().string());

        std::sort(locales.begin(), locales.end());
        return locales;
    }

    std::filesystem::path StringsDirFor(const std::filesystem::path& project) {
        return project.empty() ? std::filesystem::path{} : project.parent_path() / "strings";
    }

}
