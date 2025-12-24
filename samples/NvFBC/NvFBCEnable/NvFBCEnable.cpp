/*!
 * \brief NvFBCEnable - Utility to enable/disable NvFBC feature
 *
 * Usage:
 *   NvFBCEnable.exe -enable     Enable NvFBC
 *   NvFBCEnable.exe -disable    Disable NvFBC
 *   NvFBCEnable.exe -status     Check NvFBC status
 *
 * Must be run with Administrator privileges.
 */

#include <windows.h>
#include <iostream>
#include <limits>
#include "NvFBCLibrary.h"
#include "NvFBC/nvFBC.h"

using namespace std;

bool IsRunningAsAdmin()
{
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;

    // Allocate and initialize a SID for the administrators group
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(
            &ntAuthority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &adminGroup))
    {
        // Check if the current process token is a member of the administrators group
        if (!CheckTokenMembership(NULL, adminGroup, &isAdmin))
        {
            isAdmin = FALSE;
        }
        FreeSid(adminGroup);
    }

    return isAdmin;
}

void PrintUsage()
{
    cout << "NvFBCEnable - Utility to enable/disable NVIDIA Frame Buffer Capture\n";
    cout << "\nUsage:\n";
    cout << "  NvFBCEnable.exe -enable   Enable NvFBC\n";
    cout << "  NvFBCEnable.exe -disable  Disable NvFBC\n";
    cout << "  NvFBCEnable.exe -status   Check NvFBC status\n";
    cout << "\nNote: Must be run with Administrator privileges.\n";
}

void PrintStatus(NvFBCLibrary* pLib, int adapter = 0)
{
    NvFBCStatusEx status = {};
    status.dwVersion = NVFBC_STATUS_VER;
    status.dwAdapterIdx = adapter;

    NVFBCRESULT res = pLib->getStatus(&status);

    cout << "\n=== NvFBC Status (Adapter " << adapter << ") ===\n";

    if (res != NVFBC_SUCCESS)
    {
        cout << "  getStatus() failed with result: 0x" << hex << res << dec << "\n";
        return;
    }

    cout << "  bIsCapturePossible:        " << (status.bIsCapturePossible ? "YES" : "NO") << "\n";
    cout << "  bCurrentlyCapturing:       " << (status.bCurrentlyCapturing ? "YES" : "NO") << "\n";
    cout << "  bSupportMultiHead:         " << (status.bSupportMultiHead ? "YES" : "NO") << "\n";
    cout << "  bSupportConfigurableDiffMap: " << (status.bSupportConfigurableDiffMap ? "YES" : "NO") << "\n";
    cout << "  bSupportImageClassification: " << (status.bSupportImageClassification ? "YES" : "NO") << "\n";
    cout << "  dwNvFBCVersion:            0x" << hex << status.dwNvFBCVersion << dec << "\n";

    if (status.bIsCapturePossible)
    {
        cout << "\n  Status: NvFBC is ENABLED and ready to use.\n";
    }
    else
    {
        cout << "\n  Status: NvFBC is DISABLED or not available.\n";
        cout << "         Run with -enable to enable it (requires Administrator).\n";
    }
}

void WaitForEnter()
{
    cout << "\nPress Enter to exit...";
    cin.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');
    cin.get();
}

int main(int argc, char* argv[])
{
    cout << "NvFBCEnable v1.0 - NvFBC Control Utility\n";
    cout << "=========================================\n\n";

    if (argc < 2)
    {
        PrintUsage();
        WaitForEnter();
        return 1;
    }

    string command = argv[1];

    // Check for admin privileges if enabling/disabling
    if ((command == "-enable" || command == "-disable") && !IsRunningAsAdmin())
    {
        cerr << "ERROR: This operation requires Administrator privileges.\n";
        cerr << "Please run this program as Administrator.\n";
        WaitForEnter();
        return -1;
    }

    // Load NvFBC library
    NvFBCLibrary nvfbcLib;
    if (!nvfbcLib.load())
    {
        cerr << "ERROR: Unable to load NvFBC library.\n";
        cerr << "Make sure NVIDIA drivers are installed.\n";
        WaitForEnter();
        return -1;
    }

    cout << "NvFBC library loaded successfully.\n";

    if (command == "-status")
    {
        PrintStatus(&nvfbcLib, 0);
        WaitForEnter();
    }
    else if (command == "-enable")
    {
        cout << "\nAttempting to enable NvFBC...\n";

        NvFBCStatusEx statusBefore = {};
        statusBefore.dwVersion = NVFBC_STATUS_VER;
        statusBefore.dwAdapterIdx = 0;
        nvfbcLib.getStatus(&statusBefore);

        if (statusBefore.bIsCapturePossible)
        {
            cout << "NvFBC is already enabled.\n";
            PrintStatus(&nvfbcLib, 0);
            WaitForEnter();
            return 0;
        }

        nvfbcLib.enable(NVFBC_STATE_ENABLE);

        // Wait and check status
        cout << "Waiting for driver to update status...\n";
        Sleep(2000);

        NvFBCStatusEx statusAfter = {};
        statusAfter.dwVersion = NVFBC_STATUS_VER;
        statusAfter.dwAdapterIdx = 0;
        NVFBCRESULT res = nvfbcLib.getStatus(&statusAfter);

        if (res == NVFBC_SUCCESS && statusAfter.bIsCapturePossible)
        {
            cout << "\nSUCCESS: NvFBC enabled and ready immediately!\n";
        }
        else
        {
            cout << "\nNvFBC enable command succeeded.\n";
            cout << "However, bIsCapturePossible is still false.\n";
            cout << "\nYou may need to RESTART your application for NvFBC to become available.\n";
            cout << "This is normal behavior on some GPU/driver combinations (e.g., RTX 5080).\n";
        }

        PrintStatus(&nvfbcLib, 0);
        WaitForEnter();
    }
    else if (command == "-disable")
    {
        cout << "\nAttempting to disable NvFBC...\n";

        nvfbcLib.enable(NVFBC_STATE_DISABLE);

        Sleep(1000);

        cout << "NvFBC disable command sent.\n";
        PrintStatus(&nvfbcLib, 0);
        WaitForEnter();
    }
    else
    {
        cerr << "ERROR: Unknown command '" << command << "'\n\n";
        PrintUsage();
        WaitForEnter();
        return 1;
    }

    return 0;
}
