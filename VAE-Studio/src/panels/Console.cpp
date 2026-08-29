#include "Panels.h"

#include "vae/base/Log.h"

#include <imgui.h>
#include <spdlog/sinks/base_sink.h>

#include <deque>
#include <mutex>

namespace vae {

    namespace {

        struct Entry {
            spdlog::level::level_enum level;
            std::string text;
        };

        // A bounded ring: a long session logs a lot, and an editor panel that grows without limit
        // is a leak with a scrollbar.
        constexpr std::size_t kMaxEntries = 2000;

        std::mutex g_Mutex;
        std::deque<Entry> g_Entries;
        bool g_Installed = false;
        bool g_AutoScroll = true;

        class ConsoleSink final : public spdlog::sinks::base_sink<std::mutex> {
        protected:
            void sink_it_(const spdlog::details::log_msg& msg) override {
                spdlog::memory_buf_t formatted;
                base_sink<std::mutex>::formatter_->format(msg, formatted);

                std::string text = fmt::to_string(formatted);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();

                std::lock_guard lock(g_Mutex);
                g_Entries.push_back({ msg.level, std::move(text) });
                if (g_Entries.size() > kMaxEntries) g_Entries.pop_front();
            }
            void flush_() override {}
        };

        ImVec4 ColourFor(spdlog::level::level_enum level) {
            switch (level) {
                case spdlog::level::err:
                case spdlog::level::critical: return ImVec4(0.90f, 0.35f, 0.35f, 1.0f);
                case spdlog::level::warn:     return ImVec4(0.95f, 0.72f, 0.35f, 1.0f);
                case spdlog::level::debug:
                case spdlog::level::trace:    return ImVec4(0.58f, 0.61f, 0.67f, 1.0f);
                default:                      return ImVec4(0.86f, 0.88f, 0.91f, 1.0f);
            }
        }

    }

    void InitConsolePanel() {
        if (g_Installed) return;
        Log::AddSink(std::make_shared<ConsoleSink>());
        g_Installed = true;
    }

    void DrawConsolePanel() {
        ImGui::Begin("Console###Console");

        if (ImGui::Button("Clear")) {
            std::lock_guard lock(g_Mutex);
            g_Entries.clear();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &g_AutoScroll);
        ImGui::SameLine();
        {
            std::lock_guard lock(g_Mutex);
            ImGui::TextDisabled("%zu lines", g_Entries.size());
        }
        // Which file this session is writing. Worth showing rather than documenting: there is a
        // log per process now, so "the log" is no longer one path somebody can be told once.
        if (!Log::FilePath().empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("· %s", Log::FilePath().filename().c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n(click to copy)", Log::FilePath().c_str());
            if (ImGui::IsItemClicked())
                ImGui::SetClipboardText(Log::FilePath().c_str());
        }
        ImGui::Separator();

        ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard lock(g_Mutex);
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(g_Entries.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const Entry& entry = g_Entries[static_cast<std::size_t>(i)];
                    ImGui::PushStyleColor(ImGuiCol_Text, ColourFor(entry.level));
                    ImGui::TextUnformatted(entry.text.c_str());
                    ImGui::PopStyleColor();
                }
        }
        if (g_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        ImGui::End();
    }

}
