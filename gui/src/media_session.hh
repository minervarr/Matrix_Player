#pragma once
// The OS's idea of "something is playing", as a seam PlayerWindow can call
// without knowing which platform it is on.
//
// On Android this is the foreground service, the MediaSession, the audio focus
// and the wake lock — the four things that stop the OS freezing the process the
// moment the listener leaves the app, and the reason playback used to die at
// the first track boundary with the screen off. On the desktops it is nothing
// at all: a Wayland or Win32 app that is playing audio is simply left alone.
//
// It is a free-function seam rather than a Host method on purpose. `Host` is
// app_shell's, and app_shell is a windowing/lifecycle library shared with apps
// that have no concept of a track — the same reasoning that keeps AudioOutput
// out of it. This is the player's own vocabulary, so it lives with the player,
// exactly as os/aoas_output.hh does.
//
// Every function here is safe to call on any platform and in any state; the
// desktop build links a file where all of them do nothing (os/media_session_null.cc).
#include <functional>
#include <string>

namespace media_session {

// What the OS can ask the player to do. Deliberately the same four verbs the
// transport bar has — there is no Pause, because this player has none: an
// interruption ends the track and the next play starts it from zero.
enum class Command { Stop, Next, Prev, Play };

struct NowPlaying {
    std::string title;
    std::string artist;
    std::string album;
    std::string artPath;    // absolute path to cover art, or empty
    std::string dspTag;     // bar B's badge: EXACT / EXACT* / ALTERED / REF EQ
    int positionMs = 0;
    int durationMs = 0;
};

// Where a transport press from OUTSIDE the app arrives: the notification, a
// headset button, a Bluetooth remote, or a loss of audio focus.
//
// Called on the platform's own thread, NOT the app thread — so an
// implementation of this handler must not touch PlayerWindow directly. Post an
// AppEvent and let the app thread answer, which is the road the AOAS ownership
// callback already travels.
void setCommandHandler(std::function<void(Command)> fn);

// Playback started. Idempotent.
void begin();

// What is playing, and how far into it. Cheap to call often — the
// implementation drops updates that would tell the OS something it already
// knows, so the player's 250 ms position tick does not become 4 IPC round
// trips a second.
void update(const NowPlaying& np);

// Playback stopped. Idempotent.
void end();

}  // namespace media_session
