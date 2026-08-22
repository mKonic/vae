// lua.hpp — the C++ wrapper the official Lua distribution ships and the git mirror does not.
//
// It lives here rather than inside the submodule so the submodule stays clean, and it is first on
// the include path so sol2 finds *this* Lua rather than whatever version the system has installed.
// Without it sol2 picks up /usr/include/lua.hpp — a different release — and the build fails a long
// way from the cause.
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}
