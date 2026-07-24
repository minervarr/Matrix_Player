#pragma once
#include <windows.h>
#include <cstdio>

inline const char* logTs() {
    static char buf[16];
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}
