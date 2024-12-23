#include "WinLoad.h"

INLINE_HOOK WinLoadImageShitHook;
INLINE_HOOK WinLoadAllocateImageHook;

BOOLEAN HyperVloading = FALSE;
BOOLEAN InstalledHvLoaderHook = FALSE;

BOOLEAN ExtendedAllocation = FALSE;
BOOLEAN HookedHyperV = FALSE;
UINT64 AllocationCount = 0;

EFI_STATUS EFIAPI BlLdrLoadImage
(
	VOID* Arg1,
	CHAR16* ModulePath, 
	CHAR16* ModuleName,
	VOID* Arg4,
	VOID* Arg5, 
	VOID* Arg6, 
	VOID* Arg7, 
	PPLDR_DATA_TABLE_ENTRY lplpTableEntry,
	VOID* Arg9,
	VOID* Arg10,
	VOID* Arg11, 
	VOID* Arg12,
	VOID* Arg13,
	VOID* Arg14,
	VOID* Arg15,
	VOID* Arg16
)
{
	if (!StrCmp(ModuleName, (CHAR16*)L"hv.exe"))
		HyperVloading = TRUE;

	// disable shithook and call the original function...
	DisableInlineHook(&WinLoadImageShitHook);
	EFI_STATUS Result = ((LDR_LOAD_IMAGE)WinLoadImageShitHook.Address)
	(
		Arg1,
		ModulePath, 
		ModuleName, 
		(UINT64*)Arg4,
		(UINT32*)Arg5,
		Arg6,
		Arg7,
		lplpTableEntry,
		Arg9,
		Arg10, 
		Arg11,
		Arg12,
		Arg13,
		Arg14,
		Arg15, 
		Arg16
	);

	// continue hooking until we inject/hook into hyper-v...
	if (!HookedHyperV)
		EnableInlineHook(&WinLoadImageShitHook);

	if (!StrCmp(ModuleName, (CHAR16*)L"hv.exe"))
	{
		HookedHyperV = TRUE;
		SPUTNIK_T SputnikData;
		PLDR_DATA_TABLE_ENTRY TableEntry = *lplpTableEntry;

		// add a new section to hyper-v called "payload", then fill in sputnik data
		// and hook the vmexit handler...
		MakeSputnikData
		(
			&SputnikData, 
			(VOID*)TableEntry->ModuleBase,
			TableEntry->SizeOfImage, 
			AddSection
			(
				(VOID*)TableEntry->ModuleBase,
				(CHAR8*)"payload",
				PayLoadSize(),
				SECTION_RWX
			),
			PayLoadSize()
		);

		HookVmExit
		(
			(VOID*)SputnikData.HypervModuleBase,
			(VOID*)SputnikData.HypervModuleSize,
			MapModule(&SputnikData, (UINT8*)PayLoad)
		);

		// extend the size of the image in hyper-v's nt headers and LDR data entry...
		// this is required, if this is not done, then hyper-v will simply not be loaded...
		TableEntry->SizeOfImage = NT_HEADER(TableEntry->ModuleBase)->OptionalHeader.SizeOfImage;
	}
	return Result;
}

EFI_STATUS EFIAPI BlImgLoadPEImageEx
(
	VOID* a1,
	VOID* a2,
	CHAR16* ImagePath,
	UINT64* ImageBasePtr,
	UINT32* ImageSize,
	VOID* a6,
	VOID* a7,
	VOID* a8,
	VOID* a9, 
	VOID* a10,
	VOID* a11,
	VOID* a12,
	VOID* a13, 
	VOID* a14
)
{
	// disable shithook and call the original function...
	DisableInlineHook(&WinLoadImageShitHook);
	EFI_STATUS Result = ((LDR_LOAD_IMAGE)WinLoadImageShitHook.Address)
	(
		a1,
		a2, 
		ImagePath,
		ImageBasePtr,
		ImageSize,
		a6,
		a7, 
		a8,
		a9, 
		a10, 
		a11,
		a12, 
		a13, 
		a14,
		0,
		0
	);

	// continue hooking BlImgLoadPEImageEx until we have shithooked hvloader...
	if (!InstalledHvLoaderHook)
		EnableInlineHook(&WinLoadImageShitHook);

	if (StrStr(ImagePath, (CHAR16*)L"hvloader.efi"))
	{
		VOID* LoadImage =
			FindPattern(
				(VOID*)*ImageBasePtr,
				*ImageSize,
				(VOID*)HV_LOAD_PE_IMG_FROM_BUFFER_SIG,
				(VOID*)HV_LOAD_PE_IMG_FROM_BUFFER_MASK
			);
		VOID* AllocImage =
			FindPattern(
				(VOID*)*ImageBasePtr,
				*ImageSize,
				(VOID*)HV_ALLOCATE_IMAGE_BUFFER_SIG,
				(VOID*)HV_ALLOCATE_IMAGE_BUFFER_MASK
			);

		if(LoadImage)
			MakeInlineHook(&HvLoadImageBufferHook, (VOID*)RESOLVE_RVA(LoadImage, 5, 1), &HvBlImgLoadPEImageFromSourceBuffer, TRUE);

		MakeInlineHook(&HvLoadAllocImageHook, (VOID*)RESOLVE_RVA(AllocImage, 5, 1), &HvBlImgAllocateImageBuffer, TRUE);
		InstalledHvLoaderHook = TRUE;
	}
	return Result;
}

UINT64 EFIAPI BlImgAllocateImageBuffer
(
	VOID** imageBuffer,
	UINTN imageSize,
	UINT32 memoryType, 
	UINT32 attributes, 
	VOID* unused, 
	UINT32 Value
)
{
	//
	// The second allocation for hv.exe is used for the actual image... Wait for the second allocation before extending the allocation...
	// these allocations are not subject to change. its not a randomized or controlled order. It is what it is :|
	//
	// hv.exe
	// [BlImgAllocateImageBuffer] Alloc Base -> 0x7FFFF9FE000, Alloc Size -> 0x17C548
	// [BlImgAllocateImageBuffer] Alloc Base -> 0xFFFFF80608120000, Alloc Size -> 0x1600000
	// [BlImgAllocateImageBuffer] Alloc Base -> 0xFFFFF80606D68000, Alloc Size -> 0x2148
	// [BlLdrLoadImage] Image Base -> 0xFFFFF80608120000, Image Size -> 0x1600000
	//

	if (HyperVloading && !ExtendedAllocation && ++AllocationCount == 2)
	{
		ExtendedAllocation = TRUE;
		imageSize += PayLoadSize();
	
		// allocate the entire hyper-v module as rwx...
		memoryType = BL_MEMORY_ATTRIBUTE_RWX;
	}
	
	// disable shithook and call the original function...
	DisableInlineHook(&WinLoadAllocateImageHook);
	UINT64 Result = ((ALLOCATE_IMAGE_BUFFER)WinLoadAllocateImageHook.Address)
	(
		imageBuffer,
		imageSize,
		memoryType,
		attributes, 
		unused,
		Value
	);

	// keep hooking until we extend an allocation...
	if(!ExtendedAllocation)
		EnableInlineHook(&WinLoadAllocateImageHook);

	return Result;
}