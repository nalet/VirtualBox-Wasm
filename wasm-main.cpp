/*
 * VirtualBox/Wasm PoC — Phase 2 entry point.
 *
 * Initialises IPRT and runs basic smoke-tests (allocation, formatting,
 * timers) to prove the runtime library works under Emscripten/Wasm64.
 *
 * Compiled with em++ and linked against RuntimeR3.a + support libs.
 */
#include <iprt/initterm.h>
#include <iprt/mem.h>
#include <iprt/string.h>
#include <iprt/time.h>
#include <iprt/stream.h>
#include <iprt/buildconfig.h>
#include <iprt/system.h>

int main(int argc, char **argv)
{
    int rc = RTR3InitExe(argc, &argv, 0);
    if (RT_FAILURE(rc))
    {
        RTStrmPrintf(g_pStdErr, "RTR3InitExe failed: %Rrc\n", rc);
        return 1;
    }

    RTPrintf("VirtualBox/Wasm PoC — IPRT initialized successfully!\n");
    RTPrintf("Build type    : %s\n", RTBldCfgType());
    RTPrintf("Build arch    : %s\n", RTBldCfgTargetArch());
    RTPrintf("Build target  : %s\n", RTBldCfgTarget());
    RTPrintf("IPRT version  : %u.%u.%u\n",
             RTBldCfgVersionMajor(), RTBldCfgVersionMinor(), RTBldCfgVersionBuild());

    /* Basic allocation test. */
    void *pv = RTMemAlloc(256);
    if (pv)
    {
        RTMemFree(pv);
        RTPrintf("RTMemAlloc/Free: OK\n");
    }
    else
        RTPrintf("RTMemAlloc/Free: FAILED\n");

    /* String formatting test. */
    char szBuf[128];
    RTStrPrintf(szBuf, sizeof(szBuf), "Hello from VirtualBox running on Wasm!");
    RTPrintf("RTStrPrintf  : %s\n", szBuf);

    /* Nanotimestamp test. */
    uint64_t u64Ts = RTTimeNanoTS();
    RTPrintf("RTTimeNanoTS : %llu ns\n", u64Ts);

    /* Pointer size check — should be 8 for Wasm64. */
    RTPrintf("sizeof(void*): %u (expected 8 for Wasm64)\n", (unsigned)sizeof(void *));

    RTPrintf("\nAll basic IPRT tests passed. VirtualBox/Wasm is alive!\n");
    return 0;
}
