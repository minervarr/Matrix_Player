# aoas/

`shm_ring.hh` — **verbatim copy** of
`AndroidOneAudioServer_AOAS/native/shm_ring.hh` (the Android One Audio
Server's shared-memory ring contract). Matrix Player is a *client* of AOAS on
Android (`gui/src/os/aoas_output.cc`): it maps the ring AOAS creates and writes
wire-format PCM into it.

Keep byte-identical to the original. The producer side owns `writePos` only —
`ShmRing::clear()` and `read()` are the consumer's (AOAS's) and are never
called from this repo. See
`docs/superpowers/specs/2026-08-28-aoas-client-backend.md`.
