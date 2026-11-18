// main.cpp - mceir, a user-mode driver for the Philips eHome MCE infrared
// transceiver on Windows 11 ARM64, where the in-box USBCIR driver no longer
// exists.
//
// The pipeline is: WinUSB bulk/interrupt reads -> MCE packet framing -> IR
// pulse/space samples -> RC-6 mode 6A decode -> keystroke injection.

#include "mceir.h"

// WIN32_LEAN_AND_MEAN keeps this out of windows.h, and the volume path needs it.
#include <objbase.h>

#include <share.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

LogLevel g_logLevel = LogLevel::Normal;

// Where log lines go. Normally stdout, which is only real when a console was
// inherited; --log redirects to a file, which is the only way to see anything
// when this runs from a Scheduled Task.
static FILE*        g_logFile = nullptr;
static std::wstring g_logPath;

// The daemon runs for weeks and the log is its only output, so it is capped:
// past this size the file becomes <log>.old, replacing the previous one, and a
// fresh file is started. Uncapped, it once grew past 100 MB.
constexpr long long kLogMaxBytes = 1024 * 1024;

// LogAt runs on more than one thread (the agent's service watcher logs too),
// and RotateLog closes the stream out from under any concurrent writer.
static SRWLOCK g_logLock = SRWLOCK_INIT;

// Denies other writers but not readers, so the log can be inspected while the
// daemon runs. _wfopen_s opens files exclusively, which locked everyone out.
static FILE* OpenLogStream(const wchar_t* mode) {
    return _wfsopen(g_logPath.c_str(), mode, _SH_DENYWR);
}

static void RotateLog() {
    fclose(g_logFile);
    g_logFile = nullptr;

    // The rename fails while a reader holds the file without delete sharing
    // (Get-Content does); truncating in place still keeps the size bounded.
    const std::wstring old = g_logPath + L".old";
    const bool moved = MoveFileExW(g_logPath.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    g_logFile = OpenLogStream(moved ? L"a" : L"w");
}

bool OpenLog(const std::wstring& path) {
    g_logPath = path;
    g_logFile = OpenLogStream(L"a");
    if (!g_logFile)
        return false;

    // An append stream does not necessarily report its position as the end of
    // the file until the first write, so seek there before measuring.
    _fseeki64(g_logFile, 0, SEEK_END);
    if (_ftelli64(g_logFile) >= kLogMaxBytes)
        RotateLog();
    return g_logFile != nullptr;
}

void LogAt(LogLevel level, const char* fmt, ...) {
    if (g_logLevel < level)
        return;

    AcquireSRWLockExclusive(&g_logLock);

    FILE* out = g_logFile ? g_logFile : stdout;

    SYSTEMTIME st;
    GetLocalTime(&st);
    if (g_logFile) {
        // A log file spans days and reboots, so its lines carry the date.
        fprintf(out, "%04u-%02u-%02u %02u:%02u:%02u.%03u  ", st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    } else if (g_logLevel >= LogLevel::Verbose) {
        fprintf(out, "%02u:%02u:%02u.%03u  ", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds);
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);

    fputc('\n', out);
    fflush(out);

    if (g_logFile && _ftelli64(g_logFile) >= kLogMaxBytes)
        RotateLog();

    ReleaseSRWLockExclusive(&g_logLock);
}

std::string FormatWin32Error(DWORD err) {
    char* buf = nullptr;
    DWORD n   = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, err, 0, reinterpret_cast<char*>(&buf), 0, nullptr);

    std::string text;
    if (n && buf) {
        text.assign(buf, n);
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
            text.pop_back();
    }
    if (buf)
        LocalFree(buf);

    char suffix[32];
    sprintf_s(suffix, sizeof(suffix), " (0x%08lX)", err);
    return text.empty() ? std::string("error") + suffix : text + suffix;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

namespace {

HANDLE g_stopEvent = nullptr;

BOOL WINAPI ConsoleHandler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_stopEvent)
            SetEvent(g_stopEvent);
        return TRUE;
    default:
        return FALSE;
    }
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

// How long a silence ends a burst when the firmware has not said so itself.
constexpr ULONGLONG kIdleFlushMs = 30;

// A synthetic silence long enough to close an RC-6 frame.
constexpr uint32_t kTerminatorUs = 10 * 444;

// A remote repeats its frame roughly every 110 ms while a button is held, so
// frames arriving further apart than this are a fresh press.
constexpr ULONGLONG kRepeatGapMs = 300;

// Wait this long before a held button starts repeating, then repeat no faster
// than the remote sends.
constexpr ULONGLONG kRepeatDelayMs  = 400;
constexpr ULONGLONG kRepeatPeriodMs = 100;

constexpr DWORD kReadTickMs   = 50;
constexpr DWORD kReopenWaitMs = 3000;

// Pause after a device error before reopening, doubling while reopened
// sessions keep failing before delivering anything. See the main loop.
constexpr DWORD kErrorBackoffMinMs = 1000;
constexpr DWORD kErrorBackoffMaxMs = 8000;

// The firmware keeps IR it received while nobody was reading -- a press at the
// logon screen, before a restart, or while the machine slept -- and hands it
// over on the first read, where it would replay as a fresh press. Power
// replays as Sleep. A verbose log caught it: Power decoded 87 ms after the
// device was opened, with the next real press 85 s later. So frames decoded
// this soon after opening the device, or after this process was frozen, are
// dropped.
constexpr ULONGLONG kStaleWindowMs = 500;

// Read never blocks for longer than a tick, so a gap this long between two
// reads means the process was frozen: the machine slept, or Modern Standby
// suspended desktop processes.
constexpr ULONGLONG kFrozenGapMs = 2000;

// The firmware's receive timeout is 16 bits of 50 us units.
constexpr uint32_t kRxTimeoutMaxMs = 0xFFFF * 50 / 1000;

// ---------------------------------------------------------------------------
// Console
//
// This is linked as a Windows-subsystem binary, not a console one, so that
// nothing allocates a console window. A console application started from a
// Scheduled Task shows its console on the desktop at every logon -- the task's
// "Hidden" setting hides the *task*, not the process's window -- and there is
// no way to suppress that from the task side.
//
// The cost is that running it from a terminal would print nowhere, so when a
// parent console exists we attach to it and rebind the standard streams. Run
// from Explorer or a task there is no parent, the attach fails, and output
// quietly goes nowhere, which is what a daemon wants.
// ---------------------------------------------------------------------------

bool AttachToParentConsole() {
    // If the caller already redirected us to a pipe or a file, that handle is
    // the one they want written to. Rebinding it to CONOUT$ below would send
    // the output to the console instead and their redirection would capture
    // nothing, so check before touching anything.
    auto alreadyRedirected = [](DWORD which) {
        HANDLE h = GetStdHandle(which);
        return h != nullptr && h != INVALID_HANDLE_VALUE;
    };
    const bool haveOut = alreadyRedirected(STD_OUTPUT_HANDLE);
    const bool haveErr = alreadyRedirected(STD_ERROR_HANDLE);

    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return false;

    FILE* f = nullptr;
    if (!haveOut)
        freopen_s(&f, "CONOUT$", "w", stdout);
    if (!haveErr)
        freopen_s(&f, "CONOUT$", "w", stderr);
    return true;
}

std::wstring ExePath() {
    wchar_t path[MAX_PATH] = {};
    DWORD   n              = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return {};
    return std::wstring(path, n);
}

std::wstring ExeRelativePath(const wchar_t* leaf) {
    const std::wstring s     = ExePath();
    const size_t       slash = s.find_last_of(L'\\');
    if (slash == std::wstring::npos)
        return leaf;
    return s.substr(0, slash + 1) + leaf;
}

// Quotes an argument so that CommandLineToArgvW reads it back unchanged.
// Inside quotes a backslash is literal except in a run that ends at a quote,
// where each one must be doubled and the quote itself escaped.
std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return a;

    std::wstring out = L"\"";
    for (auto it = a.begin();; ++it) {
        size_t backslashes = 0;
        while (it != a.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == a.end()) {
            out.append(backslashes * 2, L'\\');   // they now precede our closing quote
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

// %LOCALAPPDATA%\mceir\mceir.log -- where the agent logs unless told
// otherwise, the same file the logon task used. The agent runs as the user
// with their environment, so this is their profile.
std::wstring DefaultUserLogPath() {
    wchar_t     buf[MAX_PATH] = {};
    const DWORD n             = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return {};
    const std::wstring dir = std::wstring(buf, n) + L"\\mceir";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\mceir.log";
}

// ---------------------------------------------------------------------------
// Agent supervision
//
// Started by the service (--agent), we inherit two handles from it: an event
// it sets to ask us to exit, and a handle on the service process itself. We
// exit when either is signalled -- if the service dies, an orphan would keep
// holding the transceiver and the next agent could not open it.
// ---------------------------------------------------------------------------

HANDLE g_serviceHandles[2] = {};

DWORD WINAPI WatchService(LPVOID) {
    if (WaitForMultipleObjects(2, g_serviceHandles, FALSE, INFINITE) == WAIT_FAILED)
        LOG("Cannot watch the service (%s); exiting.", FormatWin32Error(GetLastError()).c_str());
    SetEvent(g_stopEvent);
    return 0;
}

HANDLE ParseHandle(const wchar_t* s) {
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(wcstoull(s, nullptr, 10)));
}

void PrintUsage() {
    printf(
        "mceir - user-mode driver for the eHome MCE infrared receiver\n"
        "\n"
        "Usage: mceir.exe [options]\n"
        "\n"
        "  --print             decode and report button presses without injecting\n"
        "                      keystrokes. Use this to discover scancodes.\n"
        "  --dump-keymap       print the active key map and exit\n"
        "  --config <path>     key map overrides (default: mceir.conf beside the exe)\n"
        "  --rx-timeout <ms>   silence the firmware waits through before ending a\n"
        "                      burst (default 100)\n"
        "  --no-rx-timeout     do not set the receive timeout; keep the firmware default\n"
        "  -v, --verbose       report device details and decode failures\n"
        "  --trace             also dump raw USB traffic and IR samples\n"
        "  -q, --quiet         suppress all output\n"
        "  --service           run under the Service Control Manager; other options\n"
        "                      pass to the per-user agent (install-service.ps1)\n"
        "  -h, --help          this text\n"
        "\n"
        "Config file lines look like:\n"
        "    800f0426 = F5            # Guide -> F5\n"
        "    800f041e = UP repeat     # force auto-repeat on\n"
        "    800f040c = NONE          # leave Power unbound\n");
}

// ---------------------------------------------------------------------------
// Press tracking
// ---------------------------------------------------------------------------

struct PressState {
    uint32_t   scancode  = 0;
    bool       toggle    = false;
    bool       valid     = false;
    ULONGLONG  lastFrame = 0;
    ULONGLONG  pressed   = 0;
    ULONGLONG  injected  = 0;
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Before anything prints. Succeeds when started from a terminal, fails
    // when started by the Scheduled Task, which is exactly the distinction we
    // want -- and either way no new console window appears.
    AttachToParentConsole();

    bool         printOnly    = false;
    bool         dumpKeymap   = false;
    bool         setRxTimeout = true;
    uint32_t     rxTimeoutMs  = 100;
    std::wstring configPath   = ExeRelativePath(L"mceir.conf");
    std::wstring logPath;
    bool         serviceMode  = false;
    bool         agentMode    = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--print")            printOnly = true;
        else if (a == L"--service")     serviceMode = true;
        else if (a == L"--agent")       agentMode = true;
        else if (a == L"--stop-handle" && i + 1 < argc)   g_serviceHandles[0] = ParseHandle(argv[++i]);
        else if (a == L"--parent-handle" && i + 1 < argc) g_serviceHandles[1] = ParseHandle(argv[++i]);
        else if (a == L"--dump-keymap") dumpKeymap = true;
        else if (a == L"-v" || a == L"--verbose") g_logLevel = LogLevel::Verbose;
        else if (a == L"--trace")       g_logLevel = LogLevel::Trace;
        else if (a == L"-q" || a == L"--quiet")   g_logLevel = LogLevel::Quiet;
        else if (a == L"--no-rx-timeout") setRxTimeout = false;
        else if (a == L"--config" && i + 1 < argc) configPath = argv[++i];
        else if (a == L"--log" && i + 1 < argc) logPath = argv[++i];
        else if (a == L"--rx-timeout" && i + 1 < argc) {
            // _wtoi took anything: garbage became 0, which the firmware was
            // then sent, and large values overflowed the conversion to us.
            wchar_t*            end = nullptr;
            const unsigned long ms  = wcstoul(argv[++i], &end, 10);
            if (*end != L'\0' || ms < 1 || ms > kRxTimeoutMaxMs) {
                fwprintf(stderr, L"--rx-timeout takes 1 to %u (ms), not \"%s\".\n",
                         kRxTimeoutMaxMs, argv[i]);
                return 2;
            }
            rxTimeoutMs = static_cast<uint32_t>(ms);
        }
        else if (a == L"-h" || a == L"--help") { PrintUsage(); return 0; }
        else {
            fwprintf(stderr, L"Unknown option: %s\n\n", a.c_str());
            PrintUsage();
            return 2;
        }
    }

    // Everything but --service is meant for the agent the service starts.
    if (serviceMode) {
        std::wstring agentArgs;
        for (int i = 1; i < argc; ++i)
            if (std::wstring(argv[i]) != L"--service")
                agentArgs += L' ' + QuoteArg(argv[i]);
        return RunService(ExePath(), agentArgs);
    }

    if (agentMode) {
        if (!g_serviceHandles[0] || !g_serviceHandles[1]) {
            fwprintf(stderr, L"--agent is started by the mceir service; run without it.\n");
            return 2;
        }
        if (logPath.empty())
            logPath = DefaultUserLogPath();
    }

    // Opened before the keymap so that its "N overrides applied" lines land in
    // the log too.
    if (!logPath.empty()) {
        // Plain byte stream, deliberately. Opening with ccs=UTF-8 puts the
        // stream in wide orientation, and the narrow fprintf in LogAt then
        // fails the CRT's invalid-parameter check and fastfails the process.
        if (OpenLog(logPath)) {
            if (g_logLevel < LogLevel::Normal)
                g_logLevel = LogLevel::Normal;
        } else {
            LOG("Could not open log file %ls; continuing without one.", logPath.c_str());
        }
    }

    Keymap keymap;
    keymap.LoadOverrides(configPath);

    if (dumpKeymap) {
        keymap.DumpTo(stdout);
        return 0;
    }

    // Needed by the Core Audio path that handles the volume keys.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        LOG("CreateEvent failed: %s", FormatWin32Error(GetLastError()).c_str());
        return 1;
    }
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    if (agentMode) {
        HANDLE watcher = CreateThread(nullptr, 0, WatchService, nullptr, 0, nullptr);
        if (!watcher) {
            LOG("Could not start the service watcher: %s", FormatWin32Error(GetLastError()).c_str());
            return 1;
        }
        CloseHandle(watcher);
        LOG("Running as the mceir service's agent.");
    }

    PressState press;
    ULONGLONG  lastData          = GetTickCount64();
    ULONGLONG  acceptFramesAfter = 0;   // see kStaleWindowMs

    // ------------------------------------------------------------------
    // Pipeline. Each stage hands its output to the next through a callback,
    // so nothing needs to buffer a whole frame.
    // ------------------------------------------------------------------

    Rc6Decoder decoder([&](const Rc6Result& r) {
        const ULONGLONG now = GetTickCount64();

        if (now < acceptFramesAfter) {
            LOGV("Dropped %08X: received before we were listening.", r.scancode);
            return;
        }

        const bool isRepeat = press.valid && r.scancode == press.scancode &&
                              r.toggle == press.toggle &&
                              (now - press.lastFrame) < kRepeatGapMs;

        const KeyBinding* binding = keymap.Find(r.scancode);

        if (!binding) {
            if (!isRepeat)
                LOG("Unmapped button: scancode %08X  (bind it in mceir.conf)", r.scancode);
        } else if (!isRepeat) {
            if (printOnly) {
                LOG("%-12s  %08X  -> %s", binding->name.c_str(), r.scancode,
                    VirtualKeyName(binding->vk).c_str());
            } else {
                LOGV("%-12s  %08X  -> %s", binding->name.c_str(), r.scancode,
                     VirtualKeyName(binding->vk).c_str());
                InjectKey(binding->vk);
            }
            press.pressed  = now;
            press.injected = now;
        } else if (binding->repeats && (now - press.pressed) >= kRepeatDelayMs &&
                   (now - press.injected) >= kRepeatPeriodMs) {
            if (printOnly)
                LOG("%-12s  %08X  (repeat)", binding->name.c_str(), r.scancode);
            else
                InjectKey(binding->vk);
            press.injected = now;
        }

        press.scancode  = r.scancode;
        press.toggle    = r.toggle;
        press.valid     = true;
        press.lastFrame = now;
    });

    MceStream stream(
        [&](const IrSample& s) {
            LOGT("     %s %6u us", s.pulse ? "pulse" : "space", s.duration_us);
            decoder.Feed(s);
        },
        [&] {
            // The firmware says the burst is over; close the frame with a
            // silence long enough for the decoder to act on it.
            decoder.Feed(IrSample{ false, kTerminatorUs });
        });

    LOG("mceir starting. Press Ctrl+C to stop.");

    // ------------------------------------------------------------------
    // Main loop. The device is reopened if it is unplugged or errors out, so
    // this can run as a startup task without supervision.
    // ------------------------------------------------------------------

    bool        announcedMissing = false;
    std::string lastOpenError;
    DWORD       errorBackoffMs = kErrorBackoffMinMs;

    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        MceDevice device;

        if (!device.Open()) {
            // This retries every few seconds for as long as the transceiver
            // is away -- days, if it is unplugged -- so say why only when the
            // reason changes.
            if (device.LastError() != lastOpenError) {
                lastOpenError = device.LastError();
                LOG("%s", lastOpenError.c_str());
            }
            if (!announcedMissing) {
                LOG("Waiting for the transceiver...");
                announcedMissing = true;
            }
            // When it comes back it is a fresh arrival, not the next failure
            // in a run, so it starts with the shortest backoff.
            errorBackoffMs = kErrorBackoffMinMs;
            if (WaitForSingleObject(g_stopEvent, kReopenWaitMs) == WAIT_OBJECT_0)
                break;
            continue;
        }
        announcedMissing = false;
        lastOpenError.clear();

        device.Initialize(setRxTimeout, rxTimeoutMs * 1000);

        stream.Flush();
        decoder.Reset();
        press.valid       = false;
        acceptFramesAfter = GetTickCount64() + kStaleWindowMs;

        std::vector<uint8_t> buf;
        bool                 keepDevice = true;
        bool                 gotData    = false;
        bool                 failed     = false;
        ULONGLONG            lastRead   = GetTickCount64();

        while (keepDevice) {
            const MceDevice::ReadResult result = device.Read(buf, kReadTickMs, g_stopEvent);

            // Checked after the read, because a freeze usually lands inside
            // its wait, and what it then returns is the backlog.
            const ULONGLONG now = GetTickCount64();
            if (now - lastRead >= kFrozenGapMs) {
                LOGV("Resumed after %llu s frozen; dropping what arrived meanwhile.",
                     (now - lastRead) / 1000);
                acceptFramesAfter = now + kStaleWindowMs;
            }
            lastRead = now;

            switch (result) {
            case MceDevice::ReadResult::Data:
                if (!buf.empty()) {
                    gotData = true;
                    if (g_logLevel >= LogLevel::Trace) {
                        std::string hex;
                        char        t[4];
                        for (uint8_t c : buf) {
                            sprintf_s(t, sizeof(t), "%02X ", c);
                            hex += t;
                        }
                        LOGT("  <- %zu byte(s): %s", buf.size(), hex.c_str());
                    }
                    stream.Feed(buf.data(), buf.size());
                    lastData = GetTickCount64();
                }
                break;

            case MceDevice::ReadResult::Timeout:
                // Nothing arrived. If the receiver has gone quiet mid-frame,
                // close the frame out ourselves rather than waiting for the
                // next press to push it through.
                if (GetTickCount64() - lastData >= kIdleFlushMs) {
                    stream.Flush();
                    decoder.Feed(IrSample{ false, kTerminatorUs });
                    lastData = GetTickCount64();
                }
                break;

            case MceDevice::ReadResult::Stopped:
                keepDevice = false;
                break;

            case MceDevice::ReadResult::Error:
                LOG("Device error; will try to reopen.");
                failed     = true;
                keepDevice = false;
                break;
            }

            if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0)
                keepDevice = false;
        }

        // A transceiver that is dropping off the bus -- here, when the dock it
        // hangs off loses its PCIe link -- stays enumerated for several seconds
        // while every read fails with ERROR_DEV_NOT_EXIST. Open() keeps
        // succeeding, so without a pause this spun at ~5,000 reopens a second,
        // pinning a core and writing megabytes of log per dropout. A session
        // that delivered data was healthy, so its failure starts the backoff
        // over; one that failed straight away doubles it.
        if (failed) {
            if (gotData)
                errorBackoffMs = kErrorBackoffMinMs;
            if (WaitForSingleObject(g_stopEvent, errorBackoffMs) == WAIT_OBJECT_0)
                break;
            errorBackoffMs = (std::min)(errorBackoffMs * 2, kErrorBackoffMaxMs);
        }
    }

    LOG("mceir stopped.");
    ShutdownAudio();
    CloseHandle(g_stopEvent);
    CoUninitialize();
    return 0;
}
