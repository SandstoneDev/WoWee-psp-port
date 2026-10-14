/* exmrel.S -- import stub for sceKernelReleaseExceptionHandler.
 *
 * In libpspkernel.a of the new SDK, ExceptionManagerForKernel has three functions
 * (RegisterExceptionHandler 0x3FB264FC, RegisterPriorityExceptionHandler
 * 0x5A837AD4, RegisterDefaultExceptionHandler 0x565C0B0E) -- no release function.
 * NID 0x1AA6CFFA is taken from ARK-5 (SystemControl/src/nid_660_data.c:273,
 * pair 6.60 -> 6.61 0x54B0CAD2; ARK translates it itself, like the three neighbours).
 * The format is exactly what psp-build-exports --build-stubs generates
 * (sample: sdk/samples/prx/prx_loader/MyLib.S); the flags 0x00090000 and
 * the counter 0x00010005 are copied from the ExceptionManagerForKernel_0000.o block
 * of the SDK itself (psp-objdump -s -j .lib.stub). A second .lib.stub block for the
 * same library is legal for the loader: each block is resolved separately.
 *
 * WHY. Hardware run 432: without the catcher the exit to XMB is clean, with the
 * catcher -- a red stripe and the console goes dark inside sceKernelExitGame: the
 * kernel unloads our module, but the twelve priority handlers stay in the
 * ExceptionManager lists as pointers into freed code. module_stop now removes
 * them with this function, and the game unloads the module before exiting. */
	.set noreorder

#include "pspstub.s"

	/* v31: the "default" handler -- get the current one, release our own. NIDs from
	   the same ARK table (nid_660_data.c:274-275: 0xDF83875E -> 0x27E6A663,
	   0x291FF031 -> 0x409DBB53). Counter 0x0003xxxx -- three functions. */
	STUB_START "ExceptionManagerForKernel",0x00090000,0x00030005
	STUB_FUNC  0x1AA6CFFA,sceKernelReleaseExceptionHandler
	STUB_FUNC  0xDF83875E,sceKernelGetActiveDefaultExceptionHandler
	STUB_FUNC  0x291FF031,sceKernelReleaseDefaultExceptionHandler
	STUB_END
