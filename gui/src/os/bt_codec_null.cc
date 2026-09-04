// The desktop half of bt_codec.hh: no Bluetooth codec control.
//
// Capability::Unavailable is the honest answer here, and it is what the
// settings panel keys off — the Bluetooth section simply does not appear rather
// than appearing and refusing. Same treatment ALSA and JACK already get from
// the Audio Settings panel, which only offers backends this build actually has.
//
// Linux is where this stops being a stub. The plan's phase 4 is to register our
// own org.bluez.MediaEndpoint1 over D-Bus and encode SBC/LDAC/aptX in-process,
// which owns the codec outright instead of asking the OS to pick one — the same
// move the USB backend makes against the OS mixer. That work replaces this file
// on Linux and leaves Windows on it.
#include "bt_codec.hh"

namespace bt_codec {

void start() {}

Capability capability() { return Capability::Unavailable; }

Device connectedDevice() { return {}; }

Config activeConfig() { return {}; }

// Empty means "unanswered", not "none" — see the header. A desktop with no
// Bluetooth build has nothing to ask.
std::vector<CodecOption> selectableCodecs() { return {}; }

bool apply(const std::string&, const Config&) { return false; }

std::vector<Device> pairedDevices() { return {}; }

bool hasAssociation(const std::string&) { return false; }
void requestAssociation(const std::string&) {}

std::string adbGrantCommand() { return {}; }

void setDeviceConnectedHandler(std::function<void(const Device&)>) {}
void setAppliedHandler(std::function<void(const std::string&, bool, int)>) {}
void setDeviceGoneHandler(std::function<void(const std::string&)>) {}

}  // namespace bt_codec
