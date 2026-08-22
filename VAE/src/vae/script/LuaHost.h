#pragma once

#include "vae/script/Runtime.h"

#include <map>

namespace vae::script {

    // Lua 5.4 component logic.
    //
    // A script registers classes the same way a native one does, by name:
    //
    //     vae.component("Counter", {
    //         on_mount = function(self) self:set_text("Label", "text", "0") end,
    //         on_event = function(self, e)
    //             if e.kind == "clicked" and e.source == "Increment" then
    //                 self:set_state("count", self:state("count") + 1)
    //                 self:set_text("Label", "text", tostring(math.floor(self:state("count"))))
    //             end
    //         end,
    //     })
    //
    // Each live instance gets its own table whose metatable chains to the class and then to the
    // engine's methods, so `self` is a real Lua object a script can hang fields on — while anything
    // that has to survive a reload goes in the state bag, exactly as in C++.
    //
    // sol2 and the Lua headers stay behind a pimpl: they are heavy, and nothing outside this
    // translation unit has any business seeing a lua_State.
    class LuaHost final : public Host {
    public:
        LuaHost();
        ~LuaHost() override;

        std::string_view Language() const override { return "lua"; }
        void Bind(const VaeScriptAPI& api) override;

        bool Load(const std::filesystem::path& path, std::string* error) override;
        bool Reload(std::string* error) override;
        void Unload() override;
        bool Loaded() const override { return m_Loaded; }

        const VaeScriptClass* Find(std::string_view component) const override;
        std::vector<std::string> Components() const override;

    private:
        struct State;

        static LuaHost* HostFor(VaeInstance handle);
        static void Mount(VaeInstance handle);
        static void Update(VaeInstance handle, double dt);
        static void Event(VaeInstance handle, const VaeEvent* event);
        static void Unmount(VaeInstance handle);

        Scope<State> m_State;
        const VaeScriptAPI* m_Api = nullptr;
        // Pointer-stable by construction: Find hands the address out and the runtime keeps it.
        std::map<std::string, VaeScriptClass> m_Exposed;
        bool m_Loaded = false;
    };

}
