// Portable entry point: env parsing, self-test dispatch, PlayerWindow
// construction/run. Platform bootstrap (DPI awareness, crash handling, log
// redirection, COM init on Windows; signal handling on Linux) lives in each
// platform's own os/windows_host.cc / os/linux_host.cc, behind a thin
// WinMain()/main() that calls matrix_player_main() below once bootstrap is
// done — see those files.
#include "player_view.hh"
#include "usb_audio.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

// Minimal iso-streaming self-test. Streams a pure generated 440 Hz sine
// straight through the engine data path (writeFloat32 -> ring -> submitTransfer)
// with NO decoder, NO GUI, NO EQ, NO resampler, NO gapless coordinator.
// If this pops, the bug is in audio_engine / libusb iso. If clean, the bug
// is in how PlayerWindow feeds the engine.
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
                if (wr > 0) off += wr; else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(80));  // let the feeder pre-fill the ring
    drv.start();
    printf("[isotest] streaming 440 Hz for 20 s -- listen for popcorn\n"); fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(20));
    run.store(false); feeder.join();
    drv.stop(); drv.close();
    printf("[isotest] done\n"); fflush(stdout);
    return 0;
}

int matrix_player_main() {
    if (getenv("MATRIX_ISO_TEST")) {
        return runIsoSelfTest();
    }

    PlayerWindow player;
    if (!player.create()) return 1;
    player.run();
    return 0;
}
