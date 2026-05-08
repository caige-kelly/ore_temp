#ifndef ORE_SEMA_TARGET_H
#define ORE_SEMA_TARGET_H

#include <stdint.h>

struct Sema;

typedef enum {
    ORE_OS_UNKNOWN,
    ORE_OS_MACOS,
    ORE_OS_LINUX,
    ORE_OS_WINDOWS,
    ORE_OS_FREEBSD,
} TargetOS;

typedef enum {
    ORE_ARCH_UNKNOWN,
    ORE_ARCH_AARCH64,
    ORE_ARCH_X86_64,
    ORE_ARCH_X86,
    ORE_ARCH_RISCV64,
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
