// The one translation unit that compiles miniaudio.
//
// It exists rather than using the vendored miniaudio.c because of one line: stb_vorbis has to be
// compiled *before* the miniaudio implementation, and miniaudio keys its Vorbis support off whether
// stb_vorbis.h's include guard is defined by the time it is expanded. Without this file .ogg is
// simply not a format the engine can read, and the failure is a runtime "unknown format" rather
// than anything a build would catch.
//
// miniaudio ships its own copy of stb_vorbis under extras/ — used rather than VAE/vendor/stb's,
// because miniaudio patches it (MA_NO_VORBIS, the pushdata API it needs) and the two are not
// interchangeable.

// Nothing here writes audio, and the WAV/FLAC encoders are a third of the library.
#define MA_NO_ENCODING

// stb_vorbis leaves a pile of unused statics behind; they are not ours to fix.
#if defined(__GNUC__)
    #pragma GCC diagnostic ignored "-Wunused-function"
    #pragma GCC diagnostic ignored "-Wunused-value"
    #pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif

#define STB_VORBIS_HEADER_ONLY
#include "../VAE/vendor/miniaudio/extras/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

#define MINIAUDIO_IMPLEMENTATION
#include "../VAE/vendor/miniaudio/miniaudio.h"

// And now the implementation half, after miniaudio has seen the header.
#include "../VAE/vendor/miniaudio/extras/stb_vorbis.c"
