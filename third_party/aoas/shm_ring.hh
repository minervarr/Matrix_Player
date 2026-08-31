#ifndef AOAS_SHM_RING_HH
#define AOAS_SHM_RING_HH

// ShmRing — single-producer / single-consumer byte ring living in a shared
// memory region, so the producer and the consumer are in DIFFERENT PROCESSES.
//
// The index arithmetic is lifted verbatim from ae::RingBuffer
// (framework/audio_engine/core/include/core/buffer/ring_buffer.h): wrap by
// conditional subtraction rather than `%`, one byte reserved to tell full from
// empty, acquire/release pairing. That code is already exercised by
// audio_engine's dsp_null_test and by every isochronous packet the USB driver
// submits; re-deriving it here would only invent new bugs.
//
// Three things ARE different, and all three come from the process boundary:
//
//   1. The storage is not owned. ae::RingBuffer does `new uint8_t[capacity]`,
//      which cannot be handed to another process. This is a view over memory
//      the caller already mapped (ASharedMemory_create + mmap on our side,
//      the same fd mapped on the client's side).
//
//   2. The indices are UNTRUSTED. In-process, a bad index is our own bug. Here
//      the peer is a separate app that can crash mid-write, be stopped by the
//      OS between two stores, or simply be buggy, and its writePos lands
//      straight in our memcpy bounds. Every read of a peer-written index is
//      range-checked before it is used, and a corrupt one is treated as "no
//      data" rather than trusted. AOAS must never fault or read out of bounds
//      because a client misbehaved -- it is the process that must not die.
//
//   3. capacity is NOT read from the shared header on the consuming side.
//      The peer can write to the header too, so AOAS uses the size it mapped
//      itself. The header field exists to let the client discover the geometry
//      once, at attach time.
//
// No locks, no allocation, no syscalls in read()/write(): both are called from
// threads that must not block -- ours feeds the isochronous endpoint.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aoas {

// Cache line on arm64 (and x86_64). readPos and writePos are written by
// different processes on different cores; sharing a line between them would
// bounce it back and forth on every packet.
inline constexpr size_t kCacheLine = 64;

inline constexpr uint32_t kShmRingMagic   = 0x53414F41;  // 'AOAS' little-endian
inline constexpr uint32_t kShmRingVersion = 1;

// Layout of the mapped region: this header, then `capacity` bytes of PCM.
// Both processes compile this from the same source and both are 64-bit
// (the app ships arm64-v8a and x86_64 only), so the layout matches.
struct alignas(kCacheLine) ShmRingHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t capacity;    // bytes of data region following this header
    uint32_t frameBytes;  // channels * subslotSize; reads/writes stay aligned

    // Consumer (AOAS) owns readPos; producer (client) owns writePos. Each sits
    // on its own line -- see kCacheLine above.
    alignas(kCacheLine) std::atomic<uint32_t> readPos;
    alignas(kCacheLine) std::atomic<uint32_t> writePos;
};

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "a lock-based atomic here would be a futex syscall on the "
              "isochronous path, and a lock held by a dead client process "
              "would strand the server");

// Total bytes to allocate for a ring holding `capacity` bytes of audio.
inline size_t shmRingRegionBytes(size_t capacity) {
    return sizeof(ShmRingHeader) + capacity;
}

class ShmRing {
public:
    ShmRing() = default;

    // Producer side: stamp a freshly mapped region. Call once, before the fd
    // is shared, and never while the peer is attached.
    static ShmRing create(void* base, size_t regionBytes, uint32_t frameBytes) {
        if (!base || regionBytes <= sizeof(ShmRingHeader)) return ShmRing{};
        auto* h = static_cast<ShmRingHeader*>(base);
        h->magic      = kShmRingMagic;
        h->version    = kShmRingVersion;
        h->capacity   = static_cast<uint32_t>(regionBytes - sizeof(ShmRingHeader));
        h->frameBytes = frameBytes;
        h->readPos.store(0, std::memory_order_relaxed);
        h->writePos.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        return ShmRing(h, h->capacity);
    }

    // Consumer side: adopt a region the peer created. `regionBytes` is OUR
    // mapping length, not anything the peer told us -- that is the whole point
    // (see note 3 above). Returns an invalid ring if the header does not check
    // out, which is the honest answer for "the client sent us garbage".
    static ShmRing attach(void* base, size_t regionBytes) {
        if (!base || regionBytes <= sizeof(ShmRingHeader)) return ShmRing{};
        auto* h = static_cast<ShmRingHeader*>(base);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (h->magic != kShmRingMagic || h->version != kShmRingVersion) return ShmRing{};
        return ShmRing(h, regionBytes - sizeof(ShmRingHeader));
    }

    bool valid() const { return h_ != nullptr; }
    size_t capacity() const { return capacity_; }
    uint32_t frameBytes() const { return h_ ? h_->frameBytes : 0; }

    // Consumer. Returns bytes actually read (0 when empty or when the peer's
    // writePos is out of range).
    size_t read(uint8_t* out, size_t len) {
        if (!h_ || !out) return 0;
        const size_t w = loadPeer(h_->writePos);
        if (w == kBadPos) return 0;                 // corrupt peer index: no data
        const size_t r = h_->readPos.load(std::memory_order_relaxed);
        const size_t toRead = min(len, distance(w, r));

        const size_t firstPart = min(toRead, capacity_ - r);
        memcpy(out, data() + r, firstPart);
        if (toRead > firstPart) memcpy(out + firstPart, data(), toRead - firstPart);

        h_->readPos.store(static_cast<uint32_t>(advance(r, toRead)),
                          std::memory_order_release);
        return toRead;
    }

    // Producer. Returns bytes accepted; short writes mean the ring is full and
    // the caller must retry rather than drop -- dropping would be a gap in the
    // audio, which is exactly what this whole project exists to prevent.
    size_t write(const uint8_t* in, size_t len) {
        if (!h_ || !in) return 0;
        const size_t r = loadPeer(h_->readPos);
        if (r == kBadPos) return 0;
        const size_t w = h_->writePos.load(std::memory_order_relaxed);
        const size_t toWrite = min(len, freeSpace(r, w));

        const size_t firstPart = min(toWrite, capacity_ - w);
        memcpy(data() + w, in, firstPart);
        if (toWrite > firstPart) memcpy(data(), in + firstPart, toWrite - firstPart);

        h_->writePos.store(static_cast<uint32_t>(advance(w, toWrite)),
                           std::memory_order_release);
        return toWrite;
    }

    // Occupancy in bytes. Zero if the peer index is corrupt.
    size_t available() const {
        if (!h_) return 0;
        const size_t w = loadPeer(h_->writePos);
        if (w == kBadPos) return 0;
        return distance(w, h_->readPos.load(std::memory_order_acquire));
    }

    // Conservative lower bound on free space (the consumer may free more at
    // any moment). Zero if the peer index is corrupt.
    size_t freeSpace() const {
        if (!h_) return 0;
        const size_t r = loadPeer(h_->readPos);
        if (r == kBadPos) return 0;
        return freeSpace(r, h_->writePos.load(std::memory_order_relaxed));
    }

    // Drop everything buffered. Only safe when the peer is known not to be
    // running -- AOAS calls it on ownership handover, after the outgoing owner
    // has confirmed its release.
    void clear() {
        if (!h_) return;
        h_->readPos.store(0, std::memory_order_relaxed);
        h_->writePos.store(0, std::memory_order_relaxed);
    }

private:
    ShmRing(ShmRingHeader* h, size_t capacity) : h_(h), capacity_(capacity) {}

    static constexpr size_t kBadPos = static_cast<size_t>(-1);

    // Every index the OTHER process wrote passes through here. An index at or
    // past capacity would walk our memcpy off the end of the mapping.
    size_t loadPeer(const std::atomic<uint32_t>& pos) const {
        const size_t v = pos.load(std::memory_order_acquire);
        return v < capacity_ ? v : kBadPos;
    }

    uint8_t* data() const { return reinterpret_cast<uint8_t*>(h_ + 1); }

    static size_t min(size_t a, size_t b) { return a < b ? a : b; }

    // --- index arithmetic, from ae::RingBuffer (see file header) -------------
    size_t advance(size_t pos, size_t delta) const {
        const size_t next = pos + delta;
        return next >= capacity_ ? next - capacity_ : next;
    }
    size_t distance(size_t ahead, size_t behind) const {
        return ahead >= behind ? ahead - behind : ahead + capacity_ - behind;
    }
    size_t freeSpace(size_t r, size_t w) const {
        return r > w ? r - w - 1 : r + capacity_ - w - 1;
    }

    ShmRingHeader* h_ = nullptr;
    size_t capacity_  = 0;
};

}  // namespace aoas

#endif  // AOAS_SHM_RING_HH
