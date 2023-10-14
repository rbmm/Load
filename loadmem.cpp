#include "stdafx.h"

_NT_BEGIN

BOOLEAN IsImageOk(_In_ ULONG SizeOfImage, _In_ HANDLE hSection)
{
	BOOLEAN fOk = FALSE;

	SIZE_T ViewSize = 0;
	union {
		PVOID BaseAddress = 0;
		PIMAGE_DOS_HEADER pidh;
	};

	if (0 <= ZwMapViewOfSection(hSection, NtCurrentProcess(), &BaseAddress, 0, 0, 0, 
		&ViewSize, ViewUnmap, 0, PAGE_READONLY))
	{
		if (ViewSize >= SizeOfImage && pidh->e_magic == IMAGE_DOS_SIGNATURE)
		{
			ULONG VirtualAddress = pidh->e_lfanew;

			if (VirtualAddress < ViewSize - sizeof(IMAGE_NT_HEADERS))
			{
				union {
					PVOID pv;
					PIMAGE_NT_HEADERS pinth;
					PIMAGE_LOAD_CONFIG_DIRECTORY picd;
				};

				pv = RtlOffsetToPointer(BaseAddress, VirtualAddress);

				if (pinth->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR_MAGIC && 
					pinth->OptionalHeader.SizeOfImage >= SizeOfImage)
				{
					IMAGE_DATA_DIRECTORY DataDirectory = pinth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];

					if (DataDirectory.Size < __builtin_offsetof(IMAGE_LOAD_CONFIG_DIRECTORY, GuardFlags))
					{
						fOk = TRUE;
					}
					else
					{
						if (DataDirectory.VirtualAddress < ViewSize - sizeof(IMAGE_LOAD_CONFIG_DIRECTORY))
						{
							pv = RtlOffsetToPointer(BaseAddress, DataDirectory.VirtualAddress);

							fOk = picd->Size < __builtin_offsetof(IMAGE_LOAD_CONFIG_DIRECTORY, GuardFlags) || 
								!picd->GuardCFFunctionCount;
						}
					}
				}
			}
		}

		ZwUnmapViewOfSection(NtCurrentProcess(), BaseAddress);
	}

	return fOk;
}

NTSTATUS FindNoCfgDll(_In_ ULONG SizeOfImage, _Out_ PUNICODE_STRING FileName)
{
	HANDLE hFile;
	IO_STATUS_BLOCK iosb;
	UNICODE_STRING ObjectName;
	OBJECT_ATTRIBUTES oa = { sizeof(oa), 0, &ObjectName, OBJ_CASE_INSENSITIVE };
	RtlInitUnicodeString(&ObjectName, L"\\systemroot\\system32");

	NTSTATUS status = NtOpenFile(&oa.RootDirectory, 
		FILE_LIST_DIRECTORY|SYNCHRONIZE, &oa, &iosb, FILE_SHARE_VALID_FLAGS, 
		FILE_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT);

	if (0 <= status)
	{
		status = STATUS_NO_MEMORY;

		enum { buf_size = 0x10000 };

		if (PVOID buf = LocalAlloc(0, buf_size))
		{
			static const UNICODE_STRING DLL = RTL_CONSTANT_STRING(L"*.dll");

			while (0 <= (status = NtQueryDirectoryFile(oa.RootDirectory, 
				0, 0, 0, &iosb, buf, buf_size, FileDirectoryInformation,
				FALSE, const_cast<PUNICODE_STRING>(&DLL), FALSE)))
			{
				union {
					PVOID pv;
					PUCHAR pc;
					PFILE_DIRECTORY_INFORMATION pfdi;
				};

				pv = buf;

				ULONG NextEntryOffset = 0;

				do 
				{
					pc += NextEntryOffset;

					if (pfdi->EndOfFile.QuadPart >= SizeOfImage)
					{
						ObjectName.Buffer = pfdi->FileName;
						ObjectName.MaximumLength = ObjectName.Length = (USHORT)pfdi->FileNameLength;

						HMODULE hmod;
						if (STATUS_DLL_NOT_FOUND != LdrGetDllHandle(0, 0, &ObjectName, &hmod))
						{
							// dll with such name already loaded
							continue;
						}

						if (0 <= NtOpenFile(&hFile, FILE_READ_DATA|SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ, 
							FILE_NON_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT))
						{
							BOOLEAN fOk = FALSE;

							HANDLE hSection;

							if (0 <= NtCreateSection(&hSection, SECTION_MAP_READ, 0, 0, PAGE_READONLY, SEC_IMAGE_NO_EXECUTE, hFile))
							{
								fOk = IsImageOk(SizeOfImage, hSection);

								NtClose(hSection);
							}

							NtClose(hFile);

							if (0 <= status)
							{
								if (fOk)
								{
									//DbgPrint("%I64x %wZ\n", pfdi->EndOfFile.QuadPart, &ObjectName);
									status = RtlAppendUnicodeStringToString(FileName, &ObjectName);

									goto __exit;
								}
							}
						}
					}

				} while (NextEntryOffset = pfdi->NextEntryOffset);
			}
__exit:

			LocalFree(buf);
		}
		NtClose(oa.RootDirectory);
	}

	return status;
}

struct IMAGE_Ctx : public TEB_ACTIVE_FRAME
{
	inline static const char FrameName[] = "{0A0659E5-2962-480a-9A6F-01A02C5C043B}";

	PIMAGE_NT_HEADERS _M_pinth;
	PVOID _M_retAddr = 0, _M_pvImage, *_M_pBaseAddress = 0;
	PCUNICODE_STRING _M_lpFileName;
	NTSTATUS _M_status = STATUS_UNSUCCESSFUL;

	IMAGE_Ctx(PVOID pvImage, PIMAGE_NT_HEADERS pinth, PCUNICODE_STRING lpFileName) 
		: _M_pvImage(pvImage), _M_pinth(pinth), _M_lpFileName(lpFileName)
	{
		const static TEB_ACTIVE_FRAME_CONTEXT FrameContext = { 0, FrameName };
		Context = &FrameContext;
		Flags = 0;
		RtlPushFrame(this);
	}

	~IMAGE_Ctx()
	{
		RtlPopFrame(this);
	}

	static IMAGE_Ctx* get()
	{
		if (TEB_ACTIVE_FRAME * frame = RtlGetFrame())
		{
			do 
			{
				if (frame->Context->FrameName == FrameName)
				{
					return static_cast<IMAGE_Ctx*>(frame);
				}
			} while (frame = frame->Previous);
		}

		return 0;
	}
};

NTSTATUS OverwriteSection(_In_ PVOID BaseAddress, _In_ PVOID pvImage, _In_ PIMAGE_NT_HEADERS pinth)
{
	ULONG op, cb = pinth->OptionalHeader.SizeOfHeaders, VirtualSize, SizeOfRawData;
	PVOID pv = BaseAddress, VirtualAddress;
	SIZE_T ProtectSize = cb;

	NTSTATUS status;
	if (0 > (status = ZwProtectVirtualMemory(NtCurrentProcess(), &pv, &ProtectSize, PAGE_READWRITE, &op)))
	{
		return status;
	}

	memcpy(BaseAddress, pvImage, cb);

	ZwProtectVirtualMemory(NtCurrentProcess(), &pv, &ProtectSize, PAGE_READONLY, &op);

	if (ULONG NumberOfSections = pinth->FileHeader.NumberOfSections)
	{
		PIMAGE_SECTION_HEADER pish = IMAGE_FIRST_SECTION(pinth);

		do 
		{
			if (VirtualSize = pish->Misc.VirtualSize)
			{
				VirtualAddress = RtlOffsetToPointer(BaseAddress, pish->VirtualAddress);

				ULONG Characteristics = pish->Characteristics;

				if (0 > (status = ZwProtectVirtualMemory(NtCurrentProcess(), &(pv = VirtualAddress), 
					&(ProtectSize = VirtualSize), 
					Characteristics & IMAGE_SCN_MEM_EXECUTE ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &op)))
				{
					return status;
				}

				if (cb = min(VirtualSize, SizeOfRawData = pish->SizeOfRawData))
				{
					memcpy(VirtualAddress, RtlOffsetToPointer(pvImage, pish->PointerToRawData), cb);
				}

				if (SizeOfRawData < VirtualSize)
				{
					RtlZeroMemory(RtlOffsetToPointer(VirtualAddress, cb), VirtualSize - SizeOfRawData);
				}

				if (!(Characteristics & IMAGE_SCN_MEM_WRITE))
				{
					if (0 > (status = ZwProtectVirtualMemory(NtCurrentProcess(), &pv, &ProtectSize, 
						Characteristics & IMAGE_SCN_MEM_EXECUTE ? PAGE_EXECUTE_READ : PAGE_READONLY, &op)))
					{
						return status;
					}
				}
			}

		} while (pish++, --NumberOfSections);
	}

	return STATUS_SUCCESS;
}

//#define _PRINT_CPP_NAMES_
#include "../inc/asmfunc.h"

NTSTATUS __fastcall retFromMapViewOfSection(NTSTATUS status)
{
	CPP_FUNCTION;

	if (IMAGE_Ctx* ctx = IMAGE_Ctx::get())
	{
		*(void**)_AddressOfReturnAddress() = ctx->_M_retAddr;

		if (0 <= status)
		{
			PVOID BaseAddress = *ctx->_M_pBaseAddress;

			if (0 <= (status = OverwriteSection(BaseAddress, ctx->_M_pvImage, ctx->_M_pinth)))
			{
				if (BaseAddress != (PVOID)ctx->_M_pinth->OptionalHeader.ImageBase)
				{
					status = STATUS_IMAGE_NOT_AT_BASE;
				}
			}

			if (0 > status)
			{
				ZwUnmapViewOfSection(NtCurrentProcess(), BaseAddress);

				*ctx->_M_pBaseAddress = 0;
			}
		}

		ctx->_M_status = status;
	}

	return status;
}

NTSTATUS aretFromMapViewOfSection()ASM_FUNCTION;

LONG NTAPI MyVexHandler(::PEXCEPTION_POINTERS ExceptionInfo)
{
	::PEXCEPTION_RECORD ExceptionRecord = ExceptionInfo->ExceptionRecord;
	::PCONTEXT ContextRecord = ExceptionInfo->ContextRecord;
	
	if (ExceptionRecord->ExceptionCode == STATUS_SINGLE_STEP && ExceptionRecord->ExceptionAddress == ZwMapViewOfSection)
	{
		if (IMAGE_Ctx* ctx = IMAGE_Ctx::get())
		{
			UNICODE_STRING ObjectName;
			RtlInitUnicodeString(&ObjectName, (PCWSTR)reinterpret_cast<PNT_TIB>(NtCurrentTeb())->ArbitraryUserPointer);
			if (RtlEqualUnicodeString(&ObjectName, ctx->_M_lpFileName, FALSE))
			{
				ctx->_M_pBaseAddress =
#ifdef _WIN64
					(void**)ContextRecord->R8;

#define SP Rsp
#else
#define SP Esp
					((void***)ContextRecord->Esp)[3];
#endif

				*(PSIZE_T)((void**)ContextRecord->SP)[7] = ctx->_M_pinth->OptionalHeader.SizeOfImage;

				ctx->_M_retAddr = ((void**)ContextRecord->SP)[0];

				((void**)ContextRecord->SP)[0] = aretFromMapViewOfSection;
			}
		}

		ContextRecord->EFlags |= 0x10000;

		return EXCEPTION_CONTINUE_EXECUTION;
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

NTSTATUS LoadLibraryFromMem(_Out_ HMODULE* phmod, _In_ PVOID pvImage, _In_ PIMAGE_NT_HEADERS pinth, _In_ PCUNICODE_STRING lpFileName)
{
	if (PVOID VectoredHandlerHandle = RtlAddVectoredExceptionHandler(TRUE, MyVexHandler))
	{
		CONTEXT ctx = {};
		ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
		ctx.Dr3 = (ULONG_PTR)ZwMapViewOfSection;
		ctx.Dr7 = 0x440;

		NTSTATUS status;
		if (0 <= (status = ZwSetContextThread(NtCurrentThread(), &ctx)))
		{
			IMAGE_Ctx ictx(pvImage, pinth, lpFileName);

			status = LdrLoadDll(0, 0, lpFileName, phmod);

			ctx.Dr3 = 0;
			ctx.Dr7 = 0x400;
			ZwSetContextThread(NtCurrentThread(), &ctx);

			if (0 <= status && 0 > ictx._M_status)
			{
				LdrUnloadDll(*phmod);
			}

			status = ictx._M_status;
		}

		RtlRemoveVectoredExceptionHandler(VectoredHandlerHandle);

		return status;
	}

	return STATUS_UNSUCCESSFUL;
}

NTSTATUS LoadLibraryFromMem(_Out_ HMODULE* phmod, _In_ PVOID pvImage)
{
	PIMAGE_NT_HEADERS pinth = RtlImageNtHeader(pvImage);

	if (!pinth)
	{
		return STATUS_INVALID_IMAGE_FORMAT;
	}

	WCHAR FileName[0x180];
	UNICODE_STRING ObjectName = { 0, sizeof(FileName), FileName };
	
	NTSTATUS status = RtlAppendUnicodeToString(&ObjectName, L"\\\\?\\Global\\GLOBALROOT\\SystemRoot\\system32\\");
	if (0 <= status && 0 <= (status = FindNoCfgDll(pinth->OptionalHeader.SizeOfImage, &ObjectName)))
	{
		status = LoadLibraryFromMem(phmod, pvImage, pinth, &ObjectName);
	}

	return status;
}

_NT_END