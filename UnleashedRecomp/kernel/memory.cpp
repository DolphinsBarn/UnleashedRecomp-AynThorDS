#include <stdafx.h>
#include "memory.h"
#include <os/logger.h>

#include <atomic>
#ifdef __ANDROID__
#include <dlfcn.h>
#include <unwind.h>
#endif

Memory::Memory()
{
#ifdef _WIN32
    base = (uint8_t*)VirtualAlloc((void*)0x100000000ull, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        base = (uint8_t*)VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        return;

    DWORD oldProtect;
    VirtualProtect(base, 4096, PAGE_NOACCESS, &oldProtect);
#else
    base = (uint8_t*)mmap((void*)0x100000000ull, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == (uint8_t*)MAP_FAILED)
        base = (uint8_t*)mmap(NULL, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == nullptr)
        return;

#ifdef __ANDROID__
    // Keep the guest null page readable and writable. On some devices the game
    // evaluates a half-constructed animation subtree whose data pointers are 0/-1
    // (issue #27, 100% on Snapdragon 8 Gen 2 handhelds); testers confirmed the
    // reads are benign (zeros) with no visual artifacts, while faulting made the
    // game unplayable there. Indirect calls fetched through such zeros are guarded
    // separately in ppc_detail.h. Desktop builds keep the trap to catch new bugs.
#else
    mprotect(base, 4096, PROT_NONE);
#endif
#endif

    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        if (PPCFuncMappings[i].host != nullptr)
            InsertFunction(PPCFuncMappings[i].guest, PPCFuncMappings[i].host);
    }
}

void* MmGetHostAddress(uint32_t ptr)
{
    return g_memory.Translate(ptr);
}


#ifdef __ANDROID__
namespace
{
struct GuestFailureTrace
{
    uintptr_t addresses[48]{};
    size_t count{};
};

_Unwind_Reason_Code CollectGuestFailureFrame(_Unwind_Context* context, void* argument)
{
    auto& trace = *static_cast<GuestFailureTrace*>(argument);
    if (trace.count == std::size(trace.addresses))
        return _URC_END_OF_STACK;
    const auto address = static_cast<uintptr_t>(_Unwind_GetIP(context));
    if (address != 0)
        trace.addresses[trace.count++] = address;
    return _URC_NO_REASON;
}
}
#endif

void LogGuestFailure(const char* reason, PPCContext& ctx, uint32_t target)
{
#ifdef __ANDROID__
    // This runs BEFORE entering the signal handler. Unwinding and normal logging
    // here avoid adding allocator/loader work to the fatal-signal handler.
    LOGF_ERROR("Guest failure: reason={} target={:08X} r1={:08X} r3={:08X} r4={:08X} r5={:08X} r13={:08X}",
        reason, target, ctx.r1.u32, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r13.u32);
    GuestFailureTrace trace;
    _Unwind_Backtrace(CollectGuestFailureFrame, &trace);
    for (size_t i = 0; i < trace.count; ++i)
    {
        Dl_info info{};
        const auto address = trace.addresses[i];
        if (dladdr(reinterpret_cast<void*>(address), &info) && info.dli_fbase)
        {
            const char* module = info.dli_fname ? strrchr(info.dli_fname, '/') : nullptr;
            module = module ? module + 1 : (info.dli_fname ? info.dli_fname : "unknown");
            LOGF_ERROR("Guest failure frame #{}: {}+0x{:X}", i, module,
                address - reinterpret_cast<uintptr_t>(info.dli_fbase));
        }
        else
            LOGF_ERROR("Guest failure frame #{}: pc=0x{:X}", i, address);
    }
    // Only inspect a bounded address inside the fully mapped guest heap range.
    // It is a diagnostic snapshot, not evidence that the object is still alive.
    if (ctx.r3.u32 >= 0x20000 && uint64_t(ctx.r3.u32) + 32 <= 0x7FEA0000)
    {
        for (uint32_t i = 0; i < 8; ++i)
        {
            uint32_t word;
            memcpy(&word, g_memory.base + ctx.r3.u32 + i * 4, sizeof(word));
            LOGF_ERROR("Guest object word +0x{:02X}: {:08X}", i * 4, ByteSwap(word));
        }
    }
#endif
}

// Called from the hardened PPC_CALL_INDIRECT_FUNC (UnleashedRecompLib/ppc/ppc_detail.h)
// when an indirect call's target is outside the recompiled code range or resolves to no
// host function - a wild jump the process could never survive. Skipping the call keeps
// the half-constructed-state races of issue #27 non-fatal; the log keeps them visible.
extern "C" void PPCIndirectCallMissing(PPCContext& ctx, uint8_t* base, uint32_t target)
{
    static std::atomic<uint32_t> s_reportCount{ 0 };
    const auto report = s_reportCount.fetch_add(1, std::memory_order_relaxed);
    if (report < 2)
        LogGuestFailure("unmapped indirect call", ctx, target);
    if (report < 16)
    {
        LOGF_ERROR("Indirect call to unmapped guest address {:08X} skipped (r3={:08X}).",
            target, ctx.r3.u32);
    }
}
