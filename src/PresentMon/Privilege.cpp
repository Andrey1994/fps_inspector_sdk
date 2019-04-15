/*
Copyright 2017-2018 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <windows.h>

#include "Logger.hpp"
#include "Privilege.hpp"

typedef BOOL(WINAPI *OpenProcessTokenProc)(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle);
typedef BOOL(WINAPI *GetTokenInformationProc)(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass, LPVOID TokenInformation, DWORD TokenInformationLength, DWORD *ReturnLength);
typedef BOOL(WINAPI *LookupPrivilegeValueAProc)(LPCSTR lpSystemName, LPCSTR lpName, PLUID lpLuid);
typedef BOOL(WINAPI *AdjustTokenPrivilegesProc)(HANDLE TokenHandle, BOOL DisableAllPrivileges, PTOKEN_PRIVILEGES NewState, DWORD BufferLength, PTOKEN_PRIVILEGES PreviousState, PDWORD ReturnLength);

struct Advapi {
    HMODULE HModule;
    OpenProcessTokenProc OpenProcessToken;
    GetTokenInformationProc GetTokenInformation;
    LookupPrivilegeValueAProc LookupPrivilegeValueA;
    AdjustTokenPrivilegesProc AdjustTokenPrivileges;

    Advapi()
        : HModule(NULL)
    {
    }

    ~Advapi()
    {
        if (HModule != NULL) {
            FreeLibrary(HModule);
        }
    }

    bool Load()
    {
        HModule = LoadLibraryA("advapi32.dll");
        if (HModule == NULL) {
            return false;
        }

        OpenProcessToken = (OpenProcessTokenProc) GetProcAddress(HModule, "OpenProcessToken");
        GetTokenInformation = (GetTokenInformationProc) GetProcAddress(HModule, "GetTokenInformation");
        LookupPrivilegeValueA = (LookupPrivilegeValueAProc) GetProcAddress(HModule, "LookupPrivilegeValueA");
        AdjustTokenPrivileges = (AdjustTokenPrivilegesProc) GetProcAddress(HModule, "AdjustTokenPrivileges");

        if (OpenProcessToken == nullptr ||
            GetTokenInformation == nullptr ||
            LookupPrivilegeValueA == nullptr ||
            AdjustTokenPrivileges == nullptr) {
            FreeLibrary(HModule);
            HModule = NULL;
            return false;
        }

        return true;
    }

    bool HasElevatedPrivilege() const
    {
        HANDLE hToken = NULL;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
            return false;
        }

        /** BEGIN WORKAROUND: struct TOKEN_ELEVATION and enum value TokenElevation
         * are not defined in the vs2003 headers, so we reproduce them here. **/
        enum { WA_TokenElevation = 20 };
        DWORD TokenIsElevated = 0;
        /** END WA **/

        DWORD dwSize = 0;
        if (!GetTokenInformation(hToken, (TOKEN_INFORMATION_CLASS) WA_TokenElevation, &TokenIsElevated, sizeof(TokenIsElevated), &dwSize)) {
            TokenIsElevated = 0;
        }

        CloseHandle(hToken);

        return TokenIsElevated != 0;
    }

    bool EnableDebugPrivilege() const
    {
        HANDLE hToken = NULL;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken)) {
            return false;
        }

        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        bool enabled =
            LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &tp.Privileges[0].Luid) &&
            AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr) &&
            GetLastError() != ERROR_NOT_ALL_ASSIGNED;

        CloseHandle(hToken);

        return enabled;
    }
};

bool CheckPriviliges()
{
    Advapi advapi;

    if (!advapi.Load()) {
        g_InspectorLogger->warn("unable to detect privilige level.");
        return true;
    }

    if (!advapi.HasElevatedPrivilege()) {
        g_InspectorLogger->error("you need to run it with administrator priviliges.");
        return false;
    }

    return true;
}
