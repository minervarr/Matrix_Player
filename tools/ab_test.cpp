// Blind A/B test: Bitperfect vs Reference EQ
//
// Usage: matrix_ab_test.exe <path-to-flac>
//
// Each round: 5-second clip loops in a randomly-chosen mode.
// You don't know which mode you're in.
//   SPACE = toggle (restarts clip from zero so comparison is clean)
//   A     = "what I hear right now IS bitperfect"
//   B     = "what I hear right now IS ref eq"
// After MIN_ROUNDS rounds, press E to exit and save.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#include "usb_audio.h"
#include "eq_processor.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

static constexpr int LOOP_SECONDS = 5;
static constexpr int MIN_ROUNDS   = 10;

// ---------- console (writes directly to Win32 handle, bypasses stdout) ----------

static HANDLE g_con = INVALID_HANDLE_VALUE;

static void con(const char* s) {
    DWORD n;
    WriteConsoleA(g_con, s, (DWORD)strlen(s), &n, nullptr);
}

static void conf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    con(buf);
}

static void clearLine() { con("\r                                                                \r"); }

// ---------- data ----------

struct Round {
    int         num;
    std::string timestamp;
    std::string actual;   // mode that was playing when user pressed A or B
    std::string declared; // "bitperfect" or "refeq" (what user thinks it is)
    bool        correct;
};

static std::vector<Round> g_rounds;

static std::string nowIso() {
    SYSTEMTIME st; GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

static void writeCSV(const char* path) {
    std::ofstream f(path);
    f << "round,timestamp,actual_mode,declared_mode,correct\n";
    for (auto& r : g_rounds)
        f << r.num << "," << r.timestamp << ","
          << r.actual << "," << r.declared << ","
          << (r.correct ? "yes" : "no") << "\n";
}

// ---------- main ----------

int main(int argc, char* argv[]) {
    g_con = GetStdHandle(STD_OUTPUT_HANDLE);

    // Redirect stdout so audio-engine [INFO] spam goes to a log file, not the console
    freopen("ab_test_engine.log", "w", stdout);

    if (argc < 2) {
        con("Usage: matrix_ab_test.exe <path-to-flac>\n"); return 1;
    }

    drflac* flac = drflac_open_file(argv[1], nullptr);
    if (!flac) { conf("Cannot open: %s\n", argv[1]); return 1; }

    const int sampleRate = (int)flac->sampleRate;
    const int channels   = (int)flac->channels;

    const uint64_t loopFrames = (uint64_t)sampleRate * LOOP_SECONDS;
    std::vector<int32_t> loopBuf(loopFrames * channels);
    uint64_t got = drflac_read_pcm_frames_s32(flac, loopFrames, loopBuf.data());
    loopBuf.resize(got * channels);
    drflac_close(flac);

    if (got == 0) { con("No audio frames read.\n"); return 1; }

    // EQ: flat (0 filters, 0 dB) -- both modes sound identical until a real profile is wired in
    EqProcessor eq;
    eq.configure(0, nullptr, 1.0, channels, 22 /* PCM_32BIT */);

    UsbAudioDriver driver;
    if (!driver.open(0x32BB, 0x0004)) { con("Cannot open USB DAC\n"); return 1; }
    driver.parseDescriptors();
    if (!driver.configure(sampleRate, channels, 32)) {
        con("DAC rate not supported\n"); driver.close(); return 1;
    }
    driver.start();

    srand((unsigned)GetTickCount());

    std::atomic<bool> running  { true  };
    std::atomic<bool> useEq    { false };
    std::atomic<bool> resetLoop{ false };

    std::thread decodeThread([&] {
        static const int CHUNK = 2048;
        std::vector<int32_t> tmp(CHUNK * channels);
        size_t pos = 0;
        const size_t total = loopBuf.size();

        while (running.load(std::memory_order_relaxed)) {
            if (resetLoop.exchange(false, std::memory_order_acq_rel)) {
                driver.flush();
                eq.reset();
                pos = 0;
            }

            size_t n = std::min((size_t)(CHUNK * channels), total - pos);
            if (n == 0) { pos = 0; continue; }

            memcpy(tmp.data(), loopBuf.data() + pos, n * 4);
            if (useEq.load(std::memory_order_acquire))
                eq.process(reinterpret_cast<uint8_t*>(tmp.data()), (int)(n * 4));

            int32_t* ptr = tmp.data();
            int left = (int)n;
            while (left > 0 && running.load(std::memory_order_relaxed)) {
                int w = driver.writeInt32(ptr, left);
                if (w > 0) { ptr += w; left -= w; } else Sleep(1);
            }
            pos += n;
            if (pos >= total) pos = 0;
        }
    });

    // ---- UI ----
    conf("\n  %s\n  %d Hz / %d ch   loop: %llds\n\n",
         argv[1], sampleRate, channels, (long long)LOOP_SECONDS);
    con("  A = bitperfect   B = ref eq\n");
    conf("  SPACE=switch(restarts)   A=declare bitperfect   B=declare ref eq   E=exit (after %d rounds)\n\n",
         MIN_ROUNDS);

    bool canExit = false;

    for (;;) {
        int roundNum = (int)g_rounds.size() + 1;
        bool startEq = (rand() % 2) == 0;
        useEq.store(startEq, std::memory_order_release);
        resetLoop.store(true, std::memory_order_release);

        bool currentEq = startEq;  // tracks what's playing right now

        conf("  Round %d\n", roundNum);
        clearLine(); con("  [?]   SPACE=switch  A=bitperfect  B=ref eq");

        for (;;) {
            if (GetAsyncKeyState(VK_SPACE) & 0x0001) {
                currentEq = !currentEq;
                useEq.store(currentEq, std::memory_order_release);
                resetLoop.store(true, std::memory_order_release);
                clearLine(); con("  [?]   SPACE=switch  A=bitperfect  B=ref eq");
            }

            auto pick = [&](const char* declared, bool declaredIsEq) {
                bool correct = (currentEq == declaredIsEq);
                g_rounds.push_back({ roundNum, nowIso(),
                    currentEq ? "refeq" : "bitperfect",
                    declared, correct });
                conf("\n  -> %s  (%s)\n\n", declared, correct ? "correct" : "wrong");
            };

            if (GetAsyncKeyState('A') & 0x0001) { pick("bitperfect", false); break; }
            if (GetAsyncKeyState('B') & 0x0001) { pick("refeq",      true ); break; }
            if (canExit && (GetAsyncKeyState('E') & 0x0001)) { running.store(false); goto done; }

            Sleep(20);
        }

        if ((int)g_rounds.size() >= MIN_ROUNDS) {
            canExit = true;
            conf("  %d rounds done -- keep going or press E to exit\n\n", (int)g_rounds.size());
        }
    }

done:
    decodeThread.join();
    driver.stop();
    driver.close();

    int correct = 0;
    int bpPref  = 0;
    int eqPref  = 0;
    for (auto& r : g_rounds) {
        if (r.correct) correct++;
        if (r.declared == "bitperfect") bpPref++; else eqPref++;
    }
    int total = (int)g_rounds.size();

    con("\n  ---\n");
    conf("  Rounds: %d   Correct: %d / %d (%.0f%%)\n",
         total, correct, total, total ? 100.0*correct/total : 0.0);
    conf("  Declared bitperfect: %d   Declared ref eq: %d\n", bpPref, eqPref);
    con("  (flat EQ build: both modes are identical -- ~50% correct is expected)\n\n");

    writeCSV("ab_test_log.csv");
    con("  Saved: ab_test_log.csv   engine log: ab_test_engine.log\n\n");
    return 0;
}
