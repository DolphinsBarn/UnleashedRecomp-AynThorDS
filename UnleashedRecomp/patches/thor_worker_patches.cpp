#include <kernel/memory.h>
#include <os/logger.h>

#ifdef __ANDROID__
// The failing stack reaches worker entry 0x82F56618. Its pool initialization
// accepts a worker count in r5, maintains the active count at pool+328, and
// uses that active count for its waits. Keep those original bookkeeping paths.
// This is a concurrency experiment, not a repair of the invalid object itself.
PPC_FUNC_IMPL(__imp__sub_82F56B68);
PPC_FUNC(sub_82F56B68)
{
    const uint32_t pool = ctx.r3.u32;
    const int32_t requested = ctx.r5.s32;
    const uint32_t active = PPC_LOAD_U32(pool + 328);
    if (requested > 0)
        ctx.r5.u64 = active == 0 ? 1 : 0;
    LOGF("Thor worker limit: pool={:08X}, requested={}, active={}, adding={}",
        pool, requested, active, ctx.r5.s32);
    __imp__sub_82F56B68(ctx, base);
    LOGF("Thor worker limit: pool={:08X}, initialized count={}",
        pool, PPC_LOAD_U32(pool + 328));
}

// A later resize can otherwise grow the pool again. The original routine
// returns 1 when its own capacity is reached; its caller exits the growth loop.
PPC_FUNC_IMPL(__imp__sub_82F56C38);
PPC_FUNC(sub_82F56C38)
{
    if (PPC_LOAD_U32(ctx.r3.u32 + 328) >= 1)
    {
        ctx.r3.u64 = 1;
        return;
    }
    __imp__sub_82F56C38(ctx, base);
}
#endif
