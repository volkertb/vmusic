/*
 * VirtualBox ExtensionPack Skeleton
 * Copyright (C) 2006-2020 Oracle Corporation
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
 
/*
 * VMusic - a VirtualBox extension pack with various music devices
 * Copyright (C) 2022 Javier S. Pedro
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_ENABLED 1
#define LOG_ENABLE_FLOW 1
#define LOG_GROUP LOG_GROUP_DEV_SB16
    // Log level 3 is used for register reads/writes
    // Log level 7 is used for all port in/out
    // Log level 9 is used for all port in/out and PCM rendering
#include <VBox/vmm/pdmdev.h>
#include <VBox/AssertGuest.h>
#include <VBox/version.h>
#include <iprt/assert.h>
#include <iprt/file.h>
#include <iprt/mem.h>

#include "emu8k.h"

#ifndef IN_RING3
#error "R3-only driver"
#endif

#if RT_OPSYS == RT_OPSYS_LINUX
#include "pcmalsa.h"
typedef PCMOutAlsa PCMOutBackend;
#elif RT_OPSYS == RT_OPSYS_WINDOWS
#include "pcmwin.h"
typedef PCMOutWin PCMOutBackend;
#endif

/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

#define EMU_DEFAULT_IO_BASE         0x620 // to match VirtualBox's SB16 @0x220

#define EMU_DEFAULT_OUT_DEVICE      "default"
#define EMU_DEFAULT_SAMPLE_RATE     44100 /* Hz */
#define EMU_NUM_CHANNELS            2

#define EMU_DEFAULT_ONBOARD_RAM     0x7000U /* KiB */

enum {
    EMU_PORT_DATA0    = 0,
    EMU_PORT_DATA0_LO = EMU_PORT_DATA0,
    EMU_PORT_DATA0_HI = EMU_PORT_DATA0+2,
    EMU_PORT_DATA1    = 0x400,
    EMU_PORT_DATA1_LO = EMU_PORT_DATA1,
    EMU_PORT_DATA1_HI = EMU_PORT_DATA1+2,
    EMU_PORT_DATA2    = EMU_PORT_DATA1+2, // intentional overlap
    EMU_PORT_DATA3    = 0x800,
    EMU_PORT_POINTER  = 0x802
};

/** The saved state version. */
#define EMU_SAVED_STATE_VERSION     1

/** Maximum number of sound samples render in one batch by render thread. */
#define EMU_RENDER_BLOCK_TIME       5 /* in millisec */

/** The render thread will shutdown if this time passes since the last OPL register write. */
#define EMU_RENDER_SUSPEND_TIMEOUT  5000 /* in millisec */

/** Device configuration & state. */
typedef struct {
    /* Device configuration. */
    /** Base port. */
    RTIOPORT               uPort;
    /** Sample rate for PCM output. */
    uint16_t               uSampleRate;
    /** Size of onboard RAM in KiB. */
    uint16_t               uOnboardRAM;
    /** Path to find ROM file. */
    R3PTRTYPE(char *)      pszROMFile;
    /** Device for PCM output. */
    R3PTRTYPE(char *)      pszOutDevice;

    /* Runtime state. */
    /** Audio output device */
    PCMOutBackend          pcmOut;
    /** Thread that connects to PCM out, renders and pushes audio data. */
    RTTHREAD               hRenderThread;
    /** Buffer for the rendering thread to use, size defined by EMU_RENDER_BLOCK_TIME. */
    R3PTRTYPE(uint8_t *)   pbRenderBuf;
    /** Flag to signal render thread to shut down. */
    bool volatile          fShutdown;
    /** Flag from render thread indicated it has shutdown (e.g. due to error or timeout). */
    bool volatile          fStopped;
    /** (System clock) timestamp of last port write. */
    uint64_t               tmLastWrite;

    /** (Virtual clock) timestamp of last frame rendered. */
    uint64_t               tmLastRender;

    /** To protect access to opl3_chip from the render thread and main thread. */
    RTCRITSECT             critSect;
    /** Handle to emu8k. */
    R3PTRTYPE(emu8k_t*)    emu;
    /** Contents of ROM file. */
    R3PTRTYPE(void*)       rom;

    IOMIOPORTHANDLE        hIoPorts[3];
} EMUSTATE;
typedef EMUSTATE *PEMUSTATE;

#ifndef VBOX_DEVICE_STRUCT_TESTCASE

DECLINLINE(uint64_t) emuCalculateFramesFromMilli(PEMUSTATE pThis, uint64_t milli)
{
    uint64_t rate = pThis->uSampleRate;
    return (rate * milli) / 1000;
}

DECLINLINE(uint64_t) emuCalculateFramesFromNano(PEMUSTATE pThis, uint64_t nano)
{
    uint64_t rate = pThis->uSampleRate;
    return (rate * nano) / 1000000000;
}

DECLINLINE(size_t) emuCalculateBytesFromFrames(PEMUSTATE pThis, uint64_t frames)
{
    NOREF(pThis);
    return frames * sizeof(uint16_t) * EMU_NUM_CHANNELS;
}



/**
 * The render thread calls into the emulator to render audio frames, and then pushes them
 * on the PCM output device.
 * We rely on the PCM output device's blocking writes behavior to avoid running continously.
 * A small block size (EMU_RENDER_BLOCK_TIME) is also used to give the main thread some
 * opportunities to run.
 *
 * @callback_method_impl{FNRTTHREAD}
 */
static DECLCALLBACK(int) emuRenderThread(RTTHREAD ThreadSelf, void *pvUser)
{
    RT_NOREF(ThreadSelf);
    PPDMDEVINS pDevIns = (PPDMDEVINS)pvUser;
    PEMUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PEMUSTATE);
    PCMOutBackend *pPcmOut = &pThis->pcmOut;

    // Compute the max number of frames we can store on our temporary buffer.
    int16_t *buf = (int16_t*) pThis->pbRenderBuf;
    uint64_t buf_frames = emuCalculateFramesFromMilli(pThis, EMU_RENDER_BLOCK_TIME);

    Log(("emu: Starting render thread with buf_frames=%lld\n", buf_frames));

    int rc = pPcmOut->open(pThis->pszOutDevice, pThis->uSampleRate, EMU_NUM_CHANNELS);
    AssertLogRelRCReturn(rc, rc);

    while (!ASMAtomicReadBool(&pThis->fShutdown)
           && ASMAtomicReadU64(&pThis->tmLastWrite) + EMU_RENDER_SUSPEND_TIMEOUT >= RTTimeSystemMilliTS()) {
        Log9(("rendering %lld frames\n", buf_frames));

        RTCritSectEnter(&pThis->critSect);
        emu8k_render(pThis->emu, buf, buf_frames);
        pThis->tmLastRender = PDMDevHlpTMTimeVirtGetNano(pDevIns);
        RTCritSectLeave(&pThis->critSect);

        Log9(("writing %lld frames\n", buf_frames));

        ssize_t written_frames = pPcmOut->write(buf, buf_frames);
        if (written_frames < 0) {
            rc = written_frames;
            AssertLogRelMsgFailedBreak(("emu: render thread write err=%Rrc\n", written_frames));
        }

        RTThreadYield();
    }

    int rcClose = pPcmOut->close();
    AssertLogRelRC(rcClose);
    if (RT_SUCCESS(rc)) rc = rcClose;

    Log(("emu: Stopping render thread with rc=%Rrc\n", rc));

    ASMAtomicWriteBool(&pThis->fStopped, true);

    return VINF_SUCCESS;
}

/** Waits for the render thread to finish and reaps it. */
static int emuReapRenderThread(PPDMDEVINS pDevIns, RTMSINTERVAL millies = 100)
{
    PEMUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PEMUSTATE);

    if (pThis->hRenderThread != NIL_RTTHREAD) {
        int rc = RTThreadWait(pThis->hRenderThread, millies, NULL);
        if (RT_SUCCESS(rc)) {
            pThis->hRenderThread = NIL_RTTHREAD;
        } else {
            LogWarn(("emu%d: render thread did not terminate (%Rrc)\n", pDevIns->iInstance, rc));
            AssertRCReturn(rc, rc);
        }
    }

    return VINF_SUCCESS;
}

/** Raises signal for render thread to stop; potentially waits for it. */
static int emuStopRenderThread(PPDMDEVINS pDevIns, bool wait = false)
{
    PEMUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PEMUSTATE);

    if (pThis->hRenderThread == NIL_RTTHREAD) {
        // Already stopped & reaped
        return VINF_SUCCESS;
    }

    // Raise the flag for the thread
    ASMAtomicWriteBool(&pThis->fShutdown, true);

    if (wait) {
        int rc = emuReapRenderThread(pDevIns, 30000);
        AssertRCReturn(rc, rc);
    }

    return VINF_SUCCESS;
}

static void emuWakeRenderThread(PPDMDEVINS pDevIns)
{
    PEMUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PEMUSTATE);

    ASMAtomicWriteU64(&pThis->tmLastWrite, RTTimeSystemMilliTS());

    // Reap any existing render thread if it had stopped
    if (ASMAtomicReadBool(&pThis->fStopped)) {
        int rc = emuReapRenderThread(pDevIns);
        AssertLogRelRCReturnVoid(rc);
    } else if (ASMAtomicReadBool(&pThis->fShutdown)
               && pThis->hRenderThread != NIL_RTTHREAD) {
        AssertLogRelMsgFailedReturnVoid(("can't wake render thread -- it's shutting down!\n"));
    }

    // If there is no existing render thread, start a new one
    if (pThis->hRenderThread == NIL_RTTHREAD) {
        pThis->fShutdown = false;
        pThis->fStopped = false;

        Log3(("Creating render thread\n"));

        int rc = RTThreadCreateF(&pThis->hRenderThread, emuRenderThread, pDevIns, 0,
                                 RTTHREADTYPE_IO, RTTHREADFLAGS_WAITABLE,
                                 "emu%u_render", pDevIns->iInstance);
        AssertLogRelRCReturnVoid(rc);
    }
}

/**
 * @callback_method_impl{FNIOMIOPORTNEWIN}
 */
static DECLCALLBACK(VBOXSTRICTRC) emuIoPortRead(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT port, uint32_t *pu32, unsigned cb)
{
    RT_NOREF(pvUser);

    PEMUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PEMUSTATE);

    RTCritSectEnter(&pThis->critSect);
    uint64_t frames_since_last_render = emuCalculateFramesFromNano(pThis, PDMDevHlpTMTimeVirtGetNano(pDevIns) - pThis->tmLastRender);
    emu8k_update_virtual_sample_count(pThis->emu, frames_since_last_render);

    switch (cb) {
        case sizeof(uint8_t):
            *pu32 = emu8k_inb(pThis->emu, port);
            break;
        case sizeof(uint16_t):
            *pu32 = emu8k_inw(pThis->emu, port);
            break;
        case sizeof(uint32_t):
            *pu32 = RT_MAKE_U32(emu8k_inw(pThis->emu, port), emu8k_inw(pThis->emu, port + sizeof(uint16_t)));
            break;
        default:
            ASSERT_GUEST_MSG_FAILED(("port=0x%x cb=%u\n", port, cb));
            *pu32 = 0xff;
            break;
    }

    RTCritSectLeave(&pThis->critSect);

    Log9Func(("read port 0x%X (%u): %#04x\n", port, cb, *pu32));

    return VINF_SUCCESS;
}

/**
 * @callback_method_impl{FNIOMIOPORTNEWOUT}
 */
static DECLCALLBACK(VBOXSTRICTRC) emuIoPortWrite(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT port, uint32_t u32, unsigned cb)
{
    RT_NOREF(pvUser);

    Log9Func(("write port 0x%X (%u): %#04x\n", port, cb, u32));

    PEMUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PEMUSTATE);

    RTCritSectEnter(&pThis->critSect);

    switch (cb) {
        case sizeof(uint8_t):
            emu8k_outb(pThis->emu, port, u32);
            break;
        case sizeof(uint16_t):
            emu8k_outw(pThis->emu, port, u32);
            break;
        case sizeof(uint32_t):
            emu8k_outw(pThis->emu, port,                    RT_LO_U16(u32));
            emu8k_outw(pThis->emu, port + sizeof(uint16_t), RT_HI_U16(u32));
        default:
            ASSERT_GUEST_MSG_FAILED(("port=0x%x cb=%u\n", port, cb));
            break;
    }

    RTCritSectLeave(&pThis->critSect);

    emuWakeRenderThread(pDevIns);

    return VINF_SUCCESS;
}

# ifdef IN_RING3

/**
 * @callback_method_impl{FNSSMDEVSAVEEXEC}
 */
static DECLCALLBACK(int) emuR3SaveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    PEMUSTATE     pThis = PDMDEVINS_2_DATA(pDevIns, PEMUSTATE);
    PCPDMDEVHLPR3   pHlp  = pDevIns->pHlpR3;

    // TODO: Save contents of ROM & RAM?
    RT_NOREF(pSSM, pThis, pHlp);

	return 0;
}

/**
 * @callback_method_impl{FNSSMDEVLOADEXEC}
 */
static DECLCALLBACK(int) emuR3LoadExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    PEMUSTATE     pThis = PDMDEVINS_2_DATA(pDevIns, PEMUSTATE);
    PCPDMDEVHLPR3   pHlp  = pDevIns->pHlpR3;

    Assert(uPass == SSM_PASS_FINAL);
    NOREF(uPass);

    // TODO
    RT_NOREF(pSSM, pThis, pHlp);

    pThis->tmLastWrite = RTTimeSystemMilliTS();

    if (uVersion > EMU_SAVED_STATE_VERSION)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;

    return 0;
}

/**
 * @interface_method_impl{PDMDEVREG,pfnReset}
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance data.
 */
static DECLCALLBACK(void) emuR3Reset(PPDMDEVINS pDevIns)
{
    PEMUSTATE   pThis   = PDMDEVINS_2_DATA(pDevIns, PEMUSTATE);
    
    RTCritSectEnter(&pThis->critSect);
    emu8k_reset(pThis->emu);
    pThis->tmLastRender = PDMDevHlpTMTimeVirtGetNano(pDevIns);
    RTCritSectLeave(&pThis->critSect);
}

/**
 * @interface_method_impl{PDMDEVREG,pfnSuspend}
 */
static DECLCALLBACK(void) emuR3Suspend(PPDMDEVINS pDevIns)
{
    emuStopRenderThread(pDevIns);
}

/**
 * @interface_method_impl{PDMDEVREG,pfnPowerOff}
 */
static DECLCALLBACK(void) emuR3PowerOff(PPDMDEVINS pDevIns)
{
    emuStopRenderThread(pDevIns);
}

/**
 * @interface_method_impl{PDMDEVREG,pfnConstruct}
 */
static DECLCALLBACK(int) emuR3Construct(PPDMDEVINS pDevIns, int iInstance, PCFGMNODE pCfg)
{
    PDMDEV_CHECK_VERSIONS_RETURN(pDevIns);
    PEMUSTATE     pThis   = PDMDEVINS_2_DATA(pDevIns, PEMUSTATE);
    PCPDMDEVHLPR3   pHlp    = pDevIns->pHlpR3;
    int             rc;

    Assert(iInstance == 0);

    // Validate and read the configuration
    PDMDEV_VALIDATE_CONFIG_RETURN(pDevIns, "Port|OnboardRAM|ROMFile|OutDevice|SampleRate", "");

    rc = pHlp->pfnCFGMQueryPortDef(pCfg, "Port", &pThis->uPort, EMU_DEFAULT_IO_BASE);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to query \"Port\" from the config"));

    rc = pHlp->pfnCFGMQueryU16Def(pCfg, "Port", &pThis->uOnboardRAM, EMU_DEFAULT_ONBOARD_RAM);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to query \"OnboardRAM\" from the config"));

    rc = pHlp->pfnCFGMQueryStringAlloc(pCfg, "ROMFile", &pThis->pszROMFile);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to query \"RomFile\" from the config"));

    rc = pHlp->pfnCFGMQueryStringAllocDef(pCfg, "OutDevice", &pThis->pszOutDevice, EMU_DEFAULT_OUT_DEVICE);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to query \"OutDevice\" from the config"));

    rc = pHlp->pfnCFGMQueryU16Def(pCfg, "SampleRate", &pThis->uSampleRate, EMU_DEFAULT_SAMPLE_RATE);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to query \"SampleRate\" from the config"));

    // Validate and read the ROM file
    RTFILE fROM;
    uint64_t uROMSize;
    rc = RTFileOpen(&fROM, pThis->pszROMFile, RTFILE_O_READ | RTFILE_O_OPEN | RTFILE_O_DENY_WRITE);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to open ROMFile"));

    rc = RTFileQuerySize(fROM, &uROMSize);
    if (RT_FAILURE(rc) || uROMSize != _1M)
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("ROMFile is not of correct size (expecting 1MiB file)"));

    pThis->rom = RTMemAlloc(uROMSize);
    AssertPtrReturn(pThis->rom, VERR_NO_MEMORY);

    rc = RTFileRead(fROM, pThis->rom, uROMSize, NULL);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to read ROMFile"));

    // Create the device
    pThis->emu = emu8k_alloc(pThis->rom, pThis->uOnboardRAM);
    AssertPtrReturn(pThis->emu, VERR_NO_MEMORY);

    // Initialize the device
    emuR3Reset(pDevIns);

    /* Initialize now the buffer that will be used by the render thread. */
    size_t renderBlockSize = emuCalculateBytesFromFrames(pThis, emuCalculateFramesFromMilli(pThis, EMU_RENDER_BLOCK_TIME));
    pThis->pbRenderBuf = (uint8_t *) RTMemAlloc(renderBlockSize);
    AssertPtrReturn(pThis->pbRenderBuf, VERR_NO_MEMORY);

    /* Prepare the render thread, but not create it yet. */
    pThis->fShutdown = false;
    pThis->fStopped = false;
    pThis->hRenderThread = NIL_RTTHREAD;
    pThis->tmLastWrite = 0;
    rc = RTCritSectInit(&pThis->critSect);
    AssertRCReturn(rc, rc);

    // Register IO ports.
    const RTIOPORT numPorts = sizeof(uint32_t); // Each port is a "doubleword" or at least 2 words.
    rc = PDMDevHlpIoPortCreateFlagsAndMap(pDevIns, pThis->uPort + EMU_PORT_DATA0, numPorts, IOM_IOPORT_F_ABS,
                                          emuIoPortWrite, emuIoPortRead, "EMU8000 Data0", NULL, &pThis->hIoPorts[0]);
    AssertRCReturn(rc, rc);
    rc = PDMDevHlpIoPortCreateFlagsAndMap(pDevIns, pThis->uPort + EMU_PORT_DATA1, numPorts, IOM_IOPORT_F_ABS,
                                          emuIoPortWrite, emuIoPortRead, "EMU8000 Data1/2", NULL, &pThis->hIoPorts[1]);
    AssertRCReturn(rc, rc);
    rc = PDMDevHlpIoPortCreateFlagsAndMap(pDevIns, pThis->uPort + EMU_PORT_DATA3, numPorts, IOM_IOPORT_F_ABS,
                                          emuIoPortWrite, emuIoPortRead, "EMU8000 Data3/Ptr", NULL, &pThis->hIoPorts[3]);
    AssertRCReturn(rc, rc);

    /*
     * Register saved state.
     */
    rc = PDMDevHlpSSMRegister(pDevIns, EMU_SAVED_STATE_VERSION, sizeof(*pThis), emuR3SaveExec, emuR3LoadExec);
    AssertRCReturn(rc, rc);

    LogRel(("emu8000#%i: Using %hu KiB of onboard RAM\n", iInstance, pThis->uOnboardRAM));

    LogRel(("emu8000#%i: Configured on ports 0x%X-0x%X, 0x%X-0x%X, 0x%X-0x%X\n", iInstance,
            pThis->uPort + EMU_PORT_DATA0, pThis->uPort + EMU_PORT_DATA0 + numPorts - 1,
            pThis->uPort + EMU_PORT_DATA1, pThis->uPort + EMU_PORT_DATA1 + numPorts - 1,
            pThis->uPort + EMU_PORT_DATA3, pThis->uPort + EMU_PORT_DATA3 + numPorts - 1));

    return VINF_SUCCESS;
}

/**
 * @interface_method_impl{PDMDEVREG,pfnDestruct}
 */
static DECLCALLBACK(int) emuR3Destruct(PPDMDEVINS pDevIns)
{
    PEMUSTATE     pThis   = PDMDEVINS_2_DATA(pDevIns, PEMUSTATE);

    /* Shutdown AND terminate the render thread. */
    emuStopRenderThread(pDevIns, true);

    if (pThis->pbRenderBuf) {
        RTMemFree(pThis->pbRenderBuf);
        pThis->pbRenderBuf = NULL;
    }

    if (pThis->pszOutDevice) {
        PDMDevHlpMMHeapFree(pDevIns, pThis->pszOutDevice);
        pThis->pszOutDevice = NULL;
    }

    if (pThis->emu) {
        emu8k_free(pThis->emu);
        pThis->emu = NULL;
    }

    if (pThis->rom) {
        RTMemFree(pThis->rom);
        pThis->rom = NULL;
    }

    return VINF_SUCCESS;
}

# endif /* !IN_RING3 */


/**
 * The device registration structure.
 */
static const PDMDEVREG g_DeviceEmu =
{
    /* .u32Version = */             PDM_DEVREG_VERSION,
    /* .uReserved0 = */             0,
    /* .szName = */                 "emu8000",
    /* .fFlags = */                 PDM_DEVREG_FLAGS_DEFAULT_BITS | PDM_DEVREG_FLAGS_NEW_STYLE,
    /* .fClass = */                 PDM_DEVREG_CLASS_AUDIO,
    /* .cMaxInstances = */          1,
    /* .uSharedVersion = */         42,
    /* .cbInstanceShared = */       sizeof(EMUSTATE),
    /* .cbInstanceCC = */           0,
    /* .cbInstanceRC = */           0,
    /* .cMaxPciDevices = */         0,
    /* .cMaxMsixVectors = */        0,
    /* .pszDescription = */         "EMU8000.",
# if defined(IN_RING3)
    /* .pszRCMod = */               "",
    /* .pszR0Mod = */               "",
    /* .pfnConstruct = */           emuR3Construct,
    /* .pfnDestruct = */            emuR3Destruct,
    /* .pfnRelocate = */            NULL,
    /* .pfnMemSetup = */            NULL,
    /* .pfnPowerOn = */             NULL,
    /* .pfnReset = */               emuR3Reset,
    /* .pfnSuspend = */             emuR3Suspend,
    /* .pfnResume = */              NULL,
    /* .pfnAttach = */              NULL,
    /* .pfnDetach = */              NULL,
    /* .pfnQueryInterface = */      NULL,
    /* .pfnInitComplete = */        NULL,
    /* .pfnPowerOff = */            emuR3PowerOff,
    /* .pfnSoftReset = */           NULL,
    /* .pfnReserved0 = */           NULL,
    /* .pfnReserved1 = */           NULL,
    /* .pfnReserved2 = */           NULL,
    /* .pfnReserved3 = */           NULL,
    /* .pfnReserved4 = */           NULL,
    /* .pfnReserved5 = */           NULL,
    /* .pfnReserved6 = */           NULL,
    /* .pfnReserved7 = */           NULL,
# elif defined(IN_RING0)
    /* .pfnEarlyConstruct = */      NULL,
    /* .pfnConstruct = */           NULL,
    /* .pfnDestruct = */            NULL,
    /* .pfnFinalDestruct = */       NULL,
    /* .pfnRequest = */             NULL,
    /* .pfnReserved0 = */           NULL,
    /* .pfnReserved1 = */           NULL,
    /* .pfnReserved2 = */           NULL,
    /* .pfnReserved3 = */           NULL,
    /* .pfnReserved4 = */           NULL,
    /* .pfnReserved5 = */           NULL,
    /* .pfnReserved6 = */           NULL,
    /* .pfnReserved7 = */           NULL,
# elif defined(IN_RC)
    /* .pfnConstruct = */           NULL,
    /* .pfnReserved0 = */           NULL,
    /* .pfnReserved1 = */           NULL,
    /* .pfnReserved2 = */           NULL,
    /* .pfnReserved3 = */           NULL,
    /* .pfnReserved4 = */           NULL,
    /* .pfnReserved5 = */           NULL,
    /* .pfnReserved6 = */           NULL,
    /* .pfnReserved7 = */           NULL,
# else
#  error "Not in IN_RING3, IN_RING0 or IN_RC!"
# endif
    /* .u32VersionEnd = */          PDM_DEVREG_VERSION
};

# ifdef VBOX_IN_EXTPACK_R3

/**
 * @callback_method_impl{FNPDMVBOXDEVICESREGISTER}
 */
extern "C" DECLEXPORT(int) VBoxDevicesRegister(PPDMDEVREGCB pCallbacks, uint32_t u32Version)
{
    AssertLogRelMsgReturn(u32Version >= VBOX_VERSION,
                          ("u32Version=%#x VBOX_VERSION=%#x\n", u32Version, VBOX_VERSION),
                          VERR_EXTPACK_VBOX_VERSION_MISMATCH);
    AssertLogRelMsgReturn(pCallbacks->u32Version == PDM_DEVREG_CB_VERSION,
                          ("pCallbacks->u32Version=%#x PDM_DEVREG_CB_VERSION=%#x\n", pCallbacks->u32Version, PDM_DEVREG_CB_VERSION),
                          VERR_VERSION_MISMATCH);

    return pCallbacks->pfnRegister(pCallbacks, &g_DeviceEmu);
}

# endif  /* !VBOX_IN_EXTPACK_R3 */

#endif /* !VBOX_DEVICE_STRUCT_TESTCASE */
