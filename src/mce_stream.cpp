// mce_stream.cpp - turns the transceiver's byte stream into IR samples.
//
// The device sends a mix of two things on the same endpoint: replies to
// commands we issued, and IR data packets. They are distinguished by the
// header byte:
//
//   (b & 0xE0) == 0x80   IR data packet, carrying (b & 0x1F) sample bytes
//   anything else        a command or reply, whose length depends on the
//                        command and subcommand bytes
//
// Each sample byte is one run of carrier or silence: bit 7 is the level and
// bits 0..6 are the length in 50 us units. A run longer than 127 units is
// split across several bytes of the same polarity, so runs are accumulated
// here and only handed on when the polarity flips.

#include "mceir.h"

namespace {

constexpr uint8_t  kIrDataMask   = 0xE0;
constexpr uint8_t  kIrDataMatch  = 0x80;
constexpr uint8_t  kIrLengthMask = 0x1F;
constexpr uint8_t  kPulseBit     = 0x80;
constexpr uint8_t  kDurationMask = 0x7F;
constexpr uint32_t kTimeUnitUs   = 50;

constexpr uint8_t kPortIr  = 0x9F;
constexpr uint8_t kPortSys = 0xFF;

// Number of data bytes that follow a command's two header bytes.
//
// Only the commands this daemon can provoke are listed. An unrecognised
// command is assumed to carry no payload: guessing high would swallow the
// bytes of whatever follows, and because IR data headers are frequent and
// distinctive the parser re-locks almost immediately if the guess is wrong.
int CommandDataSize(uint8_t port, uint8_t sub) {
    switch (port) {
    case 0x00:
        return sub == 0xFF ? 1 : 0;

    case kPortSys:
        switch (sub) {
        case 0x11: return 5;  // port status
        case 0x0E: return 4;  // wake version
        case 0x0B: return 2;  // firmware revision
        case 0x0F: return 1;  // wake source
        case 0x17: return 1;  // wake support
        default:   return 0;
        }

    case kPortIr:
        switch (sub) {
        case 0x06: return 2;  // carrier frequency
        case 0x0B: return 2;  // number of ports
        case 0x0C: return 2;  // receive timeout (set)
        case 0x0D: return 2;  // receive timeout (reply)
        case 0x01: return 1;  // end of signal
        case 0x13: return 1;  // transmit ports
        case 0x14: return 1;  // receive port enable (set)
        case 0x15: return 1;  // receive port enable (reply)
        default:   return 0;
        }

    default:
        return 0;
    }
}

}  // namespace

void MceStream::PushSample(bool pulse, uint32_t duration_us) {
    if (m_haveRun && m_runPulse == pulse) {
        m_runDuration += duration_us;
        return;
    }
    if (m_haveRun)
        m_onSample(IrSample{ m_runPulse, m_runDuration });

    m_haveRun     = true;
    m_runPulse    = pulse;
    m_runDuration = duration_us;
}

void MceStream::Flush() {
    if (!m_haveRun)
        return;
    m_onSample(IrSample{ m_runPulse, m_runDuration });
    m_haveRun     = false;
    m_runDuration = 0;
}

void MceStream::Feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];

        switch (m_state) {
        case State::Header:
            // 0x9F is the IR-port command prefix, and it also satisfies the IR
            // data test below (0x9F & 0xE0 == 0x80). The protocol resolves the
            // clash in the command's favour, which costs the length field its
            // top value: a packet carries at most 30 samples, never 31.
            //
            // Without this the firmware's own replies -- including the
            // end-of-signal marker that closes every burst -- are each read as
            // a 31-sample packet and swallow the 31 bytes behind them.
            if (b != kPortIr && (b & kIrDataMask) == kIrDataMatch) {
                m_remaining = b & kIrLengthMask;
                if (m_remaining > 0)
                    m_state = State::IrData;
                // A zero-length IR packet carries nothing; stay in Header.
            } else {
                m_cmdBuf.clear();
                m_cmdBuf.push_back(b);
                m_state = State::CommandData;
            }
            break;

        case State::IrData: {
            --m_remaining;

            const bool     pulse = (b & kPulseBit) != 0;
            const uint32_t units = b & kDurationMask;

            if (units == 0) {
                // A zero-length run carries nothing; Linux's mceusb discards
                // these as nonsensical too. Treating one as the end of the
                // burst cut short any frame it landed in, and the firmware
                // has a real end-of-burst marker (9F 01), which is handled
                // below, and the idle flush in main.cpp backs it up.
                LOGT("  ignoring a zero-length sample (%02X)", b);
            } else {
                PushSample(pulse, units * kTimeUnitUs);
            }

            if (m_remaining == 0)
                m_state = State::Header;
            break;
        }

        case State::CommandData:
            m_cmdBuf.push_back(b);
            if (m_cmdBuf.size() == 2)
                m_remaining = CommandDataSize(m_cmdBuf[0], m_cmdBuf[1]);
            else
                --m_remaining;

            if (m_remaining == 0) {
                if (g_logLevel >= LogLevel::Trace) {
                    std::string hex;
                    char        t[4];
                    for (uint8_t c : m_cmdBuf) {
                        sprintf_s(t, sizeof(t), "%02X ", c);
                        hex += t;
                    }
                    LOGT("  <- reply           %s", hex.c_str());
                }
                // An explicit end-of-signal reply ends the burst too.
                if (m_cmdBuf.size() >= 2 && m_cmdBuf[0] == kPortIr && m_cmdBuf[1] == 0x01) {
                    Flush();
                    m_onSignalEnd();
                }
                m_state = State::Header;
            }
            break;
        }
    }
}
