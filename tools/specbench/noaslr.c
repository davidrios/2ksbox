/* noaslr: run a command with address-space randomisation off (macOS).
 *   noaslr <program> [args...]
 * For tools/specbench: a QEMU launch is sometimes 35 % slower on helper-heavy
 * code with byte-identical binaries, and the suspects are per-launch
 * placements (the JIT buffer, the heap, the stacks). With ASLR off every
 * launch gets the same layout, so the regime should become consistent if
 * placement is the cause. _POSIX_SPAWN_DISABLE_ASLR is 0x0100 in Apple's
 * spawn.h (private, but it is what lldb and `arch -no-aslr`... use). */
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef _POSIX_SPAWN_DISABLE_ASLR
#define _POSIX_SPAWN_DISABLE_ASLR 0x0100
#endif
extern char **environ;
int main(int argc, char **argv) {
    posix_spawnattr_t attr; pid_t pid; int st;
    if (argc < 2) { fprintf(stderr, "usage: noaslr <program> [args...]\n"); return 2; }
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, _POSIX_SPAWN_DISABLE_ASLR);
    if (posix_spawnp(&pid, argv[1], NULL, &attr, argv + 1, environ) != 0) { perror("posix_spawnp"); return 2; }
    if (waitpid(pid, &st, 0) < 0) { perror("waitpid"); return 2; }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
