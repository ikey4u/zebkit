/*
 * crashsite: a preloadable library that reports where a process crashed.
 *
 *   LD_PRELOAD=crashsite.so CRASHSITE_LOG=/tmp/crash.log <program>
 *
 * The engine ships without unwind tables or frame pointers, so a debugger
 * cannot walk its stack. On SIGSEGV, SIGBUS, SIGILL, SIGFPE or SIGTRAP this
 * writes the faulting address and program counter, then every word on the
 * stack that points into executable memory, each as "module+offset". Most of
 * those words are return addresses, which makes a usable approximate
 * backtrace. scripts/symbolize.py turns the offsets into function names
 * using the unstripped engine in out/<profile>/lib.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#define MAX_MAPS 512
#define STACK_WORDS 4096

static struct { uintptr_t start, end, offset; char name[96]; } maps[MAX_MAPS];
static int mapCount;
static int logFd = 2;

/* Read the executable mappings once, at load, where calling into libc is safe. */
static void readMaps(void)
{
    FILE* file = fopen("/proc/self/maps", "r");
    char line[512];
    mapCount = 0;
    while (file && fgets(line, sizeof line, file) && mapCount < MAX_MAPS) {
        unsigned long start, end, offset;
        char perms[8], path[256] = "";
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %255s", &start, &end, perms, &offset, path) < 4)
            continue;
        if (perms[2] != 'x')
            continue;
        const char* base = strrchr(path, '/');
        maps[mapCount].start = start;
        maps[mapCount].end = end;
        maps[mapCount].offset = offset;
        snprintf(maps[mapCount].name, sizeof maps[mapCount].name, "%.90s", base ? base + 1 : path);
        mapCount++;
    }
    if (file)
        fclose(file);
}

static void describe(const char* label, uintptr_t address)
{
    char line[192];
    for (int i = 0; i < mapCount; i++) {
        if (address >= maps[i].start && address < maps[i].end) {
            int length = snprintf(line, sizeof line, "%s %s+0x%lx\n", label, maps[i].name,
                (unsigned long)(address - maps[i].start + maps[i].offset));
            if (write(logFd, line, length) < 0) { }
            return;
        }
    }
}

static void onCrash(int signalNumber, siginfo_t* info, void* context)
{
    ucontext_t* uc = context;
    uintptr_t pc = uc->uc_mcontext.gregs[REG_RIP];
    uintptr_t sp = uc->uc_mcontext.gregs[REG_RSP];
    char line[160];
    /* Libraries loaded after start-up (plugins) are only known now. */
    readMaps();
    int length = snprintf(line, sizeof line, "crash pid %d signal %d address %p\n", getpid(), signalNumber, info->si_addr);
    if (write(logFd, line, length) < 0) { }
    describe("pc", pc);
    for (int i = 0; i < STACK_WORDS; i++)
        describe("stack", ((uintptr_t*)sp)[i]);
    if (write(logFd, "end\n", 4) < 0) { }
    signal(signalNumber, SIG_DFL);
    raise(signalNumber);
}

__attribute__((constructor)) static void install(void)
{
    const char* path = getenv("CRASHSITE_LOG");
    if (path)
        logFd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logFd < 0)
        logFd = 2;

    static char stack[1 << 16];
    stack_t alternate = { .ss_sp = stack, .ss_size = sizeof stack };
    sigaltstack(&alternate, NULL);

    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = onCrash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP };
    for (unsigned i = 0; i < sizeof signals / sizeof *signals; i++)
        sigaction(signals[i], &action, NULL);
}
