#include "stdafx.h"
#include "OpenRPC.h"
#include "IniParser.h"

void ApplyNotificationWorkaround() {
    DWORD base = 0, size = 0, address = ResolveFunction("xam.xex", 1183), instruction = 0;
    if (!address || !ModuleBounds("xam.xex", base, size) || !InModule(address + 0x30, 4, base, size) ||
        !ExecutableTarget(address + 0x30) || !SafeCopy(&instruction, address + 0x30, 4) || (instruction >> 16) != 0x409A) return;
    instruction = (instruction & 0xffff) | 0x48000000;
    __try { *(volatile DWORD*)(address + 0x30) = instruction; FlushCode((DWORD*)(address + 0x30), 4); }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}
void ReadConfiguration() {
    HANDLE file = CreateFile("Hdd:\\JRPC.ini", GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(file, 0), read = 0;
    if (size == INVALID_FILE_SIZE || size > OpenRpc::MaxConfig) { CloseHandle(file); return; }
    char* data = new(std::nothrow) char[size + 1];
    if (!data) { CloseHandle(file); return; }
    BOOL ok = ReadFile(file, data, size, &read, 0); CloseHandle(file);
    if (!ok || read > size) { delete[] data; return; }
    data[read] = 0;
    OpenRpc::Span section, input(data, read);
    if (OpenRpc::ConfigSection(input, "Plugins", section)) {
        for (unsigned i = 1; i <= 32; ++i) {
            char key[20], path[MAX_PATH];
            _snprintf(key, sizeof(key), "plugin%u", i);
            if (!OpenRpc::ConfigString(section, key, path, sizeof(path))) break;
            XexLoadImage(path, 8, 0, 0);
        }
    }
    if (OpenRpc::ConfigSection(input, "Settings", section)) {
        bool protect = true;
        if (OpenRpc::ConfigBool(section, "KV Stealer Protection", protect)) InterlockedExchange(&KVProtection, protect ? 1 : 0);
    }
    delete[] data;
}
DWORD WINAPI XBDMPatch(LPVOID) {
    MountPath("Hdd:", "\\Device\\Harddisk0\\Partition1");
    ApplyNotificationWorkaround();
    ReadConfiguration();
    for (unsigned i = 0; i < 6 && !ResolveFunction("xbdm.xex", 1); ++i) Sleep(612);
    if (!GetXbdmDllType()) { DbgPrint("Current xbdm.xex is unavailable or unsupported"); return 0; }
    InterlockedExchange(&Ready, 1);
    while (!InterlockedCompareExchange(&Stopping, 0, 0)) {
        ReapRequests();
        MemorySetter settings[40]; unsigned count = 0;
        { StateGuard guard; count = MemorySettingCount; memcpy(settings, MemorySettings, count * sizeof(MemorySetter)); }
        DWORD title = XamGetCurrentTitleId();
        for (unsigned i = 0; i < count; ++i) {
            if (settings[i].titleFilter && settings[i].title && settings[i].title != title) continue;
            ApplyMemorySetting(settings[i]);
        }
        Sleep(100);
    }
    return 0;
}
BOOL APIENTRY DllMain(HANDLE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&StateLock);
        HANDLE thread = 0; DWORD threadId = 0;
        DWORD status = ExCreateThread(&thread, 0, &threadId, (PVOID)XapiThreadStartup, XBDMPatch, 0, 2);
        if (status != 0 || !thread) { if (thread) CloseHandle(thread); DeleteCriticalSection(&StateLock); return FALSE; }
        CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&Ready, 0); InterlockedExchange(&Stopping, 1);
    }
    return TRUE;
}
