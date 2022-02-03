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
    // Log level 3 is used for commands, responses, etc.
    // Log level 5 is used for MIDI data in/out
    // Log level 7 is used for all port in/out
#include <VBox/vmm/pdmdev.h>
#include <VBox/AssertGuest.h>
#include <VBox/version.h>
#include <iprt/assert.h>
#include <iprt/mem.h>
#include <iprt/poll.h>
#include <iprt/circbuf.h>

#ifndef IN_RING3
#error "R3-only driver"
#endif

#if RT_OPSYS == RT_OPSYS_LINUX
#include "midialsa.h"
typedef MIDIAlsa MIDIBackend;
#elif RT_OPSYS == RT_OPSYS_WINDOWS
#include "midiwin.h"
typedef MIDIWin MIDIBackend;
#endif

/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

#define MPU_DEFAULT_IO_BASE         0x330
#define MPU_DEFAULT_IRQ             -1 /* disabled */
#define MPU_IO_SIZE                 2
#define MPU_CIRC_BUFFER_SIZE        16

enum {
    MPU_PORT_DATA = 0,
    MPU_PORT_STATUS = 1,
    MPU_PORT_COMMAND = 1
};

enum {
    MPU_COMMAND_ENTER_UART = 0x3f,
    MPU_COMMAND_RESET = 0xff
};

enum {
    MPU_RESPONSE_ACK = 0xfe
};

/** The saved state version. */
#define MPU_SAVED_STATE_VERSION     1

/** Device configuration & state struct. */
typedef struct {
    /* Device configuration. */
    /** Base port. */
    RTIOPORT               uPort;
    /** IRQ */
    int8_t                 uIrq;

    /* Current state. */
    /** MIDI backend. */
    MIDIBackend            midi;
    /** True if UART mode, false if regular/intelligent mode. */
    bool                   fModeUart;
    /** Buffer used for sending UART data. */
    R3PTRTYPE(PRTCIRCBUF)   pTxBuf;
    /** Buffer used for receiving UART data / command responses. */
    R3PTRTYPE(PRTCIRCBUF)   pRxBuf;

    /** Thread which does actual RX/TX. */
    PPDMTHREAD             pIoThread;

    IOMIOPORTHANDLE        hIoPorts;

} MPUSTATE;
/** Pointer to the shared device state.  */
typedef MPUSTATE *PMPUSTATE;

#ifndef VBOX_DEVICE_STRUCT_TESTCASE

static void mpuLowerIrq(PPDMDEVINS pDevIns)
{
    PMPUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    if (pThis->uIrq >= 0) {
        Log7Func(("irq=%RTbool\n", false));
        PDMDevHlpISASetIrqNoWait(pDevIns, pThis->uIrq, PDM_IRQ_LEVEL_LOW);
    }
}

static void mpuUpdateIrq(PPDMDEVINS pDevIns)
{
    PMPUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    // This function may be called from the IO thread, too.

    if (pThis->uIrq >= 0) {
        bool raise = RTCircBufUsed(pThis->pRxBuf) > 0;
        Log7Func(("irq=%RTbool\n", raise));
        PDMDevHlpISASetIrqNoWait(pDevIns, pThis->uIrq, raise ? PDM_IRQ_LEVEL_HIGH : PDM_IRQ_LEVEL_LOW);
    }
}

static void mpuWakeIoThread(PPDMDEVINS pDevIns)
{
    PMPUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);
    Log7(("wake io thread\n"));
    int rc = pThis->midi.pollInterrupt();
    AssertLogRelRC(rc);
}

static void mpuReset(PPDMDEVINS pDevIns)
{
    PMPUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    if (pThis->fModeUart) {
        Log(("Leaving UART mode\n"));
    }

    pThis->fModeUart = false;

    if (pThis->pIoThread) {
        int rc = PDMDevHlpThreadSuspend(pDevIns, pThis->pIoThread);
        AssertLogRelRC(rc);
    }

    mpuLowerIrq(pDevIns);

    RTCircBufReset(pThis->pTxBuf);
    RTCircBufReset(pThis->pRxBuf);

    pThis->midi.reset();

    mpuUpdateIrq(pDevIns);

    if (pThis->pIoThread) {
        int rc = PDMDevHlpThreadResume(pDevIns, pThis->pIoThread);
        AssertLogRelRC(rc);
    }
}

static void mpuEnterUart(PPDMDEVINS pDevIns)
{
    PMPUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    if (pThis->fModeUart) {
        Log2(("Already in UART mode\n"));
        return;
    }

    Log(("Entering UART mode\n"));

    pThis->fModeUart = true;

    // IO thread needs to wakeup and start polling
    mpuWakeIoThread(pDevIns);
}

static void mpuRespondData(PPDMDEVINS pDevIns, uint8_t data)
{
    PMPUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    // In UART mode we should not be generating system responses
    // which are going to corrupt the MIDI stream.
    AssertLogRelReturnVoid(!pThis->fModeUart);
    // Also the RxBuf CIRCBUF does not support multiple concurrent writers anyway,
    // and in UART mode the IoThread could be writing to it

    Log3Func(("enqueing response=0x%x\n", data));

    uint8_t *buf;
    size_t bufSize;
    RTCircBufAcquireWriteBlock(pThis->pRxBuf, 1, (void**)&buf, &bufSize);
    if (bufSize < 1) {
        LogWarnFunc(("overflow in MIDI RX buffer\n"));
        return;
    }

    *buf = data;
    RTCircBufReleaseWriteBlock(pThis->pRxBuf, 1);

    mpuUpdateIrq(pDevIns);
}

static uint8_t mpuReadData(PPDMDEVINS pDevIns)
{
    PMPUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);
    uint8_t ret = 0xff;

    // Always lower IRQ after a port read, even if there is still more data left
    mpuLowerIrq(pDevIns);

    uint8_t *buf;
    size_t bufSize;
    RTCircBufAcquireReadBlock(pThis->pRxBuf, 1, (void**)&buf, &bufSize);
    if (bufSize > 0) {
        ret = *buf;
        RTCircBufReleaseReadBlock(pThis->pRxBuf, 1);

        Log5Func(("midi_in data=0x%x\n", ret));

        // Raise the IRQ again if we have more data to read
        mpuUpdateIrq(pDevIns);

        if (pThis->fModeUart) {
            // Also ensure we wake the IO thread to poll for more data
            mpuWakeIoThread(pDevIns);
        }
    } else {
        Log3Func(("Trying to read, but no data to read, returning 0x%x\n", ret));
    }

    return ret;
}

static void mpuWriteData(PPDMDEVINS pDevIns, uint8_t data)
{
    PMPUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    if (pThis->fModeUart) {
        uint8_t *buf;
        size_t bufSize;
        RTCircBufAcquireWriteBlock(pThis->pTxBuf, 1, (void**)&buf, &bufSize);
        if (bufSize > 0) {
            *buf = data;
            RTCircBufReleaseWriteBlock(pThis->pTxBuf, 1);

            Log5Func(("midi_out data=0x%x\n", data));

            mpuWakeIoThread(pDevIns); // So that it has a chance to send the data
        } else {
            LogWarnFunc(("Overflow in MIDI TX buffer\n"));
        }
    } else {
        Log3Func(("Ignoring data, not in UART mode\n"));
    }
}

static uint8_t mpuReadStatus(PPDMDEVINS pDevIns)
{
    PMPUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    /* This port indicates whether the interface is ready to
       accept a data/command byte, or has in-bound data
       available for reading.
        Bit 6: Output Ready
         0 - The interface is ready to receive a
             data/command byte
         1 - The interface is not ready to receive a
             data/command byte
        Bit 7: Input Ready
         0 - Data is available for reading
         1 - No data is available for reading */

    uint8_t status = 0;

    Log7Func(("tx buf=%zu/%zu rx buf=%zu/%zu\n",
              RTCircBufUsed(pThis->pTxBuf), RTCircBufFree(pThis->pTxBuf),
              RTCircBufUsed(pThis->pRxBuf), RTCircBufFree(pThis->pRxBuf)));

    if (RTCircBufFree(pThis->pTxBuf) == 0) {
        status |= RT_BIT(6);
    }

    if (RTCircBufUsed(pThis->pRxBuf) == 0) {
        status |= RT_BIT(7);
    }

    Log7(("mpu status: output=%RTbool input=%RTbool\n",
          !(status & RT_BIT(6)), !(status & RT_BIT(7))));

    return status;
}

static void mpuDoCommand(PPDMDEVINS pDevIns, uint8_t cmd)
{
    PMPUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    Log3Func(("cmd = 0x%x\n", cmd));

    if (pThis->fModeUart) {
        switch (cmd) {
            case MPU_COMMAND_RESET:
                mpuReset(pDevIns);
                /* "An ACK will not be sent back upon sending a SYSTEM RESET to
                 * leave the UART MODE ($3F)". */
                break;
            case MPU_COMMAND_ENTER_UART:
                // Nothing to do?
                break;
            default:
                LogWarnFunc(("Unknown command in UART mode: 0x%hx\n", cmd));
                break;
        }
    } else {
        // Normal/intelligent mode is not implemented, save for entering UART mode
        switch (cmd) {
            case MPU_COMMAND_RESET:
                mpuReset(pDevIns);
                mpuRespondData(pDevIns, MPU_RESPONSE_ACK);
                break;
            case MPU_COMMAND_ENTER_UART:
                mpuRespondData(pDevIns, MPU_RESPONSE_ACK);
                mpuEnterUart(pDevIns);
                break;
            default:
                LogWarnFunc(("Unknown command in normal mode: 0x%hx\n", cmd));
                mpuRespondData(pDevIns, MPU_RESPONSE_ACK);
                break;
        }
    }
}

/**
 * @callback_method_impl{PFNPDMTHREADDEV}
 */
static DECLCALLBACK(int) mpuIoThreadLoop(PPDMDEVINS pDevIns, PPDMTHREAD pThread)
{
    RT_NOREF(pDevIns);
    PMPUSTATE pThis = (PMPUSTATE)pThread->pvUser;

    if (pThread->enmState == PDMTHREADSTATE_INITIALIZING)
        return VINF_SUCCESS;

    LogFlowFuncEnter();

    while (pThread->enmState == PDMTHREADSTATE_RUNNING)
    {
        uint32_t events = 0, revents = 0;

        if (pThis->fModeUart) {
            if (RTCircBufUsed(pThis->pTxBuf) > 0) {
                events |= RTPOLL_EVT_WRITE;
            }
            if (RTCircBufFree(pThis->pRxBuf) > 0) {
                events |= RTPOLL_EVT_READ;
            }
        }

        Log7Func(("polling for write=%RTbool read=%RTbool\n",
                  bool(events & RTPOLL_EVT_WRITE), bool(events & RTPOLL_EVT_READ)));

        int rc = pThis->midi.poll(events, &revents, RT_INDEFINITE_WAIT);
        if (RT_SUCCESS(rc)) {
            if (revents & RTPOLL_EVT_WRITE) {
                // Write data from the Tx Buf
                uint8_t *buf;
                size_t bufSize = RTCircBufUsed(pThis->pTxBuf);
                if (bufSize > 0) {
                    ssize_t written = 0;
                    RTCircBufAcquireReadBlock(pThis->pTxBuf, bufSize, (void**)&buf, &bufSize);
                    if (bufSize > 0) {
                        Log7Func(("writing %zu bytes\n", bufSize));
                        written = pThis->midi.write(buf, bufSize);
                        if (written < 0) {
                            LogWarn(("write failed with %Rrc\n", written));
                            written = 0;
                        }
                    }
                    RTCircBufReleaseReadBlock(pThis->pTxBuf, written);
                }
            }
            if (revents & RTPOLL_EVT_READ) {
                // Read data into the Rx Buf
                uint8_t *buf;
                size_t bufSize = RTCircBufFree(pThis->pRxBuf);
                if (bufSize > 0) {
                    ssize_t read = 0;
                    RTCircBufAcquireWriteBlock(pThis->pRxBuf, bufSize, (void**)&buf, &bufSize);
                    if (bufSize > 0) {
                        Log7Func(("reading %zu bytes\n", bufSize));
                        read = pThis->midi.read(buf, bufSize);
                        if (read < 0) {
                            LogWarnFunc(("read failed with %Rrc\n", read));
                            read = 0;
                        }
                    }
                    RTCircBufReleaseWriteBlock(pThis->pRxBuf, read);
                    if (read > 0) {
                        mpuUpdateIrq(pDevIns);
                    }
                }
            }
        } else {
            LogWarnFunc(("poll failed with %Rrc", rc));
        }
    }

    LogFlowFuncLeave();

    return VINF_SUCCESS;
}

/**
 * @callback_method_impl{PFNPDMTHREADWAKEUPDEV}
 */
static DECLCALLBACK(int) mpuIoThreadWakeup(PPDMDEVINS pDevIns, PPDMTHREAD pThread)
{
    PMPUSTATE pThis = (PMPUSTATE)pThread->pvUser;

    RT_NOREF(pDevIns);
    return pThis->midi.pollInterrupt();
}

/**
 * @callback_method_impl{FNIOMIOPORTNEWIN}
 */
static DECLCALLBACK(VBOXSTRICTRC) mpuIoPortRead(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT offPort, uint32_t *pu32, unsigned cb)
{
    RT_NOREF(pvUser);
    if (cb == 1)
    {
        uint32_t uValue;

        switch (offPort)
        {
            case MPU_PORT_DATA:
                uValue = mpuReadData(pDevIns);
                break;

            case MPU_PORT_STATUS:
                uValue = mpuReadStatus(pDevIns);
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
static DECLCALLBACK(VBOXSTRICTRC) mpuIoPortWrite(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT offPort, uint32_t u32, unsigned cb)
{
    RT_NOREF(pvUser);
    if (cb == 1)
    {
        Log7Func(("write port %u: %#04x\n", offPort, u32));

        switch (offPort)
        {
            case MPU_PORT_DATA:
                mpuWriteData(pDevIns, u32);
                break;

            case MPU_PORT_COMMAND:
                mpuDoCommand(pDevIns, u32);
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
static DECLCALLBACK(int) mpuR3SaveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    PMPUSTATE       pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);
    PCPDMDEVHLPR3   pHlp  = pDevIns->pHlpR3;

    AssertLogRel(pThis->pIoThread->enmState != PDMTHREADSTATE_RUNNING);

    pHlp->pfnSSMPutBool(pSSM, pThis->fModeUart);

	return 0;
}

/**
 * @callback_method_impl{FNSSMDEVLOADEXEC}
 */
static DECLCALLBACK(int) mpuR3LoadExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    PMPUSTATE       pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);
    PCPDMDEVHLPR3   pHlp  = pDevIns->pHlpR3;

    Assert(uPass == SSM_PASS_FINAL);
    NOREF(uPass);

    AssertLogRel(pThis->pIoThread->enmState != PDMTHREADSTATE_RUNNING);

    pHlp->pfnSSMGetBool(pSSM, &pThis->fModeUart);

    if (uVersion > MPU_SAVED_STATE_VERSION)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;

    return 0;
}

/**
 * @interface_method_impl{PDMDEVREG,pfnConstruct}
 */
static DECLCALLBACK(int) mpuR3Construct(PPDMDEVINS pDevIns, int iInstance, PCFGMNODE pCfg)
{
    PDMDEV_CHECK_VERSIONS_RETURN(pDevIns);
    PMPUSTATE       pThis   = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);
    PCPDMDEVHLPR3   pHlp    = pDevIns->pHlpR3;
    int             rc;

    Assert(iInstance == 0);

    // Validate and read the configuration.
    PDMDEV_VALIDATE_CONFIG_RETURN(pDevIns, "Port|IRQ", "");

    rc = pHlp->pfnCFGMQueryPortDef(pCfg, "Port", &pThis->uPort, MPU_DEFAULT_IO_BASE);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to query \"Port\" from the config"));

    rc = pHlp->pfnCFGMQueryS8Def(pCfg, "IRQ", &pThis->uIrq, MPU_DEFAULT_IRQ);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to query \"IRQ\" from the config"));

    LogFlowFunc(("mpu401#%i: port 0x%x irq %d\n", iInstance, pThis->uPort, pThis->uIrq));

    // Create buffers
    rc = RTCircBufCreate(&pThis->pTxBuf, MPU_CIRC_BUFFER_SIZE);
    AssertRCReturn(rc, rc);
    rc = RTCircBufCreate(&pThis->pRxBuf, MPU_CIRC_BUFFER_SIZE);
    AssertRCReturn(rc, rc);

    // Initialize the device state.
    mpuReset(pDevIns);

    // Register I/O ports
    static const IOMIOPORTDESC s_aDescs[] =
    {
        { "Data", "Data", NULL, NULL }, // base + 00h
        { "Status", "Command",  NULL, NULL },  // base + 01h
        { NULL }
    };
    rc = PDMDevHlpIoPortCreateAndMap(pDevIns, pThis->uPort, MPU_IO_SIZE, mpuIoPortWrite, mpuIoPortRead,
                                     "MPU-401", s_aDescs, &pThis->hIoPorts);
    AssertRCReturn(rc, rc);

    // Register saved state
    rc = PDMDevHlpSSMRegister(pDevIns, MPU_SAVED_STATE_VERSION, sizeof(*pThis), mpuR3SaveExec, mpuR3LoadExec);
    AssertRCReturn(rc, rc);

    // Open the MIDI device now, before we create the IO thread which may poll it.
    rc = pThis->midi.open("default");
    AssertRCReturn(rc, rc);

    // Create the IO thread; note that this starts it...
    rc = PDMDevHlpThreadCreate(pDevIns, &pThis->pIoThread, pThis, mpuIoThreadLoop,
                               mpuIoThreadWakeup, 0, RTTHREADTYPE_IO, "MpuIo");
    AssertRCReturn(rc, rc);

    LogRel(("mpu401#%i: Configured on port 0x%x-0x%x\n", iInstance, pThis->uPort, pThis->uPort + MPU_IO_SIZE - 1));
    if (pThis->uIrq >= 0) {
        LogRel(("mpu401#%i: Using IRQ %d\n", iInstance, pThis->uIrq));
    }

    return VINF_SUCCESS;
}

/**
 * @interface_method_impl{PDMDEVREG,pfnDestruct}
 */
static DECLCALLBACK(int) mpuR3Destruct(PPDMDEVINS pDevIns)
{
    PMPUSTATE     pThis   = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    if (pThis->pIoThread) {
        int rc, rcThread;
        rc = PDMDevHlpThreadDestroy(pDevIns, pThis->pIoThread, &rcThread);
        AssertLogRelRC(rc);
        pThis->pIoThread = NULL;
    }

    int rc = pThis->midi.close();
    AssertLogRelRC(rc);

    RTCircBufDestroy(pThis->pTxBuf);
    RTCircBufDestroy(pThis->pRxBuf);


    return VINF_SUCCESS;
}

/**
 * @interface_method_impl{PDMDEVREG,pfnReset}
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance data.
 */
static DECLCALLBACK(void) mpuR3Reset(PPDMDEVINS pDevIns)
{
    mpuReset(pDevIns);
}

# endif /* !IN_RING3 */


/**
 * The device registration structure.
 */
static const PDMDEVREG g_DeviceMpu =
{
    /* .u32Version = */             PDM_DEVREG_VERSION,
    /* .uReserved0 = */             0,
    /* .szName = */                 "mpu401",
    /* .fFlags = */                 PDM_DEVREG_FLAGS_DEFAULT_BITS | PDM_DEVREG_FLAGS_NEW_STYLE,
    /* .fClass = */                 PDM_DEVREG_CLASS_AUDIO,
    /* .cMaxInstances = */          1,
    /* .uSharedVersion = */         42,
    /* .cbInstanceShared = */       sizeof(MPUSTATE),
    /* .cbInstanceCC = */           0,
    /* .cbInstanceRC = */           0,
    /* .cMaxPciDevices = */         0,
    /* .cMaxMsixVectors = */        0,
    /* .pszDescription = */         "MPU-401.",
# if defined(IN_RING3)
    /* .pszRCMod = */               "",
    /* .pszR0Mod = */               "",
    /* .pfnConstruct = */           mpuR3Construct,
    /* .pfnDestruct = */            mpuR3Destruct,
    /* .pfnRelocate = */            NULL,
    /* .pfnMemSetup = */            NULL,
    /* .pfnPowerOn = */             NULL,
    /* .pfnReset = */               mpuR3Reset,
    /* .pfnSuspend = */             NULL,
    /* .pfnResume = */              NULL,
    /* .pfnAttach = */              NULL,
    /* .pfnDetach = */              NULL,
    /* .pfnQueryInterface = */      NULL,
    /* .pfnInitComplete = */        NULL,
    /* .pfnPowerOff = */            NULL,
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

    return pCallbacks->pfnRegister(pCallbacks, &g_DeviceMpu);
}

# endif  /* !VBOX_IN_EXTPACK_R3 */

#endif /* !VBOX_DEVICE_STRUCT_TESTCASE */
