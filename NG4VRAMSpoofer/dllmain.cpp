#include "pch.h"

// Suppress redefinition linkage warnings BEFORE including dxgi headers
#pragma warning(disable: 4390) // empty controlled statement

#include <Windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <set>
#include <atomic>
#include "MinHook.h"


// DO NOT link dxgi.lib
// NO: #pragma comment(lib, "dxgi.lib")

#pragma warning(disable: 4273)

// ===== Real system DXGI =====
HMODULE g_hRealDxgi = NULL;

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory)(REFIID, void**);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
typedef HRESULT(WINAPI* PFN_DXGIGetDebugInterface1)(UINT, REFIID, void**);
typedef HRESULT(WINAPI* PFN_DXGIDeclareAdapterRemovalSupport)();

PFN_CreateDXGIFactory       pReal_CreateDXGIFactory = nullptr;
PFN_CreateDXGIFactory1      pReal_CreateDXGIFactory1 = nullptr;
PFN_CreateDXGIFactory2      pReal_CreateDXGIFactory2 = nullptr;
PFN_DXGIGetDebugInterface1  pReal_DXGIGetDebugInterface1 = nullptr;
PFN_DXGIDeclareAdapterRemovalSupport pReal_DXGIDeclareAdapterRemovalSupport = nullptr;

void LoadRealDxgi() {
    if (g_hRealDxgi) return;
    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    strcat_s(sysPath, "\\dxgi.dll");
    g_hRealDxgi = LoadLibraryA(sysPath);
    if (g_hRealDxgi) {
        pReal_CreateDXGIFactory = (PFN_CreateDXGIFactory)GetProcAddress(g_hRealDxgi, "CreateDXGIFactory");
        pReal_CreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(g_hRealDxgi, "CreateDXGIFactory1");
        pReal_CreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(g_hRealDxgi, "CreateDXGIFactory2");
        pReal_DXGIGetDebugInterface1 = (PFN_DXGIGetDebugInterface1)GetProcAddress(g_hRealDxgi, "DXGIGetDebugInterface1");
        pReal_DXGIDeclareAdapterRemovalSupport = (PFN_DXGIDeclareAdapterRemovalSupport)GetProcAddress(g_hRealDxgi, "DXGIDeclareAdapterRemovalSupport");
    }
}

// ===== Spoofing =====
const UINT64 EIGHT_GB = (UINT64)8 * 1024 * 1024 * 1024;
const UINT64 FOUR_GB = (UINT64)4 * 1024 * 1024 * 1024;

static DXGI_ADAPTER_DESC  g_spoofedDesc = {};
static DXGI_ADAPTER_DESC1 g_spoofedDesc1 = {};
static std::atomic<bool> g_descCached{ false };
static std::atomic<bool> g_desc1Cached{ false };
static std::atomic<bool> g_adapterHooked{ false };
static std::atomic<bool> g_mhReady{ false };
static std::set<void*> hookedFunctions;

template <typename T>
void ApplySpoof(T* pDesc) {
    pDesc->DedicatedVideoMemory = EIGHT_GB;
    pDesc->DedicatedSystemMemory = 0;
    pDesc->SharedSystemMemory = FOUR_GB;
    pDesc->VendorId = 0x10DE;
    pDesc->DeviceId = 0x2504;
    wcscpy_s(pDesc->Description, L"NVIDIA GeForce RTX 3060");
}

// ===== MinHook vtable hooks =====
typedef HRESULT(STDMETHODCALLTYPE* GetDesc_t)(IDXGIAdapter*, DXGI_ADAPTER_DESC*);
typedef HRESULT(STDMETHODCALLTYPE* GetDesc1_t)(IDXGIAdapter1*, DXGI_ADAPTER_DESC1*);
typedef HRESULT(STDMETHODCALLTYPE* QueryVideoMemoryInfo_t)(IDXGIAdapter3*, UINT, DXGI_MEMORY_SEGMENT_GROUP, DXGI_QUERY_VIDEO_MEMORY_INFO*);

GetDesc_t oGetDesc = nullptr;
GetDesc1_t oGetDesc1 = nullptr;
QueryVideoMemoryInfo_t oQueryVideoMemoryInfo = nullptr;

bool SafeCreateHook(void* target, LPVOID detour, LPVOID* original, const char* name) {
    if (!target || !g_mhReady.load()) return false;
    if (hookedFunctions.find(target) != hookedFunctions.end()) return false;
    if (MH_CreateHook(target, detour, original) == MH_OK) {
        if (MH_EnableHook(target) == MH_OK) {
            hookedFunctions.insert(target);
            return true;
        }
    }
    return false;
}

HRESULT STDMETHODCALLTYPE Hooked_GetDesc(IDXGIAdapter* pAdapter, DXGI_ADAPTER_DESC* pDesc) {
    HRESULT hr = oGetDesc(pAdapter, pDesc);
    if (SUCCEEDED(hr) && pDesc) {
        if (!g_descCached.load()) {
            g_spoofedDesc = *pDesc;
            ApplySpoof(&g_spoofedDesc);
            g_descCached.store(true);
        }
        *pDesc = g_spoofedDesc;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_GetDesc1(IDXGIAdapter1* pAdapter, DXGI_ADAPTER_DESC1* pDesc) {
    HRESULT hr = oGetDesc1(pAdapter, pDesc);
    if (SUCCEEDED(hr) && pDesc) {
        if (!g_desc1Cached.load()) {
            g_spoofedDesc1 = *pDesc;
            ApplySpoof(&g_spoofedDesc1);
            g_desc1Cached.store(true);
        }
        *pDesc = g_spoofedDesc1;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_QueryVideoMemoryInfo(IDXGIAdapter3* pAdapter, UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP Grp, DXGI_QUERY_VIDEO_MEMORY_INFO* pInfo) {
    HRESULT hr = oQueryVideoMemoryInfo(pAdapter, NodeIndex, Grp, pInfo);
    if (SUCCEEDED(hr) && pInfo) {
        pInfo->Budget = EIGHT_GB;
        pInfo->CurrentUsage = 1024 * 1024;
        pInfo->AvailableForReservation = EIGHT_GB;
    }
    return hr;
}

void HookAdapterVtable(IDXGIAdapter1* pAdapter) {
    if (!pAdapter || g_adapterHooked.load()) return;
    void** vtable = *(void***)pAdapter;

    SafeCreateHook(vtable[8], (LPVOID)&Hooked_GetDesc, (LPVOID*)&oGetDesc, "GetDesc[8]");
    SafeCreateHook(vtable[12], (LPVOID)&Hooked_GetDesc1, (LPVOID*)&oGetDesc1, "GetDesc1[12]");

    IDXGIAdapter3* pAdapter3 = nullptr;
    if (SUCCEEDED(pAdapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&pAdapter3))) {
        void** vt3 = *(void***)pAdapter3;
        SafeCreateHook(vt3[14], (LPVOID)&Hooked_QueryVideoMemoryInfo, (LPVOID*)&oQueryVideoMemoryInfo, "QueryVideoMemoryInfo[14]");
        pAdapter3->Release();
    }

    g_adapterHooked.store(true);
}

// Helper: grab adapter from factory and hook it
void TryHookFromFactory(void* pFactoryVoid) {
    if (!pFactoryVoid || g_adapterHooked.load()) return;
    __try {
        IDXGIFactory1* pFactory = nullptr;
        if (SUCCEEDED(((IUnknown*)pFactoryVoid)->QueryInterface(__uuidof(IDXGIFactory1), (void**)&pFactory))) {
            IDXGIAdapter1* pAdapter = nullptr;
            if (SUCCEEDED(pFactory->EnumAdapters1(0, &pAdapter))) {
                HookAdapterVtable(pAdapter);
                pAdapter->Release();
            }
            pFactory->Release();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ===== Exported DXGI functions =====
extern "C" {

    HRESULT WINAPI Proxy_CreateDXGIFactory(REFIID riid, void** ppFactory) {
        LoadRealDxgi();
        if (!pReal_CreateDXGIFactory) return E_FAIL;
        HRESULT hr = pReal_CreateDXGIFactory(riid, ppFactory);
        if (SUCCEEDED(hr)) TryHookFromFactory(*ppFactory);
        return hr;
    }

    HRESULT WINAPI Proxy_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
        LoadRealDxgi();
        if (!pReal_CreateDXGIFactory1) return E_FAIL;
        HRESULT hr = pReal_CreateDXGIFactory1(riid, ppFactory);
        if (SUCCEEDED(hr)) TryHookFromFactory(*ppFactory);
        return hr;
    }

    HRESULT WINAPI Proxy_CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
        LoadRealDxgi();
        if (!pReal_CreateDXGIFactory2) return E_FAIL;
        HRESULT hr = pReal_CreateDXGIFactory2(Flags, riid, ppFactory);
        if (SUCCEEDED(hr)) TryHookFromFactory(*ppFactory);
        return hr;
    }

    HRESULT WINAPI Proxy_DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** ppDebug) {
        LoadRealDxgi();
        return pReal_DXGIGetDebugInterface1 ? pReal_DXGIGetDebugInterface1(Flags, riid, ppDebug) : E_FAIL;
    }

    HRESULT WINAPI Proxy_DXGIDeclareAdapterRemovalSupport() {
        LoadRealDxgi();
        return pReal_DXGIDeclareAdapterRemovalSupport ? pReal_DXGIDeclareAdapterRemovalSupport() : E_FAIL;
    }

} // extern "C"

// ===== DllMain =====
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        LoadRealDxgi();
        if (MH_Initialize() == MH_OK) {
            g_mhReady.store(true);
        }
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        if (g_mhReady.load()) {
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
        }
        if (g_hRealDxgi) FreeLibrary(g_hRealDxgi);
    }
    return TRUE;
}