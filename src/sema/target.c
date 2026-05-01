#include "target.h"

struct TargetInfo target_default_host(void) {
    struct TargetInfo info = {
        .os = ORE_OS_UNKNOWN,
        .arch = ORE_ARCH_UNKNOWN,
        .pointer_size = 8,
        .pointer_align = 8,
        .usize_size = 8,
        .usize_align = 8,
    };

#if defined(__APPLE__)
    info.os = ORE_OS_MACOS;
#elif defined(__linux__)
    info.os = TARGET_OS_LINUX;
#elif defined(_WIN32) || defined(_WIN64)
    info.os = TARGET_OS_WINDOWS;
#elif defined(__FreeBSD__)
    info.os = TARGET_OS_FREEBSD;
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    info.arch = ORE_ARCH_AARCH64;
#elif defined(__x86_64__) || defined(_M_X64)
    info.arch = TARGET_ARCH_X86_64;
#elif defined(__i386__) || defined(_M_IX86)
    info.arch = TARGET_ARCH_X86;
    info.pointer_size = 4;
    info.pointer_align = 4;
    info.usize_size = 4;
    info.usize_align = 4;
#elif defined(__riscv) && __riscv_xlen == 64
    info.arch = TARGET_ARCH_RISCV64;
#endif

    return info;
}

const char* target_os_name(TargetOS os) {
    switch (os) {
        case ORE_OS_MACOS:   return "macos";
        case ORE_OS_LINUX:   return "linux";
        case ORE_OS_WINDOWS: return "windows";
        case ORE_OS_FREEBSD: return "freebsd";
        case ORE_OS_UNKNOWN: return "unknown";
    }
    return "unknown";
}

const char* target_arch_name(TargetArch arch) {
    switch (arch) {
        case ORE_ARCH_AARCH64: return "aarch64";
        case ORE_ARCH_X86_64:  return "x86_64";
        case ORE_ARCH_X86:     return "x86";
        case ORE_ARCH_RISCV64: return "riscv64";
        case ORE_ARCH_UNKNOWN: return "unknown";
    }
    return "unknown";
}
