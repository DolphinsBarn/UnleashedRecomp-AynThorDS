#!/usr/bin/env python3
"""Exercise the real PPC emitter without game files, then inspect ARM64 code.

Requires Python 3 and Clang (CC/CXX/ARM_CXX may override compiler commands).
Run from any directory: python3 tools/tests/test_memory_order.py
"""
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "tools/tests"
XENON = ROOT / "tools/XenonRecomp"
OUT = ROOT / "out/tests/memory-order"
OUT.mkdir(parents=True, exist_ok=True)
CC = shlex.split(os.environ.get("CC", "clang"))
CXX = shlex.split(os.environ.get("CXX", "clang++"))
ARM_CXX = shlex.split(os.environ.get("ARM_CXX", "")) or CXX + ["--target=aarch64-linux-gnu"]


def run(command):
    subprocess.run([str(arg) for arg in command], cwd=ROOT, check=True, timeout=300)


includes = [
    XENON / name for name in (
        "XenonRecomp", "XenonUtils", "XenonAnalyse", "thirdparty/disasm",
        "thirdparty/simde", "thirdparty/fmt/include", "thirdparty/tomlplusplus/include",
        "thirdparty/xxHash",
    )
]
include_flags = [flag for path in includes for flag in ("-I", path)]
objects = []
for name in ("ppc-dis", "disasm"):
    obj = OUT / (name + ".o")
    run(CC + ["-O1", "-c", XENON / f"thirdparty/disasm/{name}.c", "-o", obj])
    objects.append(obj)

# Discard unused whole-image/XEX routines: this test only invokes the real
# instruction-emission entry point, not a substitute implementation.
emitter = OUT / "emit"
run(CXX + ["-std=c++20", "-O1", "-ffunction-sections", "-fdata-sections",
           "-DFMT_HEADER_ONLY", "-DXXH_INLINE_ALL", "-Wno-null-arithmetic"] + include_flags + [
    TESTS / "recompiler_memory_order_emit.cpp", XENON / "XenonRecomp/recompiler.cpp",
    XENON / "XenonUtils/disasm.cpp", *objects, "-Wl,--gc-sections", "-o", emitter,
])
generated = OUT / "generated.cpp"
run([emitter, generated])

# Runtime checks use the real PPCContext and its big-endian register semantics.
runtime = OUT / "runtime"
run(CXX + ["-std=c++20", "-O2", "-pthread", "-UNDEBUG"] + include_flags + [
    generated, TESTS / "recompiler_memory_order_runtime.cpp", "-o", runtime,
])
run([runtime])

# The emitted instruction bodies need only this register subset. Use it for
# freestanding target checks so an Android SDK/sysroot is not required. No
# instruction body is edited; only the include is replaced for this inspection.
minimal_context = """
typedef __UINT8_TYPE__ uint8_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __UINT64_TYPE__ uint64_t;
union PPCRegister { uint32_t u32; uint64_t u64; __INT32_TYPE__ s32; __INT64_TYPE__ s64; };
struct PPCContext {
    PPCRegister r0, r3, r4, r5, r6, reserved;
    struct { uint8_t lt, gt, eq, so; } cr0;
    struct { uint8_t so; } xer;
};
#define PPC_FUNC_IMPL(x) extern "C" void x(PPCContext& ctx, uint8_t* base)
"""
arm_source = OUT / "arm.cpp"
arm_source.write_text(generated.read_text().replace("#include <ppc_context.h>", minimal_context))
asm = OUT / "arm.s"
ir = OUT / "arm.ll"
run(ARM_CXX + ["-std=c++20", "-O2", "-ffreestanding", "-S", arm_source, "-o", asm])
run(ARM_CXX + ["-std=c++20", "-O2", "-ffreestanding", "-S", "-emit-llvm", arm_source, "-o", ir])
assembly = asm.read_text()
for name in ("sync", "lwsync", "eieio", "isync"):
    body = re.search(rf"^test_{name}:.*?(?=^\s*\.size)", assembly, re.M | re.S)
    assert body and re.search(r"\bdmb\s+ish\b", body[0]), f"{name}: missing ARM64 barrier"
llvm = ir.read_text()
for name, width in (("lwarx_zero", 32), ("lwarx", 32), ("ldarx", 64)):
    body = re.search(rf"define[^\n]*@test_{name}\(.*?^}}", llvm, re.M | re.S)
    assert body and f"load atomic i{width}" in body[0], f"{name}: reservation load is not atomic"
print("PASS: all 4 guest barriers emit DMB ISH; all reservation loads remain atomic in ARM64 IR")
