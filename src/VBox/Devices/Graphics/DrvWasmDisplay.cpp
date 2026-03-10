/*
 * VirtualBox/Wasm — Minimal display driver for browser VGA output.
 *
 * This PDM driver connects to the VGA device's LUN#0 and implements
 * PDMIDISPLAYCONNECTOR.  The VGA device renders into our framebuffer
 * (pbData) and calls pfnUpdateRect to flag dirty regions.
 *
 * JavaScript reads the framebuffer via exported C functions and paints
 * it onto an HTML canvas using requestAnimationFrame.
 *
 * Framebuffer format: 32-bit XRGB (VBox native: B in byte 0, G in 1,
 * R in 2, X in 3 on little-endian).  The JS side swizzles to RGBA
 * for ImageData.
 */

#define LOG_GROUP LOG_GROUP_DRV_DISPLAY
#include <VBox/vmm/pdmdrv.h>
#include <VBox/vmm/pdmifs.h>
#include <iprt/assert.h>
#include <iprt/mem.h>
#include <iprt/string.h>
#include <iprt/stream.h>
#include <iprt/uuid.h>
#include <VBox/err.h>

#include "VBoxDD.h"

#ifdef __EMSCRIPTEN__
# include <emscripten.h>
#endif


/*************************************************************************
 * Instance data
 *************************************************************************/
typedef struct DRVWASMDISPLAY
{
    /** Pointer to the driver instance. */
    PPDMDRVINS              pDrvIns;
    /** The display connector interface we provide (up to VGA device). */
    PDMIDISPLAYCONNECTOR    IConnector;
    /** The display port interface of the VGA device (down). */
    PPDMIDISPLAYPORT        pPort;

    /** Framebuffer — allocated by us, written by VGA device. */
    uint8_t                *pbFramebuffer;
    /** Framebuffer size in bytes. */
    uint32_t                cbFramebuffer;
    /** Current width. */
    uint32_t                cxWidth;
    /** Current height. */
    uint32_t                cyHeight;
    /** Bytes per scanline. */
    uint32_t                cbScanline;
    /** Bits per pixel. */
    uint32_t                cBitsPerPixel;
    /** Dirty flag — set by pfnUpdateRect, cleared by JS after read. */
    volatile int            fDirty;
} DRVWASMDISPLAY;
typedef DRVWASMDISPLAY *PDRVWASMDISPLAY;


/* Recover our instance data from the connector interface pointer. */
#define CON2THIS(pInterface) \
    RT_FROM_MEMBER(pInterface, DRVWASMDISPLAY, IConnector)


/*************************************************************************
 * Global pointer — only one VGA display in our Wasm VM.
 * JS-callable functions use this to find the framebuffer.
 *************************************************************************/
static PDRVWASMDISPLAY g_pWasmDisplay = NULL;


/*************************************************************************
 * PDMIDISPLAYCONNECTOR implementation
 *************************************************************************/

/**
 * Called by the VGA device when the display resolution changes.
 * We (re)allocate the framebuffer and update the connector attributes
 * that the VGA device reads (pbData, cbScanline, cBits, cx, cy).
 */
static DECLCALLBACK(int) wasmDispResize(PPDMIDISPLAYCONNECTOR pInterface,
                                        uint32_t cBits, void *pvVRAM,
                                        uint32_t cbLine,
                                        uint32_t cx, uint32_t cy)
{
    PDRVWASMDISPLAY pThis = CON2THIS(pInterface);
    RT_NOREF(pvVRAM);

    RTPrintf("[WasmDisplay] Resize: %ux%u %ubpp (scanline %u bytes)\n",
             cx, cy, cBits, cbLine);

    /* Always use 32-bit framebuffer for simplicity. */
    uint32_t cbNew = cx * cy * 4;
    if (cbNew == 0)
    {
        /* Blank/zero mode — keep old buffer. */
        return VINF_SUCCESS;
    }

    if (cbNew != pThis->cbFramebuffer)
    {
        if (pThis->pbFramebuffer)
            RTMemFree(pThis->pbFramebuffer);
        pThis->pbFramebuffer = (uint8_t *)RTMemAllocZ(cbNew);
        if (!pThis->pbFramebuffer)
            return VERR_NO_MEMORY;
        pThis->cbFramebuffer = cbNew;
    }

    pThis->cxWidth       = cx;
    pThis->cyHeight      = cy;
    pThis->cbScanline    = cx * 4;
    pThis->cBitsPerPixel = 32;

    /* Update the connector attributes the VGA device reads. */
    pThis->IConnector.pbData     = pThis->pbFramebuffer;
    pThis->IConnector.cbScanline = pThis->cbScanline;
    pThis->IConnector.cBits      = 32;  /* we always want 32-bit output */
    pThis->IConnector.cx         = cx;
    pThis->IConnector.cy         = cy;

    pThis->fDirty = 1;
    return VINF_SUCCESS;
}

/**
 * Called by the VGA device after it writes pixels into our framebuffer.
 * We just set a dirty flag — JS polls for it.
 */
static DECLCALLBACK(void) wasmDispUpdateRect(PPDMIDISPLAYCONNECTOR pInterface,
                                             uint32_t x, uint32_t y,
                                             uint32_t cx, uint32_t cy)
{
    PDRVWASMDISPLAY pThis = CON2THIS(pInterface);
    RT_NOREF(x, y, cx, cy);
    pThis->fDirty = 1;
}

/**
 * Periodic refresh callback.  The VGA device's timer calls this.
 * We ask the VGA device to push a fresh frame into our framebuffer.
 */
static DECLCALLBACK(void) wasmDispRefresh(PPDMIDISPLAYCONNECTOR pInterface)
{
    PDRVWASMDISPLAY pThis = CON2THIS(pInterface);
    if (pThis->pPort)
        pThis->pPort->pfnUpdateDisplay(pThis->pPort);
}

static DECLCALLBACK(void) wasmDispReset(PPDMIDISPLAYCONNECTOR pInterface)
{
    RT_NOREF(pInterface);
    RTPrintf("[WasmDisplay] Reset\n");
}

static DECLCALLBACK(void) wasmDispLFBModeChange(PPDMIDISPLAYCONNECTOR pInterface, bool fEnabled)
{
    RT_NOREF(pInterface);
    RTPrintf("[WasmDisplay] LFB mode: %s\n", fEnabled ? "enabled" : "disabled");
}

static DECLCALLBACK(void) wasmDispProcessAdapterData(PPDMIDISPLAYCONNECTOR pInterface,
                                                     void *pvVRAM, uint32_t u32VRAMSize)
{
    RT_NOREF(pInterface, pvVRAM, u32VRAMSize);
}

static DECLCALLBACK(void) wasmDispProcessDisplayData(PPDMIDISPLAYCONNECTOR pInterface,
                                                     void *pvVRAM, unsigned uScreenId)
{
    RT_NOREF(pInterface, pvVRAM, uScreenId);
}


/*************************************************************************
 * IBase
 *************************************************************************/

static DECLCALLBACK(void *) wasmDispQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PPDMDRVINS     pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVWASMDISPLAY pThis  = PDMINS_2_DATA(pDrvIns, PDRVWASMDISPLAY);

    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIDISPLAYCONNECTOR, &pThis->IConnector);
    return NULL;
}


/*************************************************************************
 * Construct / Destruct
 *************************************************************************/

static DECLCALLBACK(int) wasmDispConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags)
{
    RT_NOREF(pCfg, fFlags);
    PDMDRV_CHECK_VERSIONS_RETURN(pDrvIns);
    PDRVWASMDISPLAY pThis = PDMINS_2_DATA(pDrvIns, PDRVWASMDISPLAY);

    pThis->pDrvIns = pDrvIns;

    /*
     * Set up the connector interface.
     */
    pThis->IConnector.pfnResize             = wasmDispResize;
    pThis->IConnector.pfnUpdateRect         = wasmDispUpdateRect;
    pThis->IConnector.pfnRefresh            = wasmDispRefresh;
    pThis->IConnector.pfnReset              = wasmDispReset;
    pThis->IConnector.pfnLFBModeChange      = wasmDispLFBModeChange;
    pThis->IConnector.pfnProcessAdapterData = wasmDispProcessAdapterData;
    pThis->IConnector.pfnProcessDisplayData = wasmDispProcessDisplayData;

    /* Initial framebuffer: 720x400 (VGA text mode). */
    pThis->cxWidth    = 720;
    pThis->cyHeight   = 400;
    pThis->cbScanline = 720 * 4;
    pThis->cBitsPerPixel = 32;
    pThis->cbFramebuffer = 720 * 400 * 4;
    pThis->pbFramebuffer = (uint8_t *)RTMemAllocZ(pThis->cbFramebuffer);
    if (!pThis->pbFramebuffer)
        return VERR_NO_MEMORY;

    pThis->IConnector.pbData     = pThis->pbFramebuffer;
    pThis->IConnector.cbScanline = pThis->cbScanline;
    pThis->IConnector.cBits      = 32;
    pThis->IConnector.cx         = pThis->cxWidth;
    pThis->IConnector.cy         = pThis->cyHeight;

    /*
     * IBase setup.
     */
    pDrvIns->IBase.pfnQueryInterface = wasmDispQueryInterface;

    /*
     * Get the display port interface from the VGA device above us.
     */
    pThis->pPort = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMIDISPLAYPORT);
    if (!pThis->pPort)
    {
        RTPrintf("[WasmDisplay] ERROR: Failed to get PDMIDISPLAYPORT from VGA device!\n");
        return VERR_PDM_MISSING_INTERFACE;
    }

    /* Set the global pointer for JS access. */
    g_pWasmDisplay = pThis;

    RTPrintf("[WasmDisplay] Display driver attached (initial %ux%u 32bpp)\n",
             pThis->cxWidth, pThis->cyHeight);

    return VINF_SUCCESS;
}

static DECLCALLBACK(void) wasmDispDestruct(PPDMDRVINS pDrvIns)
{
    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);
    PDRVWASMDISPLAY pThis = PDMINS_2_DATA(pDrvIns, PDRVWASMDISPLAY);

    if (pThis->pbFramebuffer)
    {
        RTMemFree(pThis->pbFramebuffer);
        pThis->pbFramebuffer = NULL;
    }
    if (g_pWasmDisplay == pThis)
        g_pWasmDisplay = NULL;
}


/*************************************************************************
 * Driver registration record
 *************************************************************************/
const PDMDRVREG g_DrvWasmDisplay =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szName */
    "WasmDisplay",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "Wasm framebuffer display driver",
    /* fFlags */
    PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass */
    PDM_DRVREG_CLASS_DISPLAY,
    /* cMaxInstances */
    1,
    /* cbInstance */
    sizeof(DRVWASMDISPLAY),
    /* pfnConstruct */
    wasmDispConstruct,
    /* pfnDestruct */
    wasmDispDestruct,
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
 * JS-callable C functions (exported via Emscripten EXPORTED_FUNCTIONS)
 *
 * These are called from JavaScript to read the framebuffer.
 *************************************************************************/
extern "C" {

/** Returns pointer to the raw framebuffer (XRGB 32-bit pixels). */
EMSCRIPTEN_KEEPALIVE
uint8_t *wasmDisplayGetFB(void)
{
    return g_pWasmDisplay ? g_pWasmDisplay->pbFramebuffer : NULL;
}

/** Returns current display width. */
EMSCRIPTEN_KEEPALIVE
uint32_t wasmDisplayGetWidth(void)
{
    return g_pWasmDisplay ? g_pWasmDisplay->cxWidth : 0;
}

/** Returns current display height. */
EMSCRIPTEN_KEEPALIVE
uint32_t wasmDisplayGetHeight(void)
{
    return g_pWasmDisplay ? g_pWasmDisplay->cyHeight : 0;
}

/** Returns 1 if framebuffer has been updated since last call, and clears the flag. */
EMSCRIPTEN_KEEPALIVE
int wasmDisplayCheckDirty(void)
{
    if (!g_pWasmDisplay)
        return 0;
    int dirty = g_pWasmDisplay->fDirty;
    g_pWasmDisplay->fDirty = 0;
    return dirty;
}

/** Returns framebuffer size in bytes. */
EMSCRIPTEN_KEEPALIVE
uint32_t wasmDisplayGetFBSize(void)
{
    return g_pWasmDisplay ? g_pWasmDisplay->cbFramebuffer : 0;
}

} /* extern "C" */
