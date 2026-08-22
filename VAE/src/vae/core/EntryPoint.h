#pragma once

#include "vae/base/Log.h"
#include "vae/core/Application.h"

// Included by exactly one TU in each executable.
int main(int argc, char** argv) {
    vae::Log::Init();

    auto* app = vae::CreateApplication({ argc, argv });
    app->Run();
    const int code = app->ExitCode();
    delete app;

    vae::Log::Shutdown();
    return code;
}
