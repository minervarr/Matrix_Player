// The desktop half of media_session.hh: nothing.
//
// Windows and Linux do not freeze a process for playing audio, and neither has
// a lock-screen transport this app is expected to publish to. So the seam is
// answered honestly with no-ops rather than being #ifdef'd out at every call
// site in player_view.cc — the same treatment Host gives snapToEdge() and
// setCursor() on the platforms that genuinely lack them.
//
// A real MPRIS implementation (org.mpris.MediaPlayer2 over D-Bus) would fit
// behind this exact interface if the Linux desktop ever wants media keys. That
// is a separate piece of work and is deliberately not started here.
#include "media_session.hh"

namespace media_session {

void setCommandHandler(std::function<void(Command)>) {}
void begin() {}
void update(const NowPlaying&) {}
void end() {}

}  // namespace media_session
