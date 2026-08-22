#pragma once

#include "vae/script/Runtime.h"

namespace vae::script {

    // C++ component logic, compiled ahead of time into a `.so` the engine dlopens.
    //
    // No JIT: a 30 ms compile is below perception, and an AOT object built with `-g` is a thing gdb
    // can step through. The module is copied to a unique file before each load, because dlopen
    // hands back the cached mapping for a path it already holds and a hot reload would then load
    // the code it was trying to replace.
    class NativeHost final : public Host {
    public:
        ~NativeHost() override;

        std::string_view Language() const override { return "cpp"; }
        void Bind(const VaeScriptAPI& api) override;

        bool Load(const std::filesystem::path& path, std::string* error) override;
        bool Reload(std::string* error) override;
        void Unload() override;
        bool Loaded() const override { return m_Handle != nullptr; }

        const VaeScriptClass* Find(std::string_view component) const override;
        std::vector<std::string> Components() const override;

        // Builds `source` into `out`. Diagnostics are the compiler's own, verbatim — a script
        // author needs the line and column, not a summary of them.
        static bool Compile(const std::filesystem::path& source, const std::filesystem::path& out,
                            std::string* diagnostics);

    private:
        void  Close();

        void* m_Handle = nullptr;
        const VaeScriptAPI* m_Api = nullptr;
        std::vector<VaeScriptClass> m_Classes;
        std::filesystem::path m_Loaded;      // the unique copy actually mapped
        u32 m_Generation = 0;
    };

}
