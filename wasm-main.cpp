/*
 * VirtualBox/Wasm — VM harness for browser execution.
 *
 * Creates a minimal x86 VM with:
 *   - PC architecture (pcarch, pcbios, PCI, i8254, i8259, RTC)
 *   - VGA display (4MB VRAM)
 *   - PS/2 keyboard
 *   - IDE controller with disk image
 *   - VMMDev
 *
 * Uses IEM (Instruction Execution Manager) for CPU emulation — no hardware
 * virtualization (VT-x/AMD-V) needed, which is perfect for Wasm.
 *
 * Compiled with em++ and linked against VBoxVMM, VBoxDD, RuntimeR3, etc.
 */
#include <iprt/initterm.h>
#include <iprt/mem.h>
#include <iprt/string.h>
#include <iprt/stream.h>
#include <iprt/uuid.h>
#include <iprt/buildconfig.h>
#include <iprt/path.h>
#include <iprt/file.h>
#include <iprt/thread.h>
#include <iprt/semaphore.h>
#include <iprt/critsect.h>
/* iprt/tls.h doesn't exist — RTTlsAllocEx is in iprt/thread.h */

#include <VBox/vmm/vmapi.h>
#include <VBox/vmm/cfgm.h>
#include <VBox/vmm/pdmapi.h>
#include <VBox/vmm/pdmdrv.h>
#include <VBox/vmm/mm.h>
#include <VBox/err.h>
#include <VBox/version.h>
#include <VBox/log.h>

#include <pthread.h>

#ifdef __EMSCRIPTEN__
# include <emscripten.h>
# include <emscripten/threading.h>
#endif

/* Defined in wasm-stubs.cpp — cross-thread debug buffer */
extern "C" const char *wasmStubGetLog(void);


/*************************************************************************
 * Globals
 *************************************************************************/
static PUVM g_pUVM = NULL;
static PVM  g_pVM  = NULL;

/** Path to the CD-ROM ISO (written to Emscripten virtual FS by JS). */
static const char *g_pszCdImage = "/cdrom.iso";


/*************************************************************************
 * CFGM Configuration Constructor
 *************************************************************************/

/**
 * Builds the VM configuration tree.
 *
 * This is called by VMR3Create on the EMT thread.  We set up a minimal
 * PC-compatible machine: BIOS, PCI bus, interrupt controllers, timers,
 * VGA, PS/2 keyboard, IDE controller, and VMMDev.
 */
static DECLCALLBACK(int) vboxWasmCfgmConstructor(PUVM pUVM, PVM pVM, PCVMMR3VTABLE pVMM, void *pvUser)
{
    RT_NOREF(pUVM, pVMM, pvUser);

    int rc;
    PCFGMNODE pRoot = CFGMR3GetRoot(pVM);
    AssertReturn(pRoot, VERR_INTERNAL_ERROR);

#define INSERT_INTEGER(node, name, val) \
    do { rc = CFGMR3InsertInteger(node, name, val); AssertRCReturn(rc, rc); } while (0)
#define INSERT_STRING(node, name, val) \
    do { rc = CFGMR3InsertString(node, name, val); AssertRCReturn(rc, rc); } while (0)
#define INSERT_NODE(parent, name, ppChild) \
    do { rc = CFGMR3InsertNode(parent, name, ppChild); AssertRCReturn(rc, rc); } while (0)

    /*
     * VM properties.
     */
    INSERT_STRING(pRoot, "Name", "VBoxWasm");
    INSERT_INTEGER(pRoot, "RamSize",      (uint64_t)256 * _1M);   /* 256 MB RAM */
    INSERT_INTEGER(pRoot, "RamHoleSize",  (uint64_t)512 * _1M);
    INSERT_INTEGER(pRoot, "TimerMillies", 10);
    INSERT_INTEGER(pRoot, "NumCPUs",      1);

    /*
     * HM — force IEM-only execution (no hardware virtualization).
     */
    PCFGMNODE pHm;
    INSERT_NODE(pRoot, "HM", &pHm);
    INSERT_INTEGER(pHm, "FallbackToIEM", 1);
    INSERT_INTEGER(pHm, "FallbackToNEM", 0);

    /*
     * NEM — disable native execution manager (no KVM in Wasm).
     */
    PCFGMNODE pNem;
    INSERT_NODE(pRoot, "NEM", &pNem);
    INSERT_INTEGER(pNem, "Enabled", 0);

    /*
     * PDM — tell it to load builtin device/driver modules.
     */
    PCFGMNODE pPdm, pPdmDevices, pPdmDrivers;
    INSERT_NODE(pRoot, "PDM", &pPdm);
    INSERT_NODE(pPdm, "Devices", &pPdmDevices);
    INSERT_INTEGER(pPdmDevices, "LoadBuiltin", 1);
    INSERT_NODE(pPdm, "Drivers", &pPdmDrivers);
    INSERT_INTEGER(pPdmDrivers, "LoadBuiltin", 1);

    /*
     * Devices.
     */
    PCFGMNODE pDevices;
    INSERT_NODE(pRoot, "Devices", &pDevices);

    PCFGMNODE pDev, pInst, pCfg;

    /* ── PC Architecture ── */
    INSERT_NODE(pDevices, "pcarch", &pDev);
    INSERT_NODE(pDev, "0", &pInst);
    INSERT_INTEGER(pInst, "Trusted", 1);
    INSERT_NODE(pInst, "Config", &pCfg);

    /* ── PC BIOS ── */
    INSERT_NODE(pDevices, "pcbios", &pDev);
    INSERT_NODE(pDev, "0", &pInst);
    INSERT_INTEGER(pInst, "Trusted", 1);
    INSERT_NODE(pInst, "Config", &pCfg);
    INSERT_STRING(pCfg, "BootDevice0", "DVD");
    INSERT_STRING(pCfg, "BootDevice1", "IDE");
    INSERT_STRING(pCfg, "BootDevice2", "NONE");
    INSERT_STRING(pCfg, "BootDevice3", "NONE");
    INSERT_STRING(pCfg, "HardDiskDevice", "piix3ide");
    INSERT_STRING(pCfg, "FloppyDevice", "");
    RTUUID Uuid;
    RTUuidClear(&Uuid);
    rc = CFGMR3InsertBytes(pCfg, "UUID", &Uuid, sizeof(Uuid));
    AssertRCReturn(rc, rc);

    /* ── PCI Bus (PIIX3) ── */
    INSERT_NODE(pDevices, "pci", &pDev);
    INSERT_NODE(pDev, "0", &pInst);
    INSERT_INTEGER(pInst, "Trusted", 1);
    INSERT_NODE(pInst, "Config", &pCfg);

    /* ── PS/2 Keyboard & Mouse ── */
    INSERT_NODE(pDevices, "pckbd", &pDev);
    INSERT_NODE(pDev, "0", &pInst);
    INSERT_INTEGER(pInst, "Trusted", 1);
    INSERT_NODE(pInst, "Config", &pCfg);

    /* ── i8254 Programmable Interval Timer ── */
    INSERT_NODE(pDevices, "i8254", &pDev);
    INSERT_NODE(pDev, "0", &pInst);
    INSERT_NODE(pInst, "Config", &pCfg);

    /* ── i8259 Programmable Interrupt Controller ── */
    INSERT_NODE(pDevices, "i8259", &pDev);
    INSERT_NODE(pDev, "0", &pInst);
    INSERT_INTEGER(pInst, "Trusted", 1);
    INSERT_NODE(pInst, "Config", &pCfg);

    /* ── RTC MC146818 ── */
    INSERT_NODE(pDevices, "mc146818", &pDev);
    INSERT_NODE(pDev, "0", &pInst);
    INSERT_NODE(pInst, "Config", &pCfg);

    /* ── VGA ── */
    INSERT_NODE(pDevices, "vga", &pDev);
    INSERT_NODE(pDev, "0", &pInst);
    INSERT_INTEGER(pInst, "Trusted", 1);
    INSERT_NODE(pInst, "Config", &pCfg);
    INSERT_INTEGER(pCfg, "VRamSize", 4U * _1M);
    INSERT_INTEGER(pCfg, "FadeIn",   0);
    INSERT_INTEGER(pCfg, "FadeOut",  0);
    INSERT_INTEGER(pCfg, "LogoTime", 0);
    INSERT_STRING(pCfg,  "LogoFile", "");

    /* VGA LUN#0 — WasmDisplay driver */
    {
        PCFGMNODE pLunVga, pLunVgaCfg;
        INSERT_NODE(pInst, "LUN#0", &pLunVga);
        INSERT_STRING(pLunVga, "Driver", "WasmDisplay");
        INSERT_NODE(pLunVga, "Config", &pLunVgaCfg);
    }

    /* ── IDE Controller (PIIX3) ── */
    INSERT_NODE(pDevices, "piix3ide", &pDev);
    INSERT_NODE(pDev, "0", &pInst);
    INSERT_INTEGER(pInst, "Trusted", 1);
    INSERT_NODE(pInst, "Config", &pCfg);

    /*
     * Attach a CD-ROM ISO to the IDE secondary master (LUN#2).
     */
    PCFGMNODE pLun2, pLun2Cfg;
    INSERT_NODE(pInst, "LUN#2", &pLun2);
    INSERT_STRING(pLun2, "Driver", "VD");
    INSERT_NODE(pLun2, "Config", &pLun2Cfg);
    INSERT_STRING(pLun2Cfg, "Path",     g_pszCdImage);
    INSERT_STRING(pLun2Cfg, "Format",   "RAW");
    INSERT_STRING(pLun2Cfg, "Type",     "DVD");
    INSERT_INTEGER(pLun2Cfg, "ReadOnly", 1);

    /* ── VMMDev ── */
    INSERT_NODE(pDevices, "VMMDev", &pDev);
    INSERT_NODE(pDev, "0", &pInst);
    INSERT_INTEGER(pInst, "Trusted", 1);
    INSERT_NODE(pInst, "Config", &pCfg);

#undef INSERT_INTEGER
#undef INSERT_STRING
#undef INSERT_NODE

    RTPrintf("CFGM configuration tree built successfully.\n");
    return VINF_SUCCESS;
}


/*************************************************************************
 * Error callback — writes to shared buffer (EMT output lost in Emscripten)
 *************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#define ERR_BUF_SIZE 4096
static char g_szErrBuf[ERR_BUF_SIZE];
static volatile int g_iErrBufPos = 0;

static void errBufAppend(const char *psz)
{
    int len = (int)strlen(psz);
    int pos = __atomic_fetch_add(&g_iErrBufPos, len, __ATOMIC_SEQ_CST);
    if (pos + len < ERR_BUF_SIZE)
        memcpy(&g_szErrBuf[pos], psz, len);
}

static DECLCALLBACK(void) vboxWasmVMAtError(PUVM pUVM, void *pvUser,
                                            int rc, RT_SRC_POS_DECL,
                                            const char *pszFormat, va_list args)
{
    RT_NOREF(pUVM, pvUser);
    char szLine[512];
    snprintf(szLine, sizeof(szLine), "VM Error: rc=%d at %s:%d (%s)\n", rc, pszFile, iLine, pszFunction);
    errBufAppend(szLine);
    RTPrintf("%s", szLine);

    char szMsg[1024];
    RTStrPrintfV(szMsg, sizeof(szMsg), pszFormat, args);
    errBufAppend("  ");
    errBufAppend(szMsg);
    errBufAppend("\n");
    RTPrintf("  %s\n", szMsg);
}


/*************************************************************************
 * Override VBoxDriversRegister — register our WasmDisplay driver
 * alongside the builtin drivers from VBoxDD.
 *
 * Uses --allow-multiple-definition to override the version in VBoxDD.so.
 *************************************************************************/
extern const PDMDRVREG g_DrvWasmDisplay;
extern const PDMDRVREG g_DrvMouseQueue;
extern const PDMDRVREG g_DrvKeyboardQueue;
extern const PDMDRVREG g_DrvVD;
extern const PDMDRVREG g_DrvACPI;
extern const PDMDRVREG g_DrvAcpiCpu;
extern const PDMDRVREG g_DrvChar;

extern "C" DECLEXPORT(int) VBoxDriversRegister(PCPDMDRVREGCB pCallbacks, uint32_t u32Version)
{
    RT_NOREF(u32Version);
    int rc;
#define REGISTER_DRIVER(drv) \
    do { rc = pCallbacks->pfnRegister(pCallbacks, &drv); if (RT_FAILURE(rc)) return rc; } while (0)

    REGISTER_DRIVER(g_DrvMouseQueue);
    REGISTER_DRIVER(g_DrvKeyboardQueue);
    REGISTER_DRIVER(g_DrvVD);
    REGISTER_DRIVER(g_DrvACPI);
    REGISTER_DRIVER(g_DrvAcpiCpu);
    REGISTER_DRIVER(g_DrvChar);
    REGISTER_DRIVER(g_DrvWasmDisplay);

#undef REGISTER_DRIVER
    return VINF_SUCCESS;
}


/*************************************************************************
 * Main
 *************************************************************************/
static void *rawPthreadFunc(void *arg)
{
    *(volatile int *)arg = 42;
    return NULL;
}

static DECLCALLBACK(int) testEMTThread(RTTHREAD hSelf, void *pvUser)
{
    RT_NOREF(hSelf);
    *(volatile int *)pvUser = 99;
    return VINF_SUCCESS;
}

int main(int argc, char **argv)
{
    /*
     * Initialize IPRT.
     */
    int rc = RTR3InitExe(argc, &argv, 0);
    if (RT_FAILURE(rc))
    {
        RTPrintf("RTR3InitExe failed: %Rrc\n", rc);
        return 1;
    }

    RTPrintf("\n");
    RTPrintf("=== VirtualBox/Wasm VM Harness ===\n");
    RTPrintf("IPRT version : %u.%u.%u\n",
             RTBldCfgVersionMajor(), RTBldCfgVersionMinor(), RTBldCfgVersionBuild());
    RTPrintf("sizeof(void*): %u (Wasm%u)\n",
             (unsigned)sizeof(void *), (unsigned)sizeof(void *) * 8);
    RTPrintf("\n");

    /*
     * Check if CD-ROM image exists.
     */
    if (!RTFileExists(g_pszCdImage))
    {
        RTPrintf("No CD-ROM image found at %s\n", g_pszCdImage);
        RTPrintf("Upload an ISO image to start the VM.\n");
        return 0;
    }

    uint64_t cbDisk = 0;
    RTFileQuerySizeByPath(g_pszCdImage, &cbDisk);
    RTPrintf("CD-ROM image: %s (%llu MB)\n", g_pszCdImage, cbDisk / (1024 * 1024));

    /*
     * Create the VM.
     *
     * VMCREATE_F_DRIVERLESS: No kernel driver needed (pure IEM emulation).
     * This is exactly what we want for WebAssembly.
     */
    /* Tests removed — all pass. Go straight to VM creation. */

    RTPrintf("Creating VM...\n");
    rc = VMR3Create(1 /*cCpus*/,
                    NULL /*pVmm2UserMethods*/,
                    VMCREATE_F_DRIVERLESS,
                    vboxWasmVMAtError, NULL /*pvUserVM*/,
                    vboxWasmCfgmConstructor, NULL /*pvUserCFGM*/,
                    &g_pVM, &g_pUVM);
    if (RT_FAILURE(rc))
    {
        RTPrintf("VMR3Create failed: %Rrc (rc=%d)\n", rc, rc);

        /* Read the cross-thread stub debug log */
        const char *pszLog = wasmStubGetLog();
        if (pszLog && *pszLog)
        {
            RTPrintf("=== Stub log (from EMT thread) ===\n");
            RTPrintf("%s", pszLog);
            RTPrintf("=== End stub log ===\n");
        }
        else
            RTPrintf("(No stubs returning VERR_NOT_SUPPORTED were called)\n");

        /* Read the cross-thread error buffer */
        if (g_szErrBuf[0])
        {
            RTPrintf("=== VM Error details (from EMT thread) ===\n");
            RTPrintf("%s", g_szErrBuf);
            RTPrintf("=== End VM Error ===\n");
        }
        else
            RTPrintf("(No VM error callback was triggered)\n");

        return 1;
    }
    RTPrintf("VM created successfully!\n");

    /*
     * Power on the VM.
     *
     * This transitions the VM to VMSTATE_RUNNING and the EMT thread
     * starts executing guest code via IEM.
     */
    RTPrintf("Powering on VM...\n");
    rc = VMR3PowerOn(g_pUVM);
    if (RT_FAILURE(rc))
    {
        RTPrintf("VMR3PowerOn failed: %Rrc\n", rc);
        VMR3Destroy(g_pUVM);
        return 1;
    }
    RTPrintf("VM is running!\n");
    RTPrintf("IEM is executing guest x86 code...\n");

#ifdef __EMSCRIPTEN__
    /*
     * In the browser, we must not block the main thread.
     * The EMT runs in a pthread, so we just return from main() and let
     * Emscripten keep the runtime alive.
     *
     * emscripten_exit_with_live_runtime() prevents Emscripten from
     * shutting down when main() returns — the EMT pthread continues
     * running the VM.
     */
    RTPrintf("Main thread returning (EMT continues in background).\n");
    emscripten_exit_with_live_runtime();
#else
    /*
     * For Node.js testing, wait for the VM to finish.
     */
    RTPrintf("Press Ctrl+C to stop.\n");
    for (;;)
        RTThreadSleep(1000);
#endif

    return 0;
}
