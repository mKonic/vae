#include "vaepch.h"
#include "vae/script/Abi.h"

namespace vae::script {

    const std::vector<std::string>& LuaSelfMethods() {
        // In the order LuaHost binds them, so a diff between the two files reads straight down.
        static const std::vector<std::string> kMethods{
            "has_node", "number", "set_number", "boolean", "set_boolean", "text", "set_text",
            "set_visible", "set_enabled",
            "state", "set_state", "state_text", "set_state_text", "has_state",
            "emit", "navigate", "back", "toast", "after", "cancel", "time",
            "log", "warn", "fail", "component", "instance_name",
            "stored", "set_stored", "stored_text", "set_stored_text", "has_stored", "forget",
            "read_file", "write_file", "file_exists",
            "get", "post", "clock", "date",
            "socket_open", "socket_send", "socket_close", "socket_live",
            "set_rows", "clear_rows", "row_count",
            "play_sound", "stop_sound", "stop_sounds", "sound_playing",
            "sound_volume", "set_sound_volume",
        };
        return kMethods;
    }

    const std::vector<std::string>& LuaEventFields() {
        static const std::vector<std::string> kFields{ "kind", "source", "name", "number", "text" };
        return kFields;
    }

    const std::vector<std::string>& LuaEventKinds() {
        static const std::vector<std::string> kKinds{
            "clicked", "valueChanged", "textChanged", "submitted", "selectionChanged",
            "opened", "closed", "dismissed", "navigated", "scrolled", "timer", "http",
            "socketOpen", "socketMessage", "socketClosed",
        };
        return kKinds;
    }

    const std::vector<std::string>& LuaGlobals() {
        static const std::vector<std::string> kGlobals{
            "vae", "vae.component", "self", "event",
            "on_mount", "on_update", "on_event", "on_unmount",
            "function", "local", "return", "then", "elseif", "tostring", "tonumber", "math.floor",
            "string.format", "table.concat", "ipairs", "pairs",
        };
        return kGlobals;
    }

    const std::vector<std::string>& LuaApi() {
        static const std::vector<std::string> kNames = [] {
            std::vector<std::string> names = LuaGlobals();
            for (const std::string& kind : LuaEventKinds()) names.push_back(kind);
            for (const std::string& field : LuaEventFields()) names.push_back("event." + field);
            for (const std::string& method : LuaSelfMethods()) names.push_back("self:" + method);
            return names;
        }();
        return kNames;
    }

    const std::vector<std::string>& CppApi() {
        static const std::vector<std::string> kNames{
            "VAE_SCRIPT", "vae::Script", "vae::Event",
            "OnMount", "OnUpdate", "OnEvent", "OnUnmount",
            "Exists", "Number", "SetNumber", "Bool", "SetBool", "Text", "SetText",
            "SetColour", "SetVisible", "SetEnabled",
            "State", "SetState", "StateText", "SetStateText", "HasState",
            "Emit", "Navigate", "Back", "Toast", "After", "Cancel", "Time",
            "Log", "Info", "Warn", "Error",
            "Stored", "Store", "StoredText", "HasStored", "Forget",
            "ReadFile", "WriteFile", "FileExists", "Get", "Post", "Clock", "Date",
            "SetRows", "ClearRows", "RowCount",
            "OpenSocket", "SendSocket", "CloseSocket", "SocketLive",
            "VAE_EVENT_SOCKET_OPEN", "VAE_EVENT_SOCKET_MESSAGE", "VAE_EVENT_SOCKET_CLOSED",
            "From", "Is", "Clicked", "Changed", "Timer", "Answered", "Ok",
            "VAE_EVENT_CLICKED", "VAE_EVENT_VALUE_CHANGED", "VAE_EVENT_TEXT_CHANGED",
            "VAE_EVENT_SUBMITTED", "VAE_EVENT_TIMER", "VAE_EVENT_HTTP",
        };
        return kNames;
    }

}
