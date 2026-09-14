#!/usr/bin/env python3
"""Exercise the real worker adapters with instrumented original-call stand-ins.
This checks adapter semantics, not game scheduling or device stability.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as temporary:
    directory = Path(temporary)
    (directory / 'kernel').mkdir()
    (directory / 'os').mkdir()
    (directory / 'kernel/memory.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cstring>
union Register { uint64_t u64; int64_t s64; uint32_t u32; int32_t s32; };
struct PPCContext { Register r3{}, r4{}, r5{}; };
inline uint32_t read32(uint8_t* base, uint32_t offset) {
    uint32_t result; memcpy(&result, base + offset, 4); return __builtin_bswap32(result);
}
#define PPC_LOAD_U32(x) read32(base, (x))
#define PPC_FUNC_IMPL(x) void x(PPCContext& ctx, uint8_t* base)
#define PPC_FUNC(x) PPC_FUNC_IMPL(x)
''')
    (directory / 'os/logger.h').write_text('#define LOGF(...) ((void)0)\n')
    test = directory / 'test.cpp'
    test.write_text(r'''
#include <kernel/memory.h>
#include <cassert>
#include <cstdio>
PPC_FUNC(sub_82F56B68);
PPC_FUNC(sub_82F56C38);
int originalInitCalls = 0, originalGrowCalls = 0;
int32_t forwardedCount = 0;
void write32(uint8_t* base, uint32_t offset, uint32_t value) {
    value = __builtin_bswap32(value); memcpy(base + offset, &value, 4);
}
PPC_FUNC(__imp__sub_82F56B68) {
    ++originalInitCalls;
    assert(ctx.r3.u32 == 64 && ctx.r4.u32 == 0x12345678);
    forwardedCount = ctx.r5.s32;
    if (forwardedCount > 0)
        write32(base, 64 + 328, read32(base, 64 + 328) + forwardedCount);
    ctx.r3.u64 = 0xCAFE; // The adapter must preserve the original result.
}
PPC_FUNC(__imp__sub_82F56C38) {
    ++originalGrowCalls;
    write32(base, ctx.r3.u32 + 328, read32(base, ctx.r3.u32 + 328) + 1);
    ctx.r3.u64 = 0;
}
int main() {
    uint8_t memory[512]{};
    for (int32_t requested : { -1, 0, 1, 5, 6, 100 }) {
        for (uint32_t active : { 0u, 1u, 2u }) {
            PPCContext ctx; ctx.r3.u64 = 64; ctx.r4.u64 = 0x12345678;
            ctx.r5.s64 = requested; write32(memory, 64 + 328, active);
            int before = originalInitCalls;
            sub_82F56B68(ctx, memory);
            const int32_t expected = requested > 0 ? (active == 0 ? 1 : 0) : requested;
            assert(forwardedCount == expected && originalInitCalls == before + 1);
            assert(read32(memory, 64 + 328) == active + (expected > 0 ? expected : 0));
            assert(ctx.r3.u64 == 0xCAFE);
        }
    }
    for (uint32_t active : { 0u, 1u, 5u }) {
        PPCContext ctx; ctx.r3.u64 = 64; write32(memory, 64 + 328, active);
        int before = originalGrowCalls;
        sub_82F56C38(ctx, memory);
        assert(ctx.r3.u64 == (active == 0 ? 0u : 1u));
        assert(originalGrowCalls == before + (active == 0));
        assert(read32(memory, 64 + 328) == (active == 0 ? 1u : active));
    }
    puts("PASS: init forwarding, existing pools, capacity return, and resize blocking");
}
'''.replace('#include <cassert>', '#include <cassert>\n#include <initializer_list>'))
    executable = directory / 'test'
    subprocess.run([*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++17',
                    '-D__ANDROID__', '-I', str(directory),
                    str(root / 'UnleashedRecomp/patches/thor_worker_patches.cpp'),
                    str(test), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
