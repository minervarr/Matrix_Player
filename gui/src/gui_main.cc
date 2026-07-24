#include <windows.h>
#include <mmsystem.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <thread>
#include <atomic>
#include <vector>
#include "player_view.hh"
#include "usb_audio.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dbghelp.lib")

// windows.h only declares these behind a high enough _WIN32_WINNT/NTDDI_VERSION,
// which isn't explicitly set anywhere in this project's CMakeLists — define
// them ourselves if missing so this compiles regardless of default SDK target.
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
typedef HANDLE DPI_AWARENESS_CONTEXT;
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

// Without this, a non-DPI-aware process gets its whole window bitmap-stretched
// by DWM on any scaled display (125%/150%/200% — the default on most Windows
// laptops), blurring otherwise-crisp MSDF text. Resolved dynamically (rather
// than via manifest) so it works regardless of the CMake/Ninja build not
// embedding one.
static void enableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setCtx = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setCtx && setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();
}

// Minimal iso-streaming self-test. Streams a pure generated 440 Hz sine
// straight through the engine data path (writeFloat32 -> ring -> submitTransfer)
// with NO decoder, NO GUI, NO EQ, NO resampler, NO gapless coordinator.
// If this pops, the bug is in audio_engine / libusb iso on Windows. If clean,
// the bug is in how PlayerWindow feeds the engine.
// Trigger by setting env var MATRIX_ISO_TEST=1 before launching.
static int runIsoSelfTest() {
    printf("[isotest] starting minimal iso streaming self-test\n"); fflush(stdout);
    UsbAudioDriver drv;
    if (!drv.open(0x32BB, 0x0004)) { printf("[isotest] open failed\n"); return 1; }
    drv.parseDescriptors();
    const int rate = 48000, ch = 2;
    if (!drv.configure(rate, ch, 32)) { printf("[isotest] configure failed\n"); return 1; }

    std::atomic<bool> run{true};
    std::thread feeder([&]{
        const double w = 2.0 * 3.14159265358979323846 * 440.0 / rate;
        double phase = 0.0;
        const int FR = 256;
        std::vector<float> buf(FR * ch);
        while (run.load()) {
            for (int i = 0; i < FR; i++) {
                float s = 0.2f * (float)sin(phase);
                phase += w; if (phase > 2.0*3.14159265358979323846) phase -= 2.0*3.14159265358979323846;
                buf[i*ch] = s; buf[i*ch+1] = s;
            }
            int total = FR * ch, off = 0;
            while (off < total && run.load()) {
                int wr = drv.writeFloat32(buf.data() + off, total - off);
                if (wr > 0) off += wr; else Sleep(1);
            }
        }
    });

    Sleep(80);          // let the feeder pre-fill the ring
    drv.start();
    printf("[isotest] streaming 440 Hz for 20 s -- listen for popcorn\n"); fflush(stdout);
    Sleep(20000);
    run.store(false); feeder.join();
    drv.stop(); drv.close();
    printf("[isotest] done\n"); fflush(stdout);
    return 0;
}

static void openLogFile() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring logPath = exePath;
    logPath = logPath.substr(0, logPath.rfind(L'\\') + 1) + L"matrix_player.log";
    FILE* outFp = nullptr; _wfreopen_s(&outFp, logPath.c_str(), L"w", stdout);
    FILE* errFp = nullptr; _wfreopen_s(&errFp, logPath.c_str(), L"a", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

// Previously, an unhandled exception just killed the process with nothing
// recorded anywhere — matrix_player.log would simply stop mid-stream, no
// exception code, no clue which thread or address. This writes a minidump
// (loadable in WinDbg/Visual Studio for a full multi-thread stack trace)
// next to the log, and logs the exception code/address as the log's last
// line, before letting the crash proceed exactly as it did before.
static LONG WINAPI crashHandler(EXCEPTION_POINTERS* info) {
    printf("[Crash][ERROR] Unhandled exception 0x%08X at address %p\n",
           (unsigned)info->ExceptionRecord->ExceptionCode,
           info->ExceptionRecord->ExceptionAddress);
    fflush(stdout);

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dumpPath = exePath;
    dumpPath = dumpPath.substr(0, dumpPath.rfind(L'\\') + 1) + L"matrix_player_crash.dmp";

    HANDLE hFile = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                           MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(hFile);
        printf("[Crash][ERROR] Minidump written to %ls\n", dumpPath.c_str());
        fflush(stdout);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    enableDpiAwareness();
    openLogFile();
    SetUnhandledExceptionFilter(crashHandler);
    // Raise system timer resolution to 1 ms for the app lifetime so any
    // Sleep()/WaitForSingleObject() in audio paths (notably the pre-buffer
    // wait in PlayerWindow::onPlay) doesn't get rounded to ~15.6 ms.
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);


    char* isoTestEnv = nullptr; size_t isoTestLen = 0;
    _dupenv_s(&isoTestEnv, &isoTestLen, "MATRIX_ISO_TEST");
    bool isoTestRequested = isoTestEnv != nullptr;
    free(isoTestEnv);
    if (isoTestRequested) {
        int rc = runIsoSelfTest();
        CoUninitialize();
        timeEndPeriod(1);
        return rc;
    }

    PlayerWindow player;
    if (!player.create(hInst)) { timeEndPeriod(1); return 1; }
    player.run();

    CoUninitialize();
    timeEndPeriod(1);
    return 0;
}
