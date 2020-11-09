#include "SiglusHook.h"
#include <gdiplus.h>  
#include <Psapi.h>
#include "mt64.h"
#include <vector>
#include "resource.h"
#include "PckCommon.h"
#include <ImageHlp.h>
#include <fstream>
#include <string>


#pragma comment(lib, "gdiplus")
#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "Imagehlp.lib")
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Kernel32.lib")
#pragma comment(lib, "Version.lib")

static SiglusHook* g_Hook = NULL;

SiglusHook* GetSiglusHook()
{
	LOOP_ONCE
	{
		if (g_Hook)
		break;

		if (!g_Hook)
			g_Hook = new SiglusHook;

		if (g_Hook)
			break;

		MessageBoxW(NULL, L"Insufficient memory", L"SiglusExtract", MB_OK | MB_ICONERROR);
		ExitProcess(STATUS_NO_MEMORY);
	}
	return g_Hook;
}

void NTAPI ExecuteDumper(DWORD Pid, PBYTE* Buffer, ULONG* Size, PVOID(NTAPI *Allocater)(ULONG Size));

PVOID NTAPI LocalAllocater(ULONG Size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
}



SiglusHook::SiglusHook() :
ExeModule(NULL),
DllModule(NULL),
GameexeHandle(INVALID_HANDLE_VALUE),
StubReadFile(NULL),
StubCreateFileW(NULL),
ImageSize(0),
ImageBuffer(NULL),
ImageSizeEx(0),
ImageBufferEx(NULL),
InitKey(FALSE),
ExtraDecInPck(TRUE),
ExtraDecInDat(TRUE),
GuiHandle(INVALID_HANDLE_VALUE)
{
	std::fstream Override(L"Override.ini");
	std::string  Scene, Gameexe;
	WCHAR        WScene[MAX_PATH];
	WCHAR        WGameexe[MAX_PATH];
	NtFileDisk   File;
	
	SceneName   = L"Scene.pck";
	GameexeName = L"Gameexe.dat";


	RtlZeroMemory(WScene,   sizeof(WScene));
	RtlZeroMemory(WGameexe, sizeof(WGameexe));

	auto FileIsExist = [](LPCWSTR FileName)->BOOL
	{
		DWORD Attribute = GetFileAttributesW(FileName);
		return (Attribute != 0xFFFFFFFF) && !(Attribute & FILE_ATTRIBUTE_DIRECTORY);
	};

	LOOP_ONCE
	{
		if (!Override.is_open())
			break;

		if (!getline(Override, Scene))
			break;
		
		if (!getline(Override, Gameexe))
			break;

		Override.close();
		MultiByteToWideChar(0, 0, Scene.c_str(), Scene.length(), WScene, countof(WScene) - 1);
		MultiByteToWideChar(0, 0, Gameexe.c_str(), Gameexe.length(), WGameexe, countof(WGameexe) - 1);

		if (!(FileIsExist(WScene) && FileIsExist(WGameexe)))
			break;

		//too lazy to check file...
		SceneName   = WScene;
		GameexeName = WGameexe;
	}
}

SiglusHook::~SiglusHook()
{
}


VOID SiglusHook::UnInit()
{
	if (ImageBuffer)
		HeapFree(GetProcessHeap(), 0, ImageBuffer);

	ImageBuffer = NULL;
}

DWORD NTAPI GuiWorkerThread(PVOID UserData)
{
	SiglusHook* Hook = (SiglusHook*)UserData;
	Hook->Dialog.DoModal();
	return 0;
}


DWORD NTAPI DelayDumperThreadGUI(LPVOID _This)
{
	SiglusHook*         This;


	This = (SiglusHook*)_This;

	This->InitDialog.Create(IDD_INIT_DIALOG);
	This->InitDialog.Initialize();
	This->InitDialog.ShowWindow(SW_SHOW);

	while (This->ExInit != TRUE)
	{
		Sleep(10);
	}

	This->InitDialog.EndDialog(0);

	return 0;
}

HANDLE NTAPI HookCreateFileW(
	LPCWSTR lpFileName,
	DWORD   dwDesiredAccess,
	DWORD   dwShareMode,
	LPSECURITY_ATTRIBUTES lpSecurityAttributes,
	DWORD  dwCreationDisposition,
	DWORD  dwFlagsAndAttributes,
	HANDLE hTemplateFile
	)
{
	HANDLE                   Handle;
	SiglusHook*              Data;

	Data = GetSiglusHook();

	auto IsScenePack = [&](LPCWSTR FileName)->BOOL
	{
		LONG_PTR iPos = 0;

		for (LONG_PTR i = 0; i < lstrlenW(FileName); i++)
		{
			if (FileName[i] == L'\\' || FileName[i] == L'/')
				iPos = i;
		}

		if (iPos != 0)
			iPos++;

		return lstrcmpW(FileName + iPos, Data->SceneName.c_str()) == 0;
	};


	auto IsGameExe = [&](LPCWSTR FileName)->BOOL
	{
		LONG_PTR iPos = 0;

		for (LONG_PTR i = 0; i < lstrlenW(FileName); i++)
		{
			if (FileName[i] == L'\\' || FileName[i] == L'/')
				iPos = i;
		}

		if (iPos != 0)
			iPos++;

		return lstrcmpW(FileName + iPos, Data->GameexeName.c_str()) == 0;
	};


	auto IsMainExe = []()->BOOL
	{
		MODULEINFO  Info;
		GetModuleInformation(GetCurrentProcess(), (HMODULE)GetModuleHandleW(NULL), &Info, sizeof(Info));

		if (((PBYTE)_ReturnAddress() > (PBYTE)Info.lpBaseOfDll) &&
			((PBYTE)_ReturnAddress() < (PBYTE)Info.lpBaseOfDll + Info.SizeOfImage))
		{
			return TRUE;
		}

		return FALSE;
	};


	Handle = Data->StubCreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
		lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

	if (IsGameExe(lpFileName) && IsMainExe() && (!Data->InitKey))
	{
		Data->GameexeHandle = Handle;
		Data->InitKey = TRUE;
	}

	return Handle;
}

#include "hardwarebp.h"
#include <atomic>

HardwareBreakpoint m_Bp;
std::atomic<BOOL> InitOnce = FALSE;
std::atomic<BOOL> HookOnce = FALSE;
PVOID ExceptionHandler = NULL;
static CONTEXT SavedContext = { 0 };
static CONTEXT* pSaveContext = &SavedContext;
static BYTE  DummyStack[0x1000] = { 0 };
static PBYTE pDummyStack = DummyStack + 0x500;

//remove handler here
ASM VOID SwitchToNormal()
{
	INLINE_ASM
	{
		int 3
		mov  esp, pDummyStack
		mov  eax, ExceptionHandler
		push eax
		call RemoveVectoredExceptionHandler
		mov  ExceptionHandler, 0
		mov  eax, pSaveContext
		push eax
		call GetCurrentThreadId
		push eax
		call SetThreadContext
	}
}

LONG NTAPI FindPrivateKeyHandler(PEXCEPTION_POINTERS pExceptionInfo)
{
	NTSTATUS    Status;
	ULONG       Success;
	SiglusHook* Data;
	PBYTE       Buffer, AccessBuffer;
	BYTE        PrivateKey[16];

	static BYTE Key[256] =
	{
		0xD8, 0x29, 0xB9, 0x16, 0x3D, 0x1A, 0x76, 0xD0, 0x87, 0x9B, 0x2D, 0x0C, 0x7B, 0xD1, 0xA9, 0x19,
		0x22, 0x9F, 0x91, 0x73, 0x6A, 0x35, 0xB1, 0x7E, 0xD1, 0xB5, 0xE7, 0xE6, 0xD5, 0xF5, 0x06, 0xD6,
		0xBA, 0xBF, 0xF3, 0x45, 0x3F, 0xF1, 0x61, 0xDD, 0x4C, 0x67, 0x6A, 0x6F, 0x74, 0xEC, 0x7A, 0x6F,
		0x26, 0x74, 0x0E, 0xDB, 0x27, 0x4C, 0xA5, 0xF1, 0x0E, 0x2D, 0x70, 0xC4, 0x40, 0x5D, 0x4F, 0xDA,
		0x9E, 0xC5, 0x49, 0x7B, 0xBD, 0xE8, 0xDF, 0xEE, 0xCA, 0xF4, 0x92, 0xDE, 0xE4, 0x76, 0x10, 0xDD,
		0x2A, 0x52, 0xDC, 0x73, 0x4E, 0x54, 0x8C, 0x30, 0x3D, 0x9A, 0xB2, 0x9B, 0xB8, 0x93, 0x29, 0x55,
		0xFA, 0x7A, 0xC9, 0xDA, 0x10, 0x97, 0xE5, 0xB6, 0x23, 0x02, 0xDD, 0x38, 0x4C, 0x9B, 0x1F, 0x9A,
		0xD5, 0x49, 0xE9, 0x34, 0x0F, 0x28, 0x2D, 0x1B, 0x52, 0x39, 0x5C, 0x36, 0x89, 0x56, 0xA7, 0x96,
		0x14, 0xBE, 0x2E, 0xC5, 0x3E, 0x08, 0x5F, 0x47, 0xA9, 0xDF, 0x88, 0x9F, 0xD4, 0xCC, 0x69, 0x1F,
		0x30, 0x9F, 0xE7, 0xCD, 0x80, 0x45, 0xF3, 0xE7, 0x2A, 0x1D, 0x16, 0xB2, 0xF1, 0x54, 0xC8, 0x6C,
		0x2B, 0x0D, 0xD4, 0x65, 0xF7, 0xE3, 0x36, 0xD4, 0xA5, 0x3B, 0xD1, 0x79, 0x4C, 0x54, 0xF0, 0x2A,
		0xB4, 0xB2, 0x56, 0x45, 0x2E, 0xAB, 0x7B, 0x88, 0xC5, 0xFA, 0x74, 0xAD, 0x03, 0xB8, 0x9E, 0xD5,
		0xF5, 0x6F, 0xDC, 0xFA, 0x44, 0x49, 0x31, 0xF6, 0x83, 0x32, 0xFF, 0xC2, 0xB1, 0xE9, 0xE1, 0x98,
		0x3D, 0x6F, 0x31, 0x0D, 0xAC, 0xB1, 0x08, 0x83, 0x9D, 0x0D, 0x10, 0xD1, 0x41, 0xF9, 0x00, 0xBA,
		0x1A, 0xCF, 0x13, 0x71, 0xE4, 0x86, 0x21, 0x2F, 0x23, 0x65, 0xC3, 0x45, 0xA0, 0xC3, 0x92, 0x48,
		0x9D, 0xEA, 0xDD, 0x31, 0x2C, 0xE9, 0xE2, 0x10, 0x22, 0xAA, 0xE1, 0xAD, 0x2C, 0xC4, 0x2D, 0x7F
	};

	Data = GetSiglusHook();
	
	if (InitOnce == FALSE && pExceptionInfo->ExceptionRecord->ExceptionCode == STATUS_SINGLE_STEP)
	{
		m_Bp.Clear(pExceptionInfo->ContextRecord);
		Buffer = &Data->GameexeBytes[8];

		AccessBuffer = Data->AccessPtr - 16;
		for (ULONG i = 0; i < 16; i++)
		{
			PrivateKey[i] = AccessBuffer[i] ^ Buffer[i];
		}
		
		/*
		xxxx
		PrintConsoleW(L"%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", 
			PrivateKey[0], PrivateKey[1], PrivateKey[2], PrivateKey[3],
			PrivateKey[4], PrivateKey[5], PrivateKey[6], PrivateKey[7],
			PrivateKey[8], PrivateKey[9], PrivateKey[10], PrivateKey[11],
			PrivateKey[12], PrivateKey[13], PrivateKey[14], PrivateKey[15]);
		*/

		Data->Dialog.SetPrivateKey(PrivateKey);
		Data->Dialog.PckAndDatStatus(Data->ExtraDecInPck, Data->ExtraDecInDat);
		Status = Nt_CreateThread(GuiWorkerThread, Data, FALSE, NtCurrentProcess(), &Data->GuiHandle);
		if (NT_FAILED(Status))
			MessageBoxW(NULL, L"Failed to open SiglusExtract's main window", L"SiglusExtract", MB_OK | MB_ICONERROR);

		RtlCopyMemory(&SavedContext, pExceptionInfo->ContextRecord, sizeof(CONTEXT));
		Success = RemoveVectoredExceptionHandler(ExceptionHandler);
		//PrintConsoleW(L"remove : %d\n", Success);
		InitOnce = TRUE;

		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}


DWORD GetNtPathFromHandle(HANDLE hFile, PWSTR Path, ULONG Length)
{
	if (hFile == 0 || hFile == INVALID_HANDLE_VALUE)
		return ERROR_INVALID_HANDLE;

	BYTE  u8_Buffer[2000];
	DWORD u32_ReqLength = 0;

	UNICODE_STRING* pk_Info = &((OBJECT_NAME_INFORMATION*)u8_Buffer)->Name;
	pk_Info->Buffer = 0;
	pk_Info->Length = 0;

	NtQueryObject(hFile, ObjectNameInformation, u8_Buffer, sizeof(u8_Buffer), &u32_ReqLength);

	if (!pk_Info->Buffer || !pk_Info->Length)
		return ERROR_FILE_NOT_FOUND;

	pk_Info->Buffer[pk_Info->Length / 2] = 0;
	wcsncpy_s(Path, Length, pk_Info->Buffer, pk_Info->Length / 2);
	return 0;
}


BOOL WINAPI HookReadFile(
	HANDLE hFile,
	LPVOID lpBuffer,
	DWORD nNumberOfBytesToRead,
	LPDWORD lpNumberOfBytesRead,
	LPOVERLAPPED lpOverlapped
	)
{
	BOOL        Result;
	SiglusHook* Data;
	WCHAR       Name[1000];
	DWORD       Offset;

	Data   = GetSiglusHook();
	Offset = SetFilePointer(hFile, 0, NULL, FILE_CURRENT);
	Result = Data->StubReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);

	if (hFile == Data->GameexeHandle && Offset == 0 && HookOnce == FALSE)
	{
		Data->AccessPtr = (PBYTE)lpBuffer + 8 + 16;

		m_Bp.Set((PBYTE)lpBuffer + 8 + 16, 1, HardwareBreakpoint::Condition::Write);
		ExceptionHandler = AddVectoredExceptionHandler(1, FindPrivateKeyHandler);
		
		Data->GameexeHandle = INVALID_HANDLE_VALUE;
		HookOnce = FALSE;
	}

	return Result;
}


API_POINTER(MultiByteToWideChar) OldMultiByteToWideChar = NULL;

int
WINAPI
HookMultiByteToWideChar(
UINT CodePage,
DWORD dwFlags,
LPCCH lpMultiByteStr,
int cbMultiByte,
LPWSTR lpWideCharStr,
int cchWideChar
)
{
	switch (CodePage)
	{
	case CP_ACP:
	case CP_OEMCP:
	case CP_THREAD_ACP:
		CodePage = 932;
		break;
	}

	return OldMultiByteToWideChar(CodePage,
		dwFlags,
		lpMultiByteStr,
		cbMultiByte,
		lpWideCharStr,
		cchWideChar);
}




BOOL SelfPrivilegeUp(void)
{
	BOOL   ret;
	HANDLE hToken;
	LUID luid;
	TOKEN_PRIVILEGES tp;

	ret = OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken);
	if (!ret)
	{
		return FALSE;
	}

	ret = LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid);
	if (!ret)
	{
		CloseHandle(hToken);
		return FALSE;
	}
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	ret = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
	if (!ret)
	{
		CloseHandle(hToken);
		return FALSE;
	}
	CloseHandle(hToken);
	return TRUE;
}


DWORD NTAPI HookMapFileAndCheckSumA(
	_In_  PCSTR  Filename,
	_Out_ PDWORD HeaderSum,
	_Out_ PDWORD CheckSum
	)
{
	return CHECKSUM_SUCCESS;
}

API_POINTER(GetFileVersionInfoSizeW) StubGetFileVersionInfoSizeW = NULL;

DWORD
APIENTRY
HookGetFileVersionInfoSizeW(
_In_        LPCWSTR lptstrFilename, /* Filename of version stamped file */
_Out_opt_   LPDWORD lpdwHandle       /* Information for use by GetFileVersionInfo */
)
{
	auto IsKernel32 = [](LPCWSTR lpFileName)->BOOL
	{
		/*
		ULONG Length = StrLengthW(lpFileName);
		if (Length < 12)
		return FALSE;

		//sarcheck.dll
		return CHAR_UPPER4W(*(PULONG64)&lpFileName[Length - 0xC]) == TAG4W('KERN') &&
		CHAR_UPPER4W(*(PULONG64)&lpFileName[Length - 0x8]) == TAG4W('AL32') &&
		CHAR_UPPER4W(*(PULONG64)&lpFileName[Length - 0x4]) == TAG4W('.DLL');
		*/

		return wcsstr(lpFileName, L"kernel32.dll") != NULL;
	};

	if (IsKernel32(lptstrFilename))
		return StubGetFileVersionInfoSizeW(L"SiglusExtract.dll", lpdwHandle);

	return StubGetFileVersionInfoSizeW(lptstrFilename, lpdwHandle);
}


API_POINTER(GetFileVersionInfoW) StubGetFileVersionInfoW = NULL;

BOOL
APIENTRY
HookGetFileVersionInfoW(
_In_                LPCWSTR lptstrFilename, /* Filename of version stamped file */
_Reserved_          DWORD dwHandle,          /* Information from GetFileVersionSize */
_In_                DWORD dwLen,             /* Length of buffer for info */
LPVOID lpData            /* Buffer to place the data structure */
)
{
	auto IsKernel32 = [](LPCWSTR lpFileName)->BOOL
	{
		/*
		ULONG Length = StrLengthW(lpFileName);
		if (Length < 12)
		return FALSE;

		//sarcheck.dll
		return CHAR_UPPER4W(*(PULONG64)&lpFileName[Length - 0xC]) == TAG4W('KERN') &&
		CHAR_UPPER4W(*(PULONG64)&lpFileName[Length - 0x8]) == TAG4W('AL32') &&
		CHAR_UPPER4W(*(PULONG64)&lpFileName[Length - 0x4]) == TAG4W('.DLL');
		*/

		return wcsstr(lpFileName, L"kernel32.dll") != NULL;
	};

	if (IsKernel32(lptstrFilename))
		return StubGetFileVersionInfoW(L"SiglusExtract.dll", dwHandle, dwLen, lpData);

	return StubGetFileVersionInfoW(lptstrFilename, dwHandle, dwLen, lpData);
}

API_POINTER(GetTimeZoneInformation) StubGetTimeZoneInformation = NULL;

DWORD
WINAPI
HookGetTimeZoneInformation(
_Out_ LPTIME_ZONE_INFORMATION lpTimeZoneInformation
)
{
	static WCHAR StdName[] = L"TOKYO";
	static WCHAR DayName[] = L"Tokyo Daylight Time";

	StubGetTimeZoneInformation(lpTimeZoneInformation);

	lpTimeZoneInformation->Bias = -540;
	lpTimeZoneInformation->StandardBias = 0;

	RtlCopyMemory(lpTimeZoneInformation->StandardName, StdName, countof(StdName) * 2);
	RtlCopyMemory(lpTimeZoneInformation->DaylightName, DayName, countof(DayName) * 2);
	return 0;
}


API_POINTER(GetLocaleInfoW) StubGetLocaleInfoW = NULL;

int
WINAPI
HookGetLocaleInfoW(
LCID     Locale,
LCTYPE   LCType,
LPWSTR lpLCData,
int      cchData)
{
	if (Locale == 0x800u && LCType == 1)
	{
		RtlCopyMemory(lpLCData, L"0411", 10);
		return 5;
	}

	return StubGetLocaleInfoW(Locale, LCType, lpLCData, cchData);
}


API_POINTER(CreateProcessInternalW) StubCreateProcessInternalW = NULL;

BOOL
WINAPI
HookCreateProcessInternalW(
HANDLE                  hToken,
LPCWSTR                 lpApplicationName,
LPWSTR                  lpCommandLine,
LPSECURITY_ATTRIBUTES   lpProcessAttributes,
LPSECURITY_ATTRIBUTES   lpThreadAttributes,
BOOL                    bInheritHandles,
ULONG                   dwCreationFlags,
LPVOID                  lpEnvironment,
LPCWSTR                 lpCurrentDirectory,
LPSTARTUPINFOW          lpStartupInfo,
LPPROCESS_INFORMATION   lpProcessInformation,
PHANDLE                 phNewToken
)
{
	BOOL             Result, IsSuspended;
	NTSTATUS         Status;
	UNICODE_STRING   FullDllPath;

	auto Data = GetSiglusHook();

	RtlInitUnicodeString(&FullDllPath, (PWSTR)Data->DllPath.c_str());

	IsSuspended = !!(dwCreationFlags & CREATE_SUSPENDED);
	dwCreationFlags |= CREATE_SUSPENDED;
	Result = StubCreateProcessInternalW(
		hToken,
		lpApplicationName,
		lpCommandLine,
		lpProcessAttributes,
		lpThreadAttributes,
		bInheritHandles,
		dwCreationFlags,
		lpEnvironment,
		lpCurrentDirectory,
		lpStartupInfo,
		lpProcessInformation,
		phNewToken);

	if (!Result)
		return Result;

	Status = InjectDllToRemoteProcess(
		lpProcessInformation->hProcess,
		lpProcessInformation->hThread,
		&FullDllPath,
		IsSuspended
		);

	return TRUE;
}


BOOL SiglusHook::DetactNeedDumper()
{
	NTSTATUS   Status;
	NtFileDisk File;
	BYTE       Buffer[100];

	DisablePrivateKey = FALSE;


	LOOP_ONCE
	{
		Status = File.Open(SceneName.c_str());
		if (NT_FAILED(Status))
		{
			DisablePrivateKey = TRUE;
			MessageBoxW(NULL, L"Couldn't find Scene.pck", L"SiglusExtract", MB_OK | MB_ICONERROR);
			ExitProcess(0);
		}

		File.Read(Buffer, sizeof(SCENEHEADER));
		if (((SCENEHEADER*)Buffer)->ExtraKeyUse == 0)
		{
			PrintConsoleW(L"pck : 2nd key -> disabled\n");
			ExtraDecInPck = FALSE;
		}
		File.Close();

		Status = File.Open(GameexeName.c_str());
		if (NT_FAILED(Status))
		{
			DisablePrivateKey = TRUE;
			MessageBoxW(NULL, L"Couldn't find Gameexe.dat", L"SiglusExtract", MB_OK | MB_ICONERROR);
			ExitProcess(0);
		}
		File.Read(Buffer, 8 + 16);
		RtlCopyMemory(GameexeBytes, Buffer, 24);
		File.Close();
		if (*(PDWORD)&Buffer[4] == 0)
		{
			PrintConsoleW(L"dat : 2nd key -> disabled\n");
			ExtraDecInDat = FALSE;
		}
	}

	if (ExtraDecInDat || ExtraDecInPck)
	{
		Mp::PATCH_MEMORY_DATA p[] =
		{
			Mp::FunctionJumpVa(ReadFile, HookReadFile, &StubReadFile)
		};

		Mp::PatchMemory(p, countof(p));
	}

	return ExtraDecInDat || ExtraDecInPck;
}



API_POINTER(GetUserNameA) StubGetUserNameA = NULL;

BOOL WINAPI HookGetUserNameA(
	_Out_   LPSTR  lpBuffer,
	_Inout_ LPDWORD lpnSize
)
{
	ULONG_PTR  ReturnAddress, OpSize;
	DWORD      OldProtect;

	//PrintConsoleW(L"get user name..........\n");

	INLINE_ASM
	{
		mov eax,[ebp];
		mov ebx,[eax + 4]; //ret addr
		mov ReturnAddress, ebx;
	}

		//PrintConsoleW(L"%08x\n", ReturnAddress);

		//find the first 'jnz' 
	OpSize = 0;
	for (ULONG_PTR i = 0; i < 0x30;)
	{
		OpSize = GetOpCodeSize32((PBYTE)(ReturnAddress + i));
		if (OpSize == 2 && ((PBYTE)(ReturnAddress + i))[0] == 0x75) //short jump
		{
			VirtualProtect((PBYTE)(ReturnAddress + i), 2, PAGE_EXECUTE_READWRITE, &OldProtect);
			((PBYTE)(ReturnAddress + i))[0] = 0xB0;
			((PBYTE)(ReturnAddress + i))[1] = 0x01;
			VirtualProtect((PBYTE)(ReturnAddress + i), 2, OldProtect, &OldProtect);
			//PrintConsoleW(L"patch..........\n");
			break;
		}
		i += OpSize;
	}

	return StubGetUserNameA(lpBuffer, lpnSize);
}


typedef struct _PROCESS_TIMES
{
	LARGE_INTEGER CreationTime;
	LARGE_INTEGER ExitTime;
	LARGE_INTEGER KernelTime;
	LARGE_INTEGER UserTime;

} PROCESS_TIMES, *PPROCESS_TIMES;

#pragma comment(lib, "dbghelp.lib")

LONG NTAPI SiglusUnhandledExceptionFilter(EXCEPTION_POINTERS* Exception)
{
	WCHAR           MiniDumpFile[MAX_PATH];
	NtFileDisk      File;
	BOOL            Success;
	NTSTATUS        Status;
	PROCESS_TIMES   Times;

	MINIDUMP_EXCEPTION_INFORMATION ExceptionInformation;

	MessageBoxW(NULL, L"Crashes", 0, 0);
	NtQueryInformationProcess(NtCurrentProcess(), ProcessTimes, &Times, sizeof(Times), NULL);

	GetModuleFileNameW(GetModuleHandleW(NULL), MiniDumpFile, countof(MiniDumpFile));
	swprintf(MiniDumpFile + lstrlenW(MiniDumpFile), L".%I64X.crash.dmp", Times.CreationTime.QuadPart);

	Status = File.Create(MiniDumpFile);
	FAIL_RETURN(Status);

	ExceptionInformation.ClientPointers = FALSE;
	ExceptionInformation.ExceptionPointers = Exception;
	ExceptionInformation.ThreadId = GetCurrentThreadId();

	Success = MiniDumpWriteDump(
		NtCurrentProcess(),
		GetCurrentProcessId(),
		File,
		MiniDumpNormal,
		&ExceptionInformation,
		NULL,
		NULL
	);

	File.Close();
	ExitProcess(-1);
	return EXCEPTION_EXECUTE_HANDLER;
}

BOOL SiglusHook::InitWindow()
{
	BOOL                Status;
	PVOID               CreateProcessInternalWAddress;
	BOOL                WaitForStackDecode;

	SelfPrivilegeUp();
	SetUnhandledExceptionFilter(SiglusUnhandledExceptionFilter);

	LOOP_ONCE
	{
		ExeModule = GetModuleHandleW(NULL);
		WaitForStackDecode = DetactNeedDumper();

		ExtModuleHandle = NULL;
		CreateProcessInternalWAddress = Nt_GetProcAddress(GetKernel32Handle(), "CreateProcessInternalW");

		//Bypass VA's Japanese locale check
		{
			
			Mp::PATCH_MEMORY_DATA p[] =
			{
				//check
				Mp::FunctionJumpVa(GetTimeZoneInformation,  HookGetTimeZoneInformation,  &StubGetTimeZoneInformation ),
				Mp::FunctionJumpVa(GetLocaleInfoW,          HookGetLocaleInfoW,          &StubGetLocaleInfoW         ),
				Mp::FunctionJumpVa(GetFileVersionInfoW,     HookGetFileVersionInfoW,     &StubGetFileVersionInfoW    ),
				Mp::FunctionJumpVa(GetFileVersionInfoSizeW, HookGetFileVersionInfoSizeW, &StubGetFileVersionInfoSizeW),
				Mp::FunctionJumpVa(GetUserNameA,            HookGetUserNameA,            &StubGetUserNameA),

				//hook create process(steam)
				Mp::FunctionJumpVa(CreateProcessInternalWAddress,  HookCreateProcessInternalW,  &StubCreateProcessInternalW),
				Mp::FunctionJumpVa(MultiByteToWideChar,            HookMultiByteToWideChar,     &OldMultiByteToWideChar)
			};

			Mp::PatchMemory(p, countof(p));
		}


		Gdiplus::GdiplusStartupInput gdiplusStartupInput;
		ULONG_PTR                    gdiplusToken;
		Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);


		Mp::PATCH_MEMORY_DATA f[] =
		{
			Mp::FunctionJumpVa(CreateFileW, HookCreateFileW, &StubCreateFileW ),
			
		};
		
		if (WaitForStackDecode) {
			Status = NT_SUCCESS(Mp::PatchMemory(f, countof(f)));
		}
		else {
			Status = Nt_CreateThread(GuiWorkerThread, this, FALSE, NtCurrentProcess(), &(this->GuiHandle));
			Status = NT_SUCCESS(Status);
		}
	}

	return Status;
}
