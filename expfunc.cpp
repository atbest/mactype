#ifndef _GDIPP_EXE
#include "settings.h"
#include "override.h"
#include <tlhelp32.h>
#include <shlwapi.h>	//DLLVERSIONINFO
#include "undocAPI.h"
#include <windows.h>

// win2k�ȍ~
//#pragma comment(linker, "/subsystem:windows,5.0")

EXTERN_C LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam)
{
	//�������Ȃ�
	return CallNextHookEx(NULL, code, wParam, lParam);
}

EXTERN_C HRESULT WINAPI GdippDllGetVersion(DLLVERSIONINFO* pdvi)
{
	if (!pdvi || pdvi->cbSize < sizeof(DLLVERSIONINFO)) {
		return E_INVALIDARG;
	}

	const UINT cbSize = pdvi->cbSize;
	ZeroMemory(pdvi, cbSize);
	pdvi->cbSize = cbSize;

	HRSRC hRsrc = FindResource(GetDLLInstance(), MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);
	if (!hRsrc) {
		return E_FAIL;
	}

	HGLOBAL hGlobal = LoadResource(GetDLLInstance(), hRsrc);
	if (!hGlobal) {
		return E_FAIL;
	}

	const WORD* lpwPtr = (const WORD*)LockResource(hGlobal);
	if (lpwPtr[1] != sizeof(VS_FIXEDFILEINFO)) {
		return E_FAIL;
	}

	const VS_FIXEDFILEINFO* pvffi = (const VS_FIXEDFILEINFO*)(lpwPtr + 20);
	if (pvffi->dwSignature != VS_FFI_SIGNATURE ||
			pvffi->dwStrucVersion != VS_FFI_STRUCVERSION) {
		return E_FAIL;
	}

	//8.0.2006.1027
	// -> Major: 8, Minor: 2006, Build: 1027
	pdvi->dwMajorVersion	= HIWORD(pvffi->dwFileVersionMS);
	pdvi->dwMinorVersion	= LOWORD(pvffi->dwFileVersionMS) * 10 + HIWORD(pvffi->dwFileVersionLS);
	pdvi->dwBuildNumber		= LOWORD(pvffi->dwFileVersionLS);
	pdvi->dwPlatformID		= DLLVER_PLATFORM_NT;

	if (pdvi->cbSize < sizeof(DLLVERSIONINFO2)) {
		return S_OK;
	}

	DLLVERSIONINFO2* pdvi2 = (DLLVERSIONINFO2*)pdvi;
	pdvi2->ullVersion		= MAKEDLLVERULL(pdvi->dwMajorVersion, pdvi->dwMinorVersion, pdvi->dwBuildNumber, 2);
	return S_OK;
}

#endif	//!_GDIPP_EXE

extern LONG interlock;
extern LONG g_bHookEnabled;
#include "gdiPlusFlat2.h"

#ifdef USE_DETOURS
#include "detours.h"
#define HOOK_DEFINE(rettype, name, argtype) \
	DetourDetach(&(PVOID&)ORIG_##name, IMPL_##name);
static LONG hook_term()
{
	DetourTransactionBegin();

	DetourUpdateThread(GetCurrentThread());

#include "hooklist.h"

	LONG error = DetourTransactionCommit();

	if (error != NOERROR) {
		TRACE(_T("hook_term error: %#x\n"), error);
	}
	return error;
}
#undef HOOK_DEFINE
#else
#include "easyhook.h"
#define HOOK_MANUALLY(rettype, name, argtype) ;
#define HOOK_DEFINE(rettype, name, argtype) \
	ORIG_##name = name;
#pragma optimize("s", on)
static LONG hook_term()
{
#include "hooklist.h"
	LhUninstallAllHooks();
	return LhWaitForPendingRemovals();
}
#pragma optimize("", on)
#undef HOOK_DEFINE
#undef HOOK_MANUALLY
#endif

HMODULE GetSelfModuleHandle()
{
	MEMORY_BASIC_INFORMATION mbi;

	return ((::VirtualQuery(GetSelfModuleHandle, &mbi, sizeof(mbi)) != 0) 
		? (HMODULE) mbi.AllocationBase : NULL);
}

EXTERN_C void WINAPI CreateControlCenter(IControlCenter** ret)
{
	*ret = (IControlCenter*)new CControlCenter;
}

EXTERN_C void WINAPI ReloadConfig()
{
	CControlCenter::ReloadConfig();
}

extern HINSTANCE g_dllInstance;
EXTERN_C void SafeUnload()
{
	static BOOL bInited = false;
	if (bInited)
		return;	//������
	bInited = true;
	while (CThreadCounter::Count())
		Sleep(0);
	CCriticalSectionLock * lock = new CCriticalSectionLock;
	BOOL last;
	if (last=InterlockedExchange(&g_bHookEnabled, FALSE)) {
		if (hook_term()!=NOERROR)
		{
			InterlockedExchange(&g_bHookEnabled, last);
			bInited = false;
			delete lock;
			ExitThread(ERROR_ACCESS_DENIED);
		}
	}
	delete lock;
	while (CThreadCounter::Count())
		Sleep(10);
	Sleep(0);
	do 
	{
		Sleep(10);
	} while (CThreadCounter::Count());	//double check for xp
		
	bInited = false; 
	FreeLibraryAndExitThread(g_dllInstance, 0);
}

#ifndef Assert
#include <crtdbg.h>
#define Assert	_ASSERTE
#endif	//!Assert

#include "array.h"
#include <strsafe.h>
#include <shlwapi.h>
#include "dll.h"

//kernel32��pGetProcAddress���h�L
FARPROC K32GetProcAddress(LPCSTR lpProcName)
{
#ifndef _WIN64
	//�����n���ɂ͑Ή����Ȃ�
	Assert(!IS_INTRESOURCE(lpProcName));

	//kernel32�̃x�[�X�A�h���X�擾
	LPBYTE pBase = (LPBYTE)GetModuleHandleA("kernel32.dll");

	//���̕ӂ�100%��������͂��Ȃ̂ŃG���[�`�F�b�N���Ȃ�
	PIMAGE_DOS_HEADER pdosh = (PIMAGE_DOS_HEADER)pBase;
	Assert(pdosh->e_magic == IMAGE_DOS_SIGNATURE);
	PIMAGE_NT_HEADERS pnth = (PIMAGE_NT_HEADERS)(pBase + pdosh->e_lfanew);
	Assert(pnth->Signature == IMAGE_NT_SIGNATURE);

	const DWORD offs = pnth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
	const DWORD size = pnth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
	if (offs == 0 || size == 0) {
		return NULL;
	}

	PIMAGE_EXPORT_DIRECTORY pdir = (PIMAGE_EXPORT_DIRECTORY)(pBase + offs);
	DWORD*	pFunc = (DWORD*)(pBase + pdir->AddressOfFunctions);
	WORD*	pOrd  = (WORD*)(pBase + pdir->AddressOfNameOrdinals);
	DWORD*	pName = (DWORD*)(pBase + pdir->AddressOfNames);

	for(DWORD i=0; i<pdir->NumberOfFunctions; i++) {
		for(DWORD j=0; j<pdir->NumberOfNames; j++) {
			if(pOrd[j] != i)
				continue;

			if(strcmp((LPCSTR)pBase + pName[j], lpProcName) != 0)
				continue;

			return (FARPROC)(pBase + pFunc[i]);
		}
	}
	return NULL;
#else
	Assert(!IS_INTRESOURCE(lpProcName));

	//kernel32�̃x�[�X�A�h���X�擾
	WCHAR sysdir[MAX_PATH];
	GetWindowsDirectory(sysdir, MAX_PATH);
	if (GetModuleHandle(_T("kernelbase.dll")))	//�鿴�Լ��Ƿ������Kernelbase.dll�ļ���������˵����win7ϵͳ
		wcscat(sysdir, L"\\SysWow64\\kernelbase.dll");
	else
		wcscat(sysdir, L"\\SysWow64\\kernel32.dll");	//�����ھ���vista
	HANDLE hFile = CreateFile(sysdir, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, NULL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return NULL;
	DWORD dwSize = GetFileSize(hFile, NULL);
	BYTE* pMem = new BYTE[dwSize];	//�����ڴ�
	ReadFile(hFile, pMem, dwSize, &dwSize, NULL);//��ȡ�ļ�
	CloseHandle(hFile);

	CMemLoadDll MemDll;
	MemDll.MemLoadLibrary(pMem, dwSize, false, false);
	delete pMem;
	return FARPROC((DWORD_PTR)MemDll.MemGetProcAddress(lpProcName)-MemDll.GetImageBase());	//����ƫ��ֵ

#endif
}


#include <pshpack1.h>
class opcode_data {
private:
	BYTE	code[0x100];

	//��: dllpath��WORD���E�ɂ��Ȃ��Əꍇ�ɂ���Ă͐���ɓ��삵�Ȃ�
	WCHAR	dllpath[MAX_PATH];

public:
	opcode_data()
	{
		//int 03h�Ŗ��߂�
		FillMemory(this, sizeof(*this), 0xcc);
	}
	bool initWow64(LPDWORD remoteaddr, LONG orgEIP)	//Wow64��ʼ��
	{
		//WORD���E�`�F�b�N
		C_ASSERT((offsetof(opcode_data, dllpath) & 1) == 0);

		register BYTE* p = code;

#define emit_(t,x)	*(t* UNALIGNED)p = (t)(x); p += sizeof(t)
#define emit_db(b)	emit_(BYTE, b)
#define emit_dw(w)	emit_(WORD, w)
#define emit_dd(d)	emit_(DWORD, d)

		//�Ȃ���GetProcAddress��LoadLibraryW�̃A�h���X�����������Ȃ����Ƃ�����̂�
		//kernel32�̃w�b�_���玩�O�Ŏ擾����
		FARPROC pfn = K32GetProcAddress("LoadLibraryExW");
		if(!pfn)
			return false;

		emit_db(0x60);		//pushad

		/*
			mov eax,fs:[0x30]
			mov eax,[eax+0x0c]
			mov esi,[eax+0x1c]
			lodsd
			move ax,[eax+$08]//���ʱ��eax�б���ľ���k32�Ļ�ַ��
			��win7��õ���KernelBase.dll�ĵ�ַ
		*/
		emit_db(0x64); 
		emit_db(0xA1); 
		emit_db(0x30); 
		emit_db(00); 
		emit_db(00); 
		emit_db(00); 
		emit_db(0x8B); 
		emit_db(0x40); 
		emit_db(0x0C); 
		emit_db(0x8B); 
		emit_db(0x70); 
		emit_db(0x1C); 
		emit_db(0xAD); 
		emit_db(0x8B); 
		emit_db(0x40);
		emit_db(0x08);		//use assemble to fetch kernel base

		emit_dw(0x006A);	//push 0
		emit_dw(0x006A);	//push 0
		emit_db(0x68);		//push dllpath
		emit_dd((LONG)remoteaddr + offsetof(opcode_data, dllpath));
		emit_db(0x05);		//add eax, LoadLibraryExW offset
		emit_dd(pfn);
		emit_dw(0xD0FF);	//call eax

		emit_db(0x61);		//popad
		emit_db(0xE9);		//jmp original_EIP
		emit_dd(orgEIP - (LONG)remoteaddr - (p - code) - sizeof(LONG));

		// gdi++.dll�̃p�X
		bool bDll = !!GetModuleFileNameW(GetDLLInstance(), dllpath, MAX_PATH);
		if (bDll && wcsstr(dllpath, L"64"))
			wcscpy(wcsstr(dllpath, L"64"), wcsstr(dllpath, L"64")+2);
		return bDll;
	}
	bool init32(LPDWORD remoteaddr, LONG orgEIP)	//32λ�����ʼ��
	{
		//WORD���E�`�F�b�N
		C_ASSERT((offsetof(opcode_data, dllpath) & 1) == 0);

		register BYTE* p = code;

#define emit_(t,x)	*(t* UNALIGNED)p = (t)(x); p += sizeof(t)
#define emit_db(b)	emit_(BYTE, b)
#define emit_dw(w)	emit_(WORD, w)
#define emit_dd(d)	emit_(DWORD, d)

		//�Ȃ���GetProcAddress��LoadLibraryW�̃A�h���X�����������Ȃ����Ƃ�����̂�
		//kernel32�̃w�b�_���玩�O�Ŏ擾����
		FARPROC pfn = K32GetProcAddress("LoadLibraryW");
		if(!pfn)
			return false;

		emit_db(0x60);		//pushad
#if _DEBUG
emit_dw(0xC033);	//xor eax, eax
emit_db(0x50);		//push eax
emit_db(0x50);		//push eax
emit_db(0x68);		//push dllpath
emit_dd((LONG)remoteaddr + offsetof(opcode_data, dllpath));
emit_db(0x50);		//push eax
emit_db(0xB8);		//mov eax, MessageBoxW
emit_dd((LONG)MessageBoxW);
emit_dw(0xD0FF);	//call eax
#endif

		emit_db(0x68);		//push dllpath
		emit_dd((LONG)remoteaddr + offsetof(opcode_data, dllpath));
		emit_db(0xB8);		//mov eax, LoadLibraryW
		emit_dd(pfn);
		emit_dw(0xD0FF);	//call eax

		emit_db(0x61);		//popad
		emit_db(0xE9);		//jmp original_EIP
		emit_dd(orgEIP - (LONG)remoteaddr - (p - code) - sizeof(LONG));

		// gdi++.dll�̃p�X
		return !!GetModuleFileNameW(GetDLLInstance(), dllpath, MAX_PATH);
	}

	bool init(DWORD_PTR* remoteaddr, DWORD_PTR orgEIP)
	{
		//WORD���E�`�F�b�N
		C_ASSERT((offsetof(opcode_data, dllpath) & 1) == 0);

		register BYTE* p = code;

#define emit_(t,x)	*(t* UNALIGNED)p = (t)(x); p += sizeof(t)
#define emit_db(b)	emit_(BYTE, b)
#define emit_dw(w)	emit_(WORD, w)
#define emit_dd(d)	emit_(DWORD, d)
#define emit_ddp(dp) emit_(DWORD_PTR, dp)

		//�Ȃ���GetProcAddress��LoadLibraryW�̃A�h���X�����������Ȃ����Ƃ�����̂�
		//kernel32�̃w�b�_���玩�O�Ŏ擾����
		FARPROC pfn = GetProcAddress(GetModuleHandle(L"kernel32.dll"), "LoadLibraryW");
		//if(!pfn)
		//	return false;

		emit_db(0x50);		//push rax
		emit_db(0x51);		//push rcx
		emit_db(0x52);		//push rdx
		emit_db(0x53);		//push rbx
		emit_dd(0x28ec8348);	//sub rsp,28h
		emit_db(0x48);		//mov rcx, dllpath
		emit_db(0xB9);
		emit_ddp((DWORD_PTR)remoteaddr + offsetof(opcode_data, dllpath));
		emit_db(0x48);		//mov rsi, LoadLibraryW
		emit_db(0xBE);		
		emit_ddp(pfn);
		//emit_db(0x48);
		emit_db(0xFF);	//call rdi
		emit_db(0xD6);

		emit_dd(0x28c48348);	//add rsp,28h
		emit_db(0x5B);	
		emit_db(0x5A);	
		emit_db(0x59);	
		emit_db(0x58);		//popad		
		
		emit_db(0x48);		//mov rdi, orgRip
		emit_db(0xBE);
		emit_ddp(orgEIP);
		emit_db(0xFF);		//jmp rdi
		emit_db(0xE6);

		// gdi++.dll�̃p�X

		return !!GetModuleFileNameW(GetDLLInstance(), dllpath, MAX_PATH);
	}

};
#include <poppack.h>

#ifdef _M_IX86
// �~�߂Ă���v���Z�X��LoadLibrary����R�[�h�𒍓�
EXTERN_C BOOL WINAPI GdippInjectDLL(const PROCESS_INFORMATION* ppi)
{
	CONTEXT ctx = { 0 };
	ctx.ContextFlags = CONTEXT_CONTROL;
	//CREATE_SUSPENDED�Ȃ̂Ŋ�{�I�ɐ�������͂�
	if(!GetThreadContext(ppi->hThread, &ctx))
		return false;

	opcode_data local;
	opcode_data* remote = (opcode_data*)VirtualAllocEx(ppi->hProcess, NULL, sizeof(opcode_data), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if(!remote)
		return false;

	if(!local.init32((LPDWORD)remote, ctx.Eip)
		|| !WriteProcessMemory(ppi->hProcess, remote, &local, sizeof(opcode_data), NULL)) {
			VirtualFreeEx(ppi->hProcess, remote, 0, MEM_RELEASE);
			return false;
	}

	FlushInstructionCache(ppi->hProcess, remote, sizeof(opcode_data));
	ctx.Eip = (DWORD)remote;
	return !!SetThreadContext(ppi->hThread, &ctx);
}
#else
EXTERN_C BOOL WINAPI GdippInjectDLL(const PROCESS_INFORMATION* ppi)
{
	BOOL bWow64 = false;
	IsWow64Process(ppi->hProcess, &bWow64);
	if (bWow64)
	{
		WOW64_CONTEXT ctx = { 0 };
		ctx.ContextFlags = CONTEXT_CONTROL;
		//CREATE_SUSPENDED�Ȃ̂Ŋ�{�I�ɐ�������͂�
		if(!Wow64GetThreadContext(ppi->hThread, &ctx))
			return false;

		opcode_data local;
		LPVOID remote = VirtualAllocEx(ppi->hProcess, NULL, sizeof(opcode_data), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if(!remote)
			return false;

		if(!local.initWow64((LPDWORD)remote, ctx.Eip)
			|| !WriteProcessMemory(ppi->hProcess, remote, &local, sizeof(opcode_data), NULL)) {
				VirtualFreeEx(ppi->hProcess, remote, 0, MEM_RELEASE);
				return false;
		}

		FlushInstructionCache(ppi->hProcess, remote, sizeof(opcode_data));
		//FARPROC a=(FARPROC)remote;
		//a();
		ctx.Eip = (DWORD)remote;
		return !!Wow64SetThreadContext(ppi->hThread, &ctx);
	}
	else
	{
		CONTEXT ctx = { 0 };
		ctx.ContextFlags = CONTEXT_CONTROL;
		//CREATE_SUSPENDED�Ȃ̂Ŋ�{�I�ɐ�������͂�
		if(!GetThreadContext(ppi->hThread, &ctx))
			return false;

		opcode_data local;
		LPVOID remote = VirtualAllocEx(ppi->hProcess, NULL, sizeof(opcode_data), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if(!remote)
			return false;

		if(!local.init((DWORD_PTR*)remote, ctx.Rip)
			|| !WriteProcessMemory(ppi->hProcess, remote, &local, sizeof(opcode_data), NULL)) {
				VirtualFreeEx(ppi->hProcess, remote, 0, MEM_RELEASE);
				return false;
		}

		FlushInstructionCache(ppi->hProcess, remote, sizeof(opcode_data));
		//FARPROC a=(FARPROC)remote;
		//a();
		ctx.Rip = (DWORD_PTR)remote;
		return !!SetThreadContext(ppi->hThread, &ctx);
	}
}

#endif

template <typename _TCHAR>
int strlendb(const _TCHAR* psz)
{
	const _TCHAR* p = psz;
	while (*p) {
		for (; *p; p++);
		p++;
	}
	return p - psz + 1;
}

template <typename _TCHAR>
_TCHAR* strdupdb(const _TCHAR* psz, int pad)
{
	int len = strlendb(psz);
	_TCHAR* p = (_TCHAR*)calloc(sizeof(_TCHAR), len + pad);
	if(p) {
		memcpy(p, psz, sizeof(_TCHAR) * len);
	}
	return p;
}



bool MultiSzToArray(LPWSTR p, CArray<LPWSTR>& arr)
{
	for (; *p; ) {
		LPWSTR cp = _wcsdup(p);
		if(!cp || !arr.Add(cp)) {
			free(cp);
			return false;
		}
		for (; *p; p++);
		p++;
	}
	return true;
}

LPWSTR ArrayToMultiSz(CArray<LPWSTR>& arr)
{
	size_t cch = 1;
	for (int i=0; i<arr.GetSize(); i++) {
		cch += wcslen(arr[i]) + 1;
	}

	LPWSTR pmsz = (LPWSTR)calloc(sizeof(WCHAR), cch);
	if (!pmsz)
		return NULL;

	LPWSTR p = pmsz;
	for (int i=0; i<arr.GetSize(); i++) {
		StringCchCopyExW(p, cch, arr[i], &p, &cch, STRSAFE_NO_TRUNCATION);
		p++;
	}
	*p = 0;
	return pmsz;
}

bool AddPathEnv(CArray<LPWSTR>& arr, LPWSTR dir, int dirlen)
{
	for (int i=0; i<arr.GetSize(); i++) {
		LPWSTR env = arr[i];
		if (_wcsnicmp(env, L"PATH=", 5)) {
			continue;
		}

		LPWSTR p = env + 5;
		LPWSTR pp = p;
		for (; ;) {
			for (; *p && *p != L';'; p++);
			int len = p - pp;
			if (len == dirlen && !_wcsnicmp(pp, dir, dirlen)) {
				return false;
			}
			if (!*p)
				break;
			pp = p + 1;
			p++;
		}

		size_t cch = wcslen(env) + MAX_PATH + 4;
		env = (LPWSTR)realloc(env, sizeof(WCHAR) * cch);
		if(env) {
			StringCchCatW(env, cch, L";");
			StringCchCatW(env, cch, dir);
			arr[i] = env;
			return true;
		}
		return false;
	}

	size_t cch = dirlen + sizeof("PATH=") + 1;
	LPWSTR p = (LPWSTR)calloc(sizeof(WCHAR), cch);
	if(p) {
		StringCchCopyW(p, cch, L"PATH=");
		StringCchCatW(p, cch, dir);
		if (arr.Add(p)) {
			return true;
		}
		free(p);
	}
	return false;
}

EXTERN_C LPWSTR WINAPI GdippEnvironment(DWORD& dwCreationFlags, LPVOID lpEnvironment)
{
	TCHAR dir[MAX_PATH];
	int dirlen = GetModuleFileName(GetDLLInstance(), dir, MAX_PATH);
	LPTSTR lpfilename=dir+dirlen;
	while (lpfilename>dir && *lpfilename!=_T('\\') && *lpfilename!=_T('/')) --lpfilename;
	*lpfilename = 0;
	dirlen = wcslen(dir);

	LPWSTR pEnvW = NULL;
	if (lpEnvironment) {
		if (dwCreationFlags & CREATE_UNICODE_ENVIRONMENT) {
			pEnvW = strdupdb((LPCWSTR)lpEnvironment, MAX_PATH + 1);
		} else {
			int alen = strlendb((LPCSTR)lpEnvironment);
			int wlen = MultiByteToWideChar(CP_ACP, 0, (LPCSTR)lpEnvironment, alen, NULL, 0) + 1;
			pEnvW = (LPWSTR)calloc(sizeof(WCHAR), wlen + MAX_PATH + 1);
			if (pEnvW) {
				MultiByteToWideChar(CP_ACP, 0, (LPCSTR)lpEnvironment, alen, pEnvW, wlen);
			}
		}
	} else {
		LPWSTR block = (LPWSTR)GetEnvironmentStringsW();
		if (block) {
			pEnvW = strdupdb(block, MAX_PATH + 1);
			FreeEnvironmentStrings(block);
		}
	}

	if (!pEnvW) {
		return NULL;
	}

	CArray<LPWSTR> envs;
	bool ret = MultiSzToArray(pEnvW, envs);
	free(pEnvW);
	pEnvW = NULL;
	
	if (ret) {
		ret = AddPathEnv(envs, dir, dirlen);
	}
	if (ret) {
		pEnvW = ArrayToMultiSz(envs);
	}

	for (int i=0; i<envs.GetSize(); free(envs[i++]));

	if (!pEnvW) {
		return NULL;
	}

#ifdef _DEBUG
	{
		LPWSTR tmp = strdupdb(pEnvW, 0);
		LPWSTR tmpe = tmp + strlendb(tmp);
		PathRemoveFileSpec(dir);
		for (LPWSTR z=tmp; z<tmpe; z++)if(!*z)*z=L'\n';
			StringCchCatW(dir,MAX_PATH,L"\\");
			StringCchCatW(dir,MAX_PATH,L"gdienv.txt");
			HANDLE hf = CreateFileW(dir,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
			if(hf) {
			DWORD cb;
			WORD w = 0xfeff;
			WriteFile(hf,&w, sizeof(WORD), &cb, 0);
			WriteFile(hf,tmp, sizeof(WCHAR) * (tmpe - tmp), &cb, 0);
			SetEndOfFile(hf);
			CloseHandle(hf);
			free(tmp);
		}
	}
#endif

	dwCreationFlags |= CREATE_UNICODE_ENVIRONMENT;
	return pEnvW;
}