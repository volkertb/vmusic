/*
 * VirtualBox ExtensionPack Skeleton
 * Copyright (C) 2010-2020 Oracle Corporation
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


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include <VBox/ExtPack/ExtPack.h>

#include <VBox/err.h>
#include <VBox/version.h>
#include <VBox/vmm/cfgm.h>
#include <iprt/string.h>
#include <iprt/param.h>
#include <iprt/path.h>
#include <iprt/log.h>

/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
/** Pointer to the extension pack helpers. */
static PCVBOXEXTPACKHLP g_pHlp;


// /**
//  * @interface_method_impl{VBOXEXTPACKVMREG,pfnConsoleReady}
//  */
// static DECLCALLBACK(void)  vMusicExtPackVM_ConsoleReady(PCVBOXEXTPACKVMREG pThis, VBOXEXTPACK_IF_CS(IConsole) *pConsole);
//
// /**
//  * @interface_method_impl{VBOXEXTPACKVMREG,pfnUnload}
//  */
// static DECLCALLBACK(void) vMusicExtPackVM_Unload(PCVBOXEXTPACKVMREG pThis);

/**
 * @interface_method_impl{VBOXEXTPACKVMREG,pfnVMConfigureVMM}
 */
static DECLCALLBACK(int)  vMusicExtPackVM_VMConfigureVMM(PCVBOXEXTPACKVMREG pThis, VBOXEXTPACK_IF_CS(IConsole) *pConsole, PVM pVM)
{
    RT_NOREF(pThis, pConsole);

    PCFGMNODE pCfgRoot = CFGMR3GetRoot(pVM);
    AssertReturn(pCfgRoot, VERR_INTERNAL_ERROR_3);

    // Assume /PDM/Devices exists.
    PCFGMNODE pCfgDevices = CFGMR3GetChild(pCfgRoot, "PDM/Devices");
    AssertReturn(pCfgDevices, VERR_INTERNAL_ERROR_3);

    // Find the Adlib module and tell PDM to load it.
    char szPath[RTPATH_MAX];
    int rc = g_pHlp->pfnFindModule(g_pHlp, "AdlibR3", NULL, VBOXEXTPACKMODKIND_R3, szPath, sizeof(szPath), NULL);
    if (RT_FAILURE(rc))
        return rc;

    PCFGMNODE pCfgMine;
    rc = CFGMR3InsertNode(pCfgDevices, "Adlib", &pCfgMine);
    AssertRCReturn(rc, rc);
    rc = CFGMR3InsertString(pCfgMine, "Path", szPath);
    AssertRCReturn(rc, rc);

    // Likewise for MPU-401 module
    rc = g_pHlp->pfnFindModule(g_pHlp, "Mpu401R3", NULL, VBOXEXTPACKMODKIND_R3, szPath, sizeof(szPath), NULL);
    if (RT_FAILURE(rc))
        return rc;

    rc = CFGMR3InsertNode(pCfgDevices, "Mpu401", &pCfgMine);
    AssertRCReturn(rc, rc);
    rc = CFGMR3InsertString(pCfgMine, "Path", szPath);
    AssertRCReturn(rc, rc);

    // Likewise for Emu8000 module
    rc = g_pHlp->pfnFindModule(g_pHlp, "Emu8000R3", NULL, VBOXEXTPACKMODKIND_R3, szPath, sizeof(szPath), NULL);
    if (RT_FAILURE(rc))
        return rc;

    rc = CFGMR3InsertNode(pCfgDevices, "Emu8000", &pCfgMine);
    AssertRCReturn(rc, rc);
    rc = CFGMR3InsertString(pCfgMine, "Path", szPath);
    AssertRCReturn(rc, rc);


    return VINF_SUCCESS;
}

//
// /**
//  * @interface_method_impl{VBOXEXTPACKVMREG,pfnVMPowerOn}
//  */
// static DECLCALLBACK(int)  vMusicExtPackVM_VMPowerOn(PCVBOXEXTPACKVMREG pThis, VBOXEXTPACK_IF_CS(IConsole) *pConsole, PVM pVM);
//
// /**
//  * @interface_method_impl{VBOXEXTPACKVMREG,pfnVMPowerOff}
//  */
// static DECLCALLBACK(void) vMusicExtPackVM_VMPowerOff(PCVBOXEXTPACKVMREG pThis, VBOXEXTPACK_IF_CS(IConsole) *pConsole, PVM pVM);
//
// /**
//  * @interface_method_impl{VBOXEXTPACKVMREG,pfnQueryObject}
//  */
// static DECLCALLBACK(void) vMusicExtPackVM_QueryObject(PCVBOXEXTPACKVMREG pThis, PCRTUUID pObjectId);


static const VBOXEXTPACKVMREG g_vMusicExtPackVMReg =
{
    VBOXEXTPACKVMREG_VERSION,
    /* .uVBoxFullVersion =  */  VBOX_FULL_VERSION,
    /* .pfnConsoleReady =   */  NULL,
    /* .pfnUnload =         */  NULL,
    /* .pfnVMConfigureVMM = */  vMusicExtPackVM_VMConfigureVMM,
    /* .pfnVMPowerOn =      */  NULL,
    /* .pfnVMPowerOff =     */  NULL,
    /* .pfnQueryObject =    */  NULL,
    /* .pfnReserved1 =      */  NULL,
    /* .pfnReserved2 =      */  NULL,
    /* .pfnReserved3 =      */  NULL,
    /* .pfnReserved4 =      */  NULL,
    /* .pfnReserved5 =      */  NULL,
    /* .pfnReserved6 =      */  NULL,
    /* .uReserved7 =        */  0,
    VBOXEXTPACKVMREG_VERSION
};


/** @callback_method_impl{FNVBOXEXTPACKVMREGISTER}  */
extern "C" DECLEXPORT(int) VBoxExtPackVMRegister(PCVBOXEXTPACKHLP pHlp, PCVBOXEXTPACKVMREG *ppReg, PRTERRINFO pErrInfo)
{
    /*
     * Check the VirtualBox version.
     */
    if (!VBOXEXTPACK_IS_VER_COMPAT(pHlp->u32Version, VBOXEXTPACKHLP_VERSION))
        return RTErrInfoSetF(pErrInfo, VERR_VERSION_MISMATCH,
                             "Helper version mismatch - expected %#x got %#x",
                             VBOXEXTPACKHLP_VERSION, pHlp->u32Version);
    if (   VBOX_FULL_VERSION_GET_MAJOR(pHlp->uVBoxFullVersion) != VBOX_VERSION_MAJOR
        || VBOX_FULL_VERSION_GET_MINOR(pHlp->uVBoxFullVersion) != VBOX_VERSION_MINOR)
        return RTErrInfoSetF(pErrInfo, VERR_VERSION_MISMATCH,
                             "VirtualBox version mismatch - expected %u.%u got %u.%u",
                             VBOX_VERSION_MAJOR, VBOX_VERSION_MINOR,
                             VBOX_FULL_VERSION_GET_MAJOR(pHlp->uVBoxFullVersion),
                             VBOX_FULL_VERSION_GET_MINOR(pHlp->uVBoxFullVersion));

    /*
     * We're good, save input and return the registration structure.
     */
    g_pHlp = pHlp;
    *ppReg = &g_vMusicExtPackVMReg;

    return VINF_SUCCESS;
}

