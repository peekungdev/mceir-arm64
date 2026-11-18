// rc6_test.cpp - exercises the receive chain without the hardware.
//
// Builds RC-6 mode 6A frames, encodes them the way the transceiver's firmware
// would (50 us sample bytes, packed into IR data packets), and pushes them
// through the real MceStream and Rc6Decoder to check the scancode that comes
// out the far end.

#include "../src/mceir.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

// --- Stubs for the bits of main.cpp the pipeline links against ---------------

LogLevel g_logLevel = LogLevel::Normal;

void LogAt(LogLevel level, const char* fmt, ...) {
    if (g_logLevel < level)
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

std::string FormatWin32Error(DWORD err) {
    return "error " + std::to_string(err);
}

// --- Encoder ----------------------------------------------------------------

namespace {

constexpr uint32_t kUnitUs     = 444;
constexpr uint32_t kTimeUnitUs = 50;

void AppendSlots(std::vector<bool>& slots, bool level, size_t count) {
    slots.insert(slots.end(), count, level);
}

// One bi-phase bit: a 1 is carrier then silence, a 0 the other way round.
// `width` is 1 for an ordinary bit and 2 for the double-width trailer.
void AppendBit(std::vector<bool>& slots, bool bit, size_t width) {
    AppendSlots(slots, bit, width);
    AppendSlots(slots, !bit, width);
}

std::vector<bool> BuildFrameSlots(uint32_t body, int bodyBits, bool trailerBit) {
    std::vector<bool> slots;

    AppendSlots(slots, true, 6);   // leader carrier
    AppendSlots(slots, false, 2);  // leader silence

    AppendBit(slots, true, 1);   // start bit
    AppendBit(slots, true, 1);   // mode bit 2  }
    AppendBit(slots, true, 1);   // mode bit 1  } mode 6 == 0b110
    AppendBit(slots, false, 1);  // mode bit 0  }

    AppendBit(slots, trailerBit, 2);

    for (int i = bodyBits - 1; i >= 0; --i)
        AppendBit(slots, ((body >> i) & 1u) != 0, 1);

    AppendSlots(slots, false, 6);  // signal free
    return slots;
}

// Collapse the slot list into alternating runs, then express each run the way
// the firmware does: a level bit plus a length in 50 us units, split across as
// many bytes as it takes.
std::vector<uint8_t> SlotsToSampleBytes(const std::vector<bool>& slots) {
    std::vector<uint8_t> out;
    size_t               i = 0;

    while (i < slots.size()) {
        const bool level = slots[i];
        size_t     run   = 0;
        while (i < slots.size() && slots[i] == level) {
            ++run;
            ++i;
        }

        uint32_t units = static_cast<uint32_t>(run * kUnitUs) / kTimeUnitUs;
        if (units == 0)
            units = 1;

        while (units > 0) {
            const uint32_t chunk = units > 127 ? 127 : units;
            out.push_back(static_cast<uint8_t>((level ? 0x80 : 0x00) | chunk));
            units -= chunk;
        }
    }
    return out;
}

// Wrap the samples in IR data packets. The length field is 5 bits, but 31 is
// unusable: 0x80 | 31 is 0x9F, which is the IR-port command prefix. So the
// firmware never emits more than 30 samples in one packet, and neither do we.
constexpr size_t kMaxSamplesPerPacket = 30;

std::vector<uint8_t> Packetize(const std::vector<uint8_t>& samples) {
    std::vector<uint8_t> out;
    size_t               i = 0;
    while (i < samples.size()) {
        const size_t remaining = samples.size() - i;
        const size_t n = remaining > kMaxSamplesPerPacket ? kMaxSamplesPerPacket : remaining;
        out.push_back(static_cast<uint8_t>(0x80 | n));
        out.insert(out.end(), samples.begin() + i, samples.begin() + i + n);
        i += n;
    }
    return out;
}

std::vector<uint8_t> EncodeFrame(uint32_t body, int bodyBits, bool trailerBit) {
    return Packetize(SlotsToSampleBytes(BuildFrameSlots(body, bodyBits, trailerBit)));
}

// --- Harness ----------------------------------------------------------------

int g_failures = 0;

struct Capture {
    bool      got = false;
    Rc6Result result{};
};

// Feeds bytes through the real pipeline, in chunks, to prove that framing
// survives a packet being split across USB reads.
Capture RunPipeline(const std::vector<uint8_t>& bytes, size_t chunkSize) {
    Capture cap;

    Rc6Decoder decoder([&](const Rc6Result& r) {
        cap.got    = true;
        cap.result = r;
    });

    MceStream stream([&](const IrSample& s) { decoder.Feed(s); },
                     [&] { decoder.Feed(IrSample{ false, 10 * kUnitUs }); });

    for (size_t i = 0; i < bytes.size(); i += chunkSize) {
        const size_t n = (bytes.size() - i) < chunkSize ? (bytes.size() - i) : chunkSize;
        stream.Feed(bytes.data() + i, n);
    }

    // What the idle tick in main.cpp does once the receiver goes quiet.
    stream.Flush();
    decoder.Feed(IrSample{ false, 10 * kUnitUs });
    return cap;
}

void ExpectDecode(const char* what, uint32_t body, int bodyBits, bool trailerBit,
                  uint32_t wantScancode, bool wantToggle, size_t chunkSize = 64) {
    const Capture cap = RunPipeline(EncodeFrame(body, bodyBits, trailerBit), chunkSize);

    if (!cap.got) {
        printf("FAIL  %-34s decoded nothing\n", what);
        ++g_failures;
        return;
    }
    if (cap.result.scancode != wantScancode || cap.result.toggle != wantToggle) {
        printf("FAIL  %-34s got %08X toggle=%d, want %08X toggle=%d\n", what,
               cap.result.scancode, cap.result.toggle ? 1 : 0, wantScancode,
               wantToggle ? 1 : 0);
        ++g_failures;
        return;
    }
    printf("ok    %-34s %08X toggle=%d\n", what, cap.result.scancode,
           cap.result.toggle ? 1 : 0);
}

// Feeds already-merged pulse/space runs straight into the decoder, for frames
// captured off real hardware where the byte framing is not what is under test.
void ExpectDecodeFromRuns(const char* what, const std::vector<uint32_t>& runs,
                          uint32_t wantScancode, bool wantToggle) {
    Capture cap;
    Rc6Decoder decoder([&](const Rc6Result& r) {
        cap.got    = true;
        cap.result = r;
    });

    bool pulse = true;  // a frame always opens with the leader carrier
    for (uint32_t d : runs) {
        decoder.Feed(IrSample{ pulse, d });
        pulse = !pulse;
    }
    decoder.Feed(IrSample{ false, 10 * kUnitUs });

    if (!cap.got) {
        printf("FAIL  %-34s decoded nothing\n", what);
        ++g_failures;
        return;
    }
    if (cap.result.scancode != wantScancode || cap.result.toggle != wantToggle) {
        printf("FAIL  %-34s got %08X toggle=%d, want %08X toggle=%d\n", what,
               cap.result.scancode, cap.result.toggle ? 1 : 0, wantScancode,
               wantToggle ? 1 : 0);
        ++g_failures;
        return;
    }
    printf("ok    %-34s %08X toggle=%d\n", what, cap.result.scancode,
           cap.result.toggle ? 1 : 0);
}

void ExpectReject(const char* what, const std::vector<uint8_t>& bytes) {
    const LogLevel saved = g_logLevel;
    g_logLevel           = LogLevel::Quiet;  // the rejection reason is noise here
    const Capture cap    = RunPipeline(bytes, 64);
    g_logLevel           = saved;

    if (cap.got) {
        printf("FAIL  %-34s decoded %08X, expected rejection\n", what,
               cap.result.scancode);
        ++g_failures;
        return;
    }
    printf("ok    %-34s rejected\n", what);
}

}  // namespace

int main() {
    printf("RC-6 mode 6A receive chain\n\n");

    // The buttons a Media Center remote actually sends.
    ExpectDecode("OK", 0x800F0422, 32, true, 0x800F0422, false);
    ExpectDecode("OK, toggle set", 0x800F8422, 32, true, 0x800F0422, true);
    ExpectDecode("VolumeUp", 0x800F0410, 32, true, 0x800F0410, false);
    ExpectDecode("VolumeUp, toggle set", 0x800F8410, 32, true, 0x800F0410, true);
    ExpectDecode("Num0 (body ends in zeroes)", 0x800F0400, 32, true, 0x800F0400, false);
    ExpectDecode("Play", 0x800F0416, 32, true, 0x800F0416, false);
    ExpectDecode("PlayPause (high key byte)", 0x800F0481, 32, true, 0x800F0481, false);

    // A 32-bit body from some other manufacturer keeps its toggle bit, because
    // only Microsoft's customer code puts the toggle inside the payload.
    ExpectDecode("non-MCE customer code", 0x1234ABCD, 32, true, 0x1234ABCD, true);

    // Shorter mode 6A bodies.
    ExpectDecode("16-bit body", 0x0000BEEF, 16, false, 0x0000BEEF, false);
    ExpectDecode("24-bit body", 0x00ABCDEF, 24, false, 0x00ABCDEF, false);

    // The trailer bit is not a length selector. This remote sends 32 bits with
    // it clear, and treating it as a selector truncated the body at the 16-bit
    // customer code -- every button came out as 0x0000800F.
    ExpectDecode("32-bit body, trailer clear", 0x800F8416, 32, false, 0x800F0416, true);

    printf("\nCaptured hardware\n\n");

    // A real "Play" press off the Philips transceiver, as pulse/space runs in
    // microseconds starting with the leader carrier. Body 0x800F8416: customer
    // code 0x800F, payload toggle set, key 0x16.
    ExpectDecodeFromRuns("Play, off the wire",
                         { 2700, 850, 500, 400, 450, 450, 450, 900, 450, 850, 1400, 850,
                           450, 450, 450, 450, 450, 450, 450, 400, 500, 400, 500, 400,
                           500, 400, 450, 450, 450, 450, 450, 450, 900, 400, 500, 400,
                           500, 400, 450, 450, 450, 900, 450, 400, 500, 400, 500, 400,
                           950, 850, 450, 450, 450, 450, 450, 400, 500, 400, 950, 850,
                           900, 450, 450, 850, 500 },
                         0x800F0416, true);

    printf("\nFraming\n\n");

    // The same frame delivered in awkward pieces must decode identically.
    ExpectDecode("split into 1-byte reads", 0x800F0422, 32, true, 0x800F0422, false, 1);
    ExpectDecode("split into 7-byte reads", 0x800F0422, 32, true, 0x800F0422, false, 7);
    ExpectDecode("split into 13-byte reads", 0x800F0422, 32, true, 0x800F0422, false, 13);

    // A command reply arriving in front of the frame must not eat any of it.
    {
        std::vector<uint8_t> bytes = { 0xFF, 0x0B, 0x02, 0x01 };  // firmware revision
        const auto           frame = EncodeFrame(0x800F0422, 32, true);
        bytes.insert(bytes.end(), frame.begin(), frame.end());

        const Capture cap = RunPipeline(bytes, 64);
        if (cap.got && cap.result.scancode == 0x800F0422)
            printf("ok    %-34s %08X\n", "reply before frame", cap.result.scancode);
        else {
            printf("FAIL  %-34s\n", "reply before frame");
            ++g_failures;
        }
    }

    // Replies from the IR port start with 0x9F, which also satisfies the IR
    // data header test (0x9F & 0xE0 == 0x80). If that is not special-cased,
    // each one is read as a 31-sample packet and eats the frame behind it.
    // These are the exact reply bytes this transceiver puts on the wire.
    {
        const uint8_t replies[] = {
            0x9F, 0x0C, 0x07, 0xD0,  // receive timeout echo
            0x9F, 0x14, 0x01,        // receive port enable
            0x9F, 0x15, 0x00,        // receive port enable reply
            0xFF, 0xFE,              // system port, reset
        };

        std::vector<uint8_t> bytes(std::begin(replies), std::end(replies));
        const auto           frame = EncodeFrame(0x800F0422, 32, true);
        bytes.insert(bytes.end(), frame.begin(), frame.end());

        const Capture cap = RunPipeline(bytes, 64);
        if (cap.got && cap.result.scancode == 0x800F0422)
            printf("ok    %-34s %08X\n", "IR-port replies before frame", cap.result.scancode);
        else {
            printf("FAIL  %-34s decoded %s\n", "IR-port replies before frame",
                   cap.got ? "the wrong scancode" : "nothing");
            ++g_failures;
        }
    }

    // The end-of-signal marker sits between two bursts and must not consume
    // the second one.
    {
        std::vector<uint8_t> bytes = EncodeFrame(0x800F0410, 32, true);
        const uint8_t        sigEnd[] = { 0x9F, 0x01, 0x01 };
        bytes.insert(bytes.end(), std::begin(sigEnd), std::end(sigEnd));
        const auto second = EncodeFrame(0x800F0422, 32, true);
        bytes.insert(bytes.end(), second.begin(), second.end());

        // Only the last decode is captured, so this proves the second frame
        // survived the marker in front of it.
        const Capture cap = RunPipeline(bytes, 64);
        if (cap.got && cap.result.scancode == 0x800F0422)
            printf("ok    %-34s %08X\n", "frame after end-of-signal", cap.result.scancode);
        else {
            printf("FAIL  %-34s decoded %s\n", "frame after end-of-signal",
                   cap.got ? "the wrong scancode" : "nothing");
            ++g_failures;
        }
    }

    // A zero-length sample inside a frame is noise, not the end of the burst.
    // Treating it as the end cut the frame in two and decoded nothing.
    {
        auto samples = SlotsToSampleBytes(BuildFrameSlots(0x800F0422, 32, true));
        samples.insert(samples.begin() + static_cast<std::ptrdiff_t>(samples.size() / 2), 0x00);

        const Capture cap = RunPipeline(Packetize(samples), 64);
        if (cap.got && cap.result.scancode == 0x800F0422)
            printf("ok    %-34s %08X\n", "zero-length sample mid-frame", cap.result.scancode);
        else {
            printf("FAIL  %-34s decoded %s\n", "zero-length sample mid-frame",
                   cap.got ? "the wrong scancode" : "nothing");
            ++g_failures;
        }
    }

    printf("\nRejection\n\n");

    // Silence where the leader should be.
    ExpectReject("no leader", { 0x88, 0x08, 0x88, 0x08, 0x88, 0x08, 0x88, 0x08, 0x35 });

    // A frame whose header claims a mode other than 6A.
    {
        std::vector<bool> slots;
        AppendSlots(slots, true, 6);
        AppendSlots(slots, false, 2);
        AppendBit(slots, true, 1);   // start
        AppendBit(slots, false, 1);  // mode 0
        AppendBit(slots, false, 1);
        AppendBit(slots, false, 1);
        AppendBit(slots, true, 2);
        for (int i = 15; i >= 0; --i)
            AppendBit(slots, ((0xBEEF >> i) & 1u) != 0, 1);
        AppendSlots(slots, false, 6);
        ExpectReject("mode 0 frame", Packetize(SlotsToSampleBytes(slots)));
    }

    // A Manchester violation in the body: two slots of the same level where a
    // bit transition belongs.
    {
        std::vector<bool> slots = BuildFrameSlots(0x800F0422, 32, true);
        slots[30]               = slots[31];
        ExpectReject("Manchester violation", Packetize(SlotsToSampleBytes(slots)));
    }

    // A frame cut off partway through the body.
    {
        auto bytes = EncodeFrame(0x800F0422, 32, true);
        bytes.resize(bytes.size() / 2);
        ExpectReject("truncated frame", bytes);
    }

    printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES ABOVE.");
    return g_failures == 0 ? 0 : 1;
}
