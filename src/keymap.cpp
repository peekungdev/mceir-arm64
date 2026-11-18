// keymap.cpp - MCE button table, config overrides, and key injection.

#include "mceir.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

struct BuiltinBinding {
    uint32_t    scancode;
    const char* name;
    WORD        vk;
    bool        repeats;
};

// Buttons on a standard Media Center remote. The scancodes are the RC-6 mode
// 6A payload with Microsoft's 0x800F customer code and the toggle stripped.
//
// Every button sends something. The ones with no natural desktop equivalent
// get function keys (the "jump to a section" buttons) or mnemonic letters (the
// modal ones), all of which are easy to rebind in whatever application is
// listening. A vk of 0 still means "recognised but unbound", which is what a
// config line of NONE produces.
//
// Power sends VK_SLEEP, so a stray infrared reflection really can suspend the
// machine. To stop that without losing the button, put this in mceir.conf:
//     800f040c = NONE
const BuiltinBinding kBuiltins[] = {
    { 0x800F0400, "Num0",        '0',                  false },
    { 0x800F0401, "Num1",        '1',                  false },
    { 0x800F0402, "Num2",        '2',                  false },
    { 0x800F0403, "Num3",        '3',                  false },
    { 0x800F0404, "Num4",        '4',                  false },
    { 0x800F0405, "Num5",        '5',                  false },
    { 0x800F0406, "Num6",        '6',                  false },
    { 0x800F0407, "Num7",        '7',                  false },
    { 0x800F0408, "Num8",        '8',                  false },
    { 0x800F0409, "Num9",        '9',                  false },
    { 0x800F040A, "Clear",       VK_BACK,              false },
    { 0x800F040B, "Enter",       VK_RETURN,            false },
    { 0x800F040C, "Power",       VK_SLEEP,                    false },
    { 0x800F040D, "Start",       VK_LWIN,                    false },
    { 0x800F040E, "Mute",        VK_VOLUME_MUTE,       false },
    { 0x800F040F, "Info",        'I',                    false },
    { 0x800F0410, "VolumeUp",    VK_VOLUME_UP,         true  },
    { 0x800F0411, "VolumeDown",  VK_VOLUME_DOWN,       true  },
    { 0x800F0412, "ChannelUp",   VK_PRIOR,             true  },
    { 0x800F0413, "ChannelDown", VK_NEXT,              true  },
    { 0x800F0414, "Forward",     VK_MEDIA_NEXT_TRACK,  true  },
    { 0x800F0415, "Rewind",      VK_MEDIA_PREV_TRACK,  true  },
    { 0x800F0416, "Play",        VK_MEDIA_PLAY_PAUSE,  false },
    { 0x800F0417, "Record",      'R',                    false },
    { 0x800F0418, "Pause",       VK_MEDIA_PLAY_PAUSE,  false },
    { 0x800F0419, "Stop",        VK_MEDIA_STOP,        false },
    { 0x800F041A, "Skip",        VK_MEDIA_NEXT_TRACK,  false },
    { 0x800F041B, "Replay",      VK_MEDIA_PREV_TRACK,  false },
    { 0x800F041C, "Hash",        'N',                    false },
    { 0x800F041D, "Star",        'F',                    false },
    { 0x800F041E, "Up",          VK_UP,                true  },
    { 0x800F041F, "Down",        VK_DOWN,              true  },
    { 0x800F0420, "Left",        VK_LEFT,              true  },
    { 0x800F0421, "Right",       VK_RIGHT,             true  },
    { 0x800F0422, "OK",          VK_RETURN,            false },
    { 0x800F0423, "Back",        VK_ESCAPE,            false },
    { 0x800F0424, "DVDMenu",     VK_F12,                    false },
    { 0x800F0425, "LiveTV",      VK_F11,                    false },
    { 0x800F0426, "Guide",       VK_F5,                    false },
    { 0x800F0427, "Aspect",      'Z',                    false },
    { 0x800F0432, "Visualize",   'V',                    false },
    { 0x800F0433, "Slideshow",   'W',                    false },
    { 0x800F0434, "Eject",       'E',                    false },
    { 0x800F0446, "TV",          VK_F6,                    false },
    { 0x800F0447, "Music",       VK_F7,                    false },
    { 0x800F0448, "RecordedTV",  VK_F8,                    false },
    { 0x800F0449, "Pictures",    VK_F9,                    false },
    { 0x800F044A, "Videos",      VK_F10,                    false },
    { 0x800F044D, "Title",       'T',                    false },
    { 0x800F0450, "Radio",       'K',                    false },
    { 0x800F045A, "Subtitle",    'C',                    false },
    { 0x800F045B, "Red",         VK_F1,                    false },
    { 0x800F045C, "Green",       VK_F2,                    false },
    { 0x800F045D, "Yellow",      VK_F3,                    false },
    { 0x800F045E, "Blue",        VK_F4,                    false },
    { 0x800F046E, "PlayPause",   VK_MEDIA_PLAY_PAUSE,  false },
    { 0x800F0481, "PlayPause2",  VK_MEDIA_PLAY_PAUSE,  false },
};

struct VkName {
    const char* name;
    WORD        vk;
};

const VkName kVkNames[] = {
    { "NONE",                0                    },
    { "UP",                  VK_UP                },
    { "DOWN",                VK_DOWN              },
    { "LEFT",                VK_LEFT              },
    { "RIGHT",               VK_RIGHT             },
    { "ENTER",               VK_RETURN            },
    { "RETURN",              VK_RETURN            },
    { "ESCAPE",              VK_ESCAPE            },
    { "ESC",                 VK_ESCAPE            },
    { "BACKSPACE",           VK_BACK              },
    { "TAB",                 VK_TAB               },
    { "SPACE",               VK_SPACE             },
    { "HOME",                VK_HOME              },
    { "END",                 VK_END               },
    { "PAGEUP",              VK_PRIOR             },
    { "PAGEDOWN",            VK_NEXT              },
    { "INSERT",              VK_INSERT            },
    { "DELETE",              VK_DELETE            },
    { "LWIN",                VK_LWIN              },
    { "RWIN",                VK_RWIN              },
    { "APPS",                VK_APPS              },
    { "SLEEP",               VK_SLEEP             },
    { "VOLUME_UP",           VK_VOLUME_UP         },
    { "VOLUME_DOWN",         VK_VOLUME_DOWN       },
    { "VOLUME_MUTE",         VK_VOLUME_MUTE       },
    { "MEDIA_PLAY_PAUSE",    VK_MEDIA_PLAY_PAUSE  },
    { "MEDIA_STOP",          VK_MEDIA_STOP        },
    { "MEDIA_NEXT_TRACK",    VK_MEDIA_NEXT_TRACK  },
    { "MEDIA_PREV_TRACK",    VK_MEDIA_PREV_TRACK  },
    { "LAUNCH_MEDIA_SELECT", VK_LAUNCH_MEDIA_SELECT },
    { "BROWSER_BACK",        VK_BROWSER_BACK      },
    { "BROWSER_FORWARD",     VK_BROWSER_FORWARD   },
    { "BROWSER_HOME",        VK_BROWSER_HOME      },
};

std::string ToUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// A whole string of hex digits, with an optional 0x prefix, as a 32-bit value.
// std::stoul stopped quietly at the first bad character ("800f0422z" became
// OK's scancode) and took a sign ("-1" became 0xFFFFFFFF).
bool ParseScancode(const std::string& s, uint32_t& out) {
    size_t i = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        i = 2;
    if (i == s.size() || s.size() - i > 8)
        return false;

    uint32_t v = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        uint32_t   d;
        if (c >= '0' && c <= '9')      d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

// Keys whose scan code carries the extended-key flag. Getting this wrong makes
// arrows behave like their numeric-keypad twins in some applications.
bool IsExtendedKey(WORD vk) {
    switch (vk) {
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_INSERT: case VK_DELETE:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
    case VK_VOLUME_UP: case VK_VOLUME_DOWN: case VK_VOLUME_MUTE:
    case VK_MEDIA_PLAY_PAUSE: case VK_MEDIA_STOP:
    case VK_MEDIA_NEXT_TRACK: case VK_MEDIA_PREV_TRACK:
    case VK_LAUNCH_MEDIA_SELECT:
    case VK_BROWSER_BACK: case VK_BROWSER_FORWARD: case VK_BROWSER_HOME:
        return true;
    default:
        return false;
    }
}

}  // namespace

WORD ParseVirtualKey(const std::string& raw) {
    const std::string name = ToUpper(Trim(raw));
    if (name.empty())
        return 0xFFFF;

    for (const auto& e : kVkNames) {
        if (name == e.name)
            return e.vk;
    }

    // A bare letter or digit means that key.
    if (name.size() == 1) {
        char c = name[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return static_cast<WORD>(c);
    }

    // F1 through F24. The digits must be the whole rest of the name: atoi alone
    // read "F1X" as F1.
    if (name.size() >= 2 && name.size() <= 3 && name[0] == 'F' &&
        name.find_first_not_of("0123456789", 1) == std::string::npos) {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24)
            return static_cast<WORD>(VK_F1 + n - 1);
    }

    return 0xFFFF;
}

std::string VirtualKeyName(WORD vk) {
    if (vk == 0)
        return "NONE";
    for (const auto& e : kVkNames) {
        if (e.vk == vk && e.vk != 0)
            return e.name;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24)
        return "F" + std::to_string(vk - VK_F1 + 1);

    char buf[16];
    sprintf_s(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

Keymap::Keymap() {
    m_bindings.reserve(sizeof(kBuiltins) / sizeof(kBuiltins[0]));
    for (const auto& b : kBuiltins)
        m_bindings.push_back(KeyBinding{ b.scancode, b.name, b.vk, b.repeats });
}

const KeyBinding* Keymap::Find(uint32_t scancode) const {
    for (const auto& b : m_bindings) {
        if (b.scancode == scancode)
            return &b;
    }
    return nullptr;
}

bool Keymap::LoadOverrides(const std::wstring& path) {
    std::ifstream in(path);
    if (!in) {
        LOGV("No config file at %ls; using the built-in map.", path.c_str());
        return false;
    }

    LOG("Loading key overrides from %ls", path.c_str());

    std::string line;
    int         lineNo = 0, applied = 0;

    while (std::getline(in, line)) {
        ++lineNo;

        // Strip comments, then whitespace.
        size_t hash = line.find_first_of("#;");
        if (hash != std::string::npos)
            line.erase(hash);
        line = Trim(line);
        if (line.empty())
            continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            LOG("  %ls:%d: no '=' on this line, skipped.", path.c_str(), lineNo);
            continue;
        }

        const std::string lhs = Trim(line.substr(0, eq));
        std::istringstream rhs(Trim(line.substr(eq + 1)));

        std::string keyName, repeatWord;
        rhs >> keyName >> repeatWord;

        uint32_t scancode = 0;
        if (!ParseScancode(lhs, scancode)) {
            LOG("  %ls:%d: '%s' is not a hex scancode, skipped.", path.c_str(), lineNo,
                lhs.c_str());
            continue;
        }

        const WORD vk = ParseVirtualKey(keyName);
        if (vk == 0xFFFF) {
            LOG("  %ls:%d: unknown key name '%s', skipped.", path.c_str(), lineNo,
                keyName.c_str());
            continue;
        }

        bool repeats = false;
        bool repeatSpecified = false;
        if (!repeatWord.empty()) {
            const std::string w = ToUpper(repeatWord);
            if (w == "REPEAT") {
                repeats = true;
                repeatSpecified = true;
            } else if (w == "NOREPEAT") {
                repeats = false;
                repeatSpecified = true;
            } else {
                LOG("  %ls:%d: expected 'repeat' or 'norepeat', got '%s'; ignoring it.",
                    path.c_str(), lineNo, repeatWord.c_str());
            }
        }

        auto it = std::find_if(m_bindings.begin(), m_bindings.end(),
                               [&](const KeyBinding& b) { return b.scancode == scancode; });
        if (it != m_bindings.end()) {
            it->vk = vk;
            if (repeatSpecified)
                it->repeats = repeats;
        } else {
            m_bindings.push_back(KeyBinding{ scancode, "Custom", vk, repeats });
        }
        ++applied;
    }

    LOG("  %d override(s) applied.", applied);
    return true;
}

void Keymap::DumpTo(FILE* f) const {
    fprintf(f, "%-12s  %-12s  %-20s  %s\n", "SCANCODE", "BUTTON", "SENDS", "REPEATS");
    for (const auto& b : m_bindings) {
        fprintf(f, "%08X      %-12s  %-20s  %s\n", b.scancode, b.name.c_str(),
                VirtualKeyName(b.vk).c_str(), b.repeats ? "yes" : "no");
    }
}

void InjectKey(WORD vk) {
    if (vk == 0)
        return;

    // Volume goes straight to the audio endpoint, so it keeps working even
    // when an elevated window has focus and SendInput is refused.
    if (TryVolumeKey(vk))
        return;

    INPUT in[2] = {};
    const WORD scan  = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    const DWORD ext  = IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0;

    in[0].type           = INPUT_KEYBOARD;
    in[0].ki.wVk         = vk;
    in[0].ki.wScan       = scan;
    in[0].ki.dwFlags     = ext;

    in[1]                = in[0];
    in[1].ki.dwFlags     = ext | KEYEVENTF_KEYUP;

    const UINT sent = SendInput(2, in, sizeof(INPUT));
    if (sent == 2)
        return;

    const DWORD err = GetLastError();

    // UIPI refuses the whole call when the foreground window belongs to a
    // higher-integrity process. It is worth saying so plainly, and only once,
    // because the symptom is "the remote randomly stops working".
    static bool explained = false;
    if (err == ERROR_ACCESS_DENIED && !explained) {
        explained = true;
        LOG("Keystrokes are being refused because the focused window belongs to an");
        LOG("elevated process. Windows blocks input injection from a normal process");
        LOG("into an elevated one, so the remote will not drive that window. Volume");
        LOG("still works. Only an elevated mceir -- install-autostart.ps1 -Elevated --");
        LOG("can drive elevated windows; the service's agent runs with normal rights.");
    }
    LOGV("SendInput sent %u of 2 events: %s", sent, FormatWin32Error(err).c_str());
}
