#include "pch.h"
#include <Windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <iostream>
#include <fstream>
#include <set>
#include <mutex>
#include <atomic>
#include "MinHook.h"
#include <string>

#pragma comment(lib, "dxgi.lib")

std::mutex spooferMutex;
std::set<void*> hookedFunctions;
std::atomic<bool> g_adapterHooked{ false };
std::atomic<bool> g_initComplete{ false };

// ===== Logging: only active during init =====
void Log(const char* text) {
    if (g_initComplete.load(std::memory_order_relaxed)) return; // silence after init
    std::lock_guard<std::mutex> lock(spooferMutex);
    std::ofstream logFile("spoofer_log.txt", std::ios::app);
    if (logFile.is_open()) {
        logFile << text << "\n";
    }
}

typedef HRESULT(STDMETHODCALLTYPE* GetDesc_t)(IDXGIAdapter*, DXGI_ADAPTER_DESC*);
typedef HRESULT(STDMETHODCALLTYPE* GetDesc1_t)(IDXGIAdapter1*, DXGI_ADAPTER_DESC1*);
typedef HRESULT(STDMETHODCALLTYPE* QueryVideoMemoryInfo_t)(IDXGIAdapter3*, UINT, DXGI_MEMORY_SEGMENT_GROUP, DXGI_QUERY_VIDEO_MEMORY_INFO*);
typedef HRESULT(WINAPI* CreateDXGIFactory1_t)(REFIID, void**);
typedef HRESULT(WINAPI* CreateDXGIFactory2_t)(UINT, REFIID, void**);

GetDesc_t oGetDesc = nullptr;
GetDesc1_t oGetDesc1 = nullptr;
QueryVideoMemoryInfo_t oQueryVideoMemoryInfo = nullptr;
CreateDXGIFactory1_t oCreateDXGIFactory1 = nullptr;
CreateDXGIFactory2_t oCreateDXGIFactory2 = nullptr;

const UINT64 EIGHT_GB = (UINT64)8 * 1024 * 1024 * 1024;
const UINT64 FOUR_GB = (UINT64)4 * 1024 * 1024 * 1024;

// Pre-built spoofed descriptions — filled once, returned every time
static DXGI_ADAPTER_DESC  g_spoofedDesc = {};
static DXGI_ADAPTER_DESC1 g_spoofedDesc1 = {};
static bool g_descCached = false;
static bool g_desc1Cached = false;

template <typename T>
void ApplySpoof(T* pDesc) {
    pDesc->DedicatedVideoMemory = EIGHT_GB;
    pDesc->DedicatedSystemMemory = 0;
    pDesc->SharedSystemMemory = FOUR_GB;
    pDesc->VendorId = 0x10DE;
    pDesc->DeviceId = 0x2504;
    wcscpy_s(pDesc->Description, L"NVIDIA GeForce RTX 3060");
}

bool SafeCreateHook(void* targetFunc, LPVOID detourFunc, LPVOID* originalFunc, const char* name) {
    if (!targetFunc) return false;
    if (hookedFunctions.find(targetFunc) != hookedFunctions.end()) return false;
    MH_STATUS status = MH_CreateHook(targetFunc, detourFunc, originalFunc);
    if (status == MH_OK) {
        MH_STATUS es = MH_EnableHook(targetFunc);
        if (es == MH_OK) {
            hookedFunctions.insert(targetFunc);
            Log((std::string("SUCCESS: Hooked ") + name).c_str());
            return true;
        }
    }
    Log((std::string("FAIL: ") + name + " - " + MH_StatusToString(status)).c_str());
    return false;
}

// ===== Hooked functions — no logging, no mutex, just memcpy =====
HRESULT STDMETHODCALLTYPE Hooked_GetDesc(IDXGIAdapter* pAdapter, DXGI_ADAPTER_DESC* pDesc) {
    HRESULT hr = oGetDesc(pAdapter, pDesc);
    if (SUCCEEDED(hr) && pDesc) {
        if (!g_descCached) {
            g_spoofedDesc = *pDesc;
            ApplySpoof(&g_spoofedDesc);
            g_descCached = true;
        }
        *pDesc = g_spoofedDesc;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_GetDesc1(IDXGIAdapter1* pAdapter, DXGI_ADAPTER_DESC1* pDesc) {
    HRESULT hr = oGetDesc1(pAdapter, pDesc);
    if (SUCCEEDED(hr) && pDesc) {
        if (!g_desc1Cached) {
            g_spoofedDesc1 = *pDesc;
            ApplySpoof(&g_spoofedDesc1);
            g_desc1Cached = true;
        }
        *pDesc = g_spoofedDesc1;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_QueryVideoMemoryInfo(IDXGIAdapter3* pAdapter, UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup, DXGI_QUERY_VIDEO_MEMORY_INFO* pInfo) {
    HRESULT hr = oQueryVideoMemoryInfo(pAdapter, NodeIndex, MemorySegmentGroup, pInfo);
    if (SUCCEEDED(hr) && pInfo) {
        pInfo->Budget = EIGHT_GB;
        pInfo->CurrentUsage = 1024 * 1024;
        pInfo->AvailableForReservation = EIGHT_GB;
    }
    return hr;
}

// ===== Hook adapter vtable =====
void HookAdapterVtable(IDXGIAdapter1* pAdapter) {
    if (!pAdapter || g_adapterHooked.load()) return;
    void** vtable = *(void***)pAdapter;

    char buf[256];
    sprintf_s(buf, "Adapter vtable base=%p [8]=%p [12]=%p", vtable, vtable[8], vtable[12]);
    Log(buf);

    DXGI_ADAPTER_DESC1 desc = {};
    HRESULT hr = pAdapter->GetDesc1(&desc);
    if (FAILED(hr)) {
        Log("GetDesc1 call FAILED - skipping");
        return;
    }
    sprintf_s(buf, "Real GPU: %ws, VRAM: %llu MB", desc.Description, desc.DedicatedVideoMemory / (1024 * 1024));
    Log(buf);

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

// ===== Factory hooks =====
HRESULT WINAPI Hooked_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    Log("Hooked_CreateDXGIFactory1 CALLED");
    HRESULT hr = oCreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory && !g_adapterHooked.load()) {
        __try {
            IDXGIFactory1* pFactory = nullptr;
            if (SUCCEEDED(((IUnknown*)*ppFactory)->QueryInterface(__uuidof(IDXGIFactory1), (void**)&pFactory))) {
                IDXGIAdapter1* pAdapter = nullptr;
                if (SUCCEEDED(pFactory->EnumAdapters1(0, &pAdapter))) {
                    HookAdapterVtable(pAdapter);
                    pAdapter->Release();
                }
                pFactory->Release();
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("EXCEPTION in Factory1 adapter hook");
        }
    }
    return hr;
}

HRESULT WINAPI Hooked_CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    Log("Hooked_CreateDXGIFactory2 CALLED");
    HRESULT hr = oCreateDXGIFactory2(Flags, riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory && !g_adapterHooked.load()) {
        __try {
            IDXGIFactory1* pFactory = nullptr;
            if (SUCCEEDED(((IUnknown*)*ppFactory)->QueryInterface(__uuidof(IDXGIFactory1), (void**)&pFactory))) {
                IDXGIAdapter1* pAdapter = nullptr;
                if (SUCCEEDED(pFactory->EnumAdapters1(0, &pAdapter))) {
                    HookAdapterVtable(pAdapter);
                    pAdapter->Release();
                }
                pFactory->Release();
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("EXCEPTION in Factory2 adapter hook");
        }
    }
    return hr;
}

DWORD WINAPI InitHookThread(LPVOID lpParam) {
    Sleep(50);
    Log("=== Spoofer v9 Init ===");

    if (MH_Initialize() != MH_OK) {
        Log("MinHook FAILED");
        return 1;
    }
    Log("MinHook OK");

    MH_CreateHookApi(L"dxgi.dll", "CreateDXGIFactory1",
        (LPVOID)&Hooked_CreateDXGIFactory1, (LPVOID*)&oCreateDXGIFactory1);
    MH_CreateHookApi(L"dxgi.dll", "CreateDXGIFactory2",
        (LPVOID)&Hooked_CreateDXGIFactory2, (LPVOID*)&oCreateDXGIFactory2);
    MH_EnableHook(MH_ALL_HOOKS);
    Log("API hooks enabled");

    Sleep(3000);
    if (!g_adapterHooked.load()) {
        Log("No factory hook fired - trying direct fallback");
        if (oCreateDXGIFactory1) {
            IDXGIFactory1* pFactory = nullptr;
            if (SUCCEEDED(oCreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory))) {
                IDXGIAdapter1* pAdapter = nullptr;
                if (SUCCEEDED(pFactory->EnumAdapters1(0, &pAdapter))) {
                    HookAdapterVtable(pAdapter);
                    pAdapter->Release();
                }
                pFactory->Release();
            }
        }
    }

    g_initComplete.store(true); // disable logging from here on
    Log("=== Init complete, logging disabled ===");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, InitHookThread, NULL, 0, NULL);
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    return TRUE;
}