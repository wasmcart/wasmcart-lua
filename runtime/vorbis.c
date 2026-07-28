/* stb_vorbis in its own translation unit.
 *
 * WHY THIS FILE EXISTS: stb_vorbis.c does `#define L (PLAYBACK_LEFT|...)`.
 * Lua's headers declare every API function as `(lua_State *L, ...)`, so
 * including both in one TU turns every Lua prototype into a syntax error.
 * Keep them apart; runtime.c just declares the one decode entry point.
 */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
