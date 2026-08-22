#include "Test.h"

#include "vae/base/Log.h"

#include <cstring>

int main(int argc, char** argv) {
    vae::Log::Init();
    vae::Log::CoreLogger()->set_level(spdlog::level::warn);   // tests are noisy enough

    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--filter=", 9) == 0) filter = argv[i] + 9;

    std::printf("VAE-Tests\n");
    const int rc = test::RunAll(filter);

    vae::Log::Shutdown();
    return rc;
}
