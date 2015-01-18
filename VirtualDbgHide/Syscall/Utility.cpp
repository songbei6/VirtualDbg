#include "stdafx.h"

ULONG_PTR GetNtoskrnlBase()
{
#define IMAGE_DOS_SIGNATURE 0x5a4d

	//
	// Scan down from a given symbol�s address.
	// Align to PAGE_SIZE first.
	//
	ULONG_PTR addr	= (ULONG_PTR)&MmGetSystemRoutineAddress;
	addr			= (addr & ~0xfff);

	__try
	{
		while ((*(USHORT *)addr != IMAGE_DOS_SIGNATURE))
			addr -= PAGE_SIZE;

		return addr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}

	return 0;
}

ULONG_PTR GetSSDTBase()
{
	//
	// The SSDT is found by using the pointer located inside of
	// KeAddSystemServiceTable, which is exported by NTOSKRNL.
	//
	UNICODE_STRING routineName;
	RtlInitUnicodeString(&routineName, L"KeAddSystemServiceTable");

	PVOID KeAddSystemServiceTable = MmGetSystemRoutineAddress(&routineName);

	if (!KeAddSystemServiceTable)
		return 0;

	//
	// Get a copy of the function's code
	//
	UCHAR functionData[1024];
	ULONG functionSize = 0;
	RtlCopyMemory(functionData, KeAddSystemServiceTable, sizeof(functionData));

	for (ULONG i = 0; i < sizeof(functionData); i++)
	{
		if (functionData[i] == 0xC3)
		{
			functionSize = i + 1;
			break;
		}
	}

	//
	// Will fail if 0xC3 (RETN) is never found
	//
	if (functionSize <= 0)
		return 0;

	//
	// Determine the SSDT RVA with a byte scan
	//
	ULONG rva = 0;

	for (ULONG i = 0; i < functionSize; i++)
	{
		//
		// 48 83 BC 18 80 4A 35 00 00       cmp qword ptr [rax+rbx+354A80h], 0
		//
		if (memcmp(&functionData[i], "\x48\x83\xBC", 3) == 0)
		{
			//
			// Verify the zero
			//
			if (functionData[i + 8] == 0x00)
			{
				rva = *(ULONG *)&functionData[i + 4];
				break;
			}
		}
	}

	//
	// NtosnkrlBase + RVA = SSDT address
	//
	ULONG_PTR ssdtAddress = NtKernelBase + rva;

	//
	// Also check validity
	//
	if (!MmIsAddressValid((PVOID)ssdtAddress))
		return 0;

	return ssdtAddress;
}

ULONG_PTR GetSSDTEntry(ULONG TableIndex)
{
	PKSERVICE_TABLE_DESCRIPTOR ssdt = (PKSERVICE_TABLE_DESCRIPTOR)NtKernelSSDT;

#ifdef _WIN64
	// SSDT pointers are relative to the base in X64
	ULONG_PTR entry = (ULONG_PTR)ssdt->ServiceTable + ((ULONG_PTR)ssdt->ServiceTable[TableIndex] >> 4);
#else
	// Otherwise it's 32-bit and a direct pointer
	ULONG_PTR entry = (ULONG_PTR)ssdt->ServiceTable[TableIndex];
#endif

	//
	// Verify address
	//
	if (!MmIsAddressValid((PVOID)entry))
	{
		DbgPrint("FAILED INDEX IN GetSSDTEntry: 0x%X - 0x%p\n", TableIndex, entry);
		return 0;
	}

	return entry;
}

NTSTATUS RemoveProcessFromSysProcessInfo(PVOID SystemInformation, ULONG SystemInformationLength)
{
	//
	// Subtract the size of the base container
	//
	if (SystemInformationLength < sizeof(SYSTEM_PROCESS_INFORMATION))
		return STATUS_INFO_LENGTH_MISMATCH;

	//
	// Get a pointer to the modules and loop each index
	//
	PSYSTEM_PROCESS_INFORMATION moduleInfo = (PSYSTEM_PROCESS_INFORMATION)SystemInformation;

	PSYSTEM_PROCESS_INFORMATION prevPointer = NULL;
	PSYSTEM_PROCESS_INFORMATION currPointer = NULL;
	PSYSTEM_PROCESS_INFORMATION nextPointer = NULL;

	for (BOOLEAN breakIteration = FALSE;;)
	{
		//
		// Does this process match?
		//
		if (moduleInfo->ProcessId == (HANDLE)0xDEADBEEF)
		{
			currPointer		= moduleInfo;
			breakIteration	= TRUE;
		}

		//
		// Validate pointer
		//
		if (moduleInfo->NextEntryOffset == 0)
			break;

		ULONG_PTR nextIndex = (ULONG_PTR)moduleInfo + moduleInfo->NextEntryOffset;

		if (nextIndex >= ((ULONG_PTR)SystemInformation + SystemInformationLength))
			break;

		//
		// If this flag was set, get the next pointer in the list and exit
		//
		if (breakIteration)
		{
			nextPointer = (PSYSTEM_PROCESS_INFORMATION)nextIndex;
			break;
		}

		//
		// Move to next index
		//
		prevPointer = moduleInfo;
		moduleInfo	= (PSYSTEM_PROCESS_INFORMATION)nextIndex;
	}

	if (!currPointer)
		return STATUS_NOT_FOUND;

	//
	// Was there a previous pointer?
	//
	if (prevPointer)
	{
		//
		// Link it to the next, or set it to 0
		//
		if (nextPointer)
			prevPointer->NextEntryOffset = (ULONG)((ULONG_PTR)nextPointer - (ULONG_PTR)moduleInfo);
		else
			prevPointer->NextEntryOffset = 0;
	}

	//
	// Calculate the size of the target entry and zero it
	//
	SIZE_T zeroLength = 0;

	if (nextPointer)
	{
		//
		// There was another entry after this, so determine
		// the delta between them
		//
		zeroLength = (ULONG_PTR)currPointer - (ULONG_PTR)nextPointer;
	}
	else
	{
		//
		// Data is from 'currPointer' to SystemInformation buffer end
		//
		zeroLength = (ULONG_PTR)currPointer - ((ULONG_PTR)SystemInformation + SystemInformationLength);
	}

	RtlSecureZeroMemory(currPointer, zeroLength);
	return STATUS_SUCCESS;
}

NTSTATUS RemoveDriverFromSysModuleInfo(PVOID SystemInformation, ULONG SystemInformationLength, PULONG OutLength)
{
	//
	// Subtract the size of the base container
	//
	if (SystemInformationLength <= sizeof(SYSTEM_MODULE_INFORMATION))
		return STATUS_INFO_LENGTH_MISMATCH;

	SystemInformationLength -= sizeof(SYSTEM_MODULE_INFORMATION);

	//
	// Determine the SYSTEM_MODULE count
	//
	ULONG entryCount = SystemInformationLength / sizeof(SYSTEM_MODULE);

	//
	// Get a pointer to the modules and loop each index
	//
	PSYSTEM_MODULE_INFORMATION moduleInfo = (PSYSTEM_MODULE_INFORMATION)SystemInformation;

	PSYSTEM_MODULE startPointer = NULL;
	PSYSTEM_MODULE copyPointer	= NULL;
	ULONG remainderBytes		= 0;

	for (ULONG i = 0; i < entryCount; i++)
	{
		if (moduleInfo->Modules[i].ImageBase == (PVOID)0xfffff80010834000) // FIXME
		{
			startPointer	= &moduleInfo->Modules[i];
			copyPointer		= &moduleInfo->Modules[i + 1];
			remainderBytes	= (entryCount - (i + 1)) * sizeof(SYSTEM_MODULE);

			break;
		}
	}

	if (!startPointer || !copyPointer)
		return STATUS_NOT_FOUND;

	//
	// Overwrite the data for this driver and fix up variables
	//
	ULONG modifiedLength = (SystemInformationLength - sizeof(SYSTEM_MODULE));

	if (remainderBytes > 0)
		RtlMoveMemory(startPointer, copyPointer, remainderBytes);

	//
	// Zero the end to prevent leaking any data
	//
	RtlZeroMemory((PUCHAR)SystemInformation + modifiedLength, sizeof(SYSTEM_MODULE));

	//
	// Fix the output parameter and internal struct counter
	//
	if (OutLength)
		*OutLength = modifiedLength;

	moduleInfo->ModulesCount -= 1;
	return STATUS_SUCCESS;
}

NTSTATUS RemoveDebugObjectInfo(PVOID ObjectInformation, ULONG ObjectInformationLength)
{
	//
	// Validate the size of the base container
	//
	if (ObjectInformationLength < (sizeof(OBJECT_ALL_TYPES_INFORMATION) + sizeof(OBJECT_TYPE_INFORMATION)))
		return STATUS_INFO_LENGTH_MISMATCH;

	//
	// Enumerate all entries
	//
	POBJECT_ALL_TYPES_INFORMATION typesInfo = (POBJECT_ALL_TYPES_INFORMATION)ObjectInformation;
	POBJECT_TYPE_INFORMATION typeInfo		= typesInfo->TypeInformation;

	for (ULONG i = 0; i < typesInfo->NumberOfTypes; i++)
	{
		//
		// Should this entry be faked?
		//
		if (NT_SUCCESS(RemoveSingleDebugObjectInfo(typeInfo)))
			return STATUS_SUCCESS;

		//
		// Validate the pointer first (Aligned to 0x4)
		//
		ULONG_PTR nextType = ((ULONG_PTR)typeInfo->Name.Buffer + typeInfo->Name.Length) & 0xFFFFFFFC;

		if (nextType >= ((ULONG_PTR)ObjectInformation + ObjectInformationLength))
			break;

		//
		// Increment the loop
		//
		typeInfo = (POBJECT_TYPE_INFORMATION)nextType;
	}

	return STATUS_NOT_FOUND;
}

NTSTATUS RemoveSingleDebugObjectInfo(OBJECT_TYPE_INFORMATION *Information)
{
	//
	// Does the object type information name match "DebugObject"?
	//
	if (RtlEqualUnicodeString(&Information->Name, &Nt::DebugObjectName, FALSE))
	{
		Information->TotalNumberOfObjects = 0;
		Information->TotalNumberOfHandles = 0;

		return STATUS_SUCCESS;
	}

	return STATUS_NOT_FOUND;
}