#ifndef OPENRPC_EXTERNAL_H
#define OPENRPC_EXTERNAL_H
#include "stdafx.h"
#include "RpcCore.h"
#include <new>
extern "C" {
    void __stdcall HalSendSMCMessage(void*, void*);
    int MmGetPhysicalAddress(void*);
    HRESULT XeKeysGetVersions(DWORD, DWORD, __int64, __int64, __int64);
}
bool MemoryRange(DWORD address, size_t size, bool write = false) {
    if (!OpenRpc::Range(address, size)) return false;
    if (!size) return true;
    DWORD last = address + DWORD(size - 1), page = address;
    for (;;) {
        if (!MmIsAddressValid((PVOID)page)) return false;
        DWORD protection = XQueryMemoryProtect((PVOID)page);
        if (!protection || (protection & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        if (write && !(protection & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
        if ((page >> 12) == (last >> 12)) break;
        page = (page & ~0xfffU) + 0x1000;
    }
    return true;
}
bool SafeCopy(void* output, DWORD address, size_t size) {
    if (!MemoryRange(address, size)) return false;
    __try { memcpy(output, (const void*)address, size); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ExecutableTarget(DWORD address) {
    if (!OpenRpc::Range(address, 4, 4) || !MemoryRange(address, 4)) return false;
    DWORD p = XQueryMemoryProtect((PVOID)address);
    return (p & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
DWORD ResolveFunction(const char* module, DWORD ordinal) {
    HMODULE handle = GetModuleHandle(module);
    return handle ? (DWORD)GetProcAddress(handle, (LPCSTR)ordinal) : 0;
}
bool ModuleBounds(const char* name, DWORD& base, DWORD& size) {
    DWORD handle = (DWORD)GetModuleHandle(name);
    if (!handle || !SafeCopy(&base, handle + offsetof(LDR_DATA_TABLE_ENTRY, ImageBase), 4) ||
        !SafeCopy(&size, handle + offsetof(LDR_DATA_TABLE_ENTRY, SizeOfNtImage), 4)) return false;
    return size >= 0x1000 && size <= 0x4000000 && OpenRpc::Range(base, size);
}
bool InModule(DWORD address, size_t bytes, DWORD base, DWORD size) {
    return address >= base && bytes <= size && OpenRpc::U64(address) + bytes <= OpenRpc::U64(base) + size && MemoryRange(address, bytes);
}
bool Popup(const char* text, XNOTIFYQUEUEUI_TYPE type) {
    wchar_t message[512];
    size_t n = mbstowcs(message, text, 511);
    if (n == size_t(-1)) return false;
    message[n] = 0;
    XNotifyQueueUI(type, 0xFF, 2, message, 0);
    return true;
}
void __declspec(naked) InvalidateLine(void*) {
    __asm {
        icbi r0, r3
        blr
    }
}
void FlushCode(DWORD* address, unsigned bytes) {
    for (unsigned i = 0; i < bytes; i += 4) __dcbst(0, (char*)address + i);
    __sync();
    for (unsigned i = 0; i < bytes; i += 4) InvalidateLine((char*)address + i);
    __sync(); __isync();
}
void JumpWords(DWORD* out, DWORD target) {
    OpenRpc::U32 words[4]; OpenRpc::Jump(words, target); memcpy(out, words, sizeof(words));
}
HRESULT __declspec(naked) HrGetFileStub(char*, LPSTR, DWORD, void*) {
    __asm {
        li r3, 0x4649
        nop
        nop
        nop
        nop
        nop
        nop
        blr
    }
}
HRESULT __declspec(naked) HrConsoleFeaturesStub(char*, LPSTR, DWORD, void*) {
    __asm {
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        blr
    }
}
VOID __declspec(naked) GLPR_FUN(VOID) {
	__asm {
		std     r14, -0x98(sp)
		std     r15, -0x90(sp)
		std     r16, -0x88(sp)
		std     r17, -0x80(sp)
		std     r18, -0x78(sp)
		std     r19, -0x70(sp)
		std     r20, -0x68(sp)
		std     r21, -0x60(sp)
		std     r22, -0x58(sp)
		std     r23, -0x50(sp)
		std     r24, -0x48(sp)
		std     r25, -0x40(sp)
		std     r26, -0x38(sp)
		std     r27, -0x30(sp)
		std     r28, -0x28(sp)
		std     r29, -0x20(sp)
		std     r30, -0x18(sp)
		std     r31, -0x10(sp)
		stw     r12, -0x8(sp)
		blr
	}
}


struct PreparedHook { DWORD target; DWORD* stub; DWORD original[4], trampoline[8], jump[4]; };
struct SaveBounds { DWORD base, size; };
bool ResolveSaveHelper(void* context, OpenRpc::U32 source, OpenRpc::U32& destination) {
    const SaveBounds& bounds = *(const SaveBounds*)context;
    const DWORD* save = (const DWORD*)GLPR_FUN;
    for (unsigned k = 0; k < 19; ++k) {
        DWORD candidate[20]; size_t bytes = (20 - k) * 4;
        if (InModule(source, bytes, bounds.base, bounds.size) && SafeCopy(candidate, source, bytes) && !memcmp(candidate, save + k, bytes)) {
            destination = (DWORD)&save[k]; return true;
        }
    }
    return false;
}
bool PrepareHook(PreparedHook& hook, DWORD address, DWORD* stub, DWORD destination, DWORD base, DWORD size) {
    if (!InModule(address, 16, base, size) || !ExecutableTarget(address) || !SafeCopy(hook.original, address, 16)) return false;
    hook.target = address; hook.stub = stub;
    OpenRpc::U32 original[4], trampoline[8]; memcpy(original, hook.original, 16);
    SaveBounds bounds = {base, size};
    if (!OpenRpc::Trampoline(original, address, (DWORD)stub, trampoline, ResolveSaveHelper, &bounds)) return false;
    memcpy(hook.trampoline, trampoline, sizeof(trampoline));
    JumpWords(hook.jump, destination);
    return true;
}
bool InstallHook(const PreparedHook& hook) {
    __try {
        memcpy(hook.stub, hook.trampoline, sizeof(hook.trampoline)); FlushCode(hook.stub, sizeof(hook.trampoline));
        memcpy((void*)hook.target, hook.jump, sizeof(hook.jump)); FlushCode((DWORD*)hook.target, sizeof(hook.jump));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void RestoreHook(const PreparedHook& hook) {
    __try {
        memcpy((void*)hook.target, hook.original, sizeof(hook.original)); FlushCode((DWORD*)hook.target, sizeof(hook.original));
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
#endif
