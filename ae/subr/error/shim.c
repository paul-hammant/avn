/*
 * Copyright 2026 Paul Hammant (portions).
 * Portions copyright Apache Subversion project contributors (2001-2026).
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

/* error_shim.c — C backing for the Aether error-chain type.
 *
 * Aether structs are value types and cannot be self-referential, so the error
 * chain (linked list of {code, message, cause, file, line}) is implemented in
 * C and exposed to Aether as an opaque `ptr`. One malloc per link; callers
 * free the head and we walk the chain.
 *
 * This is the only piece of svn_error_t that needs C — the rest (error codes,
 * wrapping helpers, formatting) lives in error.ae.
 */

#include <stdlib.h>
#include <string.h>

struct svnae_error {
    int code;                   /* SVN_ERR_* code, or 0 for "no error" leaf */
    char *message;              /* malloc'd copy, owned by this link */
    struct svnae_error *cause;  /* NULL == end of chain */
    const char *file;           /* __FILE__ — static, not copied */
    int line;
};

/* Create a new error link. `message` is copied. `cause` is adopted (ownership
 * transfers to the returned error; free of head will free the whole chain).
 * `file` must outlive the error (pass a string literal). */
struct svnae_error *
svnae_error_create(int code, const char *message, struct svnae_error *cause,
                   const char *file, int line)
{
    struct svnae_error *e = malloc(sizeof *e);
    if (!e) return NULL;
    e->code = code;
    e->message = message ? strdup(message) : NULL;
    e->cause = cause;
    e->file = file;
    e->line = line;
    return e;
}

int svnae_error_code(const struct svnae_error *e)      { return e ? e->code : 0; }
const char *svnae_error_message(const struct svnae_error *e) { return e && e->message ? e->message : ""; }
struct svnae_error *svnae_error_cause(const struct svnae_error *e) { return e ? e->cause : NULL; }
const char *svnae_error_file(const struct svnae_error *e)    { return e && e->file ? e->file : ""; }
int svnae_error_line(const struct svnae_error *e)            { return e ? e->line : 0; }

/* Free the entire chain starting at head. Safe to call with NULL. */
void
svnae_error_free(struct svnae_error *e)
{
    while (e) {
        struct svnae_error *next = e->cause;
        free(e->message);
        free(e);
        e = next;
    }
}
