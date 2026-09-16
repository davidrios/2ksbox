/* specrun.c — the guest half of tools/specbench: runs a list of commands
 * one after another, times each with QueryPerformanceCounter, CRC-32s
 * what it wrote to stdout, and reports one line per run to a results file
 * (a floppy the host reads from outside) and to COM1.
 *
 *   specrun.exe <list> <results> <workdir> <reps>
 *
 * <list> is tab-separated, one command per line:
 *   name <TAB> command line <TAB> stdin file or - <TAB> extra output file to CRC or -
 * The command runs through cmd.exe /c from <workdir>, its stdout in
 * <workdir>\<name>.out, its stderr in <name>.err. Lines out:
 *   START <name> rep=<n>            (the host driver keys a GUI program on this)
 *   RESULT <name> rep=<n> ms=<elapsed> cpu=<ms> crc=<crc32 of stdout [^ extra]> exit=<code>
 *   DONE
 * ms is wall time from CreateProcess to exit; cpu is the user+kernel time of
 * every process the command started (a job object, so a GUI program
 * `start`ed by a batch file and keyed from the host counts too, and the
 * seconds spent waiting for its keys do not).
 * The CRC is the correctness check: every configuration of the emulator
 * must produce the same one for the same command, or the speedup is of a
 * different computation. Built with guest-tools' msvcrt flags (XP has no UCRT).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned crc_tab[256];
static void crc_init(void) {
    unsigned i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
}
static unsigned crc_file(const char *path) {
    static unsigned char buf[65536];
    FILE *f = fopen(path, "rb");
    unsigned c = 0xFFFFFFFFu;
    size_t n, i;
    if (!f) return 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        for (i = 0; i < n; i++) c = crc_tab[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    fclose(f);
    return c ^ 0xFFFFFFFFu;
}

static HANDLE com, hjob;
static const char *results;
static DWORD job_cpu_ms(void) {  /* user + kernel time of everything that ran in the job */
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION a;
    DWORD n;
    if (!hjob || !QueryInformationJobObject(hjob, JobObjectBasicAccountingInformation, &a, sizeof a, &n)) return 0;
    return (DWORD)((a.TotalUserTime.QuadPart + a.TotalKernelTime.QuadPart) / 10000);
}
static void report(const char *line) {
    DWORD w;
    FILE *r = fopen(results, "ab");
    if (r) { fputs(line, r); fclose(r); }
    if (com != INVALID_HANDLE_VALUE) WriteFile(com, line, (DWORD)strlen(line), &w, NULL);
    fputs(line, stdout); fflush(stdout);
}

static char *field(char **p) {  /* next tab-separated field, in place */
    char *s = *p, *t;
    if (!s) return NULL;
    t = strpbrk(s, "\t\r\n");
    if (t) { char sep = *t; *t = 0; *p = (sep == '\t') ? t + 1 : NULL; } else *p = NULL;
    return s;
}

int main(int argc, char **argv) {
    const char *list, *workdir;
    int reps, rep;
    LARGE_INTEGER freq;
    char line[2048], msg[512];
    if (argc < 5) { fprintf(stderr, "usage: specrun <list> <results> <workdir> <reps>\n"); return 2; }
    list = argv[1]; results = argv[2]; workdir = argv[3]; reps = atoi(argv[4]);
    crc_init();
    QueryPerformanceFrequency(&freq);
    hjob = CreateJobObjectA(NULL, NULL);
    com = CreateFileA("\\\\.\\COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    SetCurrentDirectoryA(workdir);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    sprintf(msg, "SPECRUN list=%s reps=%d qpc=%lu\r\n", list, reps, (unsigned long)freq.QuadPart);
    report(msg);
    for (rep = 1; rep <= reps; rep++) {
        FILE *f = fopen(list, "r");
        if (!f) { report("ERROR no list\r\n"); return 2; }
        while (fgets(line, sizeof line, f)) {
            char *p = line, *name, *cmd, *in, *extra;
            char cmdline[2200], outp[MAX_PATH], errp[MAX_PATH];
            SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
            STARTUPINFOA si; PROCESS_INFORMATION pi;
            LARGE_INTEGER t0, t1;
            DWORD code = 0, ms, cpu0, cpu;
            unsigned crc;
            HANDLE hin, hout, herr;
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == 0) continue;
            name = field(&p); cmd = field(&p); in = field(&p); extra = field(&p);
            if (!name || !cmd) continue;
            if (!in) in = "-"; if (!extra) extra = "-";
            sprintf(outp, "%s.out", name); sprintf(errp, "%s.err", name);
            hin = (strcmp(in, "-") == 0)
                ? CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, NULL)
                : CreateFileA(in, GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, NULL);
            hout = CreateFileA(outp, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, NULL);
            herr = CreateFileA(errp, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, NULL);
            memset(&si, 0, sizeof si); si.cb = sizeof si;
            si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = hin; si.hStdOutput = hout; si.hStdError = herr;
            sprintf(cmdline, "cmd.exe /c %s", cmd);
            sprintf(msg, "START %s rep=%d\r\n", name, rep);
            report(msg);
            cpu0 = job_cpu_ms();
            QueryPerformanceCounter(&t0);
            if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
                sprintf(msg, "RESULT %s rep=%d ms=0 crc=00000000 exit=-1 (CreateProcess %lu)\r\n", name, rep, (unsigned long)GetLastError());
                report(msg);
            } else {
                if (hjob) AssignProcessToJobObject(hjob, pi.hProcess);
                ResumeThread(pi.hThread);
                WaitForSingleObject(pi.hProcess, INFINITE);
                QueryPerformanceCounter(&t1);
                GetExitCodeProcess(pi.hProcess, &code);
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                ms = (DWORD)((t1.QuadPart - t0.QuadPart) * 1000 / freq.QuadPart);
                cpu = job_cpu_ms() - cpu0;
                CloseHandle(hout); CloseHandle(herr); CloseHandle(hin);
                hout = herr = hin = INVALID_HANDLE_VALUE;
                crc = crc_file(outp);
                if (strcmp(extra, "-") != 0) crc ^= crc_file(extra);
                sprintf(msg, "RESULT %s rep=%d ms=%lu cpu=%lu crc=%08x exit=%lu\r\n", name, rep, (unsigned long)ms, (unsigned long)cpu, crc, (unsigned long)code);
                report(msg);
            }
            if (hout != INVALID_HANDLE_VALUE) CloseHandle(hout);
            if (herr != INVALID_HANDLE_VALUE) CloseHandle(herr);
            if (hin != INVALID_HANDLE_VALUE) CloseHandle(hin);
        }
        fclose(f);
    }
    report("DONE\r\n");
    return 0;
}
