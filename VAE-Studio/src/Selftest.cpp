#include "Selftest.h"

#include "selftest/Harness.h"

#include "vae/base/Log.h"
#include "vae/core/Application.h"

namespace vae {

    // `--selftest` runs the editor against itself with no window and no GPU, and exits with the
    // count. The checks themselves live in `selftest/`, four files by the area they exercise —
    // this is only the order they run in and what happens at the end.
    void SelftestLayer::OnAttach() {
        selftest::Reset();

        selftest::RunDirect();
        selftest::RunAuthoring();
        selftest::RunProjects();
        selftest::RunRunning();

        const int checks = selftest::Checks();
        const int failed = selftest::Failed();
        if (failed == 0) VAE_INFO("selftest: {} checks passed", checks);
        else             VAE_ERROR("selftest: {} of {} checks FAILED", failed, checks);

        Application::Get().SetExitCode(failed == 0 ? 0 : 1);
        Application::Get().Close();
    }

}
