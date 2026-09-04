#pragma once
// The Bluetooth A2DP codec, as a seam the player can call on any platform.
//
// This is NOT part of the audio path and has nothing to do with AAudio, ALSA or
// any AudioOutput. The player hands PCM to the OS; the OS's Bluetooth stack
// encodes it to SBC / AAC / aptX / LDAC before it ever reaches the headphones.
// The codec is a property of the ROUTE, chosen once and remembered per device —
// which is why it lives beside the per-device AutoEQ assignment rather than
// beside a backend.
//
// It matters twice over:
//
//  1. The signal-chain readout exists to say what the chain ACTUALLY achieves.
//     Over Bluetooth the lossy encode is by far the largest thing happening to
//     the audio, and until now the readout said nothing about it at all.
//  2. getActiveDeviceKey() returns the bare string "aaudio" on the phone,
//     because AAudio moves the route under the app and never says where to.
//     Knowing the connected sink turns that into a real key, so an AutoEQ
//     profile follows the HEADPHONES instead of the handset.
//
// Android implements it (os/bt_codec_android.cc); the desktops answer with
// no-ops for now (os/bt_codec_null.cc). Linux is where this gets a real second
// implementation, by owning the A2DP endpoint outright rather than asking the
// OS to pick a codec — see the plan's phase 4.
#include <functional>
#include <string>
#include <vector>

namespace bt_codec {

// What this platform, on this device, will actually let the app do. Probed,
// never assumed: whether the hidden methods exist is a per-ROM question, and
// whether they may be called is a per-device one.
enum class Capability {
    Unavailable,   // no Bluetooth, or no codec control of any kind
    ReadOnly,      // the active codec can be REPORTED but not changed
    Writable,      // it can be set (a companion association, or adb)
};

// AOSP's own codec numbering — these values cross into BluetoothCodecConfig, so
// they are the platform's, not ours.
enum Codec { kSbc = 0, kAac = 1, kAptX = 2, kAptXHd = 3, kLdac = 4, kUnknown = -1 };

// Sample rate and depth travel as AOSP's BITMASKS rather than plain numbers,
// because that is what BluetoothCodecConfig takes and converting in two places
// would be two places to get it wrong.
enum SampleRate { kRate44100 = 0x1, kRate48000 = 0x2, kRate88200 = 0x4, kRate96000 = 0x8 };
enum BitDepth   { kBits16 = 0x1, kBits24 = 0x2, kBits32 = 0x4 };
enum ChannelMode { kStereo = 0x2 };
// LDAC's quality rides in codecSpecific1 (offset by 1000 on the wire, handled
// on the Java side). 0 is the best, which is not the order anyone expects.
enum LdacQuality { kLdac990 = 0, kLdac660 = 1, kLdac330 = 2, kLdacAdaptive = 3 };

struct Config {
    int codec       = kUnknown;
    int sampleRate  = kRate44100;   // a SampleRate bit
    int bits        = kBits16;      // a BitDepth bit
    int channelMode = kStereo;
    int ldacQuality = kLdac990;

    bool operator==(const Config& o) const {
        return codec == o.codec && sampleRate == o.sampleRate && bits == o.bits &&
               channelMode == o.channelMode && ldacQuality == o.ldacQuality;
    }
    bool operator!=(const Config& o) const { return !(*this == o); }
    bool valid() const { return codec != kUnknown; }
};

struct Device {
    std::string mac;    // empty when nothing is connected
    std::string name;   // falls back to the MAC when the name is unreadable
    bool empty() const { return mac.empty(); }
};

// Human labels, shared by the settings panel and the signal chain so the two
// cannot disagree. Pure functions — no platform behind them.
std::string codecName(int codec);
std::string sampleRateLabel(int mask);
std::string bitDepthLabel(int mask);
std::string ldacQualityLabel(int quality);
// One line: "LDAC 96 kHz / 24-bit / 990 kbps".
std::string summary(const Config& c);

// --- the platform ----------------------------------------------------------

// Start listening for connections. Idempotent; safe with no Bluetooth at all.
void start();

Capability capability();

// The A2DP sink in use right now, or an empty Device.
Device connectedDevice();

// What the stack is really running. An invalid Config when it cannot be read.
Config activeConfig();

// One codec this device can actually take, as the platform names it.
struct CodecOption {
    int         id = kUnknown;   // the platform's own codec id
    std::string name;            // the platform's own name for it
};

// Which codecs THIS pair of headphones can take, asked of the stack rather than
// listed here.
//
// The list used to be a fixed five — SBC, AAC, aptX, aptX HD, LDAC — and that
// was wrong in both directions on real hardware: it offered LDAC and aptX to a
// speaker that only speaks SBC and AAC, and it could never name LC3, Opus,
// aptX Adaptive or any vendor codec, because they were not in the enum. Both
// failures have one cause: the app was answering a question only the device
// can answer.
//
// The NAME comes with the id, for the same reason — a table here could only
// name what it already knew.
//
// An EMPTY vector means the question could not be answered (no permission, no
// hidden API, nothing connected). It does NOT mean "this device supports
// nothing", and a caller that draws it as an empty list is saying something
// false; say the question went unanswered instead.
std::vector<CodecOption> selectableCodecs();

// Ask for this configuration on this device. Returns false when the request
// could not even be sent. Whether it was HONOURED is a separate question,
// answered later through the applied-callback below, because the stack can
// accept a request and then negotiate something else with the headphones.
bool apply(const std::string& mac, const Config& c);

// Everything paired, for the settings list.
std::vector<Device> pairedDevices();

// Companion association: the consent that makes apply() legal for one device.
bool hasAssociation(const std::string& mac);
void requestAssociation(const std::string& mac);

// The adb line to put on screen when that is the only path left.
std::string adbGrantCommand();

// --- callbacks -------------------------------------------------------------
//
// Both fire on the PLATFORM's thread, not the app thread. An implementation of
// these must post rather than touch the player directly — the same rule
// media_session.hh states for its command handler.

// A sink connected and the stack has had time to settle. The player answers by
// applying whatever it has saved for that MAC.
void setDeviceConnectedHandler(std::function<void(const Device&)> fn);

// The verdict on an apply(): did the codec really change. `actualCodec` is what
// is running now, which is worth reporting even when it is not what was asked.
void setAppliedHandler(std::function<void(const std::string& mac, bool ok, int actualCodec)> fn);

// A sink went away.
void setDeviceGoneHandler(std::function<void(const std::string& mac)> fn);

}  // namespace bt_codec
