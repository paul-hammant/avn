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
const char* rs_add(const char*, int);
const char* rs_remove(const char*, int);
int rs_contains(const char*, int);
const char* int_to_dec(int);
int parse_int(const char*);
const char* append_csv_str(const char*, const char*);
const char* append_csv_int(const char*, int);
const char* parse_ranges(const char*, const char*, int);
const char* cancel_pairs(const char*, const char*);
const char* emit_ranges(const char*, int);
const char* emit_one(const char*, int, int, int);
int find_colon(const char*, int, int);
const char* extract_source_sets(const char*, const char*);
const char* split_left(const char*);
const char* split_right(const char*);
const char* build_source_line(const char*, const char*, const char*);
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


const char* rs_add(const char* rs, int rev) {
int n = string_length(rs);
if (n == 0) {
        {
            return int_to_dec(rev);
        }
    }
const char* out = "";
    int _heap_out = 0; (void)_heap_out;
int i = 0;
int inserted = 0;
int start = 0;
    int c;
    int at_end;
    const char* tok;
    int v;
while (i <= n) {
        {
c = 0;
at_end = 0;
if (i == n) {
                {
at_end = 1;
                }
            } else {
                {
c = string_char_at(rs, i);
                }
            }
if (at_end == 1) {
                {
if (i > start) {
                        {
tok = string_substring(rs, start, i);
v = parse_int(tok);
if ((inserted == 0) && (rev < v)) {
                                {
out = append_csv_int(out, rev);
inserted = 1;
out = append_csv_str(out, tok);
                                }
                            } else {
if ((inserted == 0) && (rev == v)) {
                                    {
out = append_csv_str(out, tok);
inserted = 1;
                                    }
                                } else {
                                    {
out = append_csv_str(out, tok);
                                    }
                                }
                            }
                        }
                    }
i = (i + 1);
                }
            } else {
                {
if (c == 44) {
                        {
if (i > start) {
                                {
tok = string_substring(rs, start, i);
v = parse_int(tok);
if ((inserted == 0) && (rev < v)) {
                                        {
out = append_csv_int(out, rev);
inserted = 1;
out = append_csv_str(out, tok);
                                        }
                                    } else {
if ((inserted == 0) && (rev == v)) {
                                            {
out = append_csv_str(out, tok);
inserted = 1;
                                            }
                                        } else {
                                            {
out = append_csv_str(out, tok);
                                            }
                                        }
                                    }
                                }
                            }
start = (i + 1);
                        }
                    }
i = (i + 1);
                }
            }
        }
    }
if (inserted == 0) {
        {
out = append_csv_int(out, rev);
        }
    }
    return out;
}

const char* rs_remove(const char* rs, int rev) {
int n = string_length(rs);
if (n == 0) {
        {
            return "";
        }
    }
const char* out = "";
    int _heap_out = 0; (void)_heap_out;
int i = 0;
int start = 0;
    int at_end;
    int c;
    int is_sep;
    const char* tok;
    int v;
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
c = string_char_at(rs, i);
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
if (i > start) {
                        {
tok = string_substring(rs, start, i);
v = parse_int(tok);
if (v != rev) {
                                {
out = append_csv_str(out, tok);
                                }
                            }
                        }
                    }
start = (i + 1);
                }
            }
i = (i + 1);
        }
    }
    return out;
}

int rs_contains(const char* rs, int rev) {
int n = string_length(rs);
if (n == 0) {
        {
            return 0;
        }
    }
int i = 0;
int start = 0;
    int at_end;
    int c;
    int is_sep;
    const char* tok;
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
c = string_char_at(rs, i);
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
if (i > start) {
                        {
tok = string_substring(rs, start, i);
if (parse_int(tok) == rev) {
                                {
                                    return 1;
                                }
                            }
                        }
                    }
start = (i + 1);
                }
            }
i = (i + 1);
        }
    }
    return 0;
}

const char* int_to_dec(int v) {
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
    const char* ch;
while (n > 0) {
        {
d = (n - ((n / 10) * 10));
ch = "";
if (d == 0) {
                {
ch = "0";
                }
            }
if (d == 1) {
                {
ch = "1";
                }
            }
if (d == 2) {
                {
ch = "2";
                }
            }
if (d == 3) {
                {
ch = "3";
                }
            }
if (d == 4) {
                {
ch = "4";
                }
            }
if (d == 5) {
                {
ch = "5";
                }
            }
if (d == 6) {
                {
ch = "6";
                }
            }
if (d == 7) {
                {
ch = "7";
                }
            }
if (d == 8) {
                {
ch = "8";
                }
            }
if (d == 9) {
                {
ch = "9";
                }
            }
out = string_concat(ch, out);
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

int parse_int(const char* s) {
int n = string_length(s);
if (n == 0) {
        {
            return 0;
        }
    }
int sign = 1;
int start = 0;
if (string_char_at(s, 0) == 45) {
        {
sign = -1;
start = 1;
        }
    }
int v = 0;
int i = start;
    int c;
while (i < n) {
        {
c = string_char_at(s, i);
if (c < 48) {
                {
                    return (v * sign);
                }
            }
if (c > 57) {
                {
                    return (v * sign);
                }
            }
v = ((v * 10) + (c - 48));
i = (i + 1);
        }
    }
    return (v * sign);
}

const char* append_csv_str(const char* dst, const char* tok) {
if (string_length(dst) == 0) {
        {
            return tok;
        }
    }
const char* with_comma = string_concat(dst, ",");
    int _heap_with_comma = 0; (void)_heap_with_comma;
    return string_concat(with_comma, tok);
}

const char* append_csv_int(const char* dst, int v) {
    return append_csv_str(dst, int_to_dec(v));
}

const char* parse_ranges(const char* rs_in, const char* list, int want_reverse) {
const char* rs = rs_in;
    int _heap_rs = 0; (void)_heap_rs;
int n = string_length(list);
int i = 0;
    int c;
    int is_reverse;
    int cc;
    int a;
    int d;
    int b;
    int e;
    int lo;
    int hi;
    int add_it;
    int r;
while (i < n) {
        {
while (i < n) {
                {
c = string_char_at(list, i);
if ((c != 44) && (c != 32)) {
                        {
                            break;
                        }
                    }
i = (i + 1);
                }
            }
if (i >= n) {
                {
                    return rs;
                }
            }
is_reverse = 0;
if (string_char_at(list, i) == 45) {
                {
is_reverse = 1;
i = (i + 1);
                }
            }
if (i >= n) {
                {
                    return rs;
                }
            }
c = string_char_at(list, i);
if ((c < 48) || (c > 57)) {
                {
while (i < n) {
                        {
cc = string_char_at(list, i);
if (cc == 44) {
                                {
                                    break;
                                }
                            }
i = (i + 1);
                        }
                    }
                }
            } else {
                {
a = 0;
while (i < n) {
                        {
d = string_char_at(list, i);
if ((d < 48) || (d > 57)) {
                                {
                                    break;
                                }
                            }
a = ((a * 10) + (d - 48));
i = (i + 1);
                        }
                    }
b = a;
if (i < n) {
                        {
if (string_char_at(list, i) == 45) {
                                {
i = (i + 1);
if (i < n) {
                                        {
e = string_char_at(list, i);
if ((e >= 48) && (e <= 57)) {
                                                {
b = 0;
while (i < n) {
                                                        {
d = string_char_at(list, i);
if ((d < 48) || (d > 57)) {
                                                                {
                                                                    break;
                                                                }
                                                            }
b = ((b * 10) + (d - 48));
i = (i + 1);
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
lo = a;
hi = b;
if (b < a) {
                        {
lo = b;
hi = a;
                        }
                    }
add_it = 0;
if ((is_reverse == 1) && (want_reverse == 1)) {
                        {
add_it = 1;
                        }
                    }
if ((is_reverse == 0) && (want_reverse == 0)) {
                        {
add_it = 1;
                        }
                    }
if (add_it == 1) {
                        {
r = lo;
while (r <= hi) {
                                {
rs = rs_add(rs, r);
r = (r + 1);
                                }
                            }
                        }
                    }
while (i < n) {
                        {
d = string_char_at(list, i);
if (d == 44) {
                                {
                                    break;
                                }
                            }
i = (i + 1);
                        }
                    }
                }
            }
        }
    }
    return rs;
}

const char* cancel_pairs(const char* fwd, const char* rev) {
int n = string_length(fwd);
const char* new_fwd = fwd;
    int _heap_new_fwd = 0; (void)_heap_new_fwd;
const char* new_rev = rev;
    int _heap_new_rev = 0; (void)_heap_new_rev;
int i = 0;
int start = 0;
    int at_end;
    int c;
    int is_sep;
    const char* tok;
    int v;
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
c = string_char_at(fwd, i);
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
if (i > start) {
                        {
tok = string_substring(fwd, start, i);
v = parse_int(tok);
if (rs_contains(new_rev, v) == 1) {
                                {
new_fwd = rs_remove(new_fwd, v);
new_rev = rs_remove(new_rev, v);
                                }
                            }
                        }
                    }
start = (i + 1);
                }
            }
i = (i + 1);
        }
    }
    return string_concat(string_concat(new_fwd, "|"), new_rev);
}

const char* emit_ranges(const char* rs, int is_reverse) {
int n = string_length(rs);
if (n == 0) {
        {
            return "";
        }
    }
const char* out = "";
    int _heap_out = 0; (void)_heap_out;
int i = 0;
int start = 0;
int prev_ok = 0;
int range_lo = 0;
int range_hi = 0;
    int at_end;
    int c;
    int is_sep;
    const char* tok;
    int v;
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
c = string_char_at(rs, i);
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
if (i > start) {
                        {
tok = string_substring(rs, start, i);
v = parse_int(tok);
if (prev_ok == 0) {
                                {
range_lo = v;
range_hi = v;
prev_ok = 1;
                                }
                            } else {
                                {
if (v == (range_hi + 1)) {
                                        {
range_hi = v;
                                        }
                                    } else {
                                        {
out = emit_one(out, range_lo, range_hi, is_reverse);
range_lo = v;
range_hi = v;
                                        }
                                    }
                                }
                            }
                        }
                    }
start = (i + 1);
                }
            }
i = (i + 1);
        }
    }
if (prev_ok == 1) {
        {
out = emit_one(out, range_lo, range_hi, is_reverse);
        }
    }
    return out;
}

const char* emit_one(const char* dst, int lo, int hi, int is_reverse) {
const char* piece = string_concat(string_concat(int_to_dec(lo), "-"), int_to_dec(hi));
    int _heap_piece = 0; (void)_heap_piece;
if (is_reverse == 1) {
        {
piece = string_concat("-", piece);
        }
    }
    return append_csv_str(dst, piece);
}

int find_colon(const char* s, int from, int until) {
int i = from;
while (i < until) {
        {
if (string_char_at(s, i) == 58) {
                {
                    return i;
                }
            }
i = (i + 1);
        }
    }
    return -1;
}

const char* extract_source_sets(const char* existing, const char* source) {
int n = string_length(existing);
if (n == 0) {
        {
            return "|";
        }
    }
int slen = string_length(source);
int line_start = 0;
const char* fwd = "";
    int _heap_fwd = 0; (void)_heap_fwd;
const char* rev = "";
    int _heap_rev = 0; (void)_heap_rev;
int i = 0;
    int at_end;
    int c;
    int is_newline;
    int colon;
    int this_len;
    int j;
    int is_match;
    const char* list_str;
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
c = string_char_at(existing, i);
                }
            }
is_newline = 0;
if (at_end == 1) {
                {
is_newline = 1;
                }
            }
if (c == 10) {
                {
is_newline = 1;
                }
            }
if (is_newline == 1) {
                {
if (i > line_start) {
                        {
colon = find_colon(existing, line_start, i);
if (colon > line_start) {
                                {
this_len = (colon - line_start);
if (this_len == slen) {
                                        {
j = 0;
is_match = 1;
while (j < slen) {
                                                {
if (string_char_at(existing, (line_start + j)) != string_char_at(source, j)) {
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
list_str = string_substring(existing, (colon + 1), i);
fwd = parse_ranges(fwd, list_str, 0);
rev = parse_ranges(rev, list_str, 1);
                                                }
                                            }
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
    return string_concat(string_concat(fwd, "|"), rev);
}

const char* split_left(const char* joined) {
int n = string_length(joined);
int i = 0;
while (i < n) {
        {
if (string_char_at(joined, i) == 124) {
                {
                    return string_substring(joined, 0, i);
                }
            }
i = (i + 1);
        }
    }
    return joined;
}

const char* split_right(const char* joined) {
int n = string_length(joined);
int i = 0;
while (i < n) {
        {
if (string_char_at(joined, i) == 124) {
                {
                    return string_substring(joined, (i + 1), n);
                }
            }
i = (i + 1);
        }
    }
    return "";
}

// Exported:
const char* mergeinfo_add_range(const char* existing, const char* source, int lo, int hi, int reverse) {
const char* pair = extract_source_sets(existing, source);
    int _heap_pair = 0; (void)_heap_pair;
const char* new_fwd = split_left(pair);
    int _heap_new_fwd = 0; (void)_heap_new_fwd;
const char* new_rev = split_right(pair);
    int _heap_new_rev = 0; (void)_heap_new_rev;
int r = (lo + 1);
while (r <= hi) {
        {
if (reverse == 1) {
                {
new_rev = rs_add(new_rev, r);
                }
            }
if (reverse == 0) {
                {
new_fwd = rs_add(new_fwd, r);
                }
            }
r = (r + 1);
        }
    }
const char* cancelled = cancel_pairs(new_fwd, new_rev);
    int _heap_cancelled = 0; (void)_heap_cancelled;
new_fwd = split_left(cancelled);
new_rev = split_right(cancelled);
int n = string_length(existing);
const char* out = "";
    int _heap_out = 0; (void)_heap_out;
int saw_source = 0;
int slen = string_length(source);
int line_start = 0;
int i = 0;
    int at_end;
    int c;
    int is_newline;
    int colon;
    int this_len;
    const char* this_src;
    int is_ours;
    int j;
    const char* line;
    const char* list_str;
    const char* of;
    const char* or_;
    const char* paircan;
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
c = string_char_at(existing, i);
                }
            }
is_newline = 0;
if (at_end == 1) {
                {
is_newline = 1;
                }
            }
if (c == 10) {
                {
is_newline = 1;
                }
            }
if (is_newline == 1) {
                {
if (i > line_start) {
                        {
colon = find_colon(existing, line_start, i);
if (colon > line_start) {
                                {
this_len = (colon - line_start);
this_src = string_substring(existing, line_start, colon);
is_ours = 0;
if (this_len == slen) {
                                        {
j = 0;
is_ours = 1;
while (j < slen) {
                                                {
if (string_char_at(existing, (line_start + j)) != string_char_at(source, j)) {
                                                        {
is_ours = 0;
                                                            break;
                                                        }
                                                    }
j = (j + 1);
                                                }
                                            }
                                        }
                                    }
if (is_ours == 1) {
                                        {
if (saw_source == 0) {
                                                {
line = build_source_line(this_src, new_fwd, new_rev);
if (string_length(line) > 0) {
                                                        {
if (string_length(out) > 0) {
                                                                {
out = string_concat(out, "\n");
                                                                }
                                                            }
out = string_concat(out, line);
                                                        }
                                                    }
saw_source = 1;
                                                }
                                            }
                                        }
                                    } else {
                                        {
list_str = string_substring(existing, (colon + 1), i);
of = parse_ranges("", list_str, 0);
or_ = parse_ranges("", list_str, 1);
paircan = cancel_pairs(of, or_);
of = split_left(paircan);
or_ = split_right(paircan);
line = build_source_line(this_src, of, or_);
if (string_length(line) > 0) {
                                                {
if (string_length(out) > 0) {
                                                        {
out = string_concat(out, "\n");
                                                        }
                                                    }
out = string_concat(out, line);
                                                }
                                            }
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
if (saw_source == 0) {
        {
line = build_source_line(source, new_fwd, new_rev);
if (string_length(line) > 0) {
                {
if (string_length(out) > 0) {
                        {
out = string_concat(out, "\n");
                        }
                    }
out = string_concat(out, line);
                }
            }
        }
    }
    return out;
}

const char* build_source_line(const char* src, const char* fwd, const char* rev) {
if (string_length(fwd) == 0) {
        {
if (string_length(rev) == 0) {
                {
                    return "";
                }
            }
        }
    }
const char* fwd_emit = emit_ranges(fwd, 0);
    int _heap_fwd_emit = 0; (void)_heap_fwd_emit;
const char* rev_emit = emit_ranges(rev, 1);
    int _heap_rev_emit = 0; (void)_heap_rev_emit;
const char* ranges = fwd_emit;
    int _heap_ranges = 0; (void)_heap_ranges;
if (string_length(ranges) == 0) {
        {
ranges = rev_emit;
        }
    } else {
        {
if (string_length(rev_emit) > 0) {
                {
ranges = string_concat(string_concat(ranges, ","), rev_emit);
                }
            }
        }
    }
if (string_length(ranges) == 0) {
        {
            return "";
        }
    }
    return string_concat(string_concat(src, ":"), ranges);
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

const char* aether_rs_add(const char* rs, int32_t rev) {
    return rs_add(rs, rev);
}
const char* aether_rs_remove(const char* rs, int32_t rev) {
    return rs_remove(rs, rev);
}
int32_t aether_rs_contains(const char* rs, int32_t rev) {
    return rs_contains(rs, rev);
}
const char* aether_int_to_dec(int32_t v) {
    return int_to_dec(v);
}
int32_t aether_parse_int(const char* s) {
    return parse_int(s);
}
const char* aether_append_csv_str(const char* dst, const char* tok) {
    return append_csv_str(dst, tok);
}
const char* aether_append_csv_int(const char* dst, int32_t v) {
    return append_csv_int(dst, v);
}
const char* aether_parse_ranges(const char* rs_in, const char* list, int32_t want_reverse) {
    return parse_ranges(rs_in, list, want_reverse);
}
const char* aether_cancel_pairs(const char* fwd, const char* rev) {
    return cancel_pairs(fwd, rev);
}
const char* aether_emit_ranges(const char* rs, int32_t is_reverse) {
    return emit_ranges(rs, is_reverse);
}
const char* aether_emit_one(const char* dst, int32_t lo, int32_t hi, int32_t is_reverse) {
    return emit_one(dst, lo, hi, is_reverse);
}
int32_t aether_find_colon(const char* s, int32_t from, int32_t until) {
    return find_colon(s, from, until);
}
const char* aether_extract_source_sets(const char* existing, const char* source) {
    return extract_source_sets(existing, source);
}
const char* aether_split_left(const char* joined) {
    return split_left(joined);
}
const char* aether_split_right(const char* joined) {
    return split_right(joined);
}
const char* aether_mergeinfo_add_range(const char* existing, const char* source, int32_t lo, int32_t hi, int32_t reverse) {
    return mergeinfo_add_range(existing, source, lo, hi, reverse);
}
const char* aether_build_source_line(const char* src, const char* fwd, const char* rev) {
    return build_source_line(src, fwd, rev);
}
