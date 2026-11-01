/* The POSIX that the engine's tests use, on Windows, so that the same tests run
 * on every system. Force-included in them there (scripts/lib.sh), with this
 * directory first for <unistd.h>, <pthread.h>, <poll.h>, <dlfcn.h> and
 * <sys/mman.h>. For tests only: the engine itself uses none of it. */

#ifndef ZEBKIT_TEST_WIN_COMPAT_H
#define ZEBKIT_TEST_WIN_COMPAT_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef small
#undef ERROR

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <process.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define WIN_COMPAT static inline

/* ---- time ---- */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#endif
typedef int clockid_t;
WIN_COMPAT int clock_gettime(clockid_t clock, struct timespec *out) {
    if (clock == CLOCK_REALTIME)
        return timespec_get(out, TIME_UTC) ? 0 : -1;
    LARGE_INTEGER now, rate;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&rate);
    out->tv_sec = (time_t)(now.QuadPart / rate.QuadPart);
    out->tv_nsec = (long)((now.QuadPart % rate.QuadPart) * 1000000000 / rate.QuadPart);
    return 0;
}
WIN_COMPAT int nanosleep(const struct timespec *wanted, struct timespec *left) {
    (void)left;
    Sleep((DWORD)(wanted->tv_sec * 1000 + (wanted->tv_nsec + 999999) / 1000000));
    return 0;
}
WIN_COMPAT int usleep(unsigned microseconds) {
    Sleep((microseconds + 999) / 1000);
    return 0;
}
WIN_COMPAT unsigned sleep(unsigned seconds) {
    Sleep(seconds * 1000);
    return 0;
}

/* ---- strings ---- */
WIN_COMPAT char *strcasestr(const char *text, const char *wanted) {
    const size_t length = strlen(wanted);
    for (; *text; ++text) {
        if (!_strnicmp(text, wanted, length))
            return (char *)text;
    }
    return length ? NULL : (char *)text;
}
WIN_COMPAT void *memmem(const void *text, size_t length, const void *wanted, size_t wanted_length) {
    const char *at = (const char *)text;
    for (; length >= wanted_length; ++at, --length) {
        if (!memcmp(at, wanted, wanted_length))
            return (void *)at;
    }
    return NULL;
}
WIN_COMPAT char *strndup(const char *text, size_t length) {
    const size_t used = strnlen(text, length);
    char *copy = (char *)malloc(used + 1);
    if (copy) {
        memcpy(copy, text, used);
        copy[used] = 0;
    }
    return copy;
}
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#ifndef strdup
#define strdup _strdup
#endif

/* ---- the environment and the file system ---- */
WIN_COMPAT int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name))
        return 0;
    return _putenv_s(name, value);
}
WIN_COMPAT int unsetenv(const char *name) {
    return _putenv_s(name, "");
}
WIN_COMPAT char *mkdtemp(char *pattern) {
    const size_t length = strlen(pattern);
    if (length < 6)
        return NULL;
    for (unsigned attempt = 0; attempt < 10000; ++attempt) {
        snprintf(pattern + length - 6, 7, "%06u", (GetCurrentProcessId() * 31u + attempt) % 1000000u);
        if (CreateDirectoryA(pattern, NULL))
            return pattern;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return NULL;
    }
    return NULL;
}
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
/* fcntl for a pipe's O_NONBLOCK, as a flag win_compat_read() looks at. */
#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 0x4000
static int win_compat_nonblocking[64];
WIN_COMPAT int fcntl(int fd, int command, ...) {
    if (command == F_GETFL)
        return 0;
    if (fd >= 0 && fd < 64)
        win_compat_nonblocking[fd] = 1;
    return 0;
}
WIN_COMPAT int win_compat_read(int fd, void *buffer, unsigned size) {
    if (fd >= 0 && fd < 64 && win_compat_nonblocking[fd]) {
        DWORD available = 0;
        PeekNamedPipe((HANDLE)_get_osfhandle(fd), NULL, 0, NULL, &available, NULL);
        if (!available) {
            errno = EAGAIN;
            return -1;
        }
    }
    return _read(fd, buffer, size);
}
#define getpid _getpid
#define access _access
#define rmdir _rmdir
#define unlink _unlink
#define close _close
#define read win_compat_read
#define write _write
#define dup _dup
#define dup2 _dup2
#ifndef F_OK
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 0
#endif
#define mkdir(path, mode) _mkdir(path)
WIN_COMPAT int pipe(int ends[2]) {
    return _pipe(ends, 4096, _O_BINARY);
}

/* ---- threads ---- */
typedef HANDLE pthread_t;
typedef void pthread_attr_t;
typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT

struct win_compat_thread {
    void *(*start)(void *);
    void *argument;
};
WIN_COMPAT DWORD WINAPI win_compat_thread_main(void *data) {
    struct win_compat_thread thread = *(struct win_compat_thread *)data;
    free(data);
    /* What a test returns is a small number, which is all an exit code keeps. */
    return (DWORD)(uintptr_t)thread.start(thread.argument);
}
WIN_COMPAT int pthread_create(pthread_t *out, const pthread_attr_t *attributes, void *(*start)(void *), void *argument) {
    (void)attributes;
    struct win_compat_thread *thread = (struct win_compat_thread *)malloc(sizeof(*thread));
    thread->start = start;
    thread->argument = argument;
    *out = CreateThread(NULL, 0, win_compat_thread_main, thread, 0, NULL);
    return *out ? 0 : EAGAIN;
}
WIN_COMPAT int pthread_join(pthread_t thread, void **result) {
    WaitForSingleObject(thread, INFINITE);
    if (result) {
        DWORD code = 0;
        GetExitCodeThread(thread, &code);
        *result = (void *)(uintptr_t)code;
    }
    CloseHandle(thread);
    return 0;
}
WIN_COMPAT pthread_t pthread_self(void) {
    return GetCurrentThread();
}
WIN_COMPAT int pthread_mutex_init(pthread_mutex_t *mutex, const void *attributes) {
    (void)attributes;
    InitializeSRWLock(mutex);
    return 0;
}
WIN_COMPAT int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    (void)mutex;
    return 0;
}
WIN_COMPAT int pthread_mutex_lock(pthread_mutex_t *mutex) {
    AcquireSRWLockExclusive(mutex);
    return 0;
}
WIN_COMPAT int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    ReleaseSRWLockExclusive(mutex);
    return 0;
}
WIN_COMPAT int pthread_cond_init(pthread_cond_t *condition, const void *attributes) {
    (void)attributes;
    InitializeConditionVariable(condition);
    return 0;
}
WIN_COMPAT int pthread_cond_destroy(pthread_cond_t *condition) {
    (void)condition;
    return 0;
}
WIN_COMPAT int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex) {
    return SleepConditionVariableSRW(condition, mutex, INFINITE, 0) ? 0 : EINVAL;
}
WIN_COMPAT int pthread_cond_signal(pthread_cond_t *condition) {
    WakeConditionVariable(condition);
    return 0;
}
WIN_COMPAT int pthread_cond_broadcast(pthread_cond_t *condition) {
    WakeAllConditionVariable(condition);
    return 0;
}
/* A thread's name: Windows 10's, looked up since older systems lack it. */
WIN_COMPAT int pthread_setname_np(pthread_t thread, const char *name) {
    typedef HRESULT(WINAPI * Set)(HANDLE, PCWSTR);
    Set set = (Set)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
    wchar_t wide[64];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64);
    return set && SUCCEEDED(set(thread, wide)) ? 0 : EINVAL;
}
WIN_COMPAT int pthread_getname_np(pthread_t thread, char *name, size_t size) {
    typedef HRESULT(WINAPI * Get)(HANDLE, PWSTR *);
    Get get = (Get)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    wchar_t *wide = NULL;
    if (!get || FAILED(get(thread, &wide)))
        return EINVAL;
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, (int)size, NULL, NULL);
    LocalFree(wide);
    return 0;
}

/* ---- signals: Windows has no masks and no SIGPIPE. What a test checks of them
 * (that the engine leaves the application's alone) has nothing to check here. ---- */
typedef int sigset_t;
struct sigaction {
    void (*sa_handler)(int);
    int sa_flags;
    sigset_t sa_mask;
};
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGUSR1
#define SIGUSR1 10
#endif
#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif
WIN_COMPAT int sigemptyset(sigset_t *set) {
    *set = 0;
    return 0;
}
WIN_COMPAT int sigaddset(sigset_t *set, int number) {
    *set |= 1 << number;
    return 0;
}
WIN_COMPAT int sigismember(const sigset_t *set, int number) {
    return (*set >> number) & 1;
}
WIN_COMPAT int pthread_sigmask(int how, const sigset_t *set, sigset_t *old) {
    static __declspec(thread) sigset_t mask;
    if (old)
        *old = mask;
    if (set)
        mask = how == SIG_BLOCK ? mask | *set : how == SIG_UNBLOCK ? mask & ~*set : *set;
    return 0;
}
WIN_COMPAT int sigaction(int number, const struct sigaction *wanted, struct sigaction *old) {
    static struct sigaction actions[32];
    if (number < 0 || number >= 32)
        return -1;
    if (old)
        *old = actions[number];
    if (wanted)
        actions[number] = *wanted;
    return 0;
}

/* ---- loading a library ---- */
#define RTLD_NOW 2
#define RTLD_LAZY 1
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 8
WIN_COMPAT void *dlopen(const char *path, int flags) {
    (void)flags;
    return LoadLibraryA(path);
}
WIN_COMPAT void *dlsym(void *library, const char *name) {
    return (void *)GetProcAddress((HMODULE)library, name);
}
WIN_COMPAT int dlclose(void *library) {
    return FreeLibrary((HMODULE)library) ? 0 : -1;
}
WIN_COMPAT const char *dlerror(void) {
    static char text[64];
    snprintf(text, sizeof(text), "Windows error %lu", GetLastError());
    return text;
}

/* ---- mapping memory: there are no such descriptors on Windows, where a frame
 * has a file mapping (zebkit_frame.handle). These always fail, for code that is
 * not reached there. ---- */
#define PROT_READ 1
#define MAP_SHARED 1
#define MAP_FAILED ((void *)-1)
WIN_COMPAT void *mmap(void *address, size_t size, int protection, int flags, int fd, long offset) {
    (void)address, (void)size, (void)protection, (void)flags, (void)fd, (void)offset;
    return MAP_FAILED;
}
WIN_COMPAT int munmap(void *address, size_t size) {
    (void)address, (void)size;
    return -1;
}

#endif /* ZEBKIT_TEST_WIN_COMPAT_H */
