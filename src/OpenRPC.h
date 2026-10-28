#ifndef OPENRPC_PLUGIN_H
#define OPENRPC_PLUGIN_H
#include "External.h"
#include "FileParser.h"
#include "RpcJobs.h"

CRITICAL_SECTION StateLock;
volatile LONG Ready = 0, Stopping = 0, KVProtection = 1;
struct StateGuard {
    StateGuard() { EnterCriticalSection(&StateLock); }
    ~StateGuard() { LeaveCriticalSection(&StateLock); }
private:
    StateGuard(const StateGuard&);
    StateGuard& operator=(const StateGuard&);
};
typedef OpenRpc::MemorySetting MemorySetter;
MemorySetter MemorySettings[40];
unsigned MemorySettingCount = 0;

void DbgPrint(const char* format, ...) {
    char buffer[512]; va_list ap; va_start(ap, format);
    _vsnprintf(buffer, sizeof(buffer) - 1, format, ap); va_end(ap);
    buffer[sizeof(buffer)-1] = 0;
    printf("[JRPC] %s\n", buffer);
}
struct RpcJob {
    OpenRpc::Request request;
    volatile LONG references, done;
    DWORD token, finished;
    size_t owner;
    bool failed;
    char result[OpenRpc::MaxResult];
    RpcJob() : references(1), done(0), token(0), finished(0), owner(0), failed(false) { result[0] = 0; }
};
struct JobPlatform {
    static long Increment(volatile long* p) { return InterlockedIncrement(p); }
    static long Decrement(volatile long* p) { return InterlockedDecrement(p); }
    static long Read(volatile long* p) { return InterlockedCompareExchange(p, 0, 0); }
    static void Exchange(volatile long* p, long n) { InterlockedExchange(p, n); }
    static void Lock() { EnterCriticalSection(&StateLock); }
    static void Unlock() { LeaveCriticalSection(&StateLock); }
};
OpenRpc::JobStore<RpcJob, JobPlatform> Jobs;
void AddReference(RpcJob* p) { Jobs.Add(p); }
void ReleaseJob(RpcJob* p) { Jobs.Release(p); }
struct JobReference {
    RpcJob* value;
    explicit JobReference(RpcJob* p = 0) : value(p) {}
    ~JobReference() { ReleaseJob(value); }
private:
    JobReference(const JobReference&);
    JobReference& operator=(const JobReference&);
};
RpcJob* CreateJob() { return Jobs.Create(); }
void JobError(RpcJob& job, const char* message) {
    job.failed = true;
    OpenRpc::Text text(job.result, sizeof(job.result)); text.Format("error=%s", message);
}
HRESULT ResponseError(LPSTR response, size_t capacity, const char* message) {
    OpenRpc::Text text(response, capacity); text.Format("error=%s", message);
    return text.Good() ? 0x2DA : E_FAIL;
}
HRESULT CopyResponse(LPSTR response, size_t capacity, const char* text, bool failed = false) {
    size_t length = strlen(text);
    if (length >= capacity) return ResponseError(response, capacity, "Result exceeds response buffer");
    memcpy(response, text, length + 1);
    return failed ? 0x2DA : S_FALSE;
}
void ReapRequests() {
    Jobs.Reap(GetTickCount(), 30000);
}

void __declspec(naked) LoadToFPR(...) { __asm blr }
typedef __int64 (__cdecl *RpcFunction)(...);

struct ResultMemory {
    bool Read(DWORD address, unsigned width, OpenRpc::U64& bits) {
        if (width == 1) { BYTE value; if (!SafeCopy(&value,address,1)) return false; bits = value; }
        else if (width == 4) { DWORD value; if (!SafeCopy(&value,address,4)) return false; bits = value; }
        else if (width == 8) { if (!SafeCopy(&bits,address,8)) return false; }
        else return false;
        return true;
    }
};
bool CaptureString(RpcJob& job, DWORD address) {
    OpenRpc::Text text(job.result,sizeof(job.result)); ResultMemory memory;
    return OpenRpc::StringResult(text,address,memory);
}
bool CaptureArray(RpcJob& job, DWORD address) {
    OpenRpc::Text text(job.result,sizeof(job.result)); ResultMemory memory;
    return OpenRpc::ArrayResult(text,job.request.type,job.request.returnCount,address,memory);
}
void InvokeRpc(RpcJob& job) {
    OpenRpc::Request& r = job.request;
    if (!ExecutableTarget(r.target)) { JobError(job, "Call address is invalid or not executable"); return; }
    LoadToFPR(r.fpr[0], r.fpr[1], r.fpr[2], r.fpr[3], r.fpr[4], r.fpr[5], r.fpr[6], r.fpr[7], r.fpr[8], r.fpr[9], r.fpr[10], r.fpr[11], r.fpr[12]);
    OpenRpc::U64 integer = ((RpcFunction)r.target)(r.params[0], r.params[1], r.params[2], r.params[3], r.params[4], r.params[5], r.params[6], r.params[7], r.params[8], r.params[9], r.params[10], r.params[11], r.params[12], r.params[13], r.params[14], r.params[15], r.params[16], r.params[17], r.params[18], r.params[19], r.params[20], r.params[21], r.params[22], r.params[23], r.params[24], r.params[25], r.params[26], r.params[27], r.params[28], r.params[29], r.params[30], r.params[31], r.params[32], r.params[33], r.params[34], r.params[35], r.params[36]);
    double floating;
    __asm fmr floating, fp1
    OpenRpc::Text text(job.result, sizeof(job.result));
    switch (r.type) {
    case 0: case 1: case 3: case 4: case 8: OpenRpc::ScalarResult(text, r.type, integer, floating); break;
    case 2: if (!CaptureString(job, DWORD(integer))) JobError(job, "Invalid or oversized string result"); break;
    case 5: case 6: case 7: case 9:
        if (!CaptureArray(job, DWORD(integer))) JobError(job, "Invalid or oversized array result");
        break;
    default: JobError(job, "Unsupported return type"); break;
    }
    if (!text.Good()) JobError(job, "Result exceeds internal response limit");
}
void InvokeProtected(RpcJob* job) {
    __try { InvokeRpc(*job); }
    __except(EXCEPTION_EXECUTE_HANDLER) { JobError(*job, "Target function raised an exception"); }
}
DWORD WINAPI JRPCCaller(LPVOID context) {
    RpcJob* job = (RpcJob*)context;
    InvokeProtected(job);
    Jobs.Complete(job, GetTickCount());
    ReleaseJob(job);
    return 0;
}

struct LaunchData {
    DWORD id, type;
    char link[MAX_PATH], device[MAX_PATH];
    USHORT major, minor, kernel, svn;
    DWORD options, debugRoutine, debugStep;
    BYTE* temperatures;
    DWORD iniPath;
};
void RunCustom(RpcJob& job) {
    OpenRpc::Request& r = job.request;
    if (!OpenRpc::CustomArguments(r)) { JobError(job, "Invalid or missing command arguments"); return; }
    OpenRpc::Text text(job.result, sizeof(job.result));
    switch (r.type) {
    case 9:
        text.Format("%X", ResolveFunction((const char*)r.args[0].bytes, DWORD(r.params[1])));
        break;
    case 10: {
        void* key = XPhysicalAlloc(0x40000, MAXULONG_PTR, 0, PAGE_READWRITE);
        if (!key) { JobError(job, "Physical allocation failed"); return; }
        memset(key, 0, 16);
        HRESULT status = XeKeysGetVersions(0x72627472, 5, 0x20,
            (OpenRpc::U64(DWORD(MmGetPhysicalAddress(key))) | (OpenRpc::U64(1) << 63)), 16);
        if (status < 0) JobError(job, "Key service failed");
        else for (unsigned i = 0; i < 16; ++i) text.Format("%02X", unsigned(((BYTE*)key)[i]));
        XPhysicalFree(key); break;
    }
    case 11: HalReturnToFirmware(HalPowerDownRoutine); text.Append("S_OK"); break;
    case 12:
        if (!Popup((const char*)r.args[0].bytes, (XNOTIFYQUEUEUI_TYPE)DWORD(r.params[1]))) JobError(job, "Invalid notification text encoding");
        else text.Append("S_OK");
        break;
    case 13: text.Format("%d", XboxKrnlVersion->Build); break;
    case 14: {
        BYTE packet[16] = {0}; packet[0] = 0x99; packet[1] = 1;
        packet[2] = BYTE((DWORD(r.params[0]) >> 3) | (DWORD(r.params[1]) >> 2) | (DWORD(r.params[2]) >> 1) | DWORD(r.params[3]));
        HalSendSMCMessage(packet, 0); text.Append("S_OK"); break;
    }
    case 15: {
        DWORD address = ResolveFunction("launch.xex", 1); LaunchData data;
        BYTE value = 0; unsigned offsets[4] = {2,2,6,8};
        if (!address || !SafeCopy(&data, address, sizeof(data)) ||
            !OpenRpc::Range((DWORD)data.temperatures, 9) || !SafeCopy(&value, (DWORD)data.temperatures + offsets[unsigned(r.params[0])], 1)) JobError(job, "Temperature service is unavailable");
        else text.Format("%X", unsigned(value));
        break;
    }
    case 16: text.Format("%X", XamGetCurrentTitleId()); break;
    case 17: {
        const char* names[] = {"Xenon","Zephyr","Falcon","Jasper","Trinity","Corona"};
        DWORD kind = (XboxHardwareInfo->Flags >> 28) & 15;
        text.Append(kind < 6 ? names[kind] : "Unknown"); break;
    }
    case 18: {
        if (!OpenRpc::Range(r.target, 4, 4) || !MemoryRange(r.target, 4, true)) { JobError(job, "Memory writer address is invalid or not writable"); return; }
        StateGuard guard;
        MemorySetter setting;
        setting.address = r.target; setting.value = DWORD(r.params[0]); setting.conditional = r.params[1] != 0;
        setting.expected = DWORD(r.params[2]); setting.titleFilter = r.params[3] != 0; setting.title = DWORD(r.params[4]);
        if (!OpenRpc::RegisterSetting(MemorySettings, MemorySettingCount, setting)) { JobError(job, "The limit of 40 has been reached"); return; }
        text.Append("ok"); break;
    }
    default: JobError(job, "Unsupported command type"); break;
    }
    if (!text.Good()) JobError(job, "Result exceeds internal response limit");
}

HRESULT hrJRPC(char* command, LPSTR response, DWORD responseBytes, void* context) {
    if (!context || !response || !responseBytes || responseBytes > 0x100000) return E_FAIL;
    size_t capacity = size_t(responseBytes), length = 0;
    if (!OpenRpc::BoundedLength(command, OpenRpc::MaxRequest, length)) return ResponseError(response, capacity, "Request is missing or too large");
    if (!strpbrk(command, " \t")) return HrConsoleFeaturesStub(command, response, responseBytes, context);
    if (!InterlockedCompareExchange(&Ready, 0, 0) || InterlockedCompareExchange(&Stopping, 0, 0)) return ResponseError(response, capacity, "Plugin is not accepting requests");
    OpenRpc::Fields fields;
    if (!OpenRpc::ParseFields(OpenRpc::Span(command, length), fields)) return ResponseError(response, capacity, "Malformed request fields");
    if (fields.Find("buf_addr")) {
        OpenRpc::U32 token = 0;
        if (!OpenRpc::FieldNumber(fields, "buf_addr", token, true)) return ResponseError(response, capacity, "Invalid request token");
        bool completed = false;
        RpcJob* job = Jobs.Poll(token, size_t(context), completed);
        JobReference hold(job);
        if (!job) return ResponseError(response, capacity, "Unknown or expired request token");
        if (completed) return CopyResponse(response, capacity, job->result, job->failed);
        OpenRpc::Text out(response, capacity); out.Format("buf_addr=%08X", token);
        return out.Good() ? S_FALSE : E_FAIL;
    }
    JobReference hold(CreateJob());
    if (!hold.value) return ResponseError(response, capacity, "RPC request limit reached or allocation failed");
    RpcJob& job = *hold.value; const char* error = 0;
    if (!OpenRpc::Parse(fields, job.request, error)) return ResponseError(response, capacity, error ? error : "Malformed payload");
    OpenRpc::Request& r = job.request;
    bool nativeCall = r.type <= 8 || (r.type == 9 && r.returnCount != 0);
    if (!nativeCall) {
        RunCustom(job); return CopyResponse(response, capacity, job.result, job.failed);
    }
    if (r.module[0]) {
        r.target = ResolveFunction(r.module, r.ordinal);
        if (!r.target) return ResponseError(response, capacity, "Could not resolve function address");
    }
    if (!ExecutableTarget(r.target)) return ResponseError(response, capacity, "Call address is invalid or not executable");
    if (!Jobs.Publish(&job, size_t(context))) return ResponseError(response, capacity, "Pending request table is full");
    AddReference(&job);
    HANDLE thread = 0; DWORD threadId = 0;
    if (r.system) {
        DWORD status = ExCreateThread(&thread, 0, &threadId, (PVOID)XapiThreadStartup, JRPCCaller, &job, 2);
        if (status != 0) { if (thread) CloseHandle(thread); thread = 0; }
    } else thread = CreateThread(0, 0, JRPCCaller, &job, 0, &threadId);
    if (!thread) {
        ReleaseJob(&job);
        Jobs.Cancel(job.token);
        return ResponseError(response, capacity, "RPC thread creation failed");
    }
    SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL); CloseHandle(thread);
    OpenRpc::Text out(response, capacity); out.Format("buf_addr=%08X", job.token);
    return out.Good() ? S_FALSE : E_FAIL;
}
HRESULT HrGetFile(char* command, LPSTR response, DWORD bytes, void* context) {
    size_t length = 0;
    if (!OpenRpc::BoundedLength(command, OpenRpc::MaxRequest, length)) return E_FAIL;
    if (InterlockedCompareExchange(&KVProtection, 0, 0)) {
        const char* name = "kv.bin";
        for (size_t i = 0; i + 6 <= length; ++i) {
            bool equal = true;
            for (unsigned k = 0; k < 6; ++k) if (OpenRpc::Lower(command[i+k]) != name[k]) equal = false;
            if (equal) return 0x82DA000E;
        }
    }
    return HrGetFileStub(command, response, bytes, context);
}
bool ReadLayoutMarker(void*, OpenRpc::U32 address, OpenRpc::U32& value) { return SafeCopy(&value,address,4); }
struct HookInstaller {
    PreparedHook hooks[2];
    bool Install(unsigned i) { return InstallHook(hooks[i]); }
    void Restore(unsigned i) { RestoreHook(hooks[i]); }
};
int GetXbdmDllType() {
    DWORD base = 0, size = 0;
    if (!ModuleBounds("xbdm.xex", base, size) || !ResolveFunction("xbdm.xex", 1)) return 0;
    OpenRpc::Layout layout; HookInstaller installer;
    unsigned kind = OpenRpc::SelectLayout(base,size,ReadLayoutMarker,0,layout);
    if (!kind || !PrepareHook(installer.hooks[0], layout.features, (DWORD*)HrConsoleFeaturesStub, (DWORD)hrJRPC, base, size)) return 0;
    if (layout.file && !PrepareHook(installer.hooks[1], layout.file, (DWORD*)HrGetFileStub, (DWORD)HrGetFile, base, size)) return 0;
    return OpenRpc::InstallPair(installer,layout.file != 0) ? kind : 0;
}
struct AtomicMemory {
    void CompareExchange(DWORD address, DWORD value, DWORD expected) { InterlockedCompareExchange((volatile LONG*)address,LONG(value),LONG(expected)); }
    void Exchange(DWORD address, DWORD value) { InterlockedExchange((volatile LONG*)address,LONG(value)); }
};
void ApplyMemorySetting(const MemorySetter& setting) {
    if (!OpenRpc::Range(setting.address, 4, 4) || !MemoryRange(setting.address, 4, true)) return;
    __try {
        AtomicMemory memory; OpenRpc::WriteSetting(setting,memory);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
#endif
