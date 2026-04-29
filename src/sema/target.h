#ifndef ORE_SEMA_TARGET_H
#define ORE_SEMA_TARGET_H

#include <stdint.h>

struct Sema;

typedef enum {
    TARGET_OS_UNKNOWN,
    TARGET_OS_MACOS,
    TARGET_OS_LINUX,
    TARGET_OS_WINDOWS,
    TARGET_OS_FREEBSD,
} TargetOS;

typedef enum {
    TARGET_ARCH_UNKNOWN,
    TARGET_ARCH_AARCH64,
    TARGET_ARCH_X86_64,
    TARGET_ARCH_X86,
    TARGET_ARCH_RISCV64,
} TargetArch;

struct TargetInfo {
    TargetOS os;
    TargetArch arch;
    uint64_t pointer_size;
    uint64_t pointer_align;
    uint64_t usize_size;
    uint64_t usize_align;
};

struct TargetInfo target_default_host(void);
const char* target_os_name(TargetOS os);
const char* target_arch_name(TargetArch arch);

#endif // ORE_SEMA_TARGET_H
