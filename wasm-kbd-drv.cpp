/*
 * VirtualBox/Wasm — Minimal keyboard driver for browser input.
 *
 * This PDM driver sits below DrvKeyboardQueue on the pckbd device's
 * LUN#0.  It implements PDMIKEYBOARDCONNECTOR (LED status from guest,
 * mostly no-ops) and stores a global pointer to DrvKeyboardQueue's
 * PDMIKEYBOARDPORT so that JavaScript can inject PS/2 scancodes.
 *
 * Threading model:
 *   - wasmKbdPutScancode() is called from the main browser thread (JS).
 *     It only writes to a shared ring buffer — no PDM calls.
 *   - wasmKbdDrainQueue() is called from the EMT thread (IEMExecLots).
 *     It reads from the ring buffer and calls pfnPutEventScan on the EMT.
 *   This avoids cross-thread PDM critical section issues.
 *
 * Driver chain:
 *   PS2K device ← DrvKeyboardQueue ← WasmKeyboard (this driver)
 *                 pfnPutEventScan ←── called from EMT via drain
 */

#define LOG_GROUP LOG_GROUP_DRV_KBD_QUEUE
#include <VBox/vmm/pdmdrv.h>
#include <VBox/vmm/pdmifs.h>
#include <iprt/assert.h>
#include <iprt/stream.h>
#include <iprt/uuid.h>
#include <VBox/err.h>

#ifdef __EMSCRIPTEN__
# include <emscripten.h>
#endif


/*************************************************************************
 * Instance data
 *************************************************************************/
typedef struct DRVWASMKBD
{
    PPDMDRVINS              pDrvIns;
    PDMIKEYBOARDCONNECTOR   IConnector;
} DRVWASMKBD;
typedef DRVWASMKBD *PDRVWASMKBD;

#define CON2THIS(pInterface) \
    RT_FROM_MEMBER(pInterface, DRVWASMKBD, IConnector)


/*************************************************************************
 * Global keyboard port — set during construct, used by EMT drain.
 *************************************************************************/
static PPDMIKEYBOARDPORT g_pKbdPort = NULL;


/*************************************************************************
 * Shared ring buffer for cross-thread scancode passing.
 * Main thread writes (wasmKbdPutScancode), EMT reads (wasmKbdDrainQueue).
 *************************************************************************/
#define WASM_KBD_BUF_SIZE 128
static volatile uint8_t  s_aKbdBuf[WASM_KBD_BUF_SIZE];
static volatile uint32_t s_iKbdWrite = 0;
static volatile uint32_t s_iKbdRead  = 0;


/*************************************************************************
 * PDMIKEYBOARDCONNECTOR — callbacks from guest (LEDs, etc.)
 *************************************************************************/

static DECLCALLBACK(void) wasmKbdLedStatusChange(PPDMIKEYBOARDCONNECTOR pInterface, PDMKEYBLEDS enmLeds)
{
    RT_NOREF(pInterface, enmLeds);
}

static DECLCALLBACK(void) wasmKbdSetActive(PPDMIKEYBOARDCONNECTOR pInterface, bool fActive)
{
    RT_NOREF(pInterface, fActive);
}

static DECLCALLBACK(void) wasmKbdFlushQueue(PPDMIKEYBOARDCONNECTOR pInterface)
{
    RT_NOREF(pInterface);
}


/*************************************************************************
 * PDMIBASE
 *************************************************************************/

static DECLCALLBACK(void *) wasmKbdQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PPDMDRVINS pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVWASMKBD pThis = PDMINS_2_DATA(pDrvIns, PDRVWASMKBD);

    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIKEYBOARDCONNECTOR, &pThis->IConnector);
    return NULL;
}


/*************************************************************************
 * Construct / Destruct
 *************************************************************************/

static DECLCALLBACK(int) wasmKbdConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags)
{
    RT_NOREF(pCfg, fFlags);
    PDMDRV_CHECK_VERSIONS_RETURN(pDrvIns);
    PDRVWASMKBD pThis = PDMINS_2_DATA(pDrvIns, PDRVWASMKBD);

    pThis->pDrvIns = pDrvIns;

    /* IBase */
    pDrvIns->IBase.pfnQueryInterface = wasmKbdQueryInterface;

    /* IKeyboardConnector */
    pThis->IConnector.pfnLedStatusChange = wasmKbdLedStatusChange;
    pThis->IConnector.pfnSetActive       = wasmKbdSetActive;
    pThis->IConnector.pfnFlushQueue      = wasmKbdFlushQueue;

    /*
     * Get the keyboard port from the driver above (DrvKeyboardQueue).
     * This is how we inject scancodes from JavaScript.
     */
    g_pKbdPort = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMIKEYBOARDPORT);
    if (g_pKbdPort)
        RTPrintf("[WasmKbd] Keyboard port obtained — input bridge ready\n");
    else
        RTPrintf("[WasmKbd] WARNING: No keyboard port interface above!\n");

    return VINF_SUCCESS;
}

static DECLCALLBACK(void) wasmKbdDestruct(PPDMDRVINS pDrvIns)
{
    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);
    g_pKbdPort = NULL;
}


/*************************************************************************
 * Driver registration
 *************************************************************************/
extern const PDMDRVREG g_DrvWasmKeyboard;
const PDMDRVREG g_DrvWasmKeyboard =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szName */
    "WasmKeyboard",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "Wasm keyboard input bridge driver",
    /* fFlags */
    PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass */
    PDM_DRVREG_CLASS_KEYBOARD,
    /* cMaxInstances */
    1,
    /* cbInstance */
    sizeof(DRVWASMKBD),
    /* pfnConstruct */
    wasmKbdConstruct,
    /* pfnDestruct */
    wasmKbdDestruct,
    /* pfnRelocate */
    NULL,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    NULL,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnPowerOff */
    NULL,
    /* pfnSoftReset */
    NULL,
    /* u32VersionEnd */
    PDM_DRVREG_VERSION
};


/*************************************************************************
 * JS-callable export — queue a PS/2 scancode (main thread safe).
 * Only writes to shared ring buffer; no PDM calls.
 *************************************************************************/
extern "C" {

EMSCRIPTEN_KEEPALIVE
int wasmKbdPutScancode(int scancode)
{
    uint32_t iWrite = s_iKbdWrite;
    uint32_t iNext  = (iWrite + 1) % WASM_KBD_BUF_SIZE;
    if (iNext == s_iKbdRead)
        return -1; /* buffer full */
    s_aKbdBuf[iWrite] = (uint8_t)scancode;
    s_iKbdWrite = iNext;
    return 0;
}

/*
 * Drain the ring buffer into the VBox keyboard port.
 * MUST be called from the EMT thread (IEMExecLots / JIT hook).
 */
EMSCRIPTEN_KEEPALIVE
int wasmKbdDrainQueue(void)
{
    if (!g_pKbdPort)
        return 0;
    int cDrained = 0;
    while (s_iKbdRead != s_iKbdWrite)
    {
        uint8_t sc = s_aKbdBuf[s_iKbdRead];
        s_iKbdRead = (s_iKbdRead + 1) % WASM_KBD_BUF_SIZE;
        g_pKbdPort->pfnPutEventScan(g_pKbdPort, sc);
        cDrained++;
    }
    return cDrained;
}

} /* extern "C" */
