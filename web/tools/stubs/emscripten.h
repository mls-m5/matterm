// Stand-in for <emscripten.h>, only so that web/main.cpp can be type-checked by
// the native compiler (web/tools/syntax-check.sh). The JavaScript in EM_ASM is
// dropped; nothing here runs.
#pragma once
#define EMSCRIPTEN_KEEPALIVE
#define EM_ASM(...) ((void)0)
#define EM_ASM_INT(...) (0)
#define EM_ASM_DOUBLE(...) (0.0)
#define EM_ASM_PTR(...) (static_cast<void *>(nullptr))
inline void emscripten_set_main_loop_arg(void (*)(void *), void *, int, int) {}
