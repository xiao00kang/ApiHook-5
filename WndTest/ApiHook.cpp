#include "ApiHook.h"
#include <TlHelp32.h>

#pragma push_macro("Process32First")
#pragma push_macro("Process32Next")
#pragma push_macro("PROCESSENTRY32")
#pragma push_macro("PPROCESSENTRY32")
#pragma push_macro("LPPROCESSENTRY32")
#undef Process32First
#undef Process32Next
#undef PROCESSENTRY32
#undef PPROCESSENTRY32
#undef LPPROCESSENTRY32

static const size_t jmpCmdSize = sizeof(void*) + 1;
static const size_t hookFunctionEntrySize = jmpCmdSize << 1;

static int EnableDebugPriv()
{
	HANDLE hToken;   //进程令牌句柄
	TOKEN_PRIVILEGES tp;  //TOKEN_PRIVILEGES结构体，其中包含一个【类型+操作】的权限数组
	LUID luid;       //上述结构体中的类型值

					 //打开进程令牌环
					 //GetCurrentProcess()获取当前进程的伪句柄，只会指向当前进程或者线程句柄，随时变化
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
		return -8;

	//获得本地进程name所代表的权限类型的局部唯一ID
	if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid))
		return -9;

	tp.PrivilegeCount = 1;    //权限数组中只有一个“元素”
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;  //权限操作
	tp.Privileges[0].Luid = luid;   //权限类型

									//调整进程权限
	if (!AdjustTokenPrivileges(hToken, 0, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL))
		return -10;
	return 0;
}

static HANDLE FindProcess(const char* name)
{
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
	if (hSnap == INVALID_HANDLE_VALUE)
		return INVALID_HANDLE_VALUE;

	PROCESSENTRY32 pe32;

	BOOL result = Process32First(hSnap, &pe32);
	while (result != FALSE)
	{
		if (strcmp(name, pe32.szExeFile) == 0)
		{
			HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pe32.th32ProcessID);
			if (hProcess)
			{
				::CloseHandle(hSnap);
				return hProcess;
			}
		}
		result = Process32Next(hSnap, &pe32);
	}

	::CloseHandle(hSnap);
	return INVALID_HANDLE_VALUE;
}

static HANDLE FindProcess(const wchar_t* name)
{
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
	if (hSnap == INVALID_HANDLE_VALUE)
		return INVALID_HANDLE_VALUE;

	PROCESSENTRY32W pe32;

	BOOL result = Process32FirstW(hSnap, &pe32);
	while (result != FALSE)
	{
		if (wcscmp(name, pe32.szExeFile) == 0)
		{
			HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pe32.th32ProcessID);
			if (hProcess)
			{
				::CloseHandle(hSnap);
				return hProcess;
			}
		}
		result = Process32NextW(hSnap, &pe32);
	}

	::CloseHandle(hSnap);
	return INVALID_HANDLE_VALUE;
}

template<typename _Ty>
static void writeMemoryOffset(void* pData, size_t offset, const _Ty& val)
{
	*(_Ty*)((char*)pData + offset) = val;
}

MemoryPointer::MemoryPointer()
	:offset(0)
{

}

MemoryPointer::MemoryPointer(size_t offset)
	: offset(offset)
{

}

MemoryPointer::MemoryPointer(const void* pData)
	: offset((size_t)pData)
{

}

size_t calJmpOffset(MemoryPointer src, MemoryPointer dest, size_t skip /*= 0*/)
{
	return dest.offset - src.offset - skip;
}

bool writeMemoryForce(const char* buf, size_t size, void* pDest)
{
	HANDLE hProcess = GetCurrentProcess();
	DWORD oldAccess = NULL;
	BOOL result = VirtualProtectEx(hProcess, pDest, size, PAGE_READWRITE, &oldAccess);
	if (result == FALSE)
		return false;
	WriteProcessMemory(hProcess, (void*)buf, pDest, size, NULL);
	result = VirtualProtectEx(hProcess, pDest, size, oldAccess, &oldAccess);
	return result != FALSE;
}

bool readMemoryForce(char* buf, size_t size, void* pReadbuf)
{
	HANDLE hProcess = GetCurrentProcess();
	DWORD oldAccess = NULL;
	BOOL result = VirtualProtectEx(hProcess, pReadbuf, size, PAGE_EXECUTE_READWRITE, &oldAccess);
	if (result == FALSE)
		return false;
	ReadProcessMemory(hProcess, pReadbuf, buf, size, NULL);
	result = VirtualProtectEx(hProcess, pReadbuf, size, oldAccess, &oldAccess);
	return result != FALSE;
}

bool setExecuteMemory(void* pData, size_t size)
{
	DWORD oldAccess = NULL;
	return VirtualProtectEx(GetCurrentProcess(), pData, size, PAGE_EXECUTE_READ, &oldAccess) != FALSE;
}

void* allocFunctionEntry()
{
	char* ptr = new char[hookFunctionEntrySize];
	memset(ptr, 0, hookFunctionEntrySize);

	DWORD oldAccess;
	VirtualProtect(ptr, hookFunctionEntrySize, PAGE_EXECUTE_READWRITE, &oldAccess);
	return ptr;
}

void freeFunctionEntry(void* pEntry)
{
	if (pEntry == NULL)
		return;
	delete[](char*)pEntry;
}

ApiHook setApiHook(void* pOldApi, void* pNewApi)
{
	if (pOldApi == NULL || pNewApi == NULL)
	{
		_set_errno(EINVAL);
		return NULL;
	}
	char* pEntry = (char*)allocFunctionEntry();
	//hook部分代码写入新地址
	memcpy_s(pEntry, jmpCmdSize, pOldApi, jmpCmdSize);
	//新地址后半部分改为jmp到原地址+5
	pEntry[jmpCmdSize] = (char)0xE9;
	size_t originAddr = calJmpOffset(&pEntry[jmpCmdSize], (size_t)pOldApi + jmpCmdSize, jmpCmdSize);

	writeMemoryOffset(pEntry, jmpCmdSize + 1, originAddr);
	HANDLE hProcess = GetCurrentProcess();
	DWORD oldAccess;
	VirtualProtectEx(hProcess, pOldApi, jmpCmdSize, PAGE_READWRITE, &oldAccess);
	//设置原始api跳转到新api
	size_t newAddr = calJmpOffset(pOldApi, pNewApi, jmpCmdSize);
	((char*)pOldApi)[0] = (char)0xE9;
	writeMemoryOffset(pOldApi, 1, newAddr);
	VirtualProtectEx(hProcess, pOldApi, jmpCmdSize, oldAccess, &oldAccess);

	ApiHook hook = {};
	hook.pEntry = pEntry;
	//返回重定向入口地址
	return hook;
}

ApiHook setDllApiHook(const char* pOldApi, void* pNewApi, HMODULE hModule)
{
	void* pFunc = ::GetProcAddress(hModule, pOldApi);
	return setApiHook(pFunc, pNewApi);
}

void unhookApiHook(ApiHook pEntry, void* pOldApi)
{
	HANDLE hProcess = GetCurrentProcess();
	DWORD oldAccess;
	VirtualProtectEx(hProcess, pOldApi, jmpCmdSize, PAGE_READWRITE, &oldAccess);
	WriteProcessMemory(hProcess, pEntry.pEntry, pOldApi, jmpCmdSize, NULL);
	VirtualProtectEx(hProcess, pOldApi, jmpCmdSize, oldAccess, &oldAccess);
	freeFunctionEntry(pEntry.pEntry);
	pEntry.pEntry = NULL;
}

ApiHook::ApiHook()
	:pEntry(NULL)
{

}

ApiHook::ApiHook(std::nullptr_t)
	: pEntry(NULL)
{

}

bool apiHookInit()
{
	HANDLE hToken;   //进程令牌句柄
	TOKEN_PRIVILEGES tp;  //TOKEN_PRIVILEGES结构体，其中包含一个【类型+操作】的权限数组
	LUID luid;       //上述结构体中的类型值
					 //打开进程令牌环
					 //GetCurrentProcess()获取当前进程的伪句柄，只会指向当前进程或者线程句柄，随时变化
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
		return false;

	//获得本地进程name所代表的权限类型的局部唯一ID
	if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid))
		return false;

	tp.PrivilegeCount = 1;    //权限数组中只有一个“元素”
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;  //权限操作
	tp.Privileges[0].Luid = luid;   //权限类型

									//调整进程权限
	if (!AdjustTokenPrivileges(hToken, 0, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL))
		return false;
	return true;
}

InjectProcess::InjectProcess()
	:hProcess(NULL)
{

}

InjectProcess::InjectProcess(const char* name)
	: hProcess(NULL)
{
	hProcess = FindProcess(name);
}
InjectProcess::InjectProcess(const wchar_t* name)
	: hProcess(NULL)
{
	hProcess = FindProcess(name);
}
InjectProcess::InjectProcess(DWORD pid)
	: hProcess(OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid))
{

}

int injectDll(InjectProcess process, const char* injectDll)
{
	//获得调试权限
	if (EnableDebugPriv() != 0)
		return -1;

	char buf[1024] = {};
	::GetModuleFileNameA(GetModuleHandle(NULL), buf, sizeof(buf));
	char* ptr = strrchr(buf, '\\');
	if (ptr)
		*ptr = 0;
	strcat_s(buf, "\\");
	strcat_s(buf, injectDll);

	HANDLE hRemoteProcess = process.hProcess;
	LPVOID pRemoteDllPath = VirtualAllocEx(hRemoteProcess, NULL, strlen(buf) + 1, MEM_COMMIT, PAGE_READWRITE);
	if (pRemoteDllPath == NULL)
		return -3;

	//远程dll路径写入
	size_t Size;
	if (WriteProcessMemory(hRemoteProcess, pRemoteDllPath, buf, strlen(buf) + 1, (SIZE_T*)&Size) == NULL)
		return -4;

	//获得远程进程中LoadLibrary()的地址
	LPTHREAD_START_ROUTINE pLoadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandle(TEXT("kernel32.dll")), "LoadLibraryA");
	if (pLoadLibrary == NULL)
		return -5;

	//启动远程线程
	DWORD dwThreadId;
	HANDLE hThread;
	if ((hThread = CreateRemoteThread(hRemoteProcess, NULL, 0, pLoadLibrary, pRemoteDllPath, 0, &dwThreadId)) == NULL)
		return -6;
	else
		::WaitForSingleObject(hThread, INFINITE);

	//释放分配内存
	if (VirtualFreeEx(hRemoteProcess, pRemoteDllPath, 0, MEM_RELEASE) == 0)
		return -8;

	//释放句柄
	if (hThread != NULL) CloseHandle(hThread);
	if (hRemoteProcess != NULL) CloseHandle(hRemoteProcess);

	return 0;
}

#pragma pop_macro("Process32First")
#pragma pop_macro("Process32Next")
#pragma pop_macro("PROCESSENTRY32")
#pragma pop_macro("PPROCESSENTRY32")
#pragma pop_macro("LPPROCESSENTRY32")