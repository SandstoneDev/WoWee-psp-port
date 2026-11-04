/* Lua chunk writer with a 32-bit size_t (fx_luadump32.c). */
#pragma once

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Dumps the Lua function on the top of the stack as a chunk the PSP's
** lundump accepts. Returns 0 on success (the writer's status otherwise). */
int fxDump32(lua_State* L, int strip, lua_Writer w, void* data);

#ifdef __cplusplus
}
#endif
