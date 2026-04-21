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
const char* json_escape_string_impl(const char*);
const char* json_int_to_dec_impl(int);
const char* escape_byte(int);
const char* digit_char(int);
const char* hex_char(int);
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


const char* json_escape_string_impl(const char* v) {
const char* out = "\"";
    int _heap_out = 0; (void)_heap_out;
int n = string_length(v);
int run_start = 0;
int i = 0;
    int c;
    int needs_escape;
while (i < n) {
        {
c = string_char_at(v, i);
needs_escape = 0;
if (c == 34) {
                {
needs_escape = 1;
                }
            }
if (c == 92) {
                {
needs_escape = 1;
                }
            }
if (c < 32) {
                {
needs_escape = 1;
                }
            }
if (needs_escape == 1) {
                {
if (i > run_start) {
                        {
out = string_concat(out, string_substring(v, run_start, i));
                        }
                    }
out = string_concat(out, escape_byte(c));
run_start = (i + 1);
                }
            }
i = (i + 1);
        }
    }
if (run_start < n) {
        {
out = string_concat(out, string_substring(v, run_start, n));
        }
    }
out = string_concat(out, "\"");
    return out;
}

// Exported:
const char* json_escape_string(const char* v) {
    return json_escape_string_impl(v);
}

// Exported:
const char* json_int_to_dec(int v) {
    return json_int_to_dec_impl(v);
}

const char* json_int_to_dec_impl(int v) {
if (v == 0) {
        {
            return "0";
        }
    }
int neg = 0;
int n = v;
if (v < 0) {
        {
neg = 1;
n = (0 - v);
        }
    }
const char* out = "";
    int _heap_out = 0; (void)_heap_out;
    int d;
while (n > 0) {
        {
d = (n - ((n / 10) * 10));
out = string_concat(digit_char(d), out);
n = (n / 10);
        }
    }
if (neg == 1) {
        {
out = string_concat("-", out);
        }
    }
    return out;
}

const char* escape_byte(int c) {
if (c == 34) {
        {
            return "\\\"";
        }
    }
if (c == 92) {
        {
            return "\\\\";
        }
    }
if (c == 10) {
        {
            return "\\n";
        }
    }
if (c == 13) {
        {
            return "\\r";
        }
    }
if (c == 9) {
        {
            return "\\t";
        }
    }
if (c == 8) {
        {
            return "\\b";
        }
    }
if (c == 12) {
        {
            return "\\f";
        }
    }
int hi = (c / 16);
int lo = (c - (hi * 16));
const char* out = "\\u00";
    int _heap_out = 0; (void)_heap_out;
out = string_concat(out, hex_char(hi));
out = string_concat(out, hex_char(lo));
    return out;
}

const char* digit_char(int d) {
if (d == 0) {
        {
            return "0";
        }
    }
if (d == 1) {
        {
            return "1";
        }
    }
if (d == 2) {
        {
            return "2";
        }
    }
if (d == 3) {
        {
            return "3";
        }
    }
if (d == 4) {
        {
            return "4";
        }
    }
if (d == 5) {
        {
            return "5";
        }
    }
if (d == 6) {
        {
            return "6";
        }
    }
if (d == 7) {
        {
            return "7";
        }
    }
if (d == 8) {
        {
            return "8";
        }
    }
if (d == 9) {
        {
            return "9";
        }
    }
    return "?";
}

const char* hex_char(int d) {
if (d < 10) {
        {
            return digit_char(d);
        }
    }
if (d == 10) {
        {
            return "a";
        }
    }
if (d == 11) {
        {
            return "b";
        }
    }
if (d == 12) {
        {
            return "c";
        }
    }
if (d == 13) {
        {
            return "d";
        }
    }
if (d == 14) {
        {
            return "e";
        }
    }
if (d == 15) {
        {
            return "f";
        }
    }
    return "?";
}

// Exported:
const char* props_blob_to_json(const char* body) {
const char* out = "{";
    int _heap_out = 0; (void)_heap_out;
int n = string_length(body);
int first = 1;
int line_start = 0;
int i = 0;
    int at_end;
    int c;
    int is_eol;
    int eq;
    int found_eq;
    const char* key;
    const char* val;
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
eq = line_start;
found_eq = 0;
while (eq < i) {
                        {
if (string_char_at(body, eq) == 61) {
                                {
found_eq = 1;
                                    break;
                                }
                            }
eq = (eq + 1);
                        }
                    }
if (found_eq == 1) {
                        {
if (first == 0) {
                                {
out = string_concat(out, ",");
                                }
                            }
first = 0;
key = string_substring(body, line_start, eq);
val = string_substring(body, (eq + 1), i);
if (string_length(key) >= 256) {
                                {
out = string_concat(out, "\"\"");
                                }
                            } else {
                                {
out = string_concat(out, json_escape_string_impl(key));
                                }
                            }
out = string_concat(out, ":");
out = string_concat(out, json_escape_string_impl(val));
                        }
                    }
line_start = (i + 1);
                }
            }
i = (i + 1);
        }
    }
out = string_concat(out, "}");
    return out;
}

// Exported:
const char* specs_to_json_array(const char* body) {
const char* out = "[";
    int _heap_out = 0; (void)_heap_out;
int n = string_length(body);
int any = 0;
int line_start = 0;
int i = 0;
    int at_end;
    int c;
    int is_eol;
    int end;
    int t;
    const char* glob;
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
end = i;
while (end > line_start) {
                        {
t = string_char_at(body, (end - 1));
if (((t != 13) && (t != 32)) && (t != 9)) {
                                {
                                    break;
                                }
                            }
end = (end - 1);
                        }
                    }
if (end > line_start) {
                        {
if (any == 1) {
                                {
out = string_concat(out, ",");
                                }
                            }
any = 1;
glob = string_substring(body, line_start, end);
out = string_concat(out, json_escape_string_impl(glob));
                        }
                    }
line_start = (i + 1);
                }
            }
i = (i + 1);
        }
    }
out = string_concat(out, "]");
    return out;
}

// Exported:
const char* rev_info_json(int rev, const char* author, const char* date, const char* msg, const char* root) {
const char* out = "{\"rev\":";
    int _heap_out = 0; (void)_heap_out;
out = string_concat(out, json_int_to_dec_impl(rev));
out = string_concat(out, ",\"author\":");
out = string_concat(out, json_escape_string_impl(author));
out = string_concat(out, ",\"date\":");
out = string_concat(out, json_escape_string_impl(date));
out = string_concat(out, ",\"msg\":");
out = string_concat(out, json_escape_string_impl(msg));
out = string_concat(out, ",\"root\":");
out = string_concat(out, json_escape_string_impl(root));
out = string_concat(out, "}");
    return out;
}

// Exported:
const char* log_entry_json(int rev, const char* author, const char* date, const char* msg) {
const char* out = "{\"rev\":";
    int _heap_out = 0; (void)_heap_out;
out = string_concat(out, json_int_to_dec_impl(rev));
out = string_concat(out, ",\"author\":");
out = string_concat(out, json_escape_string_impl(author));
out = string_concat(out, ",\"date\":");
out = string_concat(out, json_escape_string_impl(date));
out = string_concat(out, ",\"msg\":");
out = string_concat(out, json_escape_string_impl(msg));
out = string_concat(out, "}");
    return out;
}

// Exported:
const char* path_change_entry_json(const char* action, const char* path) {
const char* out = "{\"action\":";
    int _heap_out = 0; (void)_heap_out;
out = string_concat(out, json_escape_string_impl(action));
out = string_concat(out, ",\"path\":");
out = string_concat(out, json_escape_string_impl(path));
out = string_concat(out, "}");
    return out;
}

// Exported:
const char* hashes_prelude_json(const char* algo, const char* primary_hash) {
const char* out = "{\"primary\":{\"algo\":";
    int _heap_out = 0; (void)_heap_out;
out = string_concat(out, json_escape_string_impl(algo));
out = string_concat(out, ",\"hash\":");
out = string_concat(out, json_escape_string_impl(primary_hash));
out = string_concat(out, "},\"secondaries\":[");
    return out;
}

// Exported:
const char* acl_response_json(const char* rules_body, const char* effective_from) {
const char* out = "{\"rules\":[";
    int _heap_out = 0; (void)_heap_out;
int n = string_length(rules_body);
int first = 1;
int line_start = 0;
int i = 0;
    int at_end;
    int c;
    int is_eol;
    const char* rule;
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
c = string_char_at(rules_body, i);
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
if (i > line_start) {
                        {
rule = string_substring(rules_body, line_start, i);
if (first == 0) {
                                {
out = string_concat(out, ",");
                                }
                            }
first = 0;
out = string_concat(out, json_escape_string_impl(rule));
                        }
                    }
line_start = (i + 1);
                }
            }
i = (i + 1);
        }
    }
out = string_concat(out, "],\"effective_from\":");
out = string_concat(out, json_escape_string_impl(effective_from));
out = string_concat(out, "}");
    return out;
}

// Exported:
const char* secondary_entry_json(const char* algo, const char* hash) {
const char* out = "{\"algo\":";
    int _heap_out = 0; (void)_heap_out;
out = string_concat(out, json_escape_string_impl(algo));
out = string_concat(out, ",\"hash\":");
out = string_concat(out, json_escape_string_impl(hash));
out = string_concat(out, "}");
    return out;
}

// Exported:
const char* info_prelude_json(int head, const char* name, const char* hash_algo) {
const char* out = "{\"head\":";
    int _heap_out = 0; (void)_heap_out;
out = string_concat(out, json_int_to_dec_impl(head));
out = string_concat(out, ",\"name\":");
out = string_concat(out, json_escape_string_impl(name));
out = string_concat(out, ",\"hash_algo\":");
out = string_concat(out, json_escape_string_impl(hash_algo));
out = string_concat(out, ",\"default_branch\":\"main\"");
    return out;
}

// Exported:
const char* blame_entry_json(int rev, const char* author, const char* text) {
const char* out = "{\"rev\":";
    int _heap_out = 0; (void)_heap_out;
out = string_concat(out, json_int_to_dec_impl(rev));
out = string_concat(out, ",\"author\":");
out = string_concat(out, json_escape_string_impl(author));
out = string_concat(out, ",\"text\":");
out = string_concat(out, json_escape_string_impl(text));
out = string_concat(out, "}");
    return out;
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

const char* aether_json_escape_string_impl(const char* v) {
    return json_escape_string_impl(v);
}
const char* aether_json_escape_string(const char* v) {
    return json_escape_string(v);
}
const char* aether_json_int_to_dec(int32_t v) {
    return json_int_to_dec(v);
}
const char* aether_json_int_to_dec_impl(int32_t v) {
    return json_int_to_dec_impl(v);
}
const char* aether_escape_byte(int32_t c) {
    return escape_byte(c);
}
const char* aether_digit_char(int32_t d) {
    return digit_char(d);
}
const char* aether_hex_char(int32_t d) {
    return hex_char(d);
}
const char* aether_props_blob_to_json(const char* body) {
    return props_blob_to_json(body);
}
const char* aether_specs_to_json_array(const char* body) {
    return specs_to_json_array(body);
}
const char* aether_rev_info_json(int32_t rev, const char* author, const char* date, const char* msg, const char* root) {
    return rev_info_json(rev, author, date, msg, root);
}
const char* aether_log_entry_json(int32_t rev, const char* author, const char* date, const char* msg) {
    return log_entry_json(rev, author, date, msg);
}
const char* aether_path_change_entry_json(const char* action, const char* path) {
    return path_change_entry_json(action, path);
}
const char* aether_hashes_prelude_json(const char* algo, const char* primary_hash) {
    return hashes_prelude_json(algo, primary_hash);
}
const char* aether_acl_response_json(const char* rules_body, const char* effective_from) {
    return acl_response_json(rules_body, effective_from);
}
const char* aether_secondary_entry_json(const char* algo, const char* hash) {
    return secondary_entry_json(algo, hash);
}
const char* aether_info_prelude_json(int32_t head, const char* name, const char* hash_algo) {
    return info_prelude_json(head, name, hash_algo);
}
const char* aether_blame_entry_json(int32_t rev, const char* author, const char* text) {
    return blame_entry_json(rev, author, text);
}
