
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <stdint.h>
#include <cmath>
#include <string.h>

#if !defined(_M_IX86)
#error This ASI must be built for Win32 (x86), never x64.
#endif

namespace {

constexpr uintptr_t kUpdateCameraCall  = 0x00581493u;
constexpr uintptr_t kUpdateCameraFn    = 0x0044EFE0u;
constexpr uintptr_t kChangeCameraMover = 0x005F1FD0u;
constexpr uintptr_t kDefaultDriveCam   = 0x005FA820u;
constexpr uintptr_t kIsGamePaused      = 0x00571250u;
constexpr uintptr_t kPlayerGlobal      = 0x008900ACu;
constexpr uintptr_t kWorldPauseFlag    = 0x00890084u;
constexpr uintptr_t kGameFlowManager   = 0x00865480u;
constexpr uintptr_t kOrbitVtable       = 0x00787D40u;
constexpr uintptr_t kGameD3DDevice     = 0x00870974u;
constexpr uintptr_t kCameraMoverPtr    = 0x238u;
constexpr uintptr_t kPlayerCarPtr      = 0x004u;
constexpr uintptr_t kYawOffset         = 0x030u;
constexpr uintptr_t kPitchOffset       = 0x032u;
constexpr uintptr_t kRadiusOffset      = 0x048u;
constexpr int       kOrbitType         = 4;

constexpr uintptr_t kSetupExploreCall   = 0x004D937Fu;
constexpr uintptr_t kSetupRaceCall      = 0x004D9386u;
constexpr uintptr_t kSetupExploreFn     = 0x004C2900u;
constexpr uintptr_t kSetupRaceFn        = 0x004C2BD0u;
constexpr uintptr_t kPauseMenuVtable    = 0x00797CB0u;
constexpr uintptr_t kResumeFreeVtable   = 0x007954D8u;
constexpr uintptr_t kGameMalloc         = 0x00575620u;
constexpr uintptr_t kIconOptionCtor     = 0x0051F670u;
constexpr uintptr_t kRaceCoordinator    = 0x008900D8u;
constexpr uintptr_t kInRaceIndicator    = 0x0089E7B0u;
constexpr uintptr_t kRequestUnPause     = 0x005F4240u;
constexpr uintptr_t kLanguageLookup     = 0x004FF9D0u;
constexpr uint32_t  kPhotoLabelHash     = 0xD8A7135Fu;
constexpr uint32_t  kPhotoMenuIconHash  = 0xC3400A65u;
constexpr uint32_t  kAcceptEventHash    = 0x0C407210u;
constexpr DWORD     kOpenTimeoutMs      = 6000u;
constexpr DWORD     kPauseExitDelayMs   = 120u;

using UpdateCamera_t = void (__cdecl *)();
using ChangeCamera_t = void (__thiscall *)(void*, int, int);
using DefaultCamera_t = void (__thiscall *)(void*, int);
using IsPaused_t = bool (__thiscall *)(void*);
using Populate_t = int (__thiscall *)(void*);
using JMalloc_t = void* (__cdecl *)(size_t);
using IconOptionCtor_t = void* (__thiscall *)(void*, uint32_t, uint32_t, uint32_t);
using AddOption_t = int (__thiscall *)(void*, void*);
using RequestUnPause_t = void (__thiscall *)(void*, int);

static UpdateCamera_t gOriginalCameraUpdate = nullptr;
static HMODULE gSelf = nullptr;
static bool gInstalled = false;
static bool gPhotoActive = false;
static bool gPrevEscape = false;
static void* gPhotoPlayer = nullptr;
static int gOldWorldPause = 0;
static DWORD gLastFrameMs = 0;
static bool gPendingMenuOpen = false;
static DWORD gMenuOpenTick = 0;
static DWORD gUnpausedSince = 0;
static Populate_t gOriginalExplore = nullptr;
static Populate_t gOriginalRace = nullptr;
static bool gExploreHooked = false;
static bool gRaceHooked = false;
static bool gLocaleHooked = false;
static void* gLocaleTrampoline = nullptr;
static uint8_t gSavedLanguageBytes[7] = {};
static uint32_t gPhotoOptionVtable[2] = {};
static const char gPhotoLabelText[] = "Photomode";
static bool gShowButtons = true;
static char gIniPath[MAX_PATH] = {};
static volatile LONG gOverlayShutdown = 0;
static volatile LONG gOverlayPresentSeen = 0;
static volatile LONG gOverlayEndSceneSeen = 0;
static volatile LONG gOverlayPresentActiveSeen = 0;
static volatile LONG gOverlayEndSceneActiveSeen = 0;
static volatile LONG gOverlaySwapPresentActiveSeen = 0;
static volatile LONG gOverlaySwapPresentSeen = 0;
static volatile LONG gOverlaySwapPresentResultLogged = 0;
static IDirect3DDevice9* gTrackedGameDevice = nullptr;
static DWORD gLastHookRefresh = 0;
static DWORD gGameFrameSerial = 1;
static DWORD gLastOverlayDrawSerial = 0;
static bool gInsideOverlayPresent = false;
static volatile LONG gOverlayPresentResultLogged = 0;
static volatile LONG gOverlayEndSceneResultLogged = 0;
static volatile LONG gOverlayMissingCallbackLogged = 0;
static DWORD gPhotoStartTick = 0;

using Present_t = HRESULT (STDMETHODCALLTYPE *)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using EndScene_t = HRESULT (STDMETHODCALLTYPE *)(IDirect3DDevice9*);
using SwapPresent_t = HRESULT (STDMETHODCALLTYPE *)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
struct PresentHookRecord {
    void** slot;
    Present_t original;
};
struct EndSceneHookRecord {
    void** slot;
    EndScene_t original;
};
struct SwapPresentHookRecord {
    void** slot;
    SwapPresent_t original;
};
static SwapPresentHookRecord gSwapPresentHooks[8] = {};
static unsigned gSwapPresentHookCount = 0;
static PresentHookRecord gPresentHooks[16] = {};
static EndSceneHookRecord gEndSceneHooks[16] = {};
static unsigned gEndSceneHookCount = 0;
static unsigned gPresentHookCount = 0;
static void RefreshGameDeviceHooks();


struct AudioSessionMuteState {
    ISimpleAudioVolume* volume;
    BOOL previouslyMuted;
};
static AudioSessionMuteState gAudioSessions[64] = {};
static unsigned gAudioSessionCount = 0;
static bool gAudioMuteApplied = false;
static bool gAudioComOwned = false;
static DWORD gAudioOwnerThread = 0;
static DWORD gLastAudioAttempt = 0;
constexpr DWORD kAudioRetryMs = 1000u;

static void Log(const char*, ...) {}

static void LoadOverlaySettings() {
    char modulePath[MAX_PATH] = {};
    const DWORD size = GetModuleFileNameA(gSelf, modulePath, MAX_PATH);
    if (!size || size >= MAX_PATH) return;
    char* sep = nullptr;
    for (char* p = modulePath; *p; ++p)
        if (*p == '\\' || *p == '/') sep = p;
    if (!sep) return;
    strcpy_s(sep + 1, MAX_PATH - static_cast<size_t>(sep + 1 - modulePath), "NFSU2_PhotoMode.ini");
    strcpy_s(gIniPath, sizeof(gIniPath), modulePath);
    char setting[32] = {};
    GetPrivateProfileStringA("Overlay", "ShowButtons", "true", setting, sizeof(setting), gIniPath);
    gShowButtons = lstrcmpiA(setting, "false") != 0 &&
        lstrcmpiA(setting, "0") != 0 &&
        lstrcmpiA(setting, "off") != 0 &&
        lstrcmpiA(setting, "no") != 0;
}

static bool InMainGameWindow() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

static bool Held(int key) {
    return (GetAsyncKeyState(key) & static_cast<SHORT>(0x8000)) != 0;
}

static bool RegionAccessible(const void* p, size_t bytes, bool writable) {
    if (!p || !bytes) return false;
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(p, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    uintptr_t begin = reinterpret_cast<uintptr_t>(p);
    uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (begin > end || bytes > end - begin) return false;
    const DWORD pflags = info.Protect & 0xFF;
    if (writable) return pflags == PAGE_READWRITE || pflags == PAGE_WRITECOPY ||
        pflags == PAGE_EXECUTE_READWRITE || pflags == PAGE_EXECUTE_WRITECOPY;
    return pflags == PAGE_READONLY || pflags == PAGE_READWRITE || pflags == PAGE_WRITECOPY ||
        pflags == PAGE_EXECUTE || pflags == PAGE_EXECUTE_READ ||
        pflags == PAGE_EXECUTE_READWRITE || pflags == PAGE_EXECUTE_WRITECOPY;
}

template<typename T> static T* Checked(uintptr_t address, bool write = false) {
    T* p = reinterpret_cast<T*>(address);
    return RegionAccessible(p, sizeof(T), write) ? p : nullptr;
}

static void* CurrentPlayer() {
    void** ptr = Checked<void*>(kPlayerGlobal);
    if (!ptr || !*ptr) return nullptr;
    void* player = *ptr;
    if (!RegionAccessible(player, kCameraMoverPtr + sizeof(void*), false)) return nullptr;
    return player;
}

static void* CurrentMover(void* player) {
    if (!player) return nullptr;
    void** pp = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(player) + kCameraMoverPtr);
    if (!RegionAccessible(pp, sizeof(void*), false)) return nullptr;
    void* mover = *pp;
    return RegionAccessible(mover, 0xC4, false) ? mover : nullptr;
}

static bool OrbitIsCurrent(void* player) {
    void* mover = CurrentMover(player);
    if (!mover) return false;
    const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(mover);
    return vtable == kOrbitVtable;
}

static bool PatchCall(uintptr_t call, uintptr_t destination) {
    auto* p = reinterpret_cast<uint8_t*>(call);
    DWORD oldProtect = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;

    const int32_t delta = static_cast<int32_t>(destination - (call + 5));
    memcpy(p + 1, &delta, sizeof(delta));
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    DWORD ignore = 0;
    VirtualProtect(p, 5, oldProtect, &ignore);
    return true;
}

__declspec(naked) static void __cdecl PhotoLanguageLookupHook() {
    __asm {
        cmp edx, 0D8A7135Fh
        jne original_lookup
        mov eax, offset gPhotoLabelText
        ret
    original_lookup:
        jmp dword ptr [gLocaleTrampoline]
    }
}

static bool WritePatch(uintptr_t at, const void* data, size_t len) {
    if (!RegionAccessible(reinterpret_cast<void*>(at), len, false)) return false;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(reinterpret_cast<void*>(at), data, len);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), len);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(at), len, old, &ignored);
    return true;
}

static bool InstallLanguageHook() {
    const uint8_t expected[] = {0x53, 0x8B, 0x1D, 0xD8, 0x83, 0x83, 0x00};
    if (!RegionAccessible(reinterpret_cast<void*>(kLanguageLookup), 7, false) ||
        memcmp(reinterpret_cast<void*>(kLanguageLookup), expected, 7) != 0) return false;
    memcpy(gSavedLanguageBytes, expected, sizeof(expected));
    uint8_t* tramp = reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, 12, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    memcpy(tramp, expected, 7);
    tramp[7] = 0xE9;
    const int32_t back = static_cast<int32_t>((kLanguageLookup + 7u) - (reinterpret_cast<uintptr_t>(tramp) + 12u));
    memcpy(tramp + 8, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 12);
    gLocaleTrampoline = tramp;
    uint8_t jump[7] = {0xE9,0,0,0,0,0x90,0x90};
    const int32_t target = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&PhotoLanguageLookupHook) - (kLanguageLookup + 5u));
    memcpy(jump + 1, &target, 4);
    if (!WritePatch(kLanguageLookup, jump, sizeof(jump))) {
        gLocaleTrampoline = nullptr;
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    gLocaleHooked = true;
    return true;
}

static void RestoreLanguageHook() {
    if (!gLocaleHooked) return;
    auto* p = reinterpret_cast<uint8_t*>(kLanguageLookup);
    const int32_t target = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&PhotoLanguageLookupHook) - (kLanguageLookup + 5u));
    if (RegionAccessible(p, 7, false) && p[0] == 0xE9 && memcmp(p + 1, &target, 4) == 0)
        WritePatch(kLanguageLookup, gSavedLanguageBytes, sizeof(gSavedLanguageBytes));

    gLocaleHooked = false;
}

static void __fastcall PhotoOptionReact(void*, void*, void*, uint32_t message,
                                        void*, uint32_t, uint32_t) {
    if (message != kAcceptEventHash || gPhotoActive || gPendingMenuOpen) return;
    void** pCoordinator = Checked<void*>(kRaceCoordinator);
    const int* race = Checked<int>(kInRaceIndicator);
    if (!pCoordinator || !*pCoordinator || !race) {
        Log("MENU selected but race coordinator unavailable");
        return;
    }
    gMenuOpenTick = GetTickCount();
    gUnpausedSince = 0;
    gPendingMenuOpen = true;
    const int resumeType = (*race == 0) ? 2 : 1;
    Log("MENU Photomode selected; requesting native unpause type=%d", resumeType);
    auto unpause = reinterpret_cast<RequestUnPause_t>(kRequestUnPause);
    unpause(*pCoordinator, resumeType);
}

static bool AddPhotoModeOption(void* pauseMenu) {
    if (!pauseMenu || !RegionAccessible(pauseMenu, 0x150, false)) return false;
    auto** vtbl = reinterpret_cast<void**>(*reinterpret_cast<uintptr_t*>(pauseMenu));
    if (reinterpret_cast<uintptr_t>(vtbl) != kPauseMenuVtable ||
        reinterpret_cast<uintptr_t>(vtbl[6]) != 0x0050DAB0u) {
        Log("MENU no option: unexpected PauseMenu vtable");
        return false;
    }
    const auto gameMalloc = reinterpret_cast<JMalloc_t>(kGameMalloc);
    void* opt = gameMalloc(0x48);
    if (!opt) { Log("MENU allocation failed"); return false; }
    auto ctor = reinterpret_cast<IconOptionCtor_t>(kIconOptionCtor);
    ctor(opt, kPhotoMenuIconHash, kPhotoLabelHash, 0);
    *reinterpret_cast<uintptr_t*>(opt) = reinterpret_cast<uintptr_t>(gPhotoOptionVtable);
    auto add = reinterpret_cast<AddOption_t>(vtbl[6]);
    add(pauseMenu, opt);
    Log("MENU injected Photomode option into native PauseMenu (%p)", pauseMenu);
    return true;
}

static int __fastcall PauseExploreHook(void* pauseMenu, void*) {
    int result = gOriginalExplore ? gOriginalExplore(pauseMenu) : 0;
    AddPhotoModeOption(pauseMenu);
    return result;
}
static int __fastcall PauseRaceHook(void* pauseMenu, void*) {
    int result = gOriginalRace ? gOriginalRace(pauseMenu) : 0;
    AddPhotoModeOption(pauseMenu);
    return result;
}

static bool InstallPauseHooks() {

    const uint8_t exploreExpected[5] = {0xE8,0x7C,0x95,0xFE,0xFF};
    const uint8_t raceExpected[5] = {0xE8,0x45,0x98,0xFE,0xFF};
    if (!RegionAccessible(reinterpret_cast<void*>(kSetupExploreCall), 5, false) ||
        !RegionAccessible(reinterpret_cast<void*>(kSetupRaceCall), 5, false) ||
        memcmp(reinterpret_cast<void*>(kSetupExploreCall), exploreExpected, 5) != 0 ||
        memcmp(reinterpret_cast<void*>(kSetupRaceCall), raceExpected, 5) != 0) return false;
    const auto* retailVtable = reinterpret_cast<const uint32_t*>(kResumeFreeVtable);
    if (!RegionAccessible(retailVtable, sizeof(gPhotoOptionVtable), false) ||
        retailVtable[0] != 0x004AEB70u || retailVtable[1] != 0x004C2510u) return false;
    gPhotoOptionVtable[0] = retailVtable[0];
    gPhotoOptionVtable[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&PhotoOptionReact));
    gOriginalExplore = reinterpret_cast<Populate_t>(kSetupExploreFn);
    gOriginalRace = reinterpret_cast<Populate_t>(kSetupRaceFn);
    if (!PatchCall(kSetupExploreCall, reinterpret_cast<uintptr_t>(&PauseExploreHook))) return false;
    gExploreHooked = true;
    if (!PatchCall(kSetupRaceCall, reinterpret_cast<uintptr_t>(&PauseRaceHook))) return false;
    gRaceHooked = true;
    return true;
}

static void RestorePauseHooks() {
    const auto currentTarget = [](uintptr_t callSite) -> uintptr_t {
        const auto* p = reinterpret_cast<const uint8_t*>(callSite);
        if (!RegionAccessible(p, 5, false) || p[0] != 0xE8) return 0;
        int32_t rel = 0;
        memcpy(&rel, p + 1, 4);
        return callSite + 5 + static_cast<uint32_t>(rel);
    };
    if (gExploreHooked && currentTarget(kSetupExploreCall) == reinterpret_cast<uintptr_t>(&PauseExploreHook))
        PatchCall(kSetupExploreCall, kSetupExploreFn);
    if (gRaceHooked && currentTarget(kSetupRaceCall) == reinterpret_cast<uintptr_t>(&PauseRaceHook))
        PatchCall(kSetupRaceCall, kSetupRaceFn);
    gExploreHooked = gRaceHooked = false;
}

static void ReleaseAudioSessions(bool restorePreviousMute) {

    if (gAudioOwnerThread && gAudioOwnerThread != GetCurrentThreadId()) {
        Log("AUDIO restore skipped: COM owner thread mismatch");
        return;
    }
    for (unsigned i = 0; i < gAudioSessionCount; ++i) {
        if (gAudioSessions[i].volume) {
            if (restorePreviousMute) {
                const HRESULT hr = gAudioSessions[i].volume->SetMute(
                    gAudioSessions[i].previouslyMuted, nullptr);
                if (FAILED(hr)) Log("AUDIO restore failed, HRESULT=%08X", static_cast<unsigned>(hr));
            }
            gAudioSessions[i].volume->Release();
            gAudioSessions[i].volume = nullptr;
        }
        gAudioSessions[i].previouslyMuted = FALSE;
    }
    gAudioSessionCount = 0;
    gAudioMuteApplied = false;
    if (gAudioComOwned) CoUninitialize();
    gAudioComOwned = false;
    gAudioOwnerThread = 0;
}

static bool MuteProcessAudioSessions() {
    if (gAudioMuteApplied) return true;
    if (gAudioOwnerThread && gAudioOwnerThread != GetCurrentThreadId()) return false;

    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(init)) {
        gAudioComOwned = true;
    } else if (init != RPC_E_CHANGED_MODE) {
        Log("AUDIO COM initialization failed, HRESULT=%08X", static_cast<unsigned>(init));
        return false;
    }
    gAudioOwnerThread = GetCurrentThreadId();

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* devices = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        Log("AUDIO endpoint enumerator unavailable, HRESULT=%08X", static_cast<unsigned>(hr));
        ReleaseAudioSessions(false);
        return false;
    }
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
    if (FAILED(hr) || !devices) {
        Log("AUDIO active outputs unavailable, HRESULT=%08X", static_cast<unsigned>(hr));
        enumerator->Release();
        ReleaseAudioSessions(false);
        return false;
    }

    UINT deviceCount = 0;
    devices->GetCount(&deviceCount);
    const DWORD myPid = GetCurrentProcessId();
    for (UINT d = 0; d < deviceCount && gAudioSessionCount < 64u; ++d) {
        IMMDevice* device = nullptr;
        if (FAILED(devices->Item(d, &device)) || !device) continue;
        IAudioSessionManager2* manager = nullptr;
        hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
            nullptr, reinterpret_cast<void**>(&manager));
        if (SUCCEEDED(hr) && manager) {
            IAudioSessionEnumerator* sessions = nullptr;
            if (SUCCEEDED(manager->GetSessionEnumerator(&sessions)) && sessions) {
                int count = 0;
                if (SUCCEEDED(sessions->GetCount(&count))) {
                    for (int i = 0; i < count && gAudioSessionCount < 64u; ++i) {
                        IAudioSessionControl* control = nullptr;
                        if (FAILED(sessions->GetSession(i, &control)) || !control) continue;
                        IAudioSessionControl2* control2 = nullptr;
                        if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                reinterpret_cast<void**>(&control2))) && control2) {
                            DWORD pid = 0;
                            if (SUCCEEDED(control2->GetProcessId(&pid)) && pid == myPid) {
                                ISimpleAudioVolume* volume = nullptr;
                                if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
                                        reinterpret_cast<void**>(&volume))) && volume) {
                                    BOOL wasMuted = FALSE;
                                    if (SUCCEEDED(volume->GetMute(&wasMuted)) &&
                                        SUCCEEDED(volume->SetMute(TRUE, nullptr))) {
                                        gAudioSessions[gAudioSessionCount++] = { volume, wasMuted };
                                        volume = nullptr;
                                    }
                                    if (volume) volume->Release();
                                }
                            }
                            control2->Release();
                        }
                        control->Release();
                    }
                }
                sessions->Release();
            }
            manager->Release();
        }
        device->Release();
    }
    devices->Release();
    enumerator->Release();

    if (!gAudioSessionCount) {
        ReleaseAudioSessions(false);
        return false;
    }
    gAudioMuteApplied = true;
    Log("AUDIO muted %u SPEED2.EXE session(s); prior mute states saved", gAudioSessionCount);
    return true;
}

static void RestoreProcessAudioSessions() {
    if (!gAudioMuteApplied && !gAudioSessionCount && !gAudioComOwned) return;
    const unsigned count = gAudioSessionCount;
    ReleaseAudioSessions(true);
    Log("AUDIO previous mute states restored for %u session(s)", count);
}

static void StopPhoto(bool restoreCamera, bool restoreAudio = true) {
    if (!gPhotoActive) return;

    int* pause = Checked<int>(kWorldPauseFlag, true);
    if (pause) *pause = gOldWorldPause;
    void* player = CurrentPlayer();
    if (restoreCamera && player && player == gPhotoPlayer && OrbitIsCurrent(player)) {
        auto restore = reinterpret_cast<DefaultCamera_t>(kDefaultDriveCam);
        restore(player, 1);
    }
    gPhotoActive = false;
    gPhotoPlayer = nullptr;
    gLastFrameMs = 0;
    if (restoreAudio) RestoreProcessAudioSessions();
    Log("STOP photo mode; original world pause=%d", gOldWorldPause);
}

static bool StartPhoto() {
    if (gPhotoActive || !InMainGameWindow()) return false;
    void* player = CurrentPlayer();
    if (!player) { Log("START refused: no valid player"); return false; }
    void** car = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(player) + kPlayerCarPtr);
    if (!RegionAccessible(car, sizeof(void*), false) || !*car) {
        Log("START refused: no player car"); return false;
    }
    int* pause = Checked<int>(kWorldPauseFlag, true);
    if (!pause || *pause != 0) { Log("START refused: world already paused"); return false; }
    auto isPaused = reinterpret_cast<IsPaused_t>(kIsGamePaused);
    if (isPaused(reinterpret_cast<void*>(kGameFlowManager))) {
        Log("START refused: game flow paused (e.g. front-end)");
        return false;
    }
    auto change = reinterpret_cast<ChangeCamera_t>(kChangeCameraMover);
    change(player, kOrbitType, 0);
    if (!OrbitIsCurrent(player)) {
        Log("START refused: native orbit mover was not selected");
        return false;
    }
    gPhotoPlayer = player;
    gOldWorldPause = *pause;
    *pause = 1;
    gPhotoActive = true;
    gPhotoStartTick = GetTickCount();
    InterlockedExchange(&gOverlayPresentActiveSeen, 0);
    InterlockedExchange(&gOverlayEndSceneActiveSeen, 0);
    InterlockedExchange(&gOverlayPresentResultLogged, 0);
    InterlockedExchange(&gOverlayEndSceneResultLogged, 0);
    InterlockedExchange(&gOverlayMissingCallbackLogged, 0);
    gLastOverlayDrawSerial = 0;
    InterlockedExchange(&gOverlaySwapPresentActiveSeen, 0);
    InterlockedExchange(&gOverlaySwapPresentResultLogged, 0);
    gLastFrameMs = gPhotoStartTick;
    gLastAudioAttempt = gLastFrameMs;
    if (!MuteProcessAudioSessions())
        Log("AUDIO no game session found yet; retrying while Photo Mode is active");
    Log("START native OrbitCarCameraMover; world paused; audio session mute requested");
    return true;
}

static void UpdateOrbitInput(void* player, float dt) {
    void* mover = CurrentMover(player);
    if (!mover || !OrbitIsCurrent(player) || dt <= 0.0f) return;
    uint8_t* base = reinterpret_cast<uint8_t*>(mover);
    auto* yaw = reinterpret_cast<int16_t*>(base + kYawOffset);
    auto* pitch = reinterpret_cast<int16_t*>(base + kPitchOffset);
    auto* radius = reinterpret_cast<float*>(base + kRadiusOffset);

    const int32_t angleStepH = static_cast<int32_t>(14000.0f * dt);
    const int32_t angleStepV = static_cast<int32_t>(8500.0f * dt);
    int32_t nextH = static_cast<int32_t>(*yaw);
    int32_t nextV = static_cast<int32_t>(*pitch);
    if (Held(VK_LEFT)) nextH -= angleStepH;
    if (Held(VK_RIGHT)) nextH += angleStepH;
    if (Held(VK_UP)) nextV += angleStepV;
    if (Held(VK_DOWN)) nextV -= angleStepV;
    if (nextV < -15000) nextV = -15000;
    if (nextV > 15000) nextV = 15000;
    *yaw = static_cast<int16_t>(static_cast<uint16_t>(nextH));
    *pitch = static_cast<int16_t>(nextV);
    float r = *radius;
    if (!std::isfinite(r) || r < 0.5f || r > 500.0f) r = 5.0f;
    if (Held('9') || Held(VK_NUMPAD9)) r += 3.0f * dt;
    if (Held('0') || Held(VK_NUMPAD0)) r -= 3.0f * dt;
    if (r < 2.0f) r = 2.0f;
    if (r > 14.0f) r = 14.0f;
    *radius = r;
}

static void Tick() {
    if (gShowButtons) RefreshGameDeviceHooks();
    ++gGameFrameSerial;
    if (gGameFrameSerial == 0) ++gGameFrameSerial;

    const bool focused = InMainGameWindow();
    const bool escape = focused && Held(VK_ESCAPE);
    if (gPhotoActive && escape && !gPrevEscape) StopPhoto(true);
    gPrevEscape = escape;
    if (gPendingMenuOpen && !gPhotoActive) {
        const DWORD now = GetTickCount();
        if (now - gMenuOpenTick > kOpenTimeoutMs) {
            Log("MENU timed out waiting for game to resume");
            gPendingMenuOpen = false;
            gUnpausedSince = 0;
        } else {
            const auto isPaused = reinterpret_cast<IsPaused_t>(kIsGamePaused);
            const int* pause = Checked<int>(kWorldPauseFlag);
            void* player = CurrentPlayer();
            const void* car = player ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(player) + kPlayerCarPtr) : nullptr;
            if (focused && pause && !*pause && player && car &&
                !isPaused(reinterpret_cast<void*>(kGameFlowManager))) {
                if (gUnpausedSince == 0) gUnpausedSince = now;
                if (now - gUnpausedSince >= kPauseExitDelayMs) {
                    gPendingMenuOpen = false;
                    gUnpausedSince = 0;
                    if (!StartPhoto()) Log("MENU did not activate native orbit camera");
                }
            } else {
                gUnpausedSince = 0;
            }
        }
    }
    if (!gPhotoActive) return;

    void* player = CurrentPlayer();
    if (player != gPhotoPlayer || !OrbitIsCurrent(player)) {
        Log("Native orbit/player changed externally; restoring freeze");
        StopPhoto(false);
        return;
    }
    int* pause = Checked<int>(kWorldPauseFlag, true);
    if (!pause) { StopPhoto(true); return; }

    *pause = 1;
    DWORD now = GetTickCount();
    if (gShowButtons && now - gPhotoStartTick > 1800u &&
        InterlockedCompareExchange(&gOverlayPresentActiveSeen, 0, 0) == 0 &&
        InterlockedCompareExchange(&gOverlayEndSceneActiveSeen, 0, 0) == 0 &&
        InterlockedCompareExchange(&gOverlaySwapPresentActiveSeen, 0, 0) == 0 &&
        InterlockedCompareExchange(&gOverlayMissingCallbackLogged, 1, 0) == 0)
        Log("OVERLAY no native-device Present, EndScene or swap-chain Present reached active Photomode; device=%p", gTrackedGameDevice);
    if (!gAudioMuteApplied && now - gLastAudioAttempt >= kAudioRetryMs) {
        gLastAudioAttempt = now;
        MuteProcessAudioSessions();
    }
    DWORD elapsed = now - gLastFrameMs;
    gLastFrameMs = now;
    float dt = static_cast<float>(elapsed) * 0.001f;
    if (dt > 0.050f) dt = 0.050f;
    if (focused) UpdateOrbitInput(player, dt);
}

extern "C" void __cdecl CameraFrameHook() {
    Tick();
    if (gOriginalCameraUpdate) gOriginalCameraUpdate();
}


struct OverlayVertex {
    float x, y, z, rhw;
    D3DCOLOR color;
};
static constexpr DWORD kOverlayFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
static OverlayVertex gOverlayVerts[16000] = {};
static unsigned gOverlayCount = 0;

static void Vtx(float x, float y, D3DCOLOR c) {
    if (gOverlayCount >= sizeof(gOverlayVerts) / sizeof(gOverlayVerts[0])) return;
    OverlayVertex& v = gOverlayVerts[gOverlayCount++];
    v.x = x - 0.5f;
    v.y = y - 0.5f;
    v.z = 0.0f;
    v.rhw = 1.0f;
    v.color = c;
}

static void Quad(float x0, float y0, float x1, float y1, D3DCOLOR c) {
    Vtx(x0,y0,c); Vtx(x1,y0,c); Vtx(x0,y1,c);
    Vtx(x0,y1,c); Vtx(x1,y0,c); Vtx(x1,y1,c);
}

static void Tri(float x0,float y0,float x1,float y1,float x2,float y2,D3DCOLOR c) {
    Vtx(x0,y0,c); Vtx(x1,y1,c); Vtx(x2,y2,c);
}

static const unsigned char* Glyph(char c)
{
    static const unsigned char glyphs[36][7] = {
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
        {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
        {0x0F,0x10,0x10,0x10,0x10,0x10,0x0F},
        {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
        {0x0F,0x10,0x10,0x17,0x11,0x11,0x0F},
        {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
        {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
        {0x07,0x02,0x02,0x02,0x12,0x12,0x0C},
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
        {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
        {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
        {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
        {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
        {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
        {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
        {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
        {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
        {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
        {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
        {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
        {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
        {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
        {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
        {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
        {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
        {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E},
        {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E},
        {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
        {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}
    };

    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return glyphs[c - 'A'];
    if (c >= '0' && c <= '9') return glyphs[26 + (c - '0')];
    return 0;
}

static float TextW(const char* text, float px) {
    float w = 0.0f;
    if (text) for (const char* p = text; *p; ++p) w += (*p == ' ' ? 4.0f : 6.0f) * px;
    return w;
}

static void TextRaw(float x, float y, const char* text, float px, D3DCOLOR c) {
    if (!text) return;
    for (const char* p = text; *p; ++p) {
        if (*p == ' ') { x += 4.0f * px; continue; }
        const unsigned char* rows = Glyph(*p);
        if (rows) for (int r = 0; r < 7; ++r)
            for (int k = 0; k < 5; ++k)
                if (rows[r] & (1 << (4 - k)))
                    Quad(x + k * px, y + r * px, x + (k + 1) * px, y + (r + 1) * px, c);
        x += 6.0f * px;
    }
}

static void Text(float x, float y, const char* text, float px, D3DCOLOR c) {
    TextRaw(x + px * .5f, y + px * .7f, text, px, D3DCOLOR_ARGB(150, 0, 0, 0));
    TextRaw(x, y, text, px, c);
}

static void ButtonOutline(float x0, float y0, float x1, float y1, float thickness, D3DCOLOR border) {
    Quad(x0, y0, x1, y1, D3DCOLOR_ARGB(155, 5, 5, 5));
    Quad(x0, y0, x1, y0 + thickness, border);
    Quad(x0, y1 - thickness, x1, y1, border);
    Quad(x0, y0, x0 + thickness, y1, border);
    Quad(x1 - thickness, y0, x1, y1, border);
}

static void Keycap(float x, float y, float w, float h, const char* label, float px, float scale) {
    const D3DCOLOR lime = D3DCOLOR_ARGB(235, 179, 202, 147);
    ButtonOutline(x, y, x + w, y + h, 2.0f * scale, lime);
    Text(x + (w - TextW(label, px)) * 0.5f, y + (h - 7.0f * px) * 0.5f, label, px, lime);
}

static void ArrowKey(float x, float y, float s, int direction, float scale) {
    const D3DCOLOR lime = D3DCOLOR_ARGB(235, 179, 202, 147);
    ButtonOutline(x, y, x + s, y + s, 2.0f * scale, lime);
    const float cx = x + s * 0.5f;
    const float cy = y + s * 0.5f;
    const float a = s * 0.22f;
    if (direction == 0) Tri(cx, cy - a, cx - a, cy + a, cx + a, cy + a, lime);
    if (direction == 1) Tri(cx, cy + a, cx - a, cy - a, cx + a, cy - a, lime);
    if (direction == 2) Tri(cx - a, cy, cx + a, cy - a, cx + a, cy + a, lime);
    if (direction == 3) Tri(cx + a, cy, cx - a, cy - a, cx - a, cy + a, lime);
}

static HRESULT DrawU2ButtonOverlay(IDirect3DDevice9* dev) {
    if (!dev || !gShowButtons || !gPhotoActive) return D3DERR_INVALIDCALL;
    D3DVIEWPORT9 vp = {};
    HRESULT hr = dev->GetViewport(&vp);
    if (FAILED(hr)) return hr;
    const float w = static_cast<float>(vp.Width), h = static_cast<float>(vp.Height);
    if (w < 320 || h < 240) return D3DERR_INVALIDCALL;
    float scale = h / 720.0f;
    const float widthScale = w / 1280.0f;
    if (scale > widthScale) scale = widthScale;
    if (scale < 0.45f) scale = 0.45f;
    if (scale > 1.8f) scale = 1.8f;
    const float xOrigin = static_cast<float>(vp.X);
    const float y = static_cast<float>(vp.Y) + h - 58.0f * scale;
    IDirect3DStateBlock9* state = nullptr;
    hr = dev->CreateStateBlock(D3DSBT_ALL, &state);
    if (FAILED(hr) || !state) return FAILED(hr) ? hr : E_FAIL;
    hr = state->Capture();
    if (FAILED(hr)) { state->Release(); return hr; }
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetTexture(0,nullptr);
    dev->SetFVF(kOverlayFVF);
    dev->SetRenderState(D3DRS_ZENABLE,FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING,FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE,FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE,FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE,0);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE,0xF);
    dev->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    dev->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
    dev->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
    gOverlayCount = 0;
    const D3DCOLOR lime = D3DCOLOR_ARGB(240, 179, 202, 147);
    const float px = 2.0f * scale;
    const float boxH = 31.0f * scale;

    const float left = xOrigin + w * 0.155f;
    Keycap(left, y, 52.0f * scale, boxH, "ESC", px, scale);
    Text(left + 61.0f * scale, y + 8.0f * scale, "BACK", px, lime);

    const float center = xOrigin + w * 0.45f;
    Keycap(center, y, 28.0f * scale, boxH, "9", px, scale);
    Keycap(center + 32.0f * scale, y, 28.0f * scale, boxH, "0", px, scale);
    Text(center + 69.0f * scale, y + 8.0f * scale, "ZOOM", px, lime);

    const float right = xOrigin + w * 0.64f;
    const float s = 28.0f * scale;
    ArrowKey(right, y, s, 0, scale);
    ArrowKey(right + 32.0f * scale, y, s, 1, scale);
    ArrowKey(right + 64.0f * scale, y, s, 2, scale);
    ArrowKey(right + 96.0f * scale, y, s, 3, scale);
    Text(right + 133.0f * scale, y + 8.0f * scale, "VIEW CAR", px, lime);
    hr = gOverlayCount
        ? dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, gOverlayCount/3, gOverlayVerts, sizeof(OverlayVertex))
        : E_FAIL;
    state->Apply();
    state->Release();
    return hr;
}

static HRESULT STDMETHODCALLTYPE PhotoOverlayEndScene(IDirect3DDevice9* dev) {
    EndScene_t original = nullptr;
    if (dev && RegionAccessible(dev, sizeof(void*), false)) {
        auto** vtbl = *reinterpret_cast<void***>(dev);
        if (RegionAccessible(vtbl, 43 * sizeof(void*), false)) {
            void** slot = &vtbl[42];
            for (unsigned i = 0; i < gEndSceneHookCount; ++i)
                if (gEndSceneHooks[i].slot == slot) { original = gEndSceneHooks[i].original; break; }
        }
    }
    if (!original) return D3DERR_INVALIDCALL;
    if (InterlockedCompareExchange(&gOverlayEndSceneSeen, 1, 0) == 0)
        Log("OVERLAY game EndScene callback reached (device=%p)", dev);
    if (gShowButtons && gPhotoActive && !gInsideOverlayPresent &&
        InterlockedCompareExchange(&gOverlayShutdown, 0, 0) == 0) {
        InterlockedExchange(&gOverlayEndSceneActiveSeen, 1);
        const HRESULT drawn = DrawU2ButtonOverlay(dev);
        if (SUCCEEDED(drawn)) {
            gLastOverlayDrawSerial = gGameFrameSerial;
        }
        if (InterlockedCompareExchange(&gOverlayEndSceneResultLogged, 1, 0) == 0)
            Log("OVERLAY EndScene drawing result=%08X", static_cast<unsigned>(drawn));
    }
    return original(dev);
}

static HRESULT STDMETHODCALLTYPE PhotoOverlayPresent(
    IDirect3DDevice9* dev, const RECT* source, const RECT* destination, HWND window, const RGNDATA* dirty) {
    Present_t original = nullptr;
    if (dev && RegionAccessible(dev, sizeof(void*), false)) {
        auto** vtbl = *reinterpret_cast<void***>(dev);
        if (RegionAccessible(vtbl, 18 * sizeof(void*), false)) {
            void** slot = &vtbl[17];
            for (unsigned i = 0; i < gPresentHookCount; ++i)
                if (gPresentHooks[i].slot == slot) { original = gPresentHooks[i].original; break; }
        }
    }
    if (!original) return D3DERR_INVALIDCALL;
    if (InterlockedCompareExchange(&gOverlayPresentSeen, 1, 0) == 0)
        Log("OVERLAY game Present callback reached (device=%p)", dev);
    if (gShowButtons && gPhotoActive && dev &&
        InterlockedCompareExchange(&gOverlayShutdown, 0, 0) == 0) {
        InterlockedExchange(&gOverlayPresentActiveSeen, 1);
        if (gLastOverlayDrawSerial != gGameFrameSerial) {
            const HRESULT begun = dev->BeginScene();
            HRESULT drawn = begun;
            if (SUCCEEDED(begun)) {
                gInsideOverlayPresent = true;
                drawn = DrawU2ButtonOverlay(dev);
                dev->EndScene();
                gInsideOverlayPresent = false;
                if (SUCCEEDED(drawn)) gLastOverlayDrawSerial = gGameFrameSerial;
            }
            if (InterlockedCompareExchange(&gOverlayPresentResultLogged, 1, 0) == 0)
                Log("OVERLAY Present fallback BeginScene=%08X drawing=%08X", static_cast<unsigned>(begun), static_cast<unsigned>(drawn));
        } else if (InterlockedCompareExchange(&gOverlayPresentResultLogged, 1, 0) == 0) {
            Log("OVERLAY Present skipped because overlay already rendered this frame");
        }
    }
    return original(dev, source, destination, window, dirty);
}

static HRESULT STDMETHODCALLTYPE PhotoOverlaySwapPresent(
    IDirect3DSwapChain9* chain, const RECT* source, const RECT* destination,
    HWND window, const RGNDATA* dirty, DWORD flags) {
    SwapPresent_t original = nullptr;
    if (chain && RegionAccessible(chain, sizeof(void*), false)) {
        auto** vtbl = *reinterpret_cast<void***>(chain);
        if (RegionAccessible(vtbl, 4 * sizeof(void*), false)) {
            void** slot = &vtbl[3];
            for (unsigned i = 0; i < gSwapPresentHookCount; ++i)
                if (gSwapPresentHooks[i].slot == slot) {
                    original = gSwapPresentHooks[i].original;
                    break;
                }
        }
    }
    if (!original) return D3DERR_INVALIDCALL;
    if (InterlockedCompareExchange(&gOverlaySwapPresentSeen, 1, 0) == 0)
        Log("OVERLAY swap-chain Present callback reached (chain=%p)", chain);
    if (gShowButtons && gPhotoActive && !gInsideOverlayPresent &&
        InterlockedCompareExchange(&gOverlayShutdown, 0, 0) == 0) {
        InterlockedExchange(&gOverlaySwapPresentActiveSeen, 1);
        IDirect3DDevice9* dev = nullptr;
        const HRESULT deviceResult = chain->GetDevice(&dev);
        if (SUCCEEDED(deviceResult) && dev) {
            if (gLastOverlayDrawSerial != gGameFrameSerial) {
                const HRESULT begun = dev->BeginScene();
                HRESULT drawn = begun;
                if (SUCCEEDED(begun)) {
                    gInsideOverlayPresent = true;
                    drawn = DrawU2ButtonOverlay(dev);
                    dev->EndScene();
                    gInsideOverlayPresent = false;
                    if (SUCCEEDED(drawn)) gLastOverlayDrawSerial = gGameFrameSerial;
                }
                if (InterlockedCompareExchange(&gOverlaySwapPresentResultLogged, 1, 0) == 0)
                    Log("OVERLAY native swap-chain BeginScene=%08X drawing=%08X", static_cast<unsigned>(begun), static_cast<unsigned>(drawn));
            } else if (InterlockedCompareExchange(&gOverlaySwapPresentResultLogged, 1, 0) == 0) {
                Log("OVERLAY swap-chain skipped because overlay already rendered this frame");
            }
            dev->Release();
        } else if (InterlockedCompareExchange(&gOverlaySwapPresentResultLogged, 1, 0) == 0) {
            Log("OVERLAY swap-chain GetDevice failed: %08X", static_cast<unsigned>(deviceResult));
        }
    }
    return original(chain, source, destination, window, dirty, flags);
}

static bool ReplaceVtable(void** slot, void* expected, void* replacement) {
    if (!RegionAccessible(slot,sizeof(void*),false)) return false;
    DWORD protect=0;
    if (!VirtualProtect(slot,sizeof(void*),PAGE_EXECUTE_READWRITE,&protect)) return false;
    void* prior=InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),replacement,expected);
    DWORD ignored=0;
    VirtualProtect(slot,sizeof(void*),protect,&ignored);
    return prior==expected;
}

static bool HookDevice(IDirect3DDevice9* device) {
    if (!device || !RegionAccessible(device, sizeof(void*), false)) return false;
    auto** vtbl = *reinterpret_cast<void***>(device);
    if (!RegionAccessible(vtbl, 43 * sizeof(void*), false)) return false;
    bool installed = false;
    {
        void** slot = &vtbl[42];
        EndSceneHookRecord* existing = nullptr;
        for (unsigned i = 0; i < gEndSceneHookCount; ++i)
            if (gEndSceneHooks[i].slot == slot) existing = &gEndSceneHooks[i];
        if (*slot == reinterpret_cast<void*>(&PhotoOverlayEndScene)) installed = true;
        else if (*slot && (existing || gEndSceneHookCount < 16)) {
            auto original = reinterpret_cast<EndScene_t>(*slot);
            if (ReplaceVtable(slot, reinterpret_cast<void*>(original), reinterpret_cast<void*>(&PhotoOverlayEndScene))) {
                if (existing) existing->original = original;
                else gEndSceneHooks[gEndSceneHookCount++] = {slot, original};
                Log("OVERLAY native EndScene hook installed/refreshed (slot=%p target=%p)", slot, reinterpret_cast<void*>(original));
                installed = true;
            }
        }
    }
    {
        void** slot = &vtbl[17];
        PresentHookRecord* existing = nullptr;
        for (unsigned i = 0; i < gPresentHookCount; ++i)
            if (gPresentHooks[i].slot == slot) existing = &gPresentHooks[i];
        if (*slot == reinterpret_cast<void*>(&PhotoOverlayPresent)) installed = true;
        else if (*slot && (existing || gPresentHookCount < 16)) {
            auto original = reinterpret_cast<Present_t>(*slot);
            if (ReplaceVtable(slot, reinterpret_cast<void*>(original), reinterpret_cast<void*>(&PhotoOverlayPresent))) {
                if (existing) existing->original = original;
                else gPresentHooks[gPresentHookCount++] = {slot, original};
                Log("OVERLAY native Present hook installed/refreshed (slot=%p target=%p)", slot, reinterpret_cast<void*>(original));
                installed = true;
            }
        }
    }
    return installed;
}

static bool HookSwapChain(IDirect3DSwapChain9* chain) {
    if (!chain || !RegionAccessible(chain, sizeof(void*), false)) return false;
    auto** vtbl = *reinterpret_cast<void***>(chain);
    if (!RegionAccessible(vtbl, 4 * sizeof(void*), false)) return false;
    void** slot = &vtbl[3];
    if (*slot == reinterpret_cast<void*>(&PhotoOverlaySwapPresent)) return true;
    SwapPresentHookRecord* existing = nullptr;
    for (unsigned i = 0; i < gSwapPresentHookCount; ++i)
        if (gSwapPresentHooks[i].slot == slot) existing = &gSwapPresentHooks[i];
    if (!*slot || (!existing && gSwapPresentHookCount >= 8)) return false;
    auto original = reinterpret_cast<SwapPresent_t>(*slot);
    if (!ReplaceVtable(slot, reinterpret_cast<void*>(original), reinterpret_cast<void*>(&PhotoOverlaySwapPresent))) return false;
    if (existing) existing->original = original;
    else gSwapPresentHooks[gSwapPresentHookCount++] = {slot, original};
    Log("OVERLAY native swap-chain Present hook installed/refreshed (slot=%p target=%p)", slot, reinterpret_cast<void*>(original));
    return true;
}

static void RefreshGameDeviceHooks() {
    if (InterlockedCompareExchange(&gOverlayShutdown, 0, 0) != 0) return;
    const DWORD now = GetTickCount();
    if (gLastHookRefresh && now - gLastHookRefresh < 250u) return;
    gLastHookRefresh = now;
    auto** pp = Checked<IDirect3DDevice9*>(kGameD3DDevice);
    if (!pp || !*pp || !RegionAccessible(*pp, sizeof(void*), false)) return;
    IDirect3DDevice9* device = *pp;
    if (device != gTrackedGameDevice) {
        gTrackedGameDevice = device;
        auto** vtable = *reinterpret_cast<void***>(device);
        Log("OVERLAY native SPEED2.EXE device global=%08X device=%p vtable=%p", static_cast<unsigned>(kGameD3DDevice), device, vtable);
    }
    HookDevice(device);
    IDirect3DSwapChain9* chain = nullptr;
    HRESULT hr = device->GetSwapChain(0, &chain);
    if (SUCCEEDED(hr) && chain) {
        HookSwapChain(chain);
        chain->Release();
    } else if (gPhotoActive) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("OVERLAY native GetSwapChain failed: %08X", static_cast<unsigned>(hr));
        }
    }
}

static void RestoreOverlayHooks() {
    InterlockedExchange(&gOverlayShutdown,1);
    for (unsigned i=0;i<gPresentHookCount;++i)
        if (gPresentHooks[i].slot && gPresentHooks[i].original)
            ReplaceVtable(gPresentHooks[i].slot,reinterpret_cast<void*>(&PhotoOverlayPresent),reinterpret_cast<void*>(gPresentHooks[i].original));
    for (unsigned i=0;i<gEndSceneHookCount;++i)
        if (gEndSceneHooks[i].slot && gEndSceneHooks[i].original)
            ReplaceVtable(gEndSceneHooks[i].slot,reinterpret_cast<void*>(&PhotoOverlayEndScene),reinterpret_cast<void*>(gEndSceneHooks[i].original));
    for (unsigned i=0;i<gSwapPresentHookCount;++i)
        if (gSwapPresentHooks[i].slot && gSwapPresentHooks[i].original)
            ReplaceVtable(gSwapPresentHooks[i].slot,reinterpret_cast<void*>(&PhotoOverlaySwapPresent),reinterpret_cast<void*>(gSwapPresentHooks[i].original));
}

static bool VerifyExe() {

    if (reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) != 0x00400000u) {
        Log("Mismatch: EXE not loaded at 0x00400000"); return false;
    }
    const uint8_t expectedCameraFn[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x14};
    const uint8_t expectedCall[] = {0xE8,0x48,0xDB,0xEC,0xFF};
    const uint8_t expectedChange[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x6A,0xFF};
    const uint8_t expectedOrbit[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x81,0xEC};
    const uint8_t expectedPause[] = {0xA1,0x88,0x00,0x89,0x00,0x85,0xC0};
    struct Check { uintptr_t address; const uint8_t* expected; size_t size; };
    const Check checks[] = {
        {kUpdateCameraFn, expectedCameraFn, sizeof(expectedCameraFn)},
        {kUpdateCameraCall, expectedCall, sizeof(expectedCall)},
        {kChangeCameraMover, expectedChange, sizeof(expectedChange)},
        {0x0044B230, expectedOrbit, sizeof(expectedOrbit)},
        {0x005EA320, expectedPause, sizeof(expectedPause)},
        {kSetupExploreCall, (const uint8_t*)"\xE8\x7C\x95\xFE\xFF", 5},
        {kSetupRaceCall, (const uint8_t*)"\xE8\x45\x98\xFE\xFF", 5},
        {kLanguageLookup, (const uint8_t*)"\x53\x8B\x1D\xD8\x83\x83\x00", 7},
    };
    for (const auto& c : checks) {
        const void* address = reinterpret_cast<const void*>(c.address);
        if (!RegionAccessible(address, c.size, false) || memcmp(address, c.expected, c.size) != 0) {
            Log("Mismatch: EXE signature @%08X", static_cast<unsigned>(c.address));
            return false;
        }
    }
    return true;
}

static DWORD WINAPI Initialize(LPVOID) {
    LoadOverlaySettings();
    Log("==== NFSU2_PhotoMode initialized ====");
    Log("OVERLAY ShowButtons=%s (%s)", gShowButtons ? "true" : "false", gIniPath[0] ? gIniPath : "INI not found");
    Log("MENU icon texture hash: %08X (INGAME_ICON_PHOTO_MODE)", kPhotoMenuIconHash);
    if (!VerifyExe()) {
        Log("ASI disabled: executable/signatures incompatible. No patch applied.");
        return 0;
    }
    if (!InstallLanguageHook()) {
        Log("ASI disabled: could not install guarded localization hook");
        return 0;
    }
    if (!InstallPauseHooks()) {
        Log("ASI disabled: could not install guarded native pause menu hooks");
        RestorePauseHooks();
        RestoreLanguageHook();
        return 0;
    }
    gOriginalCameraUpdate = reinterpret_cast<UpdateCamera_t>(kUpdateCameraFn);
    if (!PatchCall(kUpdateCameraCall, reinterpret_cast<uintptr_t>(&CameraFrameHook))) {
        gOriginalCameraUpdate = nullptr;
        RestorePauseHooks();
        RestoreLanguageHook();
        Log("ASI disabled: could not install guarded camera hook.");
        return 0;
    }
    gInstalled = true;
    Log("Native camera, pause menu and audio hooks installed.");
    if (gShowButtons)
        Log("OVERLAY B9 will hook the live SPEED2.EXE D3D9 device at %08X via CameraFrameHook", static_cast<unsigned>(kGameD3DDevice));

    return 0;
}

static void Cleanup() {
    if (!gInstalled) return;
    gPendingMenuOpen = false;
    RestoreOverlayHooks();

    StopPhoto(false, false);
    RestorePauseHooks();
    RestoreLanguageHook();

    const auto* p = reinterpret_cast<const uint8_t*>(kUpdateCameraCall);
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&CameraFrameHook) - (kUpdateCameraCall + 5));
    if (RegionAccessible(p, 5, false) && p[0] == 0xE8 && memcmp(p + 1, &rel, 4) == 0)
        PatchCall(kUpdateCameraCall, kUpdateCameraFn);
    gInstalled = false;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        gSelf = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        Cleanup();
    }
    return TRUE;
}
