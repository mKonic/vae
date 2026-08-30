#include "Harness.h"

namespace vae::selftest {

    namespace {
        int g_Checks = 0;
        int g_Failed = 0;
        const char* g_Section = "";
    }

    void Section(const char* name) {
        g_Section = name;
        VAE_INFO("selftest · {}", name);
    }

    bool Check(bool ok, std::string_view what) {
        ++g_Checks;
        if (!ok) {
            ++g_Failed;
            VAE_ERROR("  FAIL  {}: {}", g_Section, what);
        }
        return ok;
    }

    int  Checks() { return g_Checks; }
    int  Failed() { return g_Failed; }
    void Reset()  { g_Checks = 0; g_Failed = 0; g_Section = ""; }

}
