# AYN Thor worker-limit test: 0.5.3-thor.3

Status: `thor.1` and the diagnostic `thor.2` still crash on the owner's
Thor. `thor.3` limits the implicated game worker pool to one worker to test
a concurrency hypothesis. It is not a confirmed root-cause fix and may reduce
performance. Other worker pools and graphics settings are unchanged.

## Second follow-up: captured caller stacks

The `thor.2` log enters `ActD_MykonosAct1` at 28.876 seconds. At 40.765 seconds,
a worker from entry `0x82F56618` attempts a null-target indirect call with
`r3=0`. The matching saved native symbols resolve the earliest call chain to:

`82F7E988 <- 82F7DCD8 <- 82F77188 <- 82F05180 <- 82EF1360 <- 82EF1670 <- 82F56618`.

`82F7E988` dereferences a pair of input records to get object pointers, then
calls a virtual method through an object that is null in the supplied log.
The next captured failure passes through `82F8C308` and `82F7DAE0`.
A later `KeBugCheck` runs on the main thread at 42.637 seconds, after the
invalid calls. The earliest failure is the useful lead; suppressing the fatal
stop would leave the earlier invalid state in place.

The worker setup routine `82F56B68` takes its requested worker count in `r5`,
stores the active count at pool offset 328, and starts `82F56618` workers.
The test adapter caps positive requests to one worker for a new pool, leaving
the original setup and synchronization bookkeeping intact. A second adapter
at `82F56C38` prevents later growth by using that routine's existing
capacity-exhausted return value (1). The resize caller `82F56E68` checks this
return value and exits its growth loop. Zero and negative initialization
requests retain their original behavior.

No object/vtable call is patched out by this change. The earlier diagnostics
remain. Log lines `Thor worker limit` report the requested and initialized
counts so hardware testing can verify that the override actually ran.
This does not establish a race or identify who damaged the object; fewer
workers only tests one plausible contributor.

The native adapter test compiles the actual patch file with instrumented
stand-ins for the original guest calls. It covers count forwarding, an
already populated pool, original return-value preservation, and later growth.
It does not simulate game scheduling or prove hardware stability. Run
`python3 tools/tests/test_thor_worker_limit.py`.

Install over the previous test app (same signing key and package, version
code 19). Keep the same settings and try the same route. A successful test
must complete the level without broken collisions or animations. If it still
fails, export the log with the new worker-count lines and caller stacks.

## Follow-up crash and diagnostic update

The new log identifies the first test build and the normal bundled Adreno 740
Turnip driver (`vulkan.unleashed26_1_wfm_a732.so`, Auto render mode). It enters
`ActD_MykonosAct1` at 38.819 seconds. At 62.332 seconds, worker tid 18951
(entry `0x82F56618`) attempts indirect calls to `0x00000000` and `0x00600040`
with `r3=0x0144D610`. A second worker, tid 18949 with the same entry point,
then receives SIGTRAP at 62.333 seconds.

The saved matching native symbols resolve `libmain.so+0x16bae7c` to
`KeBugCheck` in `kernel/imports.cpp` and the logged return address
`libmain.so+0x3fe7300` to generated function `__imp__sub_831B54D0`.
This is an explicit guest fatal stop following invalid calls, not evidence
of a Vulkan device-loss failure. The first build's memory-order change did
not eliminate the defect. The caller chain and object lifetime are not yet
known, so no additional race or driver workaround is applied here.

The diagnostic update captures up to 48 native frames, relative to each loaded
module, for the first two invalid indirect calls and for `KeBugCheck` and
`KeBugCheckEx`. It records guest argument registers and eight words from an
in-range guest heap object. Capture happens on the normal execution stack,
before the signal handler. It preserves the existing call-skip and fatal-stop
behavior. Frames from different threads retain their log thread IDs.

Install this APK over the first test app. The package remains
`com.sega.sonicunr.thor`; version code increases from 17 to 18 and the signing
key is unchanged, so the test app's game files, saves, and settings remain.
Keep the same driver/settings and repeat the same route. Export the new
`log.txt` after the crash. Keep the matching diagnostic native symbols to
resolve its offsets; the first build's offsets cannot be reused.

## Source baseline

This fork starts at `release/0.5.3`, commit
`5fc9642581ab1b8a0046dd25db1b63c074bd7dc2`. That branch contains the
`ExperimentalA8xx` option and 0.5.3 build identifiers in the supplied log.
Upstream `main` and the `v0.5.3` tag instead point at `57bbe855`, whose source
still identifies itself as 0.5.2. Start from the release branch when applying
the patch; do not silently rebuild from that tag.

## Findings and changes

The supplied log identifies AYN Thor / QCS8550 / Adreno 740 / Android 13,
with the experimental Adreno 8xx driver and `TU_DEBUG=sysmem,flushall`.
It enters `ActD_MykonosAct1` at 114.844 seconds and records the final frame
heartbeat at 142.083 seconds. The watchdog reports the stall at 147.834
seconds. There is no fatal signal/backtrace or explicit GPU-device-loss
message in this log. Most worker threads share the name `SDLThread`, so
their kernel wait states do not identify the blocking function or subsystem.

The matching source drops `sync`, `lwsync` and `eieio`, and does not handle
`isync`. Volatile guest accesses do not supply inter-thread hardware memory
ordering on ARM64. Dropping these instructions is a concrete defect; whether
it caused this particular freeze is still a hypothesis.

The patch:

- Emits conservative full memory fences for those four instructions. This
  preserves their data-ordering role; it does not implement guest code-cache
  invalidation or a complete PowerPC memory/reservation model.
- Uses atomic loads for `lwarx` and `ldarx` so reservation loads agree with the
  existing atomic compare-and-swap implementation of `stwcx.` and `stdcx.`.
  The existing value-based CAS approximation still has reservation/ABA limits.
- Names Android guest threads by entry point and names the render, pipeline,
  main and watchdog threads, making the next hang log more useful.
- Identifies the test build separately in Gradle and native logs.
- Installs as `com.sega.sonicunr.thor`, labeled **Unleashed Thor Test**, alongside
  upstream. Existing game files and saves stay in the original app. Use the
  test app's importer to provide its own game files; it has separate saves.
- Disables upstream update prompts for the test fork. Install subsequent test
  APKs manually with the same signing key.

The existing upstream null-page and unmapped-call guards remain in place.
They are not evidence that the underlying use-after-free has been fixed.
Full fences can affect performance; no FPS improvement is claimed.

## Verification

Run `python3 tools/tests/test_memory_order.py` with Clang and Python 3.
It compiles and executes the real instruction emitter with synthetic PPC
instructions; no Sonic files are needed.

Verified locally using the Clang 21.1 frontend shipped in Zig 0.16:

- Decoding and code generation for all four barriers and 32/64-bit reservation
  loads and conditional stores.
- Big-endian conversion, zero extension, `rA=0` handling, CAS success/failure,
  and condition-register results using the real `PPCContext`.
- 200,000 increments across four threads using the generated 32/64-bit code.
- ARM64 assembly contains `dmb ish` for each barrier; LLVM IR retains atomic
  reservation loads. These are compiler checks, not execution on ARM hardware.

`CC`, `CXX` and `ARM_CXX` can override compiler commands. Outputs go under
`out/tests/memory-order`. The `Thor source checks` workflow runs these checks
without access to game files or secrets.

The full local build used NDK `29.0.14206865` and the original bundled DXC
`1.8 (4662-416fab6b)`. All 261 generated PPC translation units compiled. The
generated game code contains 32 full fences and 10,679 atomic reservation
loads. Java compilation, native linking, and Gradle APK packaging succeeded.

Test APK: `Unleashed-Thor-Test-0.5.3-thor.1.apk` (63,440,122 bytes).
SHA-256: `565f767f4314e8e1ddf62c4636c6c40be6cbd535d4067bce26ab5485bca8c025`.
This debug-signed APK is supplied directly to the device owner; it is not
a public GitHub release. Keep its signing key for subsequent test updates.

## Producing an APK

The supplied game inputs have passed executable patching, PPC generation,
archive decompression, and shader compilation locally. The raw inputs remain
private and are absent from the public source:

| Input | Location in an installed/extracted game |
| --- | --- |
| `default.xex` | `game/default.xex` |
| `default.xexp` | `update/default.xexp` from the title update |
| `shader.ar` | `game/shader.ar` |

For a local build, put these files in `UnleashedRecompLib/private/`. For
GitHub Actions, follow [CI.md](CI.md) to configure a private input repository.
Never include these inputs, derived game code, or signing keys in the public
fork. The build requires the Android SDK/NDK and the host-generation tools
described in that document.

Rebuild **XenonRecomp** before generating the game code. Reusing an old
host binary or old `ppc_recomp.*.cpp` files would omit the memory-order fix.
The existing CMake dependency on the host binary regenerates these files
when that binary changes.

Use one stable signing key for successive test builds. An APK signed with a
new key cannot update an already installed test app. The existing CI debug
fallback generates a new key on a fresh runner; configure the signing secrets
in CI.md before using it for repeated device tests.

## First Thor test

1. Import the game into **Unleashed Thor Test**; keep mods disabled initially.
2. Explicitly choose **Bundled** and Render Mode **Auto**. Auto driver selection
   can retain an imported driver; the explicit choice makes the baseline clear.
   The logged **Adreno 8xx Experimental** selection is not this Adreno 740
   baseline. Changing drivers alone is not a confirmed crash fix.
3. Keep 50% resolution, anti-aliasing off and motion blur off for the first run.
4. Try the same first-level route, including rings, boost pads and the loop.
   Also watch for missing collision or broken animation, not just crashes.
5. If it freezes, leave it foregrounded for about 10 seconds so the watchdog
   records the stall, then export `log.txt` and `log_prev.txt` from the test
   app's transfer folder. A passing run must reach the end of the level;
   surviving the first rings alone is insufficient.

A separate run with the same settings on upstream is useful for comparison.
Do not mix changes to the driver, mods and frame limit between comparison runs.
