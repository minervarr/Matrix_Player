// The Linux half of bt_codec.hh, over BlueZ.
//
// Compiled only when audio_engine found sd-bus and libsbc (MATRIX_HAVE_BLUETOOTH,
// gated in gui/CMakeLists.txt exactly as ALSA and JACK are); otherwise the
// desktop links os/bt_codec_null.cc and the whole section disappears from the
// settings panel rather than appearing and refusing.
//
// --- what this does and does not do today -----------------------------------
//
// It REPORTS: which A2DP sink is connected, what it can decode, and what codec
// is on the wire right now. That is worth having on its own — it is what makes
// the signal chain stop pretending the last link is lossless, and what turns
// getActiveDeviceKey() into a2dp:<MAC> so an AutoEQ profile follows the
// HEADPHONES rather than the machine. The same key the Android side produces,
// so one profile covers a pair across the phone and the desktop.
//
// It does NOT yet CHOOSE the codec. Doing that means registering our own
// org.bluez.MediaEndpoint1 and owning the stream — which is the design (see
// bluez_a2dp.h for why we are not driving PipeWire instead) and is the work
// still ahead. capability() therefore answers ReadOnly, honestly, and the panel
// draws what it can rather than offering controls that would do nothing.
#include "bt_codec.hh"

#include <cstdio>
#include <mutex>

#include "backends/bluetooth/bluez_a2dp.h"

namespace {

// One bus for the process. Every call here is a blocking D-Bus round trip, so
// none of this belongs on an audio thread — and none of it is: the player calls
// bt_codec from the app thread only.
std::mutex      g_mu;
ae::bluez::Bus  g_bus;
bool            g_started = false;

// A2DP's codec ids are NOT the ids Android's BluetoothCodecConfig uses, and
// conflating them is an easy way to report AAC as aptX. On the wire: SBC=0,
// MPEG-1,2=1, MPEG-2,4 AAC=2, ATRAC=4, vendor=0xFF (LDAC and aptX both hide
// behind that, told apart by a vendor id inside the capability blob). The
// bt_codec::Codec values are Android's. This is the translation, in one place.
int codecFromA2dp(uint8_t a2dpCodecId) {
    switch (a2dpCodecId) {
    case 0x00: return bt_codec::kSbc;
    case 0x02: return bt_codec::kAac;
    // 0xFF is a vendor codec. Which one needs the four-byte vendor id and the
    // two-byte codec id out of the capability blob, and until the endpoint work
    // reads those, claiming LDAC over aptX would be a guess presented as a
    // fact. Unknown is the honest answer.
    default:   return bt_codec::kUnknown;
    }
}

// The first CONNECTED sink. BlueZ can know about many paired devices; only one
// carries audio at a time, and that is the one the chain is describing.
bool firstConnectedSink(ae::bluez::SinkDevice& out) {
    for (const ae::bluez::SinkDevice& d : g_bus.sinks()) {
        if (!d.connected) continue;
        out = d;
        return true;
    }
    return false;
}

}  // namespace

namespace bt_codec {

void start() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_started) return;
    g_started = true;
    if (!g_bus.open()) {
        // Not fatal and not worth a dialog: a machine with no Bluetooth daemon
        // is a machine with no Bluetooth section in the settings panel.
        std::printf("[Bluetooth] BlueZ unavailable: %s\n", g_bus.lastError().c_str());
        fflush(stdout);
    }
}

// ReadOnly whenever BlueZ is reachable. See the file header: changing the codec
// needs our own endpoint, and that is not built yet. Saying Writable here would
// put controls on screen that silently do nothing, which is the one thing this
// project's settings panels are careful never to do.
Capability capability() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_bus.isOpen()) return Capability::Unavailable;
    ae::bluez::SinkDevice d;
    return firstConnectedSink(d) ? Capability::ReadOnly : Capability::Unavailable;
}

Device connectedDevice() {
    std::lock_guard<std::mutex> lk(g_mu);
    Device out;
    if (!g_bus.isOpen()) return out;
    ae::bluez::SinkDevice d;
    if (!firstConnectedSink(d)) return out;
    // The MAC, in the same shape Android produces, so ONE AutoEQ profile covers
    // a pair of headphones on both machines.
    out.mac  = d.address;
    out.name = d.name.empty() ? d.address : d.name;
    return out;
}

Config activeConfig() {
    std::lock_guard<std::mutex> lk(g_mu);
    Config c;
    if (!g_bus.isOpen()) return c;
    ae::bluez::SinkDevice d;
    if (!firstConnectedSink(d)) return c;

    const ae::bluez::TransportState t = g_bus.transportFor(d.path);
    if (!t.exists) return c;   // connected, but nothing is streaming to it

    c.codec = codecFromA2dp(t.codecId);
    // Rate and depth are inside the transport's Configuration blob and are
    // codec-specific to parse. Left at zero rather than guessed: summary()
    // prints only the fields it was actually given, so the readout says "SBC"
    // rather than "SBC 44.1 kHz / 16-bit" it never read.
    c.sampleRate = 0;
    c.bits       = 0;
    return c;
}

// Unanswered, deliberately. BlueZ DOES publish what the sink can decode --
// d.codecIds, which list_bluetooth_sinks prints -- but offering it as a
// CHOICE would be offering something this backend cannot yet do: picking the
// codec means registering an endpoint per codec, and only SBC is encoded here.
// Reporting the choice before it exists is the failure this seam exists to
// avoid, so the panel is told the question went unanswered.
std::vector<CodecOption> selectableCodecs() { return {}; }

// Not yet. The panel never offers this — capability() answers ReadOnly — so
// reaching here means a saved configuration was being re-applied, and false is
// what makes the player say so instead of believing it worked.
bool apply(const std::string&, const Config&) { return false; }

std::vector<Device> pairedDevices() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<Device> out;
    if (!g_bus.isOpen()) return out;
    for (const ae::bluez::SinkDevice& d : g_bus.sinks()) {
        Device dev;
        dev.mac  = d.address;
        dev.name = d.name.empty() ? d.address : d.name;
        out.push_back(std::move(dev));
    }
    return out;
}

// Companion associations are an Android idea — the consent that lets an app
// call a privileged Bluetooth API. On Linux the equivalent question is whether
// we own the transport, which is not a permission and is not asked this way.
bool hasAssociation(const std::string&) { return false; }
void requestAssociation(const std::string&) {}

// There is no adb here, and no permission to grant.
std::string adbGrantCommand() { return {}; }

// No callbacks yet: nothing on this side watches BlueZ's signals. The player
// polls through connectedDevice() when its Audio Settings panel opens, which is
// enough for a readout and is not enough for a stream — that comes with the
// endpoint work.
void setDeviceConnectedHandler(std::function<void(const Device&)>) {}
void setAppliedHandler(std::function<void(const std::string&, bool, int)>) {}
void setDeviceGoneHandler(std::function<void(const std::string&)>) {}

}  // namespace bt_codec
