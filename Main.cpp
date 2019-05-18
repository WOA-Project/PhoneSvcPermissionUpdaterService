#include <string>
#include "Main.h"

int _tmain(int argc, TCHAR* argv[])
{
	SERVICE_TABLE_ENTRY ServiceTable[] =
	{
		{SERVICE_NAME, (LPSERVICE_MAIN_FUNCTION)ServiceMain},
		{NULL, NULL}
	};

	if (StartServiceCtrlDispatcher(ServiceTable) == FALSE)
	{
		return GetLastError();
	}

	return 0;
}


VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv)
{
	DWORD Status = E_FAIL;
	HANDLE hThread;

	g_StatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceCtrlHandler);

	if (g_StatusHandle == NULL)
	{
		goto EXIT;
	}

	// Tell the service controller we are starting
	ZeroMemory(&g_ServiceStatus, sizeof(g_ServiceStatus));
	g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	g_ServiceStatus.dwControlsAccepted = 0;
	g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
	g_ServiceStatus.dwWin32ExitCode = 0;
	g_ServiceStatus.dwServiceSpecificExitCode = 0;
	g_ServiceStatus.dwCheckPoint = 0;

	SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

	/*
	 * Perform tasks neccesary to start the service here
	 */

	// Create stop event to wait on later.
	g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (g_ServiceStopEvent == NULL)
	{
		g_ServiceStatus.dwControlsAccepted = 0;
		g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
		g_ServiceStatus.dwWin32ExitCode = GetLastError();
		g_ServiceStatus.dwCheckPoint = 1;

		goto EXIT;
	}

	// Tell the service controller we are started
	g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
	g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
	g_ServiceStatus.dwWin32ExitCode = 0;
	g_ServiceStatus.dwCheckPoint = 0;

	SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

	// Start the thread that will perform the main task of the service
	hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);

	// Wait until our worker thread exits effectively signaling that the service needs to stop
	WaitForSingleObject(hThread, INFINITE);

	/*
	 * Perform any cleanup tasks
	 */
	CloseHandle(g_ServiceStopEvent);

	g_ServiceStatus.dwControlsAccepted = 0;
	g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
	g_ServiceStatus.dwWin32ExitCode = 0;
	g_ServiceStatus.dwCheckPoint = 3;

	SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

EXIT:
	return;
}


VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode)
{
	switch (CtrlCode)
	{
	case SERVICE_CONTROL_STOP:

		if (g_ServiceStatus.dwCurrentState != SERVICE_RUNNING)
			break;

		/*
		 * Perform tasks neccesary to stop the service here
		 */

		g_ServiceStatus.dwControlsAccepted = 0;
		g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
		g_ServiceStatus.dwWin32ExitCode = 0;
		g_ServiceStatus.dwCheckPoint = 4;

		// This will signal the worker thread to start shutting down
		SetEvent(g_ServiceStopEvent);

		break;

	default:
		break;
	}
}

LONG GetStringRegKey(HKEY hKey, const std::wstring& strValueName, std::wstring& strValue, const std::wstring& strDefaultValue)
{
	strValue = strDefaultValue;
	WCHAR szBuffer[512];
	DWORD dwBufferSize = sizeof(szBuffer);
	ULONG nError;
	nError = RegQueryValueExW(hKey, strValueName.c_str(), 0, NULL, (LPBYTE)szBuffer, &dwBufferSize);
	if (ERROR_SUCCESS == nError)
	{
		strValue = szBuffer;
	}
	return nError;
}

LONG SetStringRegKey(HKEY hKey, const std::wstring& strValueName, const std::wstring& strValue, DWORD Type)
{
	ULONG nError;
	if (REG_MULTI_SZ == Type)
		nError = RegSetValueExW(hKey, strValueName.c_str(), 0, Type, (const BYTE*)strValue.c_str(), (strValue.size() + 1) * sizeof(wchar_t) + 2);
	else
		nError = RegSetValueExW(hKey, strValueName.c_str(), 0, Type, (const BYTE*)strValue.c_str(), (strValue.size() + 1) * sizeof(wchar_t));
	return nError;
}

DWORD WINAPI ServiceWorkerThread(LPVOID lpParam)
{
	HKEY hKeyPhoneSvc;
	LONG lRes = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\PhoneSvc", 0, KEY_READ | KEY_WRITE, &hKeyPhoneSvc);
	if (ERROR_SUCCESS != lRes)
	{
		return E_FAIL;
	}

	std::wstring strUserAccount;
	lRes = GetStringRegKey(hKeyPhoneSvc, L"ObjectName", strUserAccount, L"");
	if (ERROR_SUCCESS != lRes)
	{
		return E_FAIL;
	}

	bool Changed = FALSE;

	if (strUserAccount != L"LocalSystem")
	{
		Changed = TRUE;

		lRes = SetStringRegKey(hKeyPhoneSvc, L"ObjectName", L"LocalSystem", REG_SZ);
		if (ERROR_SUCCESS != lRes)
		{
			return E_FAIL;
		}
		lRes = SetStringRegKey(hKeyPhoneSvc, L"ImagePath", L"%SystemRoot%\\system32\\svchost.exe -k PhoneGroup -p", REG_EXPAND_SZ);
		if (ERROR_SUCCESS != lRes)
		{
			return E_FAIL;
		}

		HKEY hKeySvcHost;
		lRes = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Svchost", 0, KEY_READ | KEY_WRITE, &hKeySvcHost);
		if (ERROR_SUCCESS != lRes)
		{
			return E_FAIL;
		}

		const char* value = "PhoneSvc";
		const size_t cSize = strlen(value) + 1;
		wchar_t* val = new wchar_t[cSize];

		size_t outSize;
		mbstowcs_s(&outSize, val, cSize, value, cSize - 1);

		lRes = SetStringRegKey(hKeySvcHost, L"PhoneGroup", val, REG_MULTI_SZ);
		if (ERROR_SUCCESS != lRes)
		{
			return E_FAIL;
		}

		RegCloseKey(hKeySvcHost);
	}

	RegCloseKey(hKeyPhoneSvc);

	if (Changed)
	{
		// Restart service
		std::string Service = "PhoneSvc";
		SERVICE_STATUS Status;

		SC_HANDLE SCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
		SC_HANDLE SHandle = OpenService(SCManager, Service.c_str(), SC_MANAGER_ALL_ACCESS);

		if (SHandle == NULL)
		{
			CloseServiceHandle(SCManager);
			return E_FAIL;
		}

		if (!ControlService(SHandle, SERVICE_CONTROL_STOP, &Status))
		{
			CloseServiceHandle(SCManager);
			CloseServiceHandle(SHandle);
			return E_FAIL;
		}

		do
		{
			QueryServiceStatus(SHandle, &Status);
		} while (Status.dwCurrentState != SERVICE_STOPPED);


		if (!StartService(SHandle, 0, NULL))
		{
			CloseServiceHandle(SCManager);
			CloseServiceHandle(SHandle);
			return E_FAIL;
		}

		CloseServiceHandle(SCManager);
		CloseServiceHandle(SHandle);
	}
	return ERROR_SUCCESS;
}