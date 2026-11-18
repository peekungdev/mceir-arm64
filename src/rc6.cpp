// rc6.cpp - RC-6 mode 6A decoder, which is what Media Center remotes speak.
//
// An RC-6 frame is:
//
//   leader        6 units of carrier, then 2 units of silence
//   header        4 bi-phase bits: a start bit (always 1) then 3 mode bits
//   trailer       1 bi-phase bit at double width
//   body          16 or 32 bi-phase bits
//   signal free   at least 6 units of silence
//
// Bi-phase here is the RC-5 encoding inverted: a 1 is carrier then silence, a
// 0 is silence then carrier. Rather than run a transition-driven state machine
// over the sample stream, each sample is expanded into however many half-bit
// slots its duration covers. The frame then decodes positionally, which makes
// the double-width trailer fall out for free -- it simply occupies two slots
// per level instead of one.
//
// Mode 6A is variable length -- 8, 16, 20, 24 or 32 body bits -- and the
// length is carried by how much the transmitter actually sent, not by any
// field in the frame. Microsoft's remotes send 32 bits with a fixed 0x800F
// customer code, and put their own toggle in bit 15 of the body rather than
// using the trailer bit for it. The trailer bit is not a length selector:
// this remote leaves it clear on a 32-bit frame.

#include "mceir.h"


namespace {

constexpr uint32_t kUnitUs = 444;  // 16 cycles of the 36 kHz carrier

constexpr size_t kLeaderPulseSlots = 6;
constexpr size_t kLeaderSpaceSlots = 2;
constexpr size_t kHeaderBits       = 4;
constexpr size_t kTrailerSlots     = 4;  // one bit at double width

constexpr uint8_t kStartBitMask = 0x08;
constexpr uint8_t kModeMask     = 0x07;
constexpr uint8_t kMode6A       = 6;

constexpr uint32_t kMceCustomerCode = 0x800F0000;
constexpr uint32_t kMceCustomerMask = 0xFFFF0000;
constexpr uint32_t kMceToggleMask   = 0x00008000;

// A frame never contains a silence longer than three units, so anything at or
// above this is the trailer and ends the frame.
constexpr size_t kFrameEndSpaceSlots = 5;

// How much of that trailing silence to keep. The second half of a final 1 bit
// merges into it, so one slot is genuinely part of the frame; two is slack.
constexpr size_t kTrailerKeepSlots = 2;

// Enough for the longest legal frame with the trailer attached, and a cheap
// guard against a noisy stream growing the buffer without bound.
constexpr size_t kMaxSlots = 128;

// A uint32_t scancode is the ceiling, and 32 is also the longest mode 6A body.
constexpr size_t kMaxBodyBits = 32;

size_t SlotsFor(uint32_t duration_us) {
    return (duration_us + kUnitUs / 2) / kUnitUs;
}

// Mode 6A bodies come in these lengths. Anything else means the frame was
// clipped or corrupted partway through, so the bits read so far are not
// trustworthy.
bool IsValidBodyLength(size_t bits) {
    return bits == 8 || bits == 16 || bits == 20 || bits == 24 || bits == 32;
}

}  // namespace

void Rc6Decoder::Reset() {
    m_slots.clear();
    m_inFrame = false;
}

void Rc6Decoder::Feed(const IrSample& s) {
    const size_t n = SlotsFor(s.duration_us);

    if (!m_inFrame) {
        // Only a leader pulse starts a frame; everything else is noise or the
        // gap between frames.
        if (s.pulse && n == kLeaderPulseSlots) {
            m_inFrame = true;
            m_slots.assign(kLeaderPulseSlots, true);
        }
        return;
    }

    if (n == 0) {
        // Shorter than half a unit: not a real symbol.
        Reset();
        return;
    }

    const bool endOfFrame = !s.pulse && n >= kFrameEndSpaceSlots;

    // The trailing silence merges with the second half of the last body bit,
    // so a few of its slots still belong to the frame. Decode() reads only as
    // many as the frame needs and ignores the rest.
    const size_t append = endOfFrame ? kTrailerKeepSlots : n;

    if (m_slots.size() + append > kMaxSlots) {
        LOGV("RC6: frame too long, discarding.");
        Reset();
        return;
    }

    m_slots.insert(m_slots.end(), append, s.pulse);

    if (endOfFrame) {
        Decode();
        Reset();
    }
}

void Rc6Decoder::Decode() {
    size_t idx = kLeaderPulseSlots;

    auto have = [&](size_t count) { return idx + count <= m_slots.size(); };

    // Leader silence.
    if (!have(kLeaderSpaceSlots)) {
        LOGV("RC6: frame ended inside the leader.");
        return;
    }
    for (size_t i = 0; i < kLeaderSpaceSlots; ++i) {
        if (m_slots[idx + i]) {
            LOGV("RC6: leader silence is not silent.");
            return;
        }
    }
    idx += kLeaderSpaceSlots;

    // A bi-phase bit: two slots that must differ, valued by the first one.
    auto readBit = [&](bool& bit) -> bool {
        if (!have(2))
            return false;
        const bool first = m_slots[idx], second = m_slots[idx + 1];
        if (first == second)
            return false;  // Manchester violation
        bit = first;
        idx += 2;
        return true;
    };

    uint8_t header = 0;
    for (size_t i = 0; i < kHeaderBits; ++i) {
        bool bit = false;
        if (!readBit(bit)) {
            LOGV("RC6: bad header bit %zu.", i);
            return;
        }
        header = static_cast<uint8_t>((header << 1) | (bit ? 1 : 0));
    }

    if (!(header & kStartBitMask)) {
        LOGV("RC6: missing start bit (header 0x%X).", header);
        return;
    }

    const uint8_t mode = header & kModeMask;
    if (mode != kMode6A) {
        LOGV("RC6: mode %u is not 6A; ignoring.", mode);
        return;
    }

    // The trailer is one bit stretched over two slots per level.
    if (!have(kTrailerSlots)) {
        LOGV("RC6: frame ended inside the trailer.");
        return;
    }
    const bool tFirst = m_slots[idx], tSecond = m_slots[idx + 2];
    if (m_slots[idx] != m_slots[idx + 1] || m_slots[idx + 2] != m_slots[idx + 3] ||
        tFirst == tSecond) {
        LOGV("RC6: malformed trailer bit.");
        return;
    }
    idx += kTrailerSlots;

    // Mode 6A is variable length, and the length is carried by the frame
    // itself -- how many bits were actually sent -- not by any field inside
    // it. So read until the slots run out. The two slack slots appended from
    // the trailing silence stop the loop on their own: a pair of matching
    // levels is not a valid bi-phase bit.
    //
    // The trailer bit is NOT a length selector. This remote sends a 32-bit
    // body with the trailer clear, and reading it as a selector truncated
    // every frame at the 16-bit customer code.
    uint32_t body  = 0;
    size_t   count = 0;
    while (count < kMaxBodyBits) {
        bool bit = false;
        if (!readBit(bit))
            break;
        body = (body << 1) | (bit ? 1u : 0u);
        ++count;
    }

    if (!IsValidBodyLength(count)) {
        LOGV("RC6: body was %zu bits, which is not a mode 6A length.", count);
        return;
    }

    Rc6Result result{ body, false };

    // Microsoft's remotes carry their toggle inside the payload, so lift it out
    // and normalise the scancode -- otherwise every other press of the same
    // button would look like a different key.
    if (count == 32 && (body & kMceCustomerMask) == kMceCustomerCode) {
        result.toggle   = (body & kMceToggleMask) != 0;
        result.scancode = body & ~kMceToggleMask;
    } else {
        result.toggle = tFirst;
    }

    m_onResult(result);
}
