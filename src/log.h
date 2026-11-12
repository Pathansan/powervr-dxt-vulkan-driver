#pragma once

void pvr_log_init();
void pvr_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
