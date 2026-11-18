// inject_probe.cpp - works out why the volume keys do not take.
//
// Reads the master volume through Core Audio, injects a volume key three
// different ways, and reports which of them actually moved it. Restores the
// original level before exiting.

#include <windows.h>

#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <cstdio>

namespace {

IAudioEndpointVolume* g_vol = nullptr;

bool OpenVolume() {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))))
        return false;

    IMMDevice* device = nullptr;
    HRESULT    hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    enumerator->Release();
    if (FAILED(hr))
        return false;

    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&g_vol));
    device->Release();
    return SUCCEEDED(hr);
}

float Level() {
    float v = -1.0f;
    if (g_vol)
        g_vol->GetMasterVolumeLevelScalar(&v);
    return v;
}

void SetLevel(float v) {
    if (g_vol)
        g_vol->SetMasterVolumeLevelScalar(v, nullptr);
}

// Exactly what keymap.cpp does today.
void InjectCurrent(WORD vk, bool extended) {
    INPUT       in[2] = {};
    const WORD  scan  = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    const DWORD ext   = extended ? KEYEVENTF_EXTENDEDKEY : 0;

    in[0].type       = INPUT_KEYBOARD;
    in[0].ki.wVk     = vk;
    in[0].ki.wScan   = scan;
    in[0].ki.dwFlags = ext;

    in[1]            = in[0];
    in[1].ki.dwFlags = ext | KEYEVENTF_KEYUP;

    UINT sent = SendInput(2, in, sizeof(INPUT));
    printf("      SendInput returned %u (scan=0x%02X, extended=%d)\n", sent, scan,
           extended ? 1 : 0);
}

void Try(const char* label, WORD vk, void (*fn)(WORD)) {
    const float before = Level();
    fn(vk);
    Sleep(400);
    const float after = Level();
    printf("   %-38s %.3f -> %.3f   %s\n", label, before, after,
           (before != after) ? "MOVED" : "no change");
}

void VariantExtended(WORD vk)   { InjectCurrent(vk, true); }
void VariantPlain(WORD vk)      { InjectCurrent(vk, false); }

void VariantVkOnly(WORD vk) {
    INPUT in[2]      = {};
    in[0].type       = INPUT_KEYBOARD;
    in[0].ki.wVk     = vk;
    in[1]            = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    UINT sent        = SendInput(2, in, sizeof(INPUT));
    printf("      SendInput returned %u (vk only, no scan, no flags)\n", sent);
}

}  // namespace

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (!OpenVolume()) {
        printf("Could not open the default audio endpoint.\n");
        return 1;
    }

    // "watch <seconds>": report every change to the master volume, so the real
    // daemon can be run alongside and its effect observed.
    if (argc >= 2 && strcmp(argv[1], "watch") == 0) {
        const int seconds = argc >= 3 ? atoi(argv[2]) : 30;
        float     last    = Level();
        printf("watching volume for %ds, starting at %.3f\n", seconds, last);
        fflush(stdout);
        for (int i = 0; i < seconds * 20; ++i) {
            Sleep(50);
            const float now = Level();
            if (now != last) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                printf("%02u:%02u:%02u.%03u  volume %.3f -> %.3f\n", st.wHour, st.wMinute,
                       st.wSecond, st.wMilliseconds, last, now);
                fflush(stdout);
                last = now;
            }
        }
        printf("done\n");
        g_vol->Release();
        CoUninitialize();
        return 0;
    }

    const float original = Level();
    printf("Master volume: %.3f\n\n", original);

    printf("MapVirtualKey scan codes:\n");
    const struct { const char* n; WORD vk; } keys[] = {
        { "VK_VOLUME_UP",        VK_VOLUME_UP },
        { "VK_VOLUME_DOWN",      VK_VOLUME_DOWN },
        { "VK_VOLUME_MUTE",      VK_VOLUME_MUTE },
        { "VK_MEDIA_PLAY_PAUSE", VK_MEDIA_PLAY_PAUSE },
        { "VK_UP (for contrast)", VK_UP },
    };
    for (const auto& k : keys)
        printf("   %-22s 0x%02X -> scan 0x%02X\n", k.n, k.vk,
               MapVirtualKeyW(k.vk, MAPVK_VK_TO_VSC));

    printf("\nVolume-down variants:\n");
    Try("current code (scan + EXTENDEDKEY)", VK_VOLUME_DOWN, VariantExtended);
    Try("scan, no EXTENDEDKEY", VK_VOLUME_DOWN, VariantPlain);
    Try("vk only, no scan, no flags", VK_VOLUME_DOWN, VariantVkOnly);

    printf("\nRestoring volume to %.3f\n", original);
    SetLevel(original);

    if (g_vol)
        g_vol->Release();
    CoUninitialize();
    return 0;
}
