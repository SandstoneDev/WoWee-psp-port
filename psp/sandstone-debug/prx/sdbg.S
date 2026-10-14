	.set noreorder

#include "pspstub.s"

	STUB_START "sdbg",0x40090000,0x00050005
	STUB_FUNC  0x6402CBE0,psrcCrashInstall
	STUB_FUNC  0xD937F9E6,psrcCrashArmed
	STUB_FUNC  0x43B20344,psrcCrashSelfTest
	STUB_FUNC  0x42D13907,psrcCrashKernelFault
	STUB_FUNC  0x02AB646E,psrcCrashUninstall
	STUB_END
