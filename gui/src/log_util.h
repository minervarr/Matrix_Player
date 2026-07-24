#pragma once
#include <cstdio>
#include <chrono>
#include <ctime>

inline const char* logTs() {
    static char buf[16];
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tmVal{};
#ifdef _WIN32
    localtime_s(&tmVal, &t);
#else
    localtime_r(&t, &tmVal);
#endif
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
             tmVal.tm_hour, tmVal.tm_min, tmVal.tm_sec, (int)ms.count());
    return buf;
}
