// device.cpp - WinUSB discovery, pipe setup and I/O for the MCE transceiver.

#include <initguid.h>

#include "mceir.h"

#include <cfgmgr32.h>
#include <winusbio.h>

#include <algorithm>
#include <cstdarg>

// Must match DeviceInterfaceGUIDs in mceir.inf.
DEFINE_GUID(GUID_DEVINTERFACE_MCEIR,
            0x65216fe6, 0x9e29, 0x41cb, 0x9a, 0xbd, 0x4c, 0xbe, 0x9c, 0xad, 0x8a, 0xa4);

namespace {

constexpr UCHAR kDirectionIn = 0x80;

// ---------------------------------------------------------------------------
// MCE wire protocol constants.
//
// Only the receive side is implemented. Values follow the MCE transceiver
// protocol as documented by the Linux mceusb driver; the port byte selects
// which subsystem of the firmware a command is aimed at.
// ---------------------------------------------------------------------------

constexpr uint8_t kPortIr          = 0x9F;
constexpr uint8_t kPortSys         = 0xFF;
constexpr uint8_t kCmdNull         = 0x00;
constexpr uint8_t kCmdResume       = 0xAA;
constexpr uint8_t kCmdGetRevision  = 0x0B;
constexpr uint8_t kCmdGetRxTimeout = 0x0D;
constexpr uint8_t kCmdSetRxTimeout = 0x0C;
constexpr uint8_t kCmdGetRxSensor  = 0x15;

// The device expresses the receive timeout in the same 50 us unit it uses for
// sample durations.
constexpr uint32_t kTimeUnitUs = 50;

const char* PipeTypeName(USBD_PIPE_TYPE t) {
    switch (t) {
    case UsbdPipeTypeControl:     return "control";
    case UsbdPipeTypeIsochronous: return "isochronous";
    case UsbdPipeTypeBulk:        return "bulk";
    case UsbdPipeTypeInterrupt:   return "interrupt";
    default:                      return "unknown";
    }
}

}  // namespace

MceDevice::~MceDevice() {
    Close();
}

bool MceDevice::Fail(const char* fmt, ...) {
    char    buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_error = buf;
    return false;
}

bool MceDevice::Open() {
    m_error.clear();

    ULONG     listLen = 0;
    CONFIGRET cr      = CM_Get_Device_Interface_List_SizeW(
        &listLen, const_cast<LPGUID>(&GUID_DEVINTERFACE_MCEIR), nullptr,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS)
        return Fail("Could not size the device interface list (CONFIGRET 0x%lX).", cr);

    std::vector<wchar_t> list(listLen);
    cr = CM_Get_Device_Interface_ListW(
        const_cast<LPGUID>(&GUID_DEVINTERFACE_MCEIR), nullptr, list.data(), listLen,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS)
        return Fail("Could not read the device interface list (CONFIGRET 0x%lX).", cr);

    // A multi-sz; an empty first string means nothing is present.
    if (list.empty() || list[0] == L'\0')
        return Fail("No MCE transceiver found: our device interface is not registered. "
                    "If it is plugged in, mceir.inf is not bound to it.");

    m_path = list.data();
    LOGV("Device path: %ls", m_path.c_str());

    m_file = CreateFileW(m_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (m_file == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return Fail("Could not open the device: %s%s", FormatWin32Error(err).c_str(),
                    err == ERROR_ACCESS_DENIED
                        ? " Another process already has the transceiver open."
                        : "");
    }

    if (!WinUsb_Initialize(m_file, &m_winusb)) {
        Fail("WinUsb_Initialize failed: %s", FormatWin32Error(GetLastError()).c_str());
        Close();
        return false;
    }

    if (!FindPipes()) {
        Close();
        return false;
    }

    m_ovEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_ovEvent) {
        Fail("CreateEvent failed: %s", FormatWin32Error(GetLastError()).c_str());
        Close();
        return false;
    }

    m_buf.resize(std::max<size_t>(64, static_cast<size_t>(m_maxPacketIn) * 4));
    return true;
}

bool MceDevice::FindPipes() {
    USB_INTERFACE_DESCRIPTOR iface = {};
    if (!WinUsb_QueryInterfaceSettings(m_winusb, 0, &iface))
        return Fail("WinUsb_QueryInterfaceSettings failed: %s",
                    FormatWin32Error(GetLastError()).c_str());

    LOGV("Interface 0: %u endpoint(s), class 0x%02X subclass 0x%02X protocol 0x%02X",
         iface.bNumEndpoints, iface.bInterfaceClass, iface.bInterfaceSubClass,
         iface.bInterfaceProtocol);

    bool haveIn = false, haveOut = false;

    for (UCHAR i = 0; i < iface.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipe = {};
        if (!WinUsb_QueryPipe(m_winusb, 0, i, &pipe)) {
            LOGV("  WinUsb_QueryPipe(%u) failed: %s", i,
                 FormatWin32Error(GetLastError()).c_str());
            continue;
        }

        const bool in = (pipe.PipeId & kDirectionIn) != 0;
        LOGV("  endpoint 0x%02X  %-11s  max packet %u  interval %u", pipe.PipeId,
             PipeTypeName(pipe.PipeType), pipe.MaximumPacketSize, pipe.Interval);

        // Transceivers in this family use interrupt endpoints on some firmware
        // revisions and bulk on others, so accept either.
        if (pipe.PipeType != UsbdPipeTypeBulk && pipe.PipeType != UsbdPipeTypeInterrupt)
            continue;

        if (in && !haveIn) {
            m_pipeIn      = pipe.PipeId;
            m_maxPacketIn = pipe.MaximumPacketSize;
            haveIn        = true;
        } else if (!in && !haveOut) {
            m_pipeOut = pipe.PipeId;
            haveOut   = true;
        }
    }

    if (!haveIn)
        return Fail("The device exposes no bulk or interrupt IN endpoint; cannot receive.");
    if (!haveOut)
        LOG("No OUT endpoint found; initialisation commands will be skipped.");

    LOG("Transceiver ready: IN 0x%02X%s", m_pipeIn, haveOut ? "" : " (receive only)");

    // Our own wait loop provides the timeout, so the pipe itself must not time
    // out and cancel a read that is simply waiting for a key press.
    ULONG zero = 0;
    WinUsb_SetPipePolicy(m_winusb, m_pipeIn, PIPE_TRANSFER_TIMEOUT, sizeof(zero), &zero);

    // A short packet is a complete IR burst, not a truncated read: let it
    // satisfy the request immediately instead of waiting to fill the buffer.
    UCHAR off = FALSE, on = TRUE;
    WinUsb_SetPipePolicy(m_winusb, m_pipeIn, IGNORE_SHORT_PACKETS, sizeof(off), &off);
    WinUsb_SetPipePolicy(m_winusb, m_pipeIn, ALLOW_PARTIAL_READS, sizeof(on), &on);
    WinUsb_SetPipePolicy(m_winusb, m_pipeIn, AUTO_CLEAR_STALL, sizeof(on), &on);

    if (haveOut) {
        ULONG writeTimeout = 1000;
        WinUsb_SetPipePolicy(m_winusb, m_pipeOut, PIPE_TRANSFER_TIMEOUT,
                             sizeof(writeTimeout), &writeTimeout);
        WinUsb_SetPipePolicy(m_winusb, m_pipeOut, AUTO_CLEAR_STALL, sizeof(on), &on);
    }

    return true;
}

bool MceDevice::WriteCommand(const uint8_t* data, size_t len, const char* what) {
    if (!m_pipeOut)
        return false;

    OVERLAPPED ov = {};
    ov.hEvent     = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        return false;

    ULONG transferred = 0;
    bool  ok = WinUsb_WritePipe(m_winusb, m_pipeOut, const_cast<PUCHAR>(data),
                                static_cast<ULONG>(len), &transferred, &ov) != FALSE;
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = WinUsb_GetOverlappedResult(m_winusb, &ov, &transferred, TRUE) != FALSE;

    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(ov.hEvent);

    if (!ok) {
        LOGV("  %s: write failed: %s", what, FormatWin32Error(err).c_str());
        return false;
    }

    if (g_logLevel >= LogLevel::Trace) {
        std::string hex;
        char        b[4];
        for (size_t i = 0; i < len; ++i) {
            sprintf_s(b, sizeof(b), "%02X ", data[i]);
            hex += b;
        }
        LOGT("  -> %-16s %s", what, hex.c_str());
    }
    return true;
}

bool MceDevice::Initialize(bool setRxTimeout, uint32_t rxTimeout_us) {
    if (!m_pipeOut) {
        LOGV("Skipping initialisation: no OUT endpoint.");
        return true;
    }

    // Bring the firmware out of any low-power state before asking it anything.
    // The queries are not strictly needed to receive, but their replies are the
    // first proof in the log that the command channel works at all.
    const uint8_t resume[]      = { kCmdNull, kPortSys, kCmdResume };
    const uint8_t getRevision[] = { kPortSys, kCmdGetRevision };
    const uint8_t getTimeout[]  = { kPortIr, kCmdGetRxTimeout };
    const uint8_t getSensor[]   = { kPortIr, kCmdGetRxSensor };

    WriteCommand(resume, sizeof(resume), "RESUME");
    WriteCommand(getRevision, sizeof(getRevision), "GET_REVISION");
    WriteCommand(getTimeout, sizeof(getTimeout), "GET_RX_TIMEOUT");
    WriteCommand(getSensor, sizeof(getSensor), "GET_RX_SENSOR");

    if (setRxTimeout) {
        // How long a silence the firmware waits through before declaring the
        // burst over. Too short truncates a frame mid-flight; too long delays
        // every key. 100 ms sits well clear of the RC-6 trailer.
        uint32_t units = rxTimeout_us / kTimeUnitUs;
        if (units > 0xFFFF)
            units = 0xFFFF;
        const uint8_t setTimeout[] = {
            kPortIr,
            kCmdSetRxTimeout,
            static_cast<uint8_t>((units >> 8) & 0xFF),
            static_cast<uint8_t>(units & 0xFF),
        };
        WriteCommand(setTimeout, sizeof(setTimeout), "SET_RX_TIMEOUT");
    }

    return true;
}

MceDevice::ReadResult MceDevice::Read(std::vector<uint8_t>& out, DWORD timeoutMs,
                                      HANDLE stopEvent) {
    out.clear();

    if (!m_pending) {
        ResetEvent(m_ovEvent);
        ZeroMemory(&m_ov, sizeof(m_ov));
        m_ov.hEvent = m_ovEvent;

        ULONG transferred = 0;
        if (WinUsb_ReadPipe(m_winusb, m_pipeIn, m_buf.data(),
                            static_cast<ULONG>(m_buf.size()), &transferred, &m_ov)) {
            out.assign(m_buf.begin(), m_buf.begin() + transferred);
            return ReadResult::Data;
        }

        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            LOG("Read failed: %s", FormatWin32Error(err).c_str());
            return ReadResult::Error;
        }
        m_pending = true;
    }

    HANDLE waits[2] = { m_ovEvent, stopEvent };
    DWORD  n        = stopEvent ? 2 : 1;
    DWORD  w        = WaitForMultipleObjects(n, waits, FALSE, timeoutMs);

    switch (w) {
    case WAIT_OBJECT_0: {
        m_pending         = false;
        ULONG transferred = 0;
        if (!WinUsb_GetOverlappedResult(m_winusb, &m_ov, &transferred, FALSE)) {
            DWORD err = GetLastError();
            // A cancelled read is the expected outcome of shutting down.
            if (err == ERROR_OPERATION_ABORTED)
                return ReadResult::Stopped;
            LOG("Read completion failed: %s", FormatWin32Error(err).c_str());
            return ReadResult::Error;
        }
        out.assign(m_buf.begin(), m_buf.begin() + transferred);
        return ReadResult::Data;
    }
    case WAIT_OBJECT_0 + 1:
        return ReadResult::Stopped;
    case WAIT_TIMEOUT:
        // The read stays in flight deliberately: reissuing it every tick would
        // drop whatever arrived in the gap.
        return ReadResult::Timeout;
    default:
        LOG("Wait failed: %s", FormatWin32Error(GetLastError()).c_str());
        return ReadResult::Error;
    }
}

void MceDevice::Close() {
    if (m_winusb) {
        if (m_pending) {
            WinUsb_AbortPipe(m_winusb, m_pipeIn);
            ULONG transferred = 0;
            WinUsb_GetOverlappedResult(m_winusb, &m_ov, &transferred, TRUE);
            m_pending = false;
        }
        WinUsb_Free(m_winusb);
        m_winusb = nullptr;
    }
    if (m_ovEvent) {
        CloseHandle(m_ovEvent);
        m_ovEvent = nullptr;
    }
    if (m_file != INVALID_HANDLE_VALUE) {
        CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
    }
}
