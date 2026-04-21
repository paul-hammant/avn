#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <setjmp.h>
#include "aether_panic.h"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>
#else
#include <unistd.h>
#include <sched.h>
#endif
#ifdef _WIN32
#  define aether_aligned_alloc(align, size) _aligned_malloc((size), (align))
#else
#  define aether_aligned_alloc(align, size) aligned_alloc((align), (size))
#endif
#ifndef likely
#  if defined(__GNUC__) || defined(__clang__)
#    define likely(x)   __builtin_expect(!!(x), 1)
#    define unlikely(x) __builtin_expect(!!(x), 0)
#  else
#    define likely(x)   (x)
#    define unlikely(x) (x)
#  endif
#endif
#ifndef AETHER_GCC_COMPAT
#  if (defined(__GNUC__) || defined(__clang__)) && !defined(__EMSCRIPTEN__)
#    define AETHER_GCC_COMPAT 1
#  else
#    define AETHER_GCC_COMPAT 0
#  endif
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#ifdef _WIN32
static inline int64_t _aether_clock_ns(void) {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (int64_t)((double)now.QuadPart / freq.QuadPart * 1000000000.0);
}
#elif defined(__EMSCRIPTEN__)
static inline int64_t _aether_clock_ns(void) {
    return (int64_t)(emscripten_get_now() * 1000000.0);
}
#elif defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 0)
static inline int64_t _aether_clock_ns(void) { return 0; }
#else
static inline int64_t _aether_clock_ns(void) {
    struct timespec _ts;
    clock_gettime(CLOCK_MONOTONIC, &_ts);
    return (int64_t)_ts.tv_sec * 1000000000LL + _ts.tv_nsec;
}
#endif
#include <stdarg.h>
static void* _aether_interp(const char* fmt, ...) {
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    char* str = (char*)malloc(len + 1);
    vsnprintf(str, len + 1, fmt, args2);
    va_end(args2);
    return (void*)str;
}
static inline const char* _aether_safe_str(const void* s) {
    return s ? (const char*)s : "(null)";
}
#if !AETHER_GCC_COMPAT
static void* _aether_ref_new(intptr_t val) { intptr_t* r = malloc(sizeof(intptr_t)); *r = val; return (void*)r; }
#endif
typedef struct { void (*fn)(void); void* env; } _AeClosure;
static inline void* _aether_box_closure(_AeClosure c) { _AeClosure* p = malloc(sizeof(_AeClosure)); *p = c; return (void*)p; }
static inline _AeClosure _aether_unbox_closure(void* p) { return *(_AeClosure*)p; }
typedef struct { _AeClosure compute; intptr_t value; int evaluated; } _AeThunk;
static inline void* _aether_thunk_new(_AeClosure c) { _AeThunk* t = malloc(sizeof(_AeThunk)); t->compute = c; t->value = 0; t->evaluated = 0; return (void*)t; }
static inline intptr_t _aether_thunk_force(void* p) { _AeThunk* t = (_AeThunk*)p; if (!t->evaluated) { t->value = (intptr_t)((intptr_t(*)(void*))t->compute.fn)(t->compute.env); t->evaluated = 1; } return t->value; }
static inline void _aether_thunk_free(void* p) { if (p) free(p); }
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 1) && !defined(__arm__) && !defined(__thumb__)
#include <termios.h>
static struct termios _aether_orig_termios;
static void _aether_raw_mode(void) {
    tcgetattr(0, &_aether_orig_termios);
    struct termios raw = _aether_orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &raw);
}
static void _aether_cooked_mode(void) {
    tcsetattr(0, TCSANOW, &_aether_orig_termios);
}
#else
static void _aether_raw_mode(void) {}
static void _aether_cooked_mode(void) {}
#endif
static void* _aether_ctx_stack[64];
static int _aether_ctx_depth = 0;
static inline void _aether_ctx_push(void* ctx) { if (_aether_ctx_depth < 64) _aether_ctx_stack[_aether_ctx_depth++] = ctx; }
static inline void _aether_ctx_pop(void) { if (_aether_ctx_depth > 0) _aether_ctx_depth--; }
static inline void* _aether_ctx_get(void) { return _aether_ctx_depth > 0 ? _aether_ctx_stack[_aether_ctx_depth-1] : (void*)0; }

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

void aether_args_init(int argc, char** argv);


typedef struct { int _0; const char* _1; } _tuple_int_string;
typedef struct { float _0; const char* _1; } _tuple_float_string;

// Forward declarations
static _tuple_int_string string_to_int(const char*);
static _tuple_int_string string_to_long(const char*);
static _tuple_float_string string_to_float(const char*);
static _tuple_float_string string_to_double(const char*);

// Import: std.string
// Extern C function: string_new
void* string_new(const char*);

// Extern C function: string_from_cstr
void* string_from_cstr(const char*);

// Extern C function: string_from_literal
void* string_from_literal(const char*);

// Extern C function: string_new_with_length
void* string_new_with_length(const char*, int);

// Extern C function: string_empty
void* string_empty();

// Extern C function: string_retain
void string_retain(const char*);

// Extern C function: string_release
void string_release(const char*);

// Extern C function: string_free
void string_free(const char*);

// Extern C function: string_concat
const char* string_concat(const char*, const char*);

// Extern C function: string_length
int string_length(const char*);

// Extern C function: string_char_at
int string_char_at(const char*, int);

// Extern C function: string_equals
int string_equals(const char*, const char*);

// Extern C function: string_compare
int string_compare(const char*, const char*);

// Extern C function: string_starts_with
int string_starts_with(const char*, const char*);

// Extern C function: string_ends_with
int string_ends_with(const char*, const char*);

// Extern C function: string_contains
int string_contains(const char*, const char*);

// Extern C function: string_index_of
int string_index_of(const char*, const char*);

// Extern C function: string_substring
const char* string_substring(const char*, int, int);

// Extern C function: string_to_upper
const char* string_to_upper(const char*);

// Extern C function: string_to_lower
const char* string_to_lower(const char*);

// Extern C function: string_trim
const char* string_trim(const char*);

// Extern C function: string_split
void* string_split(const char*, const char*);

// Extern C function: string_array_size
int string_array_size(void*);

// Extern C function: string_array_get
void* string_array_get(void*, int);

// Extern C function: string_array_free
void string_array_free(void*);

// Extern C function: string_to_cstr
const char* string_to_cstr(const char*);

// Extern C function: string_from_int
void* string_from_int(int);

// Extern C function: string_from_float
void* string_from_float(double);

// Extern C function: string_to_int_raw
int string_to_int_raw(const char*, void*);

// Extern C function: string_to_long_raw
int string_to_long_raw(const char*, void*);

// Extern C function: string_to_float_raw
int string_to_float_raw(const char*, void*);

// Extern C function: string_to_double_raw
int string_to_double_raw(const char*, void*);

// Extern C function: string_try_int
int string_try_int(const char*);

// Extern C function: string_get_int
int string_get_int(const char*);

// Extern C function: string_try_long
int string_try_long(const char*);

// Extern C function: string_get_long
int64_t string_get_long(const char*);

// Extern C function: string_try_float
int string_try_float(const char*);

// Extern C function: string_get_float
double string_get_float(const char*);

// Extern C function: string_try_double
int string_try_double(const char*);

// Extern C function: string_get_double
double string_get_double(const char*);


// Exported:
const char* paths_index_lookup(const char* body, const char* path) {
int n = string_length(body);
int plen = string_length(path);
if (n == 0) {
        {
            return "";
        }
    }
if (plen == 0) {
        {
            return "";
        }
    }
int line_start = 0;
int i = 0;
    int at_end;
    int c;
    int is_eol;
    int sp;
    int sha_len;
    int name_off;
    int name_len;
    int is_match;
    int j;
while (i <= n) {
        {
at_end = 0;
c = 0;
if (i == n) {
                {
at_end = 1;
                }
            } else {
                {
c = string_char_at(body, i);
                }
            }
is_eol = 0;
if (at_end == 1) {
                {
is_eol = 1;
                }
            }
if (c == 10) {
                {
is_eol = 1;
                }
            }
if (is_eol == 1) {
                {
sp = line_start;
while (sp < i) {
                        {
if (string_char_at(body, sp) == 32) {
                                {
                                    break;
                                }
                            }
sp = (sp + 1);
                        }
                    }
if ((sp > line_start) && (sp < i)) {
                        {
sha_len = (sp - line_start);
name_off = (sp + 1);
name_len = (i - name_off);
if (name_len == plen) {
                                {
is_match = 1;
j = 0;
while (j < plen) {
                                        {
if (string_char_at(body, (name_off + j)) != string_char_at(path, j)) {
                                                {
is_match = 0;
                                                    break;
                                                }
                                            }
j = (j + 1);
                                        }
                                    }
if (is_match == 1) {
                                        {
                                            return string_substring(body, line_start, (line_start + sha_len));
                                        }
                                    }
                                }
                            }
                        }
                    }
line_start = (i + 1);
                }
            }
i = (i + 1);
        }
    }
    return "";
}

static _tuple_int_string string_to_int(const char* s) {
int ok = string_try_int(s);
if (ok == 0) {
        {
            return (_tuple_int_string){0, "invalid integer"};
        }
    }
    return (_tuple_int_string){string_get_int(s), ""};
}

static _tuple_int_string string_to_long(const char* s) {
int ok = string_try_long(s);
if (ok == 0) {
        {
            return (_tuple_int_string){0, "invalid long"};
        }
    }
    return (_tuple_int_string){string_get_long(s), ""};
}

static _tuple_float_string string_to_float(const char* s) {
int ok = string_try_float(s);
if (ok == 0) {
        {
            return (_tuple_float_string){0.0, "invalid float"};
        }
    }
    return (_tuple_float_string){string_get_float(s), ""};
}

static _tuple_float_string string_to_double(const char* s) {
int ok = string_try_double(s);
if (ok == 0) {
        {
            return (_tuple_float_string){0.0, "invalid double"};
        }
    }
    return (_tuple_float_string){string_get_double(s), ""};
}


/* --- aether_<name> alias stubs (--emit=lib) --- */
#include <stdint.h>
typedef struct AetherValue AetherValue;  /* opaque */

const char* aether_paths_index_lookup(const char* body, const char* path) {
    return paths_index_lookup(body, path);
}
