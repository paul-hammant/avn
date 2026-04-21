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
int after_first_space(const char*);
const char* rb_int_to_dec(int);
const char* rb_digit_char(int);
const char* pilins_insert_sorted(const char*, const char*, const char*);
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


int after_first_space(const char* line) {
int n = string_length(line);
int i = 0;
while (i < n) {
        {
if (string_char_at(line, i) == 32) {
                {
                    return (i + 1);
                }
            }
i = (i + 1);
        }
    }
    return -1;
}

// Exported:
const char* format_primary_hash(const char* line) {
int start = after_first_space(line);
if (start < 0) {
        {
            return "";
        }
    }
int n = string_length(line);
int i = start;
while (i < n) {
        {
if (string_char_at(line, i) == 44) {
                {
                    break;
                }
            }
i = (i + 1);
        }
    }
    return string_substring(line, start, i);
}

// Exported:
int format_secondary_count(const char* line) {
int start = after_first_space(line);
if (start < 0) {
        {
            return 0;
        }
    }
int n = string_length(line);
int i = start;
while (i < n) {
        {
if (string_char_at(line, i) == 44) {
                {
                    break;
                }
            }
i = (i + 1);
        }
    }
if (i >= n) {
        {
            return 0;
        }
    }
int count = 0;
i = (i + 1);
int tok_start = i;
    int at_end;
    int c;
    int is_sep;
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
c = string_char_at(line, i);
                }
            }
is_sep = 0;
if (at_end == 1) {
                {
is_sep = 1;
                }
            }
if (c == 44) {
                {
is_sep = 1;
                }
            }
if (is_sep == 1) {
                {
if (i > tok_start) {
                        {
count = (count + 1);
                        }
                    }
tok_start = (i + 1);
                }
            }
i = (i + 1);
        }
    }
    return count;
}

// Exported:
const char* format_secondary_hash(const char* line, int target) {
int start = after_first_space(line);
if (start < 0) {
        {
            return "";
        }
    }
int n = string_length(line);
int i = start;
while (i < n) {
        {
if (string_char_at(line, i) == 44) {
                {
                    break;
                }
            }
i = (i + 1);
        }
    }
if (i >= n) {
        {
            return "";
        }
    }
int idx = 0;
i = (i + 1);
int tok_start = i;
    int at_end;
    int c;
    int is_sep;
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
c = string_char_at(line, i);
                }
            }
is_sep = 0;
if (at_end == 1) {
                {
is_sep = 1;
                }
            }
if (c == 44) {
                {
is_sep = 1;
                }
            }
if (is_sep == 1) {
                {
if (i > tok_start) {
                        {
if (idx == target) {
                                {
                                    return string_substring(line, tok_start, i);
                                }
                            }
idx = (idx + 1);
                        }
                    }
tok_start = (i + 1);
                }
            }
i = (i + 1);
        }
    }
    return "";
}

// Exported:
const char* rev_blob_body(const char* root, const char* branch, const char* props, const char* acl, int prev, const char* author, const char* date, const char* log) {
const char* out = "root: ";
    int _heap_out = 0; (void)_heap_out;
out = string_concat(out, root);
out = string_concat(out, "\nbranch: ");
out = string_concat(out, branch);
out = string_concat(out, "\nprops: ");
out = string_concat(out, props);
out = string_concat(out, "\nacl: ");
out = string_concat(out, acl);
out = string_concat(out, "\nprev: ");
out = string_concat(out, rb_int_to_dec(prev));
out = string_concat(out, "\nauthor: ");
out = string_concat(out, author);
out = string_concat(out, "\ndate: ");
out = string_concat(out, date);
out = string_concat(out, "\nlog: ");
out = string_concat(out, log);
out = string_concat(out, "\n");
    return out;
}

const char* rb_int_to_dec(int v) {
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
out = string_concat(rb_digit_char(d), out);
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

const char* rb_digit_char(int d) {
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

// Exported:
const char* paths_index_sort_by_path(const char* body) {
int n = string_length(body);
if (n == 0) {
        {
            return "";
        }
    }
const char* entries = "";
    int _heap_entries = 0; (void)_heap_entries;
int line_start = 0;
int i = 0;
    int at_end;
    int c;
    int is_eol;
    int sp;
    const char* path;
    const char* line;
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
if (i > line_start) {
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
path = string_substring(body, (sp + 1), i);
line = string_substring(body, line_start, i);
entries = pilins_insert_sorted(entries, path, line);
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
const char* out = "";
    int _heap_out = 0; (void)_heap_out;
int m = string_length(entries);
int start = 0;
int j = 0;
    int cc;
    int k;
while (j < m) {
        {
cc = string_char_at(entries, j);
if (cc == 9) {
                {
k = (j + 1);
while (k < m) {
                        {
if (string_char_at(entries, k) == 10) {
                                {
                                    break;
                                }
                            }
k = (k + 1);
                        }
                    }
out = string_concat(out, string_substring(entries, (j + 1), k));
out = string_concat(out, "\n");
j = (k + 1);
start = j;
                }
            } else {
                {
j = (j + 1);
                }
            }
        }
    }
    return out;
}

const char* pilins_insert_sorted(const char* entries, const char* path, const char* line) {
int n = string_length(entries);
int plen = string_length(path);
if (n == 0) {
        {
const char* out = string_concat(path, "\t");
            int _heap_out = 0; (void)_heap_out;
out = string_concat(out, line);
            return string_concat(out, "\n");
        }
    }
const char* out = "";
    int _heap_out = 0; (void)_heap_out;
int inserted = 0;
int line_start = 0;
int i = 0;
    int tab;
    int eol;
    const char* this_path;
    const char* one;
while (i < n) {
        {
tab = line_start;
while (tab < n) {
                {
if (string_char_at(entries, tab) == 9) {
                        {
                            break;
                        }
                    }
tab = (tab + 1);
                }
            }
eol = tab;
while (eol < n) {
                {
if (string_char_at(entries, eol) == 10) {
                        {
                            break;
                        }
                    }
eol = (eol + 1);
                }
            }
this_path = string_substring(entries, line_start, tab);
if (inserted == 0) {
                {
if (string_compare(path, this_path) < 0) {
                        {
one = string_concat(path, "\t");
one = string_concat(one, line);
one = string_concat(one, "\n");
out = string_concat(out, one);
inserted = 1;
                        }
                    }
                }
            }
out = string_concat(out, string_substring(entries, line_start, (eol + 1)));
line_start = (eol + 1);
i = line_start;
        }
    }
if (inserted == 0) {
        {
one = string_concat(path, "\t");
one = string_concat(one, line);
one = string_concat(one, "\n");
out = string_concat(out, one);
        }
    }
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

int32_t aether_after_first_space(const char* line) {
    return after_first_space(line);
}
const char* aether_format_primary_hash(const char* line) {
    return format_primary_hash(line);
}
int32_t aether_format_secondary_count(const char* line) {
    return format_secondary_count(line);
}
const char* aether_format_secondary_hash(const char* line, int32_t target) {
    return format_secondary_hash(line, target);
}
const char* aether_rev_blob_body(const char* root, const char* branch, const char* props, const char* acl, int32_t prev, const char* author, const char* date, const char* log) {
    return rev_blob_body(root, branch, props, acl, prev, author, date, log);
}
const char* aether_rb_int_to_dec(int32_t v) {
    return rb_int_to_dec(v);
}
const char* aether_rb_digit_char(int32_t d) {
    return rb_digit_char(d);
}
const char* aether_paths_index_sort_by_path(const char* body) {
    return paths_index_sort_by_path(body);
}
const char* aether_pilins_insert_sorted(const char* entries, const char* path, const char* line) {
    return pilins_insert_sorted(entries, path, line);
}
