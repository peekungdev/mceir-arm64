// keymap_test.cpp - key names and config-file parsing for the key map.

#include "../src/mceir.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <string>

// --- Stubs for what keymap.cpp links against --------------------------------

LogLevel g_logLevel = LogLevel::Quiet;   // the parser's complaints are expected here

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

// InjectKey reaches for this; nothing here injects.
bool TryVolumeKey(WORD) {
    return false;
}

// --- Harness ----------------------------------------------------------------

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        ++g_failures;
}

WORD VkOf(const Keymap& map, uint32_t scancode) {
    const KeyBinding* b = map.Find(scancode);
    return b ? b->vk : 0xFFFF;
}

}  // namespace

int main() {
    printf("Key names\n\n");

    Check("F5", ParseVirtualKey("F5") == VK_F5);
    Check("f24, lower case", ParseVirtualKey("f24") == VK_F24);
    Check("F25 rejected", ParseVirtualKey("F25") == 0xFFFF);
    Check("F1X rejected, not read as F1", ParseVirtualKey("F1X") == 0xFFFF);
    Check("bare F is the letter", ParseVirtualKey("F") == 'F');
    Check("NONE unbinds", ParseVirtualKey("none") == 0);
    Check("unknown name rejected", ParseVirtualKey("BOGUS") == 0xFFFF);

    printf("\nConfig file\n\n");

    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"mceir_keymap_test.conf";
    {
        std::ofstream f(path);
        f << "# a comment line\n"
             "800f040c = NONE              # Power unbound\n"
             "0x800F0426 = F9 norepeat     # 0x prefix\n"
             "800f041e = UP norepeat\n"
             "800f0422z = F6               # trailing garbage\n"
             "-1 = F7                      # a sign\n"
             "800f0423 = F1X               # bad key name\n"
             "1800f0400 = A                # more than 32 bits\n"
             "12345678 = A repeat          # a new button\n";
    }

    Keymap map;
    map.LoadOverrides(path);
    DeleteFileW(path.c_str());

    Check("NONE applied (Power unbound)", VkOf(map, 0x800F040C) == 0);
    Check("0x prefix accepted", VkOf(map, 0x800F0426) == VK_F9);
    Check("norepeat applied", map.Find(0x800F041E) && !map.Find(0x800F041E)->repeats);
    Check("trailing garbage rejected (OK unchanged)", VkOf(map, 0x800F0422) == VK_RETURN);
    Check("signed value rejected", map.Find(0xFFFFFFFF) == nullptr);
    Check("bad key name rejected (Back unchanged)", VkOf(map, 0x800F0423) == VK_ESCAPE);
    Check("over 32 bits rejected (Num0 unchanged)", VkOf(map, 0x800F0400) == '0');
    Check("new button added, repeating",
          VkOf(map, 0x12345678) == 'A' && map.Find(0x12345678)->repeats);

    printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES ABOVE.");
    return g_failures == 0 ? 0 : 1;
}
