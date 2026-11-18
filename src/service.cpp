// service.cpp - mceir as a Windows service.
//
// A service cannot handle the remote itself. It runs in session 0, which has
// no interactive desktop, so SendInput from there lands on a desktop nobody
// sees. The service is therefore only a supervisor: it keeps one copy of this
// exe -- the agent -- running *as the user logged on at the console*, in their
// session, where it owns the transceiver and injects keys exactly as it does
// when started by hand. It starts the agent at logon, moves it when the console
// session changes (user switch), and restarts it if it dies.
//
// The agent gets the user's own token from WTSQueryUserToken, so it has the
// same rights a logon task would give it and nothing more. That also means UIPI
// still keeps it out of elevated windows (volume excepted -- see audio.cpp).

#include "mceir.h"

#include <sddl.h>
#include <userenv.h>
#include <winsvc.h>
#include <wtsapi32.h>

#include <algorithm>

namespace {

wchar_t g_serviceName[] = L"mceir";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS        g_status       = {};

HANDLE g_serviceStop  = nullptr;   // the SCM asked us to stop
HANDLE g_sessionEvent = nullptr;   // the console session may have changed

// Session notifications, handed from the control handler to Supervise: the
// last session reported logging off, and the last reported logging on.
volatile LONG g_logoffSession = 0;
volatile LONG g_logonSession  = 0;

// The two handles the agent inherits: an event asking it to exit, and a handle
// on this process so that it exits if we die -- an orphan would keep holding
// the transceiver and the next agent could not open it. Inheriting them avoids
// a named object the user would need a special DACL to open, and a normal user
// could not open a SYSTEM process by id anyway.
HANDLE g_agentStop = nullptr;
HANDLE g_selfForAgent = nullptr;

std::wstring g_exePath;
std::wstring g_agentArgs;

// How long the agent gets to close the transceiver and exit before it is
// terminated.
constexpr DWORD kAgentExitWaitMs = 5000;

// Restart delay for an agent that failed to start or died, doubling while it
// keeps dying young.
constexpr ULONGLONG kRestartMinMs = 2000;
constexpr ULONGLONG kRestartMaxMs = 60000;
constexpr ULONGLONG kStableRunMs  = 30000;

// Session notifications are the main signal; this re-check catches a missed one.
constexpr DWORD kPollMs = 2000;

// Exit codes meaning the agent's session is being logged off: its processes
// are ended with DBG_TERMINATE_PROCESS, and one started meanwhile fails with
// STATUS_DLL_INIT_FAILED_LOGOFF ("the window station is shutting down").
constexpr DWORD kExitLogoffTerminate = 0x40010004;
constexpr DWORD kExitDllInitLogoff   = 0xC000026B;

// How long a session stays written off after looking like it was logging off,
// in case that reading was wrong and it is in fact still in use.
constexpr ULONGLONG kLogoffGraceMs = 60000;

void ReportStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHintMs = 0) {
    static DWORD checkpoint = 1;

    g_status.dwServiceType      = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState     = state;
    g_status.dwWin32ExitCode    = exitCode;
    g_status.dwWaitHint         = waitHintMs;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING
                                      ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN |
                                            SERVICE_ACCEPT_SESSIONCHANGE
                                      : 0;
    g_status.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI ControlHandler(DWORD control, DWORD eventType, LPVOID eventData, LPVOID) {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        // StopAgent can wait twice: for a clean exit, then after terminating.
        ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 2 * kAgentExitWaitMs + 1000);
        SetEvent(g_serviceStop);
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
        if (eventData) {
            const LONG session = static_cast<LONG>(
                static_cast<const WTSSESSION_NOTIFICATION*>(eventData)->dwSessionId);
            if (eventType == WTS_SESSION_LOGOFF)
                InterlockedExchange(&g_logoffSession, session);
            else if (eventType == WTS_SESSION_LOGON)
                InterlockedExchange(&g_logonSession, session);
        }
        SetEvent(g_sessionEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// The session with the physical keyboard and screen, or 0 while there is none
// (mid-switch). Session 0 itself never is.
DWORD ConsoleSession() {
    const DWORD s = WTSGetActiveConsoleSessionId();
    return s == 0xFFFFFFFF ? 0 : s;
}

std::wstring Quote(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

enum class StartResult { Started, NoUser, Failed };

StartResult StartAgent(DWORD session, HANDLE& agent) {
    agent = nullptr;

    HANDLE token = nullptr;
    if (!WTSQueryUserToken(session, &token)) {
        const DWORD err = GetLastError();
        if (err == ERROR_NO_TOKEN)
            return StartResult::NoUser;
        LOG("WTSQueryUserToken(%lu) failed: %s", session, FormatWin32Error(err).c_str());
        return StartResult::Failed;
    }

    // The user's environment, so the agent's %LOCALAPPDATA% is theirs.
    void* env = nullptr;
    if (!CreateEnvironmentBlock(&env, token, FALSE))
        env = nullptr;

    // Inherit exactly these two handles and nothing else of ours -- the open
    // log file included.
    HANDLE inherit[2] = { g_agentStop, g_selfForAgent };
    SIZE_T attrSize   = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());

    const bool attrsReady = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) != FALSE;
    bool       ok         = attrsReady &&
              UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                        sizeof(inherit), nullptr, nullptr) != FALSE;
    DWORD err = ok ? 0 : GetLastError();

    PROCESS_INFORMATION pi = {};
    if (ok) {
        std::wstring cmd =
            Quote(g_exePath) + L" --agent" +
            L" --stop-handle " + std::to_wstring(reinterpret_cast<uintptr_t>(g_agentStop)) +
            L" --parent-handle " + std::to_wstring(reinterpret_cast<uintptr_t>(g_selfForAgent)) +
            g_agentArgs;

        wchar_t        desktop[] = L"winsta0\\default";
        STARTUPINFOEXW si        = {};
        si.StartupInfo.cb        = sizeof(si);
        si.StartupInfo.lpDesktop = desktop;
        si.lpAttributeList       = attrs;

        DWORD flags = EXTENDED_STARTUPINFO_PRESENT;
        if (env)
            flags |= CREATE_UNICODE_ENVIRONMENT;

        ok = CreateProcessAsUserW(token, g_exePath.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                  flags, env, nullptr, &si.StartupInfo, &pi) != FALSE;
        if (!ok)
            err = GetLastError();   // before the cleanup below can overwrite it
    }
    if (attrsReady)
        DeleteProcThreadAttributeList(attrs);

    if (env)
        DestroyEnvironmentBlock(env);
    CloseHandle(token);

    if (!ok) {
        LOG("Could not start the agent in session %lu: %s", session,
            FormatWin32Error(err).c_str());
        return StartResult::Failed;
    }

    CloseHandle(pi.hThread);
    LOG("Agent started in session %lu (pid %lu).", session, pi.dwProcessId);
    agent = pi.hProcess;
    return StartResult::Started;
}

void StopAgent(HANDLE agent) {
    SetEvent(g_agentStop);
    if (WaitForSingleObject(agent, kAgentExitWaitMs) != WAIT_OBJECT_0) {
        LOG("The agent did not exit in time; terminating it.");
        TerminateProcess(agent, 1);
        WaitForSingleObject(agent, kAgentExitWaitMs);
    }
    ResetEvent(g_agentStop);
    CloseHandle(agent);
}

void Supervise() {
    HANDLE    agent           = nullptr;
    DWORD     agentSession    = 0;
    ULONGLONG agentStarted    = 0;
    ULONGLONG restartDelay    = kRestartMinMs;
    ULONGLONG nextStart       = 0;
    bool      announcedNoUser = false;

    // A session being logged off. Its processes are being torn down, so an
    // agent started there dies at once, or grabs the transceiver and is then
    // killed -- and WTSQueryUserToken keeps handing out the user's token until
    // the logoff completes, so it has to be tracked here.
    DWORD     endingSession = 0;
    ULONGLONG endingSince   = 0;

    for (;;) {
        const DWORD console = ConsoleSession();

        if (const LONG s = InterlockedExchange(&g_logoffSession, 0)) {
            endingSession = static_cast<DWORD>(s);
            endingSince   = GetTickCount64();
        }
        if (const LONG s = InterlockedExchange(&g_logonSession, 0)) {
            if (static_cast<DWORD>(s) == endingSession)
                endingSession = 0;
        }
        if (endingSession &&
            (console != endingSession || GetTickCount64() - endingSince >= kLogoffGraceMs))
            endingSession = 0;

        // Only one process can hold the transceiver, so the old agent has to be
        // gone before the new one starts.
        if (agent && console != agentSession) {
            LOG("Console session changed (%lu -> %lu); moving the agent.", agentSession, console);
            StopAgent(agent);
            agent        = nullptr;
            nextStart    = 0;
            restartDelay = kRestartMinMs;
        }

        if (!agent && console != 0 && console != endingSession && GetTickCount64() >= nextStart) {
            switch (StartAgent(console, agent)) {
            case StartResult::Started:
                agentSession    = console;
                agentStarted    = GetTickCount64();
                announcedNoUser = false;
                break;
            case StartResult::NoUser:
                // Nobody is logged on at the console yet. Not a failure, so no
                // backoff: the logon notification, or the next poll, retries.
                if (!announcedNoUser) {
                    LOG("Waiting for a user to log on at the console.");
                    announcedNoUser = true;
                }
                break;
            case StartResult::Failed:
                nextStart    = GetTickCount64() + restartDelay;
                restartDelay = (std::min)(restartDelay * 2, kRestartMaxMs);
                break;
            }
        }

        HANDLE      waits[3] = { g_serviceStop, g_sessionEvent, agent };
        const DWORD w        = WaitForMultipleObjects(agent ? 3 : 2, waits, FALSE, kPollMs);

        if (w == WAIT_OBJECT_0)
            break;

        if (w == WAIT_OBJECT_0 + 1)
            nextStart = 0;   // a logon is worth trying straight away

        if (w == WAIT_OBJECT_0 + 2) {
            DWORD code = 0;
            GetExitCodeProcess(agent, &code);
            CloseHandle(agent);
            agent = nullptr;

            // Logoff ends the agent along with the rest of the session. Starting
            // another one there would only be killed the same way, so wait for
            // the next logon, which starts one in the new session.
            if (code == kExitLogoffTerminate || code == kExitDllInitLogoff) {
                LOG("The agent ended with session %lu (code 0x%lX); waiting for the next logon.",
                    agentSession, code);
                endingSession = agentSession;
                endingSince   = GetTickCount64();
                nextStart     = 0;
                restartDelay  = kRestartMinMs;
                continue;
            }

            const ULONGLONG ran = GetTickCount64() - agentStarted;
            if (ran >= kStableRunMs)
                restartDelay = kRestartMinMs;
            LOG("The agent exited (code 0x%lX) after %llu s.", code, ran / 1000);
            nextStart    = GetTickCount64() + restartDelay;
            restartDelay = (std::min)(restartDelay * 2, kRestartMaxMs);
        }
    }

    if (agent)
        StopAgent(agent);
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(g_serviceName, ControlHandler, nullptr);
    if (!g_statusHandle) {
        LOG("RegisterServiceCtrlHandlerEx failed: %s", FormatWin32Error(GetLastError()).c_str());
        return;
    }
    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    SECURITY_ATTRIBUTES inheritable = { sizeof(inheritable), nullptr, TRUE };

    g_serviceStop  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_sessionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_agentStop    = CreateEventW(&inheritable, TRUE, FALSE, nullptr);
    const BOOL self = DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
                                      &g_selfForAgent, SYNCHRONIZE, TRUE, 0);
    if (!g_serviceStop || !g_sessionEvent || !g_agentStop || !self) {
        const DWORD err = GetLastError();
        LOG("Could not create the service's handles: %s", FormatWin32Error(err).c_str());
        ReportStatus(SERVICE_STOPPED, err);
        return;
    }

    ReportStatus(SERVICE_RUNNING);
    LOG("mceir service running.");

    Supervise();

    LOG("mceir service stopped.");
    ReportStatus(SERVICE_STOPPED);
}

// %ProgramData%\mceir, for the service's own log. This process is SYSTEM and
// rotates its log by renaming it, so the directory must not be writable by
// ordinary users -- the ACL %ProgramData% would hand down lets them create
// files, and links, in it.
std::wstring ServiceLogDir() {
    wchar_t     buf[MAX_PATH] = {};
    const DWORD n             = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L"C:\\ProgramData");
    dir += L"\\mceir";

    // SYSTEM and Administrators: full control. Users: read.
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)", SDDL_REVISION_1,
            &sa.lpSecurityDescriptor, nullptr)) {
        CreateDirectoryW(dir.c_str(), &sa);
        LocalFree(sa.lpSecurityDescriptor);
    }
    return dir;
}

}  // namespace

int RunService(const std::wstring& exePath, const std::wstring& agentArgs) {
    g_exePath   = exePath;
    g_agentArgs = agentArgs;

    if (g_logLevel < LogLevel::Normal)
        g_logLevel = LogLevel::Normal;
    OpenLog(ServiceLogDir() + L"\\service.log");

    SERVICE_TABLE_ENTRYW table[] = { { g_serviceName, ServiceMain }, { nullptr, nullptr } };
    if (!StartServiceCtrlDispatcherW(table)) {
        const DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            LOG("--service is for the Service Control Manager; install with install-service.ps1.");
        else
            LOG("StartServiceCtrlDispatcher failed: %s", FormatWin32Error(err).c_str());
        return 1;
    }
    return 0;
}
