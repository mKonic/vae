#pragma once

#include "vae/core/Layer.h"

#include <filesystem>

namespace vae {

    // `VAE-Studio --convert <in> [out]` — read a document in whatever format it is in and write it
    // as format 3. No window and no device: it is the document layer and the standard library, and
    // both are headless.
    //
    // It exists because a format change has to be checkable against real files rather than only
    // against fixtures the same code built. `--convert --check` reads the result back and compares
    // it node for node against what went in, and says so. `--bench N` loads the input N times and
    // reports the mean, which is how the "JSON parses faster" half of D8 gets retired with a number
    // rather than a shrug.
    class ConvertLayer final : public Layer {
    public:
        ConvertLayer(std::filesystem::path in, std::filesystem::path out, bool check, int bench)
            : Layer("Convert"), m_In(std::move(in)), m_Out(std::move(out)), m_Check(check),
              m_Bench(bench) {}
        void OnAttach() override;

    private:
        std::filesystem::path m_In;
        std::filesystem::path m_Out;
        bool m_Check;
        int m_Bench;
    };

}
