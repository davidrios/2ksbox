/* What Wine's winecrt0 gives its test programs and a mingw link does not:
 * the four debug helpers wine/debug.h declares (wine_dbgstr_* formats into
 * __wine_dbg_strdup's buffers). Built into d3d8_test.exe / d3d9_test.exe by
 * guest-tools/build-winetests.sh (track M16). */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <wine/debug.h>

/* a ring of buffers, as winecrt0's: a test prints a few dbgstr results in
 * one ok() and each must live until the call returns */
static char ring[32][1024];
static LONG next;

const char *__cdecl __wine_dbg_strdup(const char *str)
{
    char *b = ring[InterlockedIncrement(&next) & 31];
    size_t n = strlen(str);

    if (n >= sizeof(ring[0]))
        n = sizeof(ring[0]) - 1;
    memcpy(b, str, n);
    b[n] = 0;
    return b;
}

int __cdecl __wine_dbg_output(const char *str)
{
    return fputs(str, stderr) < 0 ? -1 : (int)strlen(str);
}

int __cdecl __wine_dbg_header(enum __wine_debug_class cls,
                              struct __wine_debug_channel *channel,
                              const char *function)
{
    (void)cls; (void)channel; (void)function;
    return -1;  /* no channel is ever on: nothing is printed */
}

unsigned char __cdecl __wine_dbg_get_channel_flags(struct __wine_debug_channel *channel)
{
    (void)channel;
    return 0;
}
