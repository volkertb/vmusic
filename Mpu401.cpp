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
#define MPU_IO_SIZE                 2

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

    /* Current state. */
    /** Whether we have an input/result byte waiting to be read. */
    bool                   fHaveInput;
    /** Current input byte waiting to be read. */
    uint8_t                uInput;
    /** True if UART mode, false if regular/intelligent mode. */
    bool                   fModeUart;
    /** MIDI backend. */
    MIDIBackend            midi;

    IOMIOPORTHANDLE        hIoPorts;
} MPUSTATE;
/** Pointer to the shared device state.  */
typedef MPUSTATE *PMPUSTATE;

#ifndef VBOX_DEVICE_STRUCT_TESTCASE

static void mpuReset(PPDMDEVINS pDevIns)
{
    PMPUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    pThis->midi.reset();

    if (pThis->fModeUart) {
        Log(("Leaving UART mode"));
    }

    pThis->fModeUart = false;
    pThis->fHaveInput = false;
    pThis->uInput = 0;
}

static void mpuRespondData(PPDMDEVINS pDevIns, uint8_t data)
{
    PMPUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    Log3Func(("enqueing response=0x%x\n", data));

    pThis->fHaveInput = true;
    pThis->uInput = data;
}

static uint8_t mpuReadData(PPDMDEVINS pDevIns)
{
    PMPUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    if (pThis->fHaveInput) {
        pThis->fHaveInput = false;
        return pThis->uInput;
    }

    if (pThis->fModeUart) {
        uint8_t data;
        ssize_t read = pThis->midi.read(&data, 1);
        if (read == 1) {
            Log5Func(("midi_in data=0x%x\n", data));
            return data;
        }
    }

    LogWarnFunc(("Trying to read, but no data to read\n"));

    return MPU_RESPONSE_ACK;
}

static void mpuWriteData(PPDMDEVINS pDevIns, uint8_t data)
{
    PMPUSTATE pThis = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    if (pThis->fModeUart) {
        ssize_t written = pThis->midi.write(&data, 1);
        if (written == 1) {
            Log5Func(("midi_out data=0x%x\n", data));
        }
    } else {
        LogWarnFunc(("Ignoring data, not in UART mode\n"));
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

    bool outputReady = !pThis->fModeUart || pThis->midi.writeAvail() >= 1;
    if (!outputReady) {
        status |= RT_BIT(6);
    }

    bool inputReady = pThis->fHaveInput
            || (pThis->fModeUart && pThis->midi.readAvail() >= 1);
    if (!inputReady) {
        status |= RT_BIT(7);
    }

    Log5(("mpu status: outputReady=%RTbool inputReady=%RTbool\n", outputReady, inputReady));

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
                mpuRespondData(pDevIns, MPU_RESPONSE_ACK);
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
                Log(("Entering UART mode\n"));
                pThis->fModeUart = true;
                mpuRespondData(pDevIns, MPU_RESPONSE_ACK);
                break;
            default:
                LogWarnFunc(("Unknown command in normal mode: 0x%hx\n", cmd));
                mpuRespondData(pDevIns, MPU_RESPONSE_ACK);
                break;
        }
    }
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

    pHlp->pfnSSMPutBool(pSSM, pThis->fHaveInput);
    pHlp->pfnSSMPutU8  (pSSM, pThis->uInput);
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

    pHlp->pfnSSMGetBool(pSSM, &pThis->fHaveInput);
    pHlp->pfnSSMGetU8  (pSSM, &pThis->uInput);
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

    /*
     * Validate and read the configuration.
     */
    PDMDEV_VALIDATE_CONFIG_RETURN(pDevIns, "Port", "");

    rc = pHlp->pfnCFGMQueryPortDef(pCfg, "Port", &pThis->uPort, MPU_DEFAULT_IO_BASE);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc, N_("Failed to query \"Port\" from the config"));

    LogFlowFunc(("mpu401#%i: port 0x%x\n", iInstance, pThis->uPort));

    /*
     * Initialize the device state.
     */
    mpuReset(pDevIns);

    /*
     * Register I/O ports.
     */
    static const IOMIOPORTDESC s_aDescs[] =
    {
        { "Data", "Data", NULL, NULL }, // base + 00h
        { "Status", "Command",  NULL, NULL },  // base + 01h
        { NULL }
    };
    rc = PDMDevHlpIoPortCreateAndMap(pDevIns, pThis->uPort, MPU_IO_SIZE, mpuIoPortWrite, mpuIoPortRead,
                                     "MPU-401", s_aDescs, &pThis->hIoPorts);
    AssertRCReturn(rc, rc);

    /*
     * Register saved state.
     */
    rc = PDMDevHlpSSMRegister(pDevIns, MPU_SAVED_STATE_VERSION, sizeof(*pThis), mpuR3SaveExec, mpuR3LoadExec);
    AssertRCReturn(rc, rc);

    /* Open the MIDI device now. */
    rc = pThis->midi.open("default");
    AssertRCReturn(rc, rc);

    LogRel(("mpu401#%i: Configured on port 0x%x-0x%x\n", iInstance, pThis->uPort, pThis->uPort + MPU_IO_SIZE - 1));

    return VINF_SUCCESS;
}

/**
 * @interface_method_impl{PDMDEVREG,pfnDestruct}
 */
static DECLCALLBACK(int) mpuR3Destruct(PPDMDEVINS pDevIns)
{
    PMPUSTATE     pThis   = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    int rc = pThis->midi.close();
    AssertRCReturn(rc, rc);

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

/**
 * @interface_method_impl{PDMDEVREG,pfnPowerOff}
 */
static DECLCALLBACK(void) mpuR3PowerOff(PPDMDEVINS pDevIns)
{
    PMPUSTATE     pThis   = PDMDEVINS_2_DATA(pDevIns, PMPUSTATE);

    int rc = pThis->midi.close();
    AssertRC(rc);
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
    /* .pfnPowerOff = */            mpuR3PowerOff,
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
