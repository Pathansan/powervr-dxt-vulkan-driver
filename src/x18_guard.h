#pragma once

#include <stdint.h>

// On Windows ARM64 / ARM64EC (Wine / Proton), x18 is reserved for the TEB.
// Native Android AAPCS64 libraries treat x18 as a platform scratch register.
// PvrX18Guard saves x18 on entry and restores it on exit around vendor calls.
struct PvrX18Guard {
#if defined(__aarch64__)
    uint64_t saved_x18;
    __attribute__((always_inline)) inline PvrX18Guard() {
        asm volatile("mov %0, x18" : "=r"(saved_x18));
    }
    __attribute__((always_inline)) inline ~PvrX18Guard() {
        asm volatile("mov x18, %0" :: "r"(saved_x18) : "x18");
    }
#else
    inline PvrX18Guard() {}
    inline ~PvrX18Guard() {}
#endif
};
