/* platform.h: the host's system, behind a handful of calls. UCMapKitHost.cpp is the protocol and the commands;
   everything a system does differently is here: a path in the file system's letters, a file, the clock, the
   executable's folder, a library and its exports, and (on macOS and Linux) the fault reporting of a command.
   Windows keeps the calls it always had (windows.h); macOS and Linux use POSIX (dlfcn.h, signal.h, unistd.h). */
#ifndef UC_PLATFORM_H
#define UC_PLATFORM_H

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE             // dl_iterate_phdr, prctl: before any header
#endif

#if defined(_WIN32)
#define UC_BRIDGE_FILE "UCEngineBridge.dll"
#elif defined(__APPLE__)
#define UC_BRIDGE_FILE "UCEngineBridge.dylib"
#else
#define UC_BRIDGE_FILE "UCEngineBridge.so"
#endif

#ifdef _WIN32

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <string>
#include <vector>

typedef std::wstring HostPath;              // the file system's own letters (UTF-16)
typedef DWORD HostTick;                     // milliseconds since the system started
#define UC_TICK_FMT "%lu"

// UTF-8 as the file system's UTF-16 (a path may have any letter)
inline HostPath pathOf(const char* text) {
    int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    HostPath out(size > 1 ? size - 1 : 0, L'\0');
    if (size > 1) MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], size);
    return out;
}
inline FILE* hostOpen(const HostPath& path, const char* mode) {
    std::wstring letters(mode, mode + strlen(mode));
    return _wfopen(path.c_str(), letters.c_str());
}
inline bool hostRemove(const HostPath& path) { return DeleteFileW(path.c_str()) != 0; }
inline bool hostFileSize(const HostPath& path, unsigned long long& size) {
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return false;
    size = ((unsigned long long)info.nFileSizeHigh << 32) | (unsigned long long)info.nFileSizeLow;
    return true;
}
inline bool hostChangeDirectory(const HostPath& path) { return SetCurrentDirectoryW(path.c_str()) != 0; }
inline HostTick hostTicks() { return GetTickCount(); }
inline bool hostFinite(float value) { return _finite(value) != 0; }

// The host's own file, then a name in its folder.
inline HostPath hostExecutable() {
    std::vector<wchar_t> path(MAX_PATH);
    DWORD n;
    while ((n = GetModuleFileNameW(NULL, &path[0], (DWORD)path.size())) == path.size()) path.resize(path.size() * 2);
    return HostPath(&path[0], n);
}
inline HostPath besideHost(const char* name) {
    HostPath exe = hostExecutable();
    return exe.substr(0, exe.find_last_of(L'\\') + 1) + pathOf(name);
}
// A library, by its full path (never searched for), and one of its C exports.
inline void* hostOpenLibrary(const HostPath& path) {
    return (void*)LoadLibraryExW(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}
inline void* hostLibrarySymbol(void* library, const char* name) {
    return (void*)GetProcAddress((HMODULE)library, name);
}

#else

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/ucontext.h>               // ucontext_t (ucontext.h is the deprecated X/Open one)
#else
#include <link.h>
#include <sys/prctl.h>                  // PR_SET_PDEATHSIG, PR_SET_NO_NEW_PRIVS
#include <ucontext.h>
#endif

typedef std::string HostPath;               // UTF-8, as the file system has it here
typedef unsigned long long HostTick;        // milliseconds since the system started
#define UC_TICK_FMT "%llu"

// A path is UTF-8 all the way through (dlopen, fopen, chdir take it as it is).
inline HostPath pathOf(const char* text) { return HostPath(text); }
inline FILE* hostOpen(const HostPath& path, const char* mode) {
    struct stat info;
    if (stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) return NULL;   // as _wfopen: a folder is no file
    std::string letters;
    for (const char* p = mode; *p; p++)
        if (*p != 't') letters += *p;       // "rt": a POSIX text file needs no letter
    return fopen(path.c_str(), letters.c_str());
}
inline bool hostRemove(const HostPath& path) { return remove(path.c_str()) == 0; }
inline bool hostFileSize(const HostPath& path, unsigned long long& size) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) return false;
    size = (unsigned long long)info.st_size;
    return true;
}
inline bool hostChangeDirectory(const HostPath& path) { return chdir(path.c_str()) == 0; }
inline HostTick hostTicks() {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (HostTick)now.tv_sec * 1000 + (HostTick)(now.tv_nsec / 1000000);
}
inline bool hostFinite(float value) { return isfinite(value) != 0; }

// The host's own file, then a name in its folder.
inline HostPath hostExecutable() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);      // -1, with the room it needs
    if (!size) return HostPath();
    std::vector<char> path(size + 1, '\0');
    if (_NSGetExecutablePath(&path[0], &size) != 0) return HostPath();
    path[size] = '\0';
#else
    std::vector<char> path(1024, '\0');
    for (;;) {
        ssize_t n = readlink("/proc/self/exe", &path[0], path.size() - 1);
        if (n < 0) return HostPath();
        if ((size_t)n < path.size() - 1) { path[n] = '\0'; break; }
        path.resize(path.size() * 2);
    }
#endif
    char resolved[4096];
    if (realpath(&path[0], resolved)) return HostPath(resolved);
    return HostPath(&path[0]);
}
inline HostPath besideHost(const char* name) {
    HostPath exe = hostExecutable();
    size_t slash = exe.find_last_of('/');
    return (slash == std::string::npos ? HostPath() : exe.substr(0, slash + 1)) + name;
}
// A library, by its full path (never searched for), and one of its C exports.
inline void* hostOpenLibrary(const HostPath& path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
inline void* hostLibrarySymbol(void* library, const char* name) { return dlsym(library, name); }

// ---------------------------------------------------------------- a crash in a command
// Windows has structured exceptions: guardedCommand() catches them with __try. macOS and Linux have signals:
// a handler reports the signal and jumps back to the point that guardedCommand() set, so a crash in the engine
// fails its command and not the session. The jump is taken only on the thread that set the point and only
// while a command runs; a fault anywhere else ends the process as it always would have.
// The one thing a handler may do is plain arithmetic and write(2): no printf, no malloc, no dladdr.

struct UCHostModule { uintptr_t base; char name[96]; };
inline UCHostModule UCHostModules[128];
inline int UCHostModuleCount = 0;
inline sigjmp_buf UCHostFaultJump;
inline volatile sig_atomic_t UCHostFaultArmed = 0;
inline pthread_t UCHostFaultThread;

// The last part of a path, for a module's name.
inline void hostNameCopy(char* out, size_t size, const char* path) {
    const char* base = path ? path : "?";
    for (const char* p = base; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    size_t n = 0;
    while (base[n] && n + 1 < size) { out[n] = base[n]; n++; }
    out[n] = '\0';
}

// The images the process has, once, at startup: the handler must not ask the loader anything.
#if !defined(__APPLE__)
inline int hostModuleVisit(struct dl_phdr_info* info, size_t, void*) {
    if (UCHostModuleCount >= 128) return 1;
    uintptr_t base = (uintptr_t)info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_LOAD) { base += (uintptr_t)info->dlpi_phdr[i].p_vaddr; break; }
    HostPath exe = hostExecutable();
    UCHostModules[UCHostModuleCount].base = base;
    hostNameCopy(UCHostModules[UCHostModuleCount].name, sizeof(UCHostModules[0].name),
                 info->dlpi_name && *info->dlpi_name ? info->dlpi_name : exe.c_str());
    UCHostModuleCount++;
    return 0;
}
#endif
inline void hostModulesScan() {
    UCHostModuleCount = 0;
#if defined(__APPLE__)
    uint32_t count = _dyld_image_count();
    HostPath exe = hostExecutable();
    for (uint32_t i = 0; i < count && UCHostModuleCount < 128; i++) {
        const struct mach_header* header = _dyld_get_image_header(i);
        const char* name = _dyld_get_image_name(i);
        if (!header) continue;
        UCHostModules[UCHostModuleCount].base = (uintptr_t)header;
        hostNameCopy(UCHostModules[UCHostModuleCount].name, sizeof(UCHostModules[0].name),
                     name ? name : exe.c_str());
        UCHostModuleCount++;
    }
#else
    dl_iterate_phdr(hostModuleVisit, NULL);
#endif
}
// The image an address is in: the closest one below it (the loader's own rule).
inline bool hostModuleAt(uintptr_t address, const char*& name, uintptr_t& offset) {
    int best = -1;
    uintptr_t base = 0;
    for (int i = 0; i < UCHostModuleCount; i++) {
        uintptr_t at = UCHostModules[i].base;
        if (at <= address && (best < 0 || at >= base)) { best = i; base = at; }
    }
    if (best < 0) return false;
    name = UCHostModules[best].name;
    offset = address - base;
    return true;
}

// 0x and the hex digits, at least `digits` of them (the width of a printf %lx).
inline size_t hostHex(char* out, uintptr_t value, int digits) {
    static const char letters[] = "0123456789abcdef";
    char backwards[2 * sizeof(uintptr_t) + 2];
    int n = 0;
    if (!value) backwards[n++] = '0';
    while (value) { backwards[n++] = letters[value & 15]; value >>= 4; }
    while (n < digits) backwards[n++] = '0';
    size_t size = 0;
    out[size++] = '0';
    out[size++] = 'x';
    while (n) out[size++] = backwards[--n];
    return size;
}

inline void hostFaultHandler(int sig, siginfo_t* info, void* context) {
    const bool armed = UCHostFaultArmed && pthread_equal(pthread_self(), UCHostFaultThread);
    const uintptr_t address = (uintptr_t)(info ? info->si_addr : NULL);
    uintptr_t pc = 0;
    const char* access = "read";
    ucontext_t* machine = (ucontext_t*)context;
#if defined(__APPLE__)
#if defined(__x86_64__)
    if (machine) pc = (uintptr_t)machine->uc_mcontext->__ss.__rip;
#elif defined(__aarch64__)
    if (machine) pc = (uintptr_t)machine->uc_mcontext->__ss.__pc;
#endif
#elif defined(__linux__)
#if defined(__x86_64__)
    if (machine) {
        pc = (uintptr_t)machine->uc_mcontext.gregs[REG_RIP];
        if (sig == SIGSEGV && (machine->uc_mcontext.gregs[REG_ERR] & 0x2)) access = "write";
    }
#elif defined(__aarch64__)
    if (machine) pc = (uintptr_t)machine->uc_mcontext.pc;
#endif
#endif
    char line[256];
    size_t size = 0;
    if (sig == SIGSEGV || sig == SIGBUS) {
        // The line of the Windows handler: <module>+0x<offset>|<read|write> 0x<address>.
        const char* module = "?";
        uintptr_t offset = 0;
        hostModuleAt(pc, module, offset);
        for (const char* p = "UCMapKit|av|"; *p; p++) line[size++] = *p;
        for (const char* p = module; *p && size < sizeof(line) - 40; p++) line[size++] = *p;
        line[size++] = '+';
        size += hostHex(line + size, offset, 0);
        line[size++] = '|';
        for (const char* p = access; *p; p++) line[size++] = *p;
        line[size++] = ' ';
        size += hostHex(line + size, address, 8);
        line[size++] = '\n';
        write(STDOUT_FILENO, line, size);
        size = 0;
    }
    for (const char* p = "UCMapKit|fault|"; *p; p++) line[size++] = *p;
    size += hostHex(line + size, (uintptr_t)sig, 8);   // the signal, as the code of the exception
    line[size++] = '\n';
    if (!armed) {
        // Not inside a command: put the signal back and die the way the program would have. The av line above is
        // what the Windows reportFault() says of a fault anywhere; UCMapKit|fault comes from __except alone.
        struct sigaction plain;
        memset(&plain, 0, sizeof(plain));
        plain.sa_handler = SIG_DFL;
        sigemptyset(&plain.sa_mask);
        sigaction(sig, &plain, NULL);
        sigset_t set;                       // the signal is blocked while its handler runs: let it through
        sigemptyset(&set);
        sigaddset(&set, sig);
        sigprocmask(SIG_UNBLOCK, &set, NULL);
        raise(sig);
        _exit(1);
    }
    write(STDOUT_FILENO, line, size);
    siglongjmp(UCHostFaultJump, 1);
}

inline void hostFaultInstall() {
    hostModulesScan();
    static char* alternate = NULL;          // the handler needs a stack even after a stack overflow
    const size_t altSize = 64 * 1024;
    alternate = (char*)malloc(altSize);
    if (alternate) {
        stack_t stack;
        memset(&stack, 0, sizeof(stack));
        stack.ss_sp = alternate;
        stack.ss_size = altSize;
        stack.ss_flags = 0;
        sigaltstack(&stack, NULL);
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = hostFaultHandler;
    action.sa_flags = SA_SIGINFO | (alternate ? SA_ONSTACK : 0);
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGTRAP, &action, NULL);      // __builtin_trap, a breakpoint: a structured exception too
}

#endif /* _WIN32 */

#endif /* UC_PLATFORM_H */
