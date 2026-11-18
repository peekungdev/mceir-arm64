// audio.cpp - volume keys via Core Audio instead of SendInput.
//
// SendInput is subject to UIPI: a normal-integrity process cannot inject into
// a higher-integrity foreground window, and the call fails outright with
// ERROR_ACCESS_DENIED rather than partially. So focusing any elevated window
// -- Task Manager, an admin console, a UAC prompt -- kills every remote button
// until focus moves away.
//
// Volume, mute and the like are not really keystrokes though: they are global
// endpoint operations that happen to be spelled as keys. Driving the default
// audio endpoint directly sidesteps UIPI entirely, so the volume buttons keep
// working no matter what has focus. VolumeStepUp/Down use the endpoint's own
// step table, which is what the keyboard keys drive too, so the feel is the
// same.

#include "mceir.h"

#include <endpointvolume.h>
#include <mmdeviceapi.h>

namespace {

IMMDeviceEnumerator* g_enumerator = nullptr;

void ReleaseEnumerator() {
    if (g_enumerator) {
        g_enumerator->Release();
        g_enumerator = nullptr;
    }
}

bool EnsureEnumerator() {
    if (g_enumerator)
        return true;

    const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        __uuidof(IMMDeviceEnumerator),
                                        reinterpret_cast<void**>(&g_enumerator));
    if (FAILED(hr)) {
        LOGV("Core Audio: CoCreateInstance failed (0x%08lX).", hr);
        g_enumerator = nullptr;
        return false;
    }
    return true;
}

HRESULT Apply(IAudioEndpointVolume* endpoint, WORD vk) {
    switch (vk) {
    case VK_VOLUME_UP:
        return endpoint->VolumeStepUp(nullptr);
    case VK_VOLUME_DOWN:
        return endpoint->VolumeStepDown(nullptr);
    case VK_VOLUME_MUTE: {
        BOOL muted = FALSE;
        HRESULT hr = endpoint->GetMute(&muted);
        if (FAILED(hr))
            return hr;
        return endpoint->SetMute(muted ? FALSE : TRUE, nullptr);
    }
    default:
        return E_NOTIMPL;
    }
}

// The default endpoint is looked up for every key rather than cached. The old
// cache assumed a switched-away device would start failing calls, but it only
// does if it disappears: switch the default to headphones while the speakers
// stay present and the cached interface kept working -- on the speakers.
HRESULT ApplyToDefault(WORD vk) {
    if (!EnsureEnumerator())
        return E_FAIL;

    IMMDevice* device = nullptr;
    HRESULT    hr     = g_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (FAILED(hr)) {
        LOGV("Core Audio: no default render endpoint (0x%08lX).", hr);
        return hr;
    }

    IAudioEndpointVolume* endpoint = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&endpoint));
    device->Release();
    if (FAILED(hr)) {
        LOGV("Core Audio: could not activate volume control (0x%08lX).", hr);
        return hr;
    }

    hr = Apply(endpoint, vk);
    endpoint->Release();
    return hr;
}

}  // namespace

bool IsVolumeKey(WORD vk) {
    return vk == VK_VOLUME_UP || vk == VK_VOLUME_DOWN || vk == VK_VOLUME_MUTE;
}

bool TryVolumeKey(WORD vk) {
    if (!IsVolumeKey(vk))
        return false;

    HRESULT hr = ApplyToDefault(vk);
    if (SUCCEEDED(hr))
        return true;

    // The enumerator itself goes stale if the audio service restarts; rebuild
    // it and give the key one more go.
    LOGV("Core Audio: call failed (0x%08lX); starting over.", hr);
    ReleaseEnumerator();

    hr = ApplyToDefault(vk);
    if (SUCCEEDED(hr))
        return true;

    LOGV("Core Audio: still failing (0x%08lX); falling back to SendInput.", hr);
    return false;
}

void ShutdownAudio() {
    ReleaseEnumerator();
}
