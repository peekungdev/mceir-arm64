// mceir.h - shared declarations for the MCE infrared receiver daemon.

#pragma once

#include <windows.h>
#include <winusb.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

enum class LogLevel { Quiet, Normal, Verbose, Trace };

extern LogLevel g_logLevel;

void LogAt(LogLevel level, const char* fmt, ...);

#define LOG(...)     LogAt(LogLevel::Normal,  __VA_ARGS__)
#define LOGV(...)    LogAt(LogLevel::Verbose, __VA_ARGS__)
#define LOGT(...)    LogAt(LogLevel::Trace,   __VA_ARGS__)

// Sends log lines to a file (appended, size-capped, readable while open)
// instead of stdout. Returns false if it cannot be opened.
bool OpenLog(const std::wstring& path);

std::string FormatWin32Error(DWORD err);

// ---------------------------------------------------------------------------
// Service mode (service.cpp)
//
// Runs under the Service Control Manager as a supervisor that keeps an agent
// copy of exePath, started with agentArgs, running as the user logged on at
// the console. Returns the process exit code.
// ---------------------------------------------------------------------------

int RunService(const std::wstring& exePath, const std::wstring& agentArgs);

// ---------------------------------------------------------------------------
// An IR sample: one run of carrier ("pulse") or no carrier ("space").
//
// Durations are microseconds. Adjacent samples always alternate polarity --
// the stream layer merges same-polarity runs before handing them on, because
// the hardware splits long runs across multiple bytes.
// ---------------------------------------------------------------------------

struct IrSample {
    bool     pulse;
    uint32_t duration_us;
};

// ---------------------------------------------------------------------------
// Device: WinUSB discovery and pipe I/O
// ---------------------------------------------------------------------------

class MceDevice {
public:
    MceDevice() = default;
    ~MceDevice();

    MceDevice(const MceDevice&)            = delete;
    MceDevice& operator=(const MceDevice&) = delete;

    // Finds the first present device exposing our interface GUID and opens it.
    // Returns false on failure without logging it, and LastError() says why:
    // the caller retries every few seconds, so it decides what to repeat.
    bool Open();
    void Close();

    bool IsOpen() const { return m_winusb != nullptr; }

    const std::string& LastError() const { return m_error; }

    const std::wstring& Path() const { return m_path; }

    // Sends the gen2 initialisation commands. Failures here are logged but not
    // fatal: several transceivers stream IR data regardless.
    bool Initialize(bool setRxTimeout, uint32_t rxTimeout_us);

    // Starts an overlapped read if one is not already in flight, then waits up
    // to timeoutMs for it or for stopEvent. On data, fills `out` and returns
    // ReadResult::Data. A timeout is normal and means "nothing received yet".
    enum class ReadResult { Data, Timeout, Stopped, Error };
    ReadResult Read(std::vector<uint8_t>& out, DWORD timeoutMs, HANDLE stopEvent);

private:
    bool FindPipes();
    bool WriteCommand(const uint8_t* data, size_t len, const char* what);

    // Records an Open() failure for LastError(). Always returns false.
    bool Fail(const char* fmt, ...);

    HANDLE          m_file    = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE m_winusb = nullptr;
    std::wstring    m_path;
    std::string     m_error;

    UCHAR           m_pipeIn  = 0;
    UCHAR           m_pipeOut = 0;
    USHORT          m_maxPacketIn = 0;

    // A single read is kept in flight across calls so that a timeout does not
    // cancel it -- cancelling an interrupt pipe every tick loses samples that
    // arrived during the gap.
    OVERLAPPED      m_ov      = {};
    HANDLE          m_ovEvent = nullptr;
    bool            m_pending = false;
    std::vector<uint8_t> m_buf;
};

// ---------------------------------------------------------------------------
// Stream: MCE packet framing -> IR samples
// ---------------------------------------------------------------------------

class MceStream {
public:
    using SampleFn    = std::function<void(const IrSample&)>;
    using SignalEndFn = std::function<void()>;

    MceStream(SampleFn onSample, SignalEndFn onSignalEnd)
        : m_onSample(std::move(onSample)), m_onSignalEnd(std::move(onSignalEnd)) {}

    void Feed(const uint8_t* data, size_t len);

    // Emits any run being accumulated. Called when the receiver goes idle.
    void Flush();

private:
    void PushSample(bool pulse, uint32_t duration_us);

    SampleFn    m_onSample;
    SignalEndFn m_onSignalEnd;

    // Framing state, carried across USB reads because packets straddle them.
    enum class State { Header, IrData, CommandData };
    State   m_state    = State::Header;
    uint8_t m_cmd      = 0;      // first byte of a command, for sizing
    int     m_remaining = 0;     // bytes still owed to the current packet
    std::vector<uint8_t> m_cmdBuf;

    // Run accumulator.
    bool     m_haveRun     = false;
    bool     m_runPulse    = false;
    uint32_t m_runDuration = 0;
};

// ---------------------------------------------------------------------------
// Decoder: RC-6 mode 6A
// ---------------------------------------------------------------------------

struct Rc6Result {
    uint32_t scancode;   // e.g. 0x800F0422 for MCE "OK"
    bool     toggle;     // flips on every fresh key press
};

class Rc6Decoder {
public:
    using ResultFn = std::function<void(const Rc6Result&)>;

    explicit Rc6Decoder(ResultFn onResult) : m_onResult(std::move(onResult)) {}

    void Feed(const IrSample& s);
    void Reset();

private:
    void Decode();

    ResultFn m_onResult;

    // Half-bit slots: one entry per RC6_UNIT of carrier or silence.
    std::vector<bool> m_slots;
    bool              m_inFrame = false;
};

// ---------------------------------------------------------------------------
// Key mapping and injection
// ---------------------------------------------------------------------------

struct KeyBinding {
    uint32_t    scancode;
    std::string name;     // MCE button name, shown by --print
    WORD        vk;       // 0 = recognised but deliberately unbound
    bool        repeats;  // auto-repeat while the button is held
};

class Keymap {
public:
    Keymap();

    // Merges overrides from a config file. A missing file is not an error.
    bool LoadOverrides(const std::wstring& path);

    const KeyBinding* Find(uint32_t scancode) const;

    void DumpTo(FILE* f) const;

private:
    std::vector<KeyBinding> m_bindings;
};

// Renders a virtual-key code using the same names the config file accepts.
std::string VirtualKeyName(WORD vk);

// Look up a VK by the names used in the config file ("VOLUME_UP", "F5", "A").
// Returns 0xFFFF if unknown, so that 0 can mean "explicitly unbound".
WORD ParseVirtualKey(const std::string& name);

void InjectKey(WORD vk);

// ---------------------------------------------------------------------------
// Volume, handled through Core Audio rather than SendInput -- see audio.cpp for
// why. TryVolumeKey returns false if it could not take the key, in which case
// the caller should fall back to injecting it.
// ---------------------------------------------------------------------------

bool IsVolumeKey(WORD vk);
bool TryVolumeKey(WORD vk);
void ShutdownAudio();
