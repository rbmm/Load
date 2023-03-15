#include "stdafx.h"

_NT_BEGIN

NTSTATUS LoadLibraryFromMem(_Out_ HMODULE* phmod, _In_ PVOID pvImage);

EXTERN_C
NTSYSCALLAPI
NTSTATUS
NTAPI
NtAlertThreadByThreadId(
						_In_ HANDLE ThreadId
						);

EXTERN_C
NTSYSCALLAPI
NTSTATUS
NTAPI
NtWaitForAlertByThreadId(
						 _In_ PVOID Address,
						 _In_opt_ PLARGE_INTEGER Timeout
						 );

extern volatile const UCHAR guz = 0;

struct WaitData 
{
	HANDLE _M_hKey = 0, _M_hEvent = 0;
	HMODULE _M_hmod = 0;
	ULONG _M_hash = 0;
	ULONG _M_dwThreadId = 0;

	BOOL StartNotify()
	{
		IO_STATUS_BLOCK iosb;
		return 0 <= ZwNotifyChangeKey(_M_hKey, _M_hEvent, 0, 0, &iosb, 
			REG_NOTIFY_CHANGE_LAST_SET|REG_NOTIFY_THREAD_AGNOSTIC, FALSE, 0, 0, TRUE);
	}

	PTP_WAIT Create(POBJECT_ATTRIBUTES poa)
	{
		if (0 <= ZwOpenKey(&_M_hKey, KEY_NOTIFY|KEY_QUERY_VALUE, poa))
		{
			if (_M_hEvent = CreateEvent(0, FALSE, FALSE, 0))
			{
				if (0 <= LdrAddRefDll(0, (HMODULE)&__ImageBase))
				{
					if (PTP_WAIT pwa = CreateThreadpoolWait(_S_WaitCallback, this, 0))
					{
						WaitCallback(0, pwa);
						return pwa;
					}

					LdrUnloadDll((HMODULE)&__ImageBase);
				}

				NtClose(_M_hEvent);
				_M_hEvent = 0;
			}

			NtClose(_M_hKey);
			_M_hKey = 0;
		}

		return 0;
	}

	~WaitData()
	{
		if (_M_hEvent)
		{
			NtClose(_M_hEvent);
		}

		if (_M_hKey)
		{
			NtClose(_M_hKey);
		}
	}

	void Stop(PTP_WAIT pwa)
	{
		_M_dwThreadId = GetCurrentThreadId();

		if (SetEvent(_M_hEvent))
		{
			NtWaitForAlertByThreadId(0, 0);
		}

		CloseThreadpoolWait(pwa);

		if (_M_hmod)
		{
			FreeLibrary(_M_hmod);
			_M_hmod = 0;
		}
	}

	static VOID CALLBACK _S_WaitCallback(
		__inout      PTP_CALLBACK_INSTANCE Instance,
		__inout_opt  PVOID Context,
		__inout      PTP_WAIT pwa,
		__in         TP_WAIT_RESULT WaitResult
		)
	{
		if (WAIT_OBJECT_0 != WaitResult)
		{
			__debugbreak();
		}

		reinterpret_cast<WaitData*>(Context)->WaitCallback(Instance, pwa);
	}

	VOID WaitCallback(_In_ PTP_CALLBACK_INSTANCE Instance, _In_ PTP_WAIT pwa);
};

inline BOOL IsRegSz(PKEY_VALUE_PARTIAL_INFORMATION pkvpi)
{
	ULONG DataLength = pkvpi->DataLength;
	return pkvpi->Type == REG_SZ && DataLength - 1 < MAXUSHORT && !(DataLength & 1) && 
		!(PWSTR)RtlOffsetToPointer(pkvpi->Data, DataLength)[-1];
}

VOID CALLBACK WaitData::WaitCallback(_In_ PTP_CALLBACK_INSTANCE Instance, _In_ PTP_WAIT pwa)
{
	if (ULONG dwThreadId = _M_dwThreadId)
	{
		FreeLibraryWhenCallbackReturns(Instance, (HMODULE)&__ImageBase);
		NtAlertThreadByThreadId((HANDLE)(ULONG_PTR)dwThreadId);
		return ;
	}

	HANDLE hKey = _M_hKey;

	NTSTATUS status;
	union {
		PVOID buf;
		PKEY_VALUE_PARTIAL_INFORMATION pkvpi;
	};

	static const UNICODE_STRING empty{};
	PVOID stack = alloca(guz);

	ULONG cb = 0, rcb = 0x80;
	do 
	{
		if (cb < rcb)
		{
			cb = RtlPointerToOffset(buf = alloca(rcb - cb), stack);
		}

		status = ZwQueryValueKey(hKey, &empty, KeyValuePartialInformation, buf, cb, &rcb);

	} while (STATUS_BUFFER_OVERFLOW == status);

	if (0 <= status && IsRegSz(pkvpi))
	{
		ULONG hash;
		UNICODE_STRING ObjectName;
		RtlInitUnicodeString(&ObjectName, (PWSTR)pkvpi->Data);

		if (0 <= RtlHashUnicodeString(&ObjectName, TRUE, HASH_STRING_ALGORITHM_DEFAULT, &hash) && 
			hash != _M_hash)
		{
			_M_hash = hash;

			DbgPrint("%wZ\n", &ObjectName);

			if (_M_hmod)
			{
				FreeLibrary(_M_hmod);
				_M_hmod = 0;
			}

			if (HMODULE hmod = LoadLibraryExW((PWSTR)pkvpi->Data, 0, LOAD_LIBRARY_AS_DATAFILE))
			{
				LoadLibraryFromMem(&_M_hmod, PAGE_ALIGN(hmod));
				FreeLibrary(hmod);
			}
		}
	}

	if (StartNotify())
	{
		SetThreadpoolWait(pwa, _M_hEvent, 0);
	}
}

void ep(void*)
{
	{
		WaitData wd;
		STATIC_OBJECT_ATTRIBUTES(oa, "\\registry\\MACHINE\\SOFTWARE\\Cygwin");
		if (PTP_WAIT pwa = wd.Create(&oa))
		{
			MessageBoxW(0, 0, 0, MB_ICONWARNING);
			wd.Stop(pwa);
		}
	}

	ExitProcess(0);
}

_NT_END