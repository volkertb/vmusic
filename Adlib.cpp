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
    // Log level 9 is used for PCM rendering
#include <VBox/vmm/pdmdev.h>
#include <VBox/AssertGuest.h>
#include <VBox/version.h>
#include <iprt/assert.h>
#include <iprt/mem.h>

#include "opl3.h"

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

#define ADLIB_DEFAULT_IO_BASE         0x388

#define ADLIB_DEFAULT_OUT_DEVICE    "default"
#define ADLIB_DEFAULT_SAMPLE_RATE   22050 /* Hz */
#define ADLIB_NUM_CHANNELS          2 /* as we are actually supporting OPL3 */

enum {
    ADLIB_PORT_ADDR = 0,
    ADLIB_PORT_STATUS = 0,
    ADLIB_PORT_DATA = 1,
    ADLIB_PORT_ADDR2 = 2,
    ADLIB_PORT_DATA2 = 3
};

/** The saved state version. */
#define ADLIB_SAVED_STATE_VERSION     1

/** Maximum number of sound samples render in one batch by render thread. */
#define ADLIB_RENDER_BLOCK_TIME     5 /* in millisec */

/** The render thread will shutdown if this time passes since the last OPL register write. */
#define ADLIB_RENDER_SUSPEND_TIMEOUT 5000 /* in millisec */

#define OPL2_NUM_IO_PORTS       2
#define OPL3_NUM_IO_PORTS       4

#define OPL_TIMER1_PERIOD       80    /* microseconds */
#define OPL_TIMER2_PERIOD       320

enum {
    OPL_REG_WAVEFORM_ENABLE = 0x01,
    OPL_REG_TIMER1          = 0x02,
    OPL_REG_TIMER2          = 0x03,
    OPL_REG_TIMER_CTRL      = 0x04,
    OPL_REG_FM_MODE         = 0x08
};

/** Device configuration & state. */
typedef struct {
    /* Device configuration. */
    /** Whether to emulate an OPL3. */
    bool                   fOPL3;
    /** Base port. */
    RTIOPORT               uPort;
    /** Base port for mirror (e.g. SB16 compatibility). May be 0. */
    RTIOPORT               uMirrorPort;
    /** Sample rate for PCM output. */
    uint16_t               uSampleRate;
    /** Device for PCM output. */
    R3PTRTYPE(char *)      pszOutDevice;

    /* Runtime state. */
    /** Audio output device */
    PCMOutBackend          pcmOut;
    /** Thread that connects to PCM out, renders and pushes audio data. */
    RTTHREAD               hRenderThread;
    /** Buffer for the rendering thread to use, size defined by ADLIB_RENDER_BLOCK_TIME. */
    R3PTRTYPE(uint8_t *)   pbRenderBuf;
    /** Flag to signal render thread to shut down. */
    bool volatile          fShutdown;
    /** Flag from render thread indicated it has shutdown (e.g. due to error or timeout). */
    bool volatile          fStopped;
    /** (System clock) timestamp of last OPL chip access. */
    uint64_t               tmLastWrite;

    /** To protect access to opl3_chip from the render thread and main thread. */
    RTCRITSECT             critSect;
    /** Handle to nuked. */
    opl3_chip              opl;

    /** Current selected register index */
    uint16_t               oplReg;

    /** OPL timer status */
    uint8_t                timer1Value,  timer2Value;
    uint64_t               timer1Expire, timer2Expire; /* (virtual clock) timestamps */
    bool                   timer1Enable, timer2Enable;

    IOMIOPORTHANDLE        hIoPorts;
    IOMIOPORTHANDLE        hMirrorPorts;
} ADLIBSTATE;
typedef ADLIBSTATE *PADLIBSTATE;

#ifndef VBOX_DEVICE_STRUCT_TESTCASE

static inline uint64_t adlibCalculateFramesFromMilli(PADLIBSTATE pThis, uint64_t milli)
{
    uint64_t rate = pThis->uSampleRate;
    return (rate * milli) / 1000;
}

static inline size_t adlibCalculateBytesFromFrames(PADLIBSTATE pThis, uint64_t frames)
{
    NOREF(pThis);
    return frames * sizeof(uint16_t) * ADLIB_NUM_CHANNELS;
}

static uint64_t adlibCalculateTimerExpire(PPDMDEVINS pDevIns, uint8_t value, uint64_t period)
{
    uint64_t delay_usec = (0x100 - value) * period;
    if (delay_usec < 100)  delay_usec = 0; // short delay: Likely just checking for OPL precense; fire timer now.
    uint64_t freq = PDMDevHlpTMTimeVirtGetFreq(pDevIns);
    uint64_t delay_ticks = (delay_usec * freq) / 1000000UL /*1usec in hz*/;
    uint64_t now_ticks = PDMDevHlpTMTimeVirtGet(pDevIns);
    return now_ticks + delay_ticks;
}

/**
 * The render thread calls into the emulator to render audio frames, and then pushes them
 * on the PCM output device.
 * We rely on the PCM output device's blocking writes behavior to avoid running continously.
 * A small block size (ADLIB_RENDER_BLOCK_TIME) is also used to give the main thread some
 * opportunities to run.
 *
 * @callback_method_impl{FNRTTHREAD}
 */
static DECLCALLBACK(int) adlibRenderThread(RTTHREAD ThreadSelf, void *pvUser)
{
    RT_NOREF(ThreadSelf);
    PADLIBSTATE pThis = (PADLIBSTATE)pvUser;
    PCMOutBackend *pPcmOut = &pThis->pcmOut;

    // Compute the max number of frames we can store on our temporary buffer.
    int16_t *buf = (int16_t*) pThis->pbRenderBuf;
    uint64_t buf_frames = adlibCalculateFramesFromMilli(pThis, ADLIB_RENDER_BLOCK_TIME);

    Log(("adlib: Starting render thread with buf_frames=%lld\n", buf_frames));

    int rc = pPcmOut->open(pThis->pszOutDevice, pThis->uSampleRate, ADLIB_NUM_CHANNELS);
    AssertLogRelRCReturn(rc, rc);

    while (!ASMAtomicReadBool(&pThis->fShutdown)
           && ASMAtomicReadU64(&pThis->tmLastWrite) + ADLIB_RENDER_SUSPEND_TIMEOUT >= RTTimeSystemMilliTS()) {
        Log9(("rendering %lld frames\n", buf_frames));

        RTCritSectEnter(&pThis->critSect);
        OPL3_GenerateStream(&pThis->opl, buf, buf_frames);
        RTCritSectLeave(&pThis->critSect);

        Log9(("writing %lld frames\n", buf_frames));

        ssize_t written_frames = pPcmOut->write(buf, buf_frames);
        if (written_frames < 0) {
            rc = written_frames;
            AssertLogRelMsgFailedBreak(("adlib: render thread write err=%Rrc\n", written_frames));
        }

        RTThreadYield();
    }

    int rcClose = pPcmOut->close();
    AssertLogRelRC(rcClose);
    if (RT_SUCCESS(rc)) rc = rcClose;

    Log(("adlib: Stopping render thread with rc=%Rrc\n", rc));

    ASMAtomicWriteBool(&pThis->fStopped, true);

    return VINF_SUCCESS;
}

/** Waits for the render thread to finish and reaps it. */
static int adlibReapRenderThread(PPDMDEVINS pDevIns, RTMSINTERVAL millies = 100)
{
    PADLIBSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PADLIBSTATE);

    if (pThis->hRenderThread != NIL_RTTHREAD) {
        int rc = RTThreadWait(pThis->hRenderThread, millies, NULL);
        if (RT_SUCCESS(rc)) {
            pThis->hRenderThread = NIL_RTTHREAD;
        } else {
            LogWarn(("adlib%d: render thread did not terminate (%Rrc)\n", pDevIns->iInstance, rc));
            AssertRCReturn(rc, rc);
        }
    }

    return VINF_SUCCESS;
}

/** Raises signal for render thread to stop; potentially waits for it. */
static int adlibStopRenderThread(PPDMDEVINS pDevIns, bool wait = false)
{
    PADLIBSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PADLIBSTATE);

    if (pThis->hRenderThread == NIL_RTTHREAD) {
        // Already stopped & reaped
        return VINF_SUCCESS;
    }

    // Raise the flag for the thread
    ASMAtomicWriteBool(&pThis->fShutdown, true);

    if (wait) {
        int rc = adlibReapRenderThread(pDevIns, 30000);
        AssertRCReturn(rc, rc);
    }

    return VINF_SUCCESS;
}

static void adlibWakeRenderThread(PPDMDEVINS pDevIns)
{
    PADLIBSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PADLIBSTATE);

    ASMAtomicWriteU64(&pThis->tmLastWrite, RTTimeSystemMilliTS());

    // Reap any existing render thread if it had stopped
    if (ASMAtomicReadBool(&pThis->fStopped)) {
        int rc = adlibReapRenderThread(pDevIns);
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

        int rc = RTThreadCreateF(&pThis->hRenderThread, adlibRenderThread, pThis, 0,
                                 RTTHREADTYPE_IO, RTTHREADFLAGS_WAITABLE,
                                 "adlib%u_render", pDevIns->iInstance);
        AssertLogRelRCReturnVoid(rc);
    }
}

static uint8_t adlibReadStatus(PPDMDEVINS pDevIns)
{
    PADLIBSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PADLIBSTATE);
    uint8_t status = 0;

    /* The status byte has the following structure:
       Bit 7 - set if either timer has expired.
           6 - set if timer 1 has expired.
           5 - set if timer 2 has expired. */

    uint64_t tmNow = PDMDevHlpTMTimeVirtGet(pDevIns);

    Log5Func(("tmNow=%llu timer1=%llu timer2=%llu\n", tmNow,
              pThis->timer1Enable ? pThis->timer1Expire : 0,
              pThis->timer2Enable ? pThis->timer2Expire : 0));

    if (pThis->timer1Enable && tmNow > pThis->timer1Expire) {
        status |= RT_BIT(7) | RT_BIT(6);
    }
    if (pThis->timer2Enable && tmNow > pThis->timer2Expire) {
        status |= RT_BIT(7) | RT_BIT(5);
    }
    if (!pThis->fOPL3) {
        // OPL2 seems to have this as special signature.
        status |= 0x6;
    }

    Log3Func(("status=0x%x\n", status));

    return status;
}

static void adlibWriteRegister(PPDMDEVINS pDevIns, uint16_t reg, uint8_t value)
{
    PADLIBSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PADLIBSTATE);

    // Any write to a register causes the render thread to be waken up
    adlibWakeRenderThread(pDevIns);

    Log3Func(("0x%x = 0x%x\n", reg, value));

    switch (reg)
    {
        case OPL_REG_TIMER1:
            /* Timer 1 Data.  If Timer 1 is enabled, the value in this
               register will be incremented until it overflows.  Upon
               overflow, the sound card will signal a TIMER interrupt
               (INT 08) and set bits 7 and 6 in its status byte.  The
               value for this timer is incremented every eighty (80)
               microseconds. */
            pThis->timer1Value = value;
            if (pThis->timer1Enable) {
                pThis->timer1Expire = adlibCalculateTimerExpire(pDevIns, pThis->timer1Value, OPL_TIMER1_PERIOD);
            }
            break;

        case OPL_REG_TIMER2:
            /* Timer 2 Data.  If Timer 2 is enabled, the value in this
               register will be incremented until it overflows.  Upon
               overflow, the sound card will signal a TIMER interrupt
               (INT 08) and set bits 7 and 5 in its status byte.  The
               value for this timer is incremented every three hundred
               twenty (320) microseconds. */
            pThis->timer2Value = value;
            if (pThis->timer2Enable) {
                pThis->timer2Expire = adlibCalculateTimerExpire(pDevIns, pThis->timer2Value, OPL_TIMER2_PERIOD);
            }
            break;

        case OPL_REG_TIMER_CTRL:
            /* Timer Control Byte
               bit 7 - Resets the flags for timers 1 & 2.  If set,
                       all other bits are ignored.
               bit 6 - Masks Timer 1.  If set, bit 0 is ignored.
               bit 5 - Masks Timer 2.  If set, bit 1 is ignored.
               bit 1 - When clear, Timer 2 does not operate.
                       When set, the value from byte 03 is loaded into
                       Timer 2, and incrementation begins.
               bit 0 - When clear, Timer 1 does not operate.
                       When set, the value from byte 02 is loaded into
                       Timer 1, and incrementation begins.  */
            if (value & RT_BIT(7)) {
                pThis->timer1Enable = false;
                pThis->timer2Enable = false;
            } else {
                if (!(value & RT_BIT(6))) {
                    pThis->timer1Enable = value & RT_BIT(0);
                    if (pThis->timer1Enable) {
                        pThis->timer1Expire = adlibCalculateTimerExpire(pDevIns, pThis->timer1Value, OPL_TIMER1_PERIOD);
                    }
                }
                if (!(value & RT_BIT(5))) {
                    pThis->timer2Enable = value & RT_BIT(1);
                    if (pThis->timer2Enable) {
                        pThis->timer2Expire = adlibCalculateTimerExpire(pDevIns, pThis->timer2Value, OPL_TIMER2_PERIOD);
                    }
                }
            }
            break;

        default:
            RTCritSectEnter(&pThis->critSect);
            OPL3_WriteRegBuffered(&pThis->opl, reg, value);
            RTCritSectLeave(&pThis->critSect);
            break;
    }
}

/**
 * @callback_method_impl{FNIOMIOPORTNEWIN}
 */
static DECLCALLBACK(VBOXSTRICTRC) adlibIoPortRead(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT offPort, uint32_t *pu32, unsigned cb)
{
    RT_NOREF(pvUser);
    if (cb == 1)
    {
        uint32_t uValue;

        switch (offPort)
        {
            case ADLIB_PORT_STATUS:
                uValue = adlibReadStatus(pDevIns);
                break;
            default:
                ASSERT_GUEST_MSG_FAILED(("invalid port %#x\n", offPort));
                uValue = 0xff;
                break;
        }

        Log7Func(("read port %u: %#04x\n", offPort, uValue));

        *pu32 = uValue;
        return VINF_SUCCESS;
    }
    ASSERT_GUEST_MSG_FAILED(("offPort=%#x cb=%d\n", offPort, cb));
    return VERR_IOM_IOPORT_UNUSED;
}

/**
 * @callback_method_impl{FNIOMIOPORTNEWOUT}
 */
static DECLCALLBACK(VBOXSTRICTRC) adlibIoPortWrite(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT offPort, uint32_t u32, unsigned cb)
{
    RT_NOREF(pvUser);
    if (cb == 1)
    {
        PADLIBSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PADLIBSTATE);
        Log7Func(("write port %u: %#04x\n", offPort, u32));

        uint8_t val = u32;

        switch (offPort)
        {
            case ADLIB_PORT_ADDR:
                pThis->oplReg = val;
                break;
            case ADLIB_PORT_ADDR2:
                pThis->oplReg = val | 0x100;
                break;
            case ADLIB_PORT_DATA:
            case ADLIB_PORT_DATA2:
                adlibWriteRegister(pDevIns, pThis->oplReg, val);
                break;

            default:
                ASSERT_GUEST_MSG_FAILED(("invalid port %#x\n", offPort));
                break;
        }
    }
    else
        ASSERT_GUEST_MSG_FAILED(("offPort=%#x cb=%d\n", offPort, cb));
    return VINF_SUCCESS;
}

# ifdef IN_RING3

/**
 * @callback_method_impl{FNSSMDEVSAVEEXEC}
 */
static DECLCALLBACK(int) adlibR3SaveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    PADLIBSTATE     pThis = PDMDEVINS_2_DATA(pDevIns, PADLIBSTATE);
    PCPDMDEVHLPR3   pHlp  = pDevIns->pHlpR3;

    // Don't care if the configuration changes after resume, so not saving it

    // However save as much of the current state as possible
    pHlp->pfnSSMPutU16   (pSSM, pThis->oplReg);

    // TODO: We should save a copy of all current registers

    pHlp->pfnSSMPutU8    (pSSM, pThis->timer1Value);
    pHlp->pfnSSMPutU8    (pSSM, pThis->timer2Value);
    pHlp->pfnSSMPutU64   (pSSM, pThis->timer1Expire);
    pHlp->pfnSSMPutU64   (pSSM, pThis->timer2Expire);
    pHlp->pfnSSMPutBool  (pSSM, pThis->timer1Enable);
    pHlp->pfnSSMPutBool  (pSSM, pThis->timer2Enable);

	return 0;
}

/**
 * @callback_method_impl{FNSSMDEVLOADEXEC}
 */
static DECLCALLBACK(int) adlibR3LoadExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    PADLIBSTATE     pThis = PDMDEVINS_2_DATA(pDevIns, PADLIBSTATE);
    PCPDMDEVHLPR3   pHlp  = pDevIns->pHlpR3;

    Assert(uPass == SSM_PASS_FINAL);
    NOREF(uPass);

    pHlp->pfnSSMGetU16   (pSSM, &pThis->oplReg);

    pHlp->pfnSSMGetU8    (pSSM, &pThis->timer1Value);
    pHlp->pfnSSMGetU8    (pSSM, &pThis->timer2Value);
    pHlp->pfnSSMGetU64   (pSSM, &pThis->timer1Expire);
    pHlp->pfnSSMGetU64   (pSSM, &pThis->timer2Expire);
    pHlp->pfnSSMGetBool  (pSSM, &pThis->timer1Enable);
    pHlp->pfnSSMGetBool  (pSSM, &pThis->timer2Enable);

    pThis->tmLastWrite = RTTimeSystemMilliTS();

    if (uVersion > ADLIB_SAVED_STATE_VERSION)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;

    return 0;
}

/**
 * @interface_method_impl{PDMDEVREG,pfnReset}
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance data.
 */
static DECLCALLBACK(void) adlibR3Reset(PPDMDEVINS pDevIns)
{
    PADLIBSTATE   pThis   = PDMDEVINS_2_DATA(pDevIns, PADLIBSTATE);
    
    RTCritSectEnter(&pThis->critSect);
    OPL3_Reset(&pThis->opl, pThis->uSampleRate);
    RTCritSectLeave(&pThis->critSect);
	pThis->oplReg = 0;
    pThis->timer1Enable = false;
    pThis->timer1Expire = 0;
    pThis->timer1Value = 0;
    pThis->timer2Enable = false;
    pThis->timer2Expire = 0;
    pThis->timer2Value = 0;
}

/**
 * @interface_method_impl{PDMDEVREG,pfnSuspend}
 */
static DECLCALLBACK(void) adlibR3Suspend(PPDMDEVINS pDevIns)
{
    adlibStopRenderThread(pDevIns);
}

/**
 * @interface_method_impl{PDMDEVREG,pfnPowerOff}
 */
static DECLCALLBACK(void) adlibR3PowerOff(PPDMDEVINS pDevIns)
{
    adlibStopRenderThread(pDevIns);
}

/**
 * @interface_method_impl{PDMDEVREG,pfnConstruct}
 */
static DECLCALLBACK(int) adlibR3Construct(PPDMDEVINS pDevIns, int iInstance, PCFGMNODE pCfg)
{
    PDMDEV_CHECK_VERSIONS_RETURN(pDevIns);
    PADLIBSTATE     pThis   = PDMDEVINS_2_DATA(pDevIns, PADLIBSTATE);
    PCPDMDEVHLPR3   pHlp    = pDevIns->pHlpR3;
    int             rc;

    Assert(iInstance == 0);

    /*
     * Validate and read the configuration.
     */
    PDMDEV_VALIDATE_CONFIG_RETURN(pDevIns, "OPL3|Port|MirrorPort|OutDevice|SampleRate", "");

    rc = pHlp->pfnCFGMQueryBoolDef(pCfg, "OPL3", &pThis->fOPL3, true);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to query \"OPL3\" from the config"));

    rc = pHlp->pfnCFGMQueryPortDef(pCfg, "Port", &pThis->uPort, ADLIB_DEFAULT_IO_BASE);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to query \"Port\" from the config"));

    rc = pHlp->pfnCFGMQueryPortDef(pCfg, "MirrorPort", &pThis->uMirrorPort, 0);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to query \"MirrorPort\" from the config"));

    rc = pHlp->pfnCFGMQueryStringAllocDef(pCfg, "OutDevice", &pThis->pszOutDevice, ADLIB_DEFAULT_OUT_DEVICE);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to query \"OutDevice\" from the config"));

    rc = pHlp->pfnCFGMQueryU16Def(pCfg, "SampleRate", &pThis->uSampleRate, ADLIB_DEFAULT_SAMPLE_RATE);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to query \"SampleRate\" from the config"));

    /*
     * Initialize the device state.
     */
    adlibR3Reset(pDevIns);

    /* Initialize now the buffer that will be used by the render thread. */
    size_t renderBlockSize = adlibCalculateBytesFromFrames(pThis, adlibCalculateFramesFromMilli(pThis, ADLIB_RENDER_BLOCK_TIME));
    pThis->pbRenderBuf = (uint8_t *) RTMemAlloc(renderBlockSize);
    AssertReturn(pThis->pbRenderBuf, VERR_NO_MEMORY);

    /* Prepare the render thread, but not create it yet. */
    pThis->fShutdown = false;
    pThis->fStopped = false;
    pThis->hRenderThread = NIL_RTTHREAD;
    pThis->tmLastWrite = 0;
    rc = RTCritSectInit(&pThis->critSect);
    AssertRCReturn(rc, rc);

    /*
     * Register I/O ports.
     */
    static const IOMIOPORTDESC s_aDescs[] =
    {
	    { "Status", "Address", "Status register", "Primary index register" }, // base + 00h
	    { NULL,     "Data",  NULL, "Primary data register" },                 // base + 01h
	    { NULL,     "Address2", NULL, "Secondary index register (OPL3)" },    // base + 02h
	    { NULL,     "Data2",  NULL, "Secondary data register (OPL3)" },       // base + 03h
        { NULL }
    };
    const unsigned int numPorts = pThis->fOPL3 ? OPL3_NUM_IO_PORTS : OPL2_NUM_IO_PORTS;
    rc = PDMDevHlpIoPortCreateAndMap(pDevIns, pThis->uPort, numPorts, adlibIoPortWrite, adlibIoPortRead,
                                     "Adlib", s_aDescs, &pThis->hIoPorts);
    AssertRCReturn(rc, rc);

    if (pThis->uMirrorPort) {
        rc = PDMDevHlpIoPortCreateAndMap(pDevIns, pThis->uMirrorPort, numPorts, adlibIoPortWrite, adlibIoPortRead,
                                         "AdlibMirror", s_aDescs, &pThis->hMirrorPorts);
        AssertRCReturn(rc, rc);
    } else {
        pThis->hMirrorPorts = 0;
    }

    /*
     * Register saved state.
     */
    rc = PDMDevHlpSSMRegister(pDevIns, ADLIB_SAVED_STATE_VERSION, sizeof(*pThis), adlibR3SaveExec, adlibR3LoadExec);
    AssertRCReturn(rc, rc);

    LogRel(("adlib%i: Configured on ports 0x%x-0x%x\n", iInstance, pThis->uPort, pThis->uPort + numPorts - 1));
    if (pThis->uMirrorPort && pThis->hMirrorPorts) {
        LogRel(("adlib%i: Mirrored on ports 0x%x-0x%x\n", iInstance, pThis->uMirrorPort, pThis->uMirrorPort + numPorts - 1));
    }

    return VINF_SUCCESS;
}

/**
 * @interface_method_impl{PDMDEVREG,pfnDestruct}
 */
static DECLCALLBACK(int) adlibR3Destruct(PPDMDEVINS pDevIns)
{
    PADLIBSTATE     pThis   = PDMDEVINS_2_DATA(pDevIns, PADLIBSTATE);

    /* Shutdown AND terminate the render thread. */
    adlibStopRenderThread(pDevIns, true);

    if (pThis->pbRenderBuf) {
        RTMemFree(pThis->pbRenderBuf);
        pThis->pbRenderBuf = NULL;
    }

    if (pThis->pszOutDevice) {
        PDMDevHlpMMHeapFree(pDevIns, pThis->pszOutDevice);
        pThis->pszOutDevice = NULL;
    }

    return VINF_SUCCESS;
}

# endif /* !IN_RING3 */


/**
 * The device registration structure.
 */
static const PDMDEVREG g_DeviceAdlib =
{
    /* .u32Version = */             PDM_DEVREG_VERSION,
    /* .uReserved0 = */             0,
    /* .szName = */                 "adlib",
    /* .fFlags = */                 PDM_DEVREG_FLAGS_DEFAULT_BITS | PDM_DEVREG_FLAGS_NEW_STYLE,
    /* .fClass = */                 PDM_DEVREG_CLASS_AUDIO,
    /* .cMaxInstances = */          1,
    /* .uSharedVersion = */         42,
    /* .cbInstanceShared = */       sizeof(ADLIBSTATE),
    /* .cbInstanceCC = */           0,
    /* .cbInstanceRC = */           0,
    /* .cMaxPciDevices = */         0,
    /* .cMaxMsixVectors = */        0,
    /* .pszDescription = */         "Adlib.",
# if defined(IN_RING3)
    /* .pszRCMod = */               "",
    /* .pszR0Mod = */               "",
    /* .pfnConstruct = */           adlibR3Construct,
    /* .pfnDestruct = */            adlibR3Destruct,
    /* .pfnRelocate = */            NULL,
    /* .pfnMemSetup = */            NULL,
    /* .pfnPowerOn = */             NULL,
    /* .pfnReset = */               adlibR3Reset,
    /* .pfnSuspend = */             adlibR3Suspend,
    /* .pfnResume = */              NULL,
    /* .pfnAttach = */              NULL,
    /* .pfnDetach = */              NULL,
    /* .pfnQueryInterface = */      NULL,
    /* .pfnInitComplete = */        NULL,
    /* .pfnPowerOff = */            adlibR3PowerOff,
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

    return pCallbacks->pfnRegister(pCallbacks, &g_DeviceAdlib);
}

# endif  /* !VBOX_IN_EXTPACK_R3 */

#endif /* !VBOX_DEVICE_STRUCT_TESTCASE */
