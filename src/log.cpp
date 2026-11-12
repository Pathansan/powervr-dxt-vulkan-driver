#include "log.h"
#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <mutex>

static std::mutex g_log_mutex;
static FILE* g_log_file = nullptr;
static bool g_log_checked = false;

void pvr_log_init() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_checked) return;
    g_log_checked = true;

    const char* path = getenv("PVR_ICD_LOG_FILE");
    if (!path || !path[0]) {
        path = "/sdcard/Download/pvr_icd.log";
    }
    g_log_file = fopen(path, "a");
    if (g_log_file) {
        fprintf(g_log_file, "=== PowerVR DXT Compat ICD initialized ===\n");
        fflush(g_log_file);
    }
}

void pvr_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "PVR_ICD", fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log_checked) {
        g_log_checked = true;
        const char* path = getenv("PVR_ICD_LOG_FILE");
        if (!path || !path[0]) path = "/sdcard/Download/pvr_icd.log";
        g_log_file = fopen(path, "a");
    }
    if (g_log_file) {
        va_start(args, fmt);
        vfprintf(g_log_file, fmt, args);
        va_end(args);
        fputc('\n', g_log_file);
        fflush(g_log_file);
    }
}
