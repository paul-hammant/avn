/* utf8/shim.c — thin wrapper around libutf8proc's NFC normalization.
 *
 * svn normalizes internal paths to NFC so that macOS (which stores them
 * NFD on HFS+/APFS) round-trips correctly against Linux/Windows. We'll
 * call svnae_utf8_nfc at the boundaries where we first receive a path
 * from the OS (readdir, argv, environment) and trust the internal form
 * everywhere else.
 */

#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

/* Returns a newly-malloc'd NUL-terminated NFC form of `s`, or NULL on error.
 * Caller frees via svnae_utf8_free. */
char *
svnae_utf8_nfc(const char *s)
{
    utf8proc_uint8_t *out = utf8proc_NFC((const utf8proc_uint8_t *)s);
    return (char *)out;
}

void svnae_utf8_free(char *p) { free(p); }
