/*
** MediaViewer addition (not part of Lua). The add-on runtime's watchdog
** (src/addon/script_runtime.cpp) stops a script by raising a flag; the VM sees
** it through the count hook, but a few library loops run no VM instruction for
** as long as the script's arguments say (pattern backtracking, table.move over
** 2^63 indices, a sort without a comparator). They call this. Defined by the
** runtime; the state's extra space holds its pointer.
*/
#ifndef mv_interrupt_h
#define mv_interrupt_h

#include "lua.h"

int mv_lua_interrupted (lua_State *L);

#define mv_lua_check_interrupt(L) \
  do { if (l_unlikely(mv_lua_interrupted(L))) luaL_error(L, "stopped"); } while (0)

#endif
