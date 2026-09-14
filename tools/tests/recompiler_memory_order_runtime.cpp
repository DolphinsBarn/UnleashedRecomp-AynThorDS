#define PPC_CONFIG_H_INCLUDED
#include <ppc_context.h>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

PPC_FUNC_IMPL(test_lwarx_zero);
PPC_FUNC_IMPL(test_lwarx);
PPC_FUNC_IMPL(test_ldarx);
PPC_FUNC_IMPL(test_stwcx);
PPC_FUNC_IMPL(test_stdcx);

int main()
{
    // Use actual, aligned integer objects, viewed as the guest byte arena.
    alignas(32) uint64_t memory64[8]{};
    auto* base64 = reinterpret_cast<uint8_t*>(memory64);
    alignas(32) uint32_t memory32[16]{};
    auto* base32 = reinterpret_cast<uint8_t*>(memory32);
    PPCContext ctx{};
    ctx.r0.u32 = 0xDEADBEEF;
    ctx.r4.u32 = 8;
    ctx.r5.u32 = 16;
    ctx.xer.so = 1;

    memory32[2] = __builtin_bswap32(0x89ABCDEFu);
    test_lwarx_zero(ctx, base32);
    assert(ctx.r3.u64 == 0x89ABCDEFu); // rA=0 ignores the value of r0; zero extends.

    memory32[6] = __builtin_bswap32(0xFEDCBA98u);
    test_lwarx(ctx, base32);
    assert(ctx.r3.u64 == 0xFEDCBA98u);
    ctx.r6.u32 = 0x01234567;
    test_stwcx(ctx, base32);
    assert(ctx.cr0.eq == 1 && ctx.cr0.lt == 0 && ctx.cr0.gt == 0 && ctx.cr0.so == 1);
    assert(__builtin_bswap32(memory32[6]) == 0x01234567u);

    // An intervening writer must make the store-conditional fail and leave the
    // new value intact. Verify success/failure for both reservation widths.
    test_lwarx(ctx, base32);
    memory32[6] = __builtin_bswap32(99u);
    test_stwcx(ctx, base32);
    assert(ctx.cr0.eq == 0 && __builtin_bswap32(memory32[6]) == 99u);

    memory64[3] = __builtin_bswap64(0xFEDCBA9876543210ull);
    test_ldarx(ctx, base64);
    assert(ctx.r3.u64 == 0xFEDCBA9876543210ull);
    ctx.r6.u64 = 0x0123456789ABCDEFull;
    test_stdcx(ctx, base64);
    assert(ctx.cr0.eq == 1 && __builtin_bswap64(memory64[3]) == ctx.r6.u64);
    test_ldarx(ctx, base64);
    memory64[3] = __builtin_bswap64(99ull);
    test_stdcx(ctx, base64);
    assert(ctx.cr0.eq == 0 && __builtin_bswap64(memory64[3]) == 99ull);

    // Multiple native threads execute the recompiler's actual 32-bit and
    // 64-bit load-reserve/store-conditional output, retrying on CAS failure.
    constexpr unsigned workers = 4;
    constexpr unsigned iterations = 25000;
    memory32[6] = 0;
    memory64[3] = 0;
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < workers; ++i)
    {
        threads.emplace_back([&] {
            PPCContext worker{};
            worker.r4.u32 = 8;
            worker.r5.u32 = 16;
            for (unsigned j = 0; j < iterations; ++j)
            {
                do {
                    test_lwarx(worker, base32);
                    worker.r6.u32 = worker.r3.u32 + 1;
                    test_stwcx(worker, base32);
                } while (!worker.cr0.eq);
                do {
                    test_ldarx(worker, base64);
                    worker.r6.u64 = worker.r3.u64 + 1;
                    test_stdcx(worker, base64);
                } while (!worker.cr0.eq);
            }
        });
    }
    for (auto& thread : threads)
        thread.join();
    assert(__builtin_bswap32(memory32[6]) == workers * iterations);
    assert(__builtin_bswap64(memory64[3]) == workers * iterations);
    std::cout << "PASS: endian conversion, rA=0, CAS success/failure, 200000 concurrent increments\n";
}
