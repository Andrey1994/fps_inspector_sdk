#include <stdint.h>
#include <windows.h>
#include "timing.h"

#define FILETIME_TO_UNIX 116444736000000000i64

double getCurrentTime() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    int64_t t = ((int64_t) ft.dwHighDateTime << 32L) | (int64_t) ft.dwLowDateTime;
    return (t - FILETIME_TO_UNIX) / (10.0 * 1000.0 * 1000.0);
}
