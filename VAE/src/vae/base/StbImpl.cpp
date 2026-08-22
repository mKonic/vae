// The single translation unit that instantiates the stb single-header libraries.
// Kept in VAE-Core: font rasterization and image decoding are CPU work that the headless test
// binary needs, and neither has anything to do with the GPU.

#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cassert>

// stb_rect_pack MUST come first: stb_truetype declares its own stand-in stbrp_* types when
// STB_RECT_PACK_VERSION is not already defined, and those then conflict with the real ones.
#define STB_RECT_PACK_IMPLEMENTATION
#include <stb_rect_pack.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

// STBI_NO_SIMD: stb_image's SSE2 resampler passes a runtime value to _mm_slli_si128, which gcc 16
// rejects in a C++ TU ("the last argument must be an 8-bit immediate"). Image decoding is not on
// any hot path here — it happens once per asset import.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#include <stb_image.h>
