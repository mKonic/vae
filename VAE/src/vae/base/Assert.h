#pragma once

#include "vae/base/Log.h"

#include <cstdlib>

// VAE_ASSERT is compiled out in Dist. VAE_VERIFY always evaluates its condition and only
// the diagnostic is stripped — use it when the check has a side effect worth keeping.
#ifdef VAE_DIST
    #define VAE_ASSERT(cond, ...) ((void)0)
    #define VAE_CORE_ASSERT(cond, ...) ((void)0)
    #define VAE_VERIFY(cond, ...) ((void)(cond))
#else
    #define VAE_INTERNAL_FAIL(logger, cond, ...) do {                                  \
            logger("Assertion failed: {} @ {}:{}", #cond, __FILE__, __LINE__);          \
            logger(__VA_ARGS__);                                                        \
            std::abort();                                                               \
        } while (0)

    #define VAE_ASSERT(cond, ...)      do { if (!(cond)) VAE_INTERNAL_FAIL(VAE_ERROR, cond, __VA_ARGS__); } while (0)
    #define VAE_CORE_ASSERT(cond, ...) do { if (!(cond)) VAE_INTERNAL_FAIL(VAE_CORE_ERROR, cond, __VA_ARGS__); } while (0)
    #define VAE_VERIFY(cond, ...)      VAE_CORE_ASSERT(cond, __VA_ARGS__)
#endif
