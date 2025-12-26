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
#include <string>
#include "NvFBCLibrary.h"
#include "NvFBC/nvFBC.h"
#include "AdminCheck.h"

using namespace std;

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

    cout << "\n====== NvFBC Status (Adapter " << adapter << ") ======\n";
    cout << "  Running as Administrator:     " << (IsRunningAsAdmin() ? "YES" : "NO") << "\n";

    if (res != NVFBC_SUCCESS)
    {
        cout << "  getStatus() failed with result: 0x" << hex << res << dec << "\n";
        return;
    }

    cout << "  bIsCapturePossible:           " << (status.bIsCapturePossible ? "YES" : "NO") << "\n";
    cout << "  bCurrentlyCapturing:          " << (status.bCurrentlyCapturing ? "YES" : "NO") << "\n";
    cout << "  bSupportMultiHead:            " << (status.bSupportMultiHead ? "YES" : "NO") << "\n";
    cout << "  bSupportConfigurableDiffMap:  " << (status.bSupportConfigurableDiffMap ? "YES" : "NO") << "\n";
    cout << "  bSupportImageClassification:  " << (status.bSupportImageClassification ? "YES" : "NO") << "\n";
    cout << "  dwNvFBCVersion:               0x" << hex << status.dwNvFBCVersion << dec << "\n";

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
    cout << "\nPress Enter to continue...";
    cin.get();
}

void ShowMenu()
{
    cout << "1. Enable NvFBC\n";
    cout << "2. Disable NvFBC\n";
    cout << "3. Check Status\n";
    cout << "4. Exit\n";
    cout << "\nSelect an option (1-4): ";
}

void HandleEnable(NvFBCLibrary* nvfbcLib, int adapter = 0)
{
    cout << "\nAttempting to enable NvFBC on adapter " << adapter << "...\n";

    NvFBCStatusEx statusBefore = {};
    statusBefore.dwVersion = NVFBC_STATUS_VER;
    statusBefore.dwAdapterIdx = adapter;
    nvfbcLib->getStatus(&statusBefore);

    if (statusBefore.bIsCapturePossible)
    {
        cout << "NvFBC is already enabled on adapter " << adapter << ".\n";
        PrintStatus(nvfbcLib, adapter);
        return;
    }

    nvfbcLib->enable(NVFBC_STATE_ENABLE);

    // Wait and check status
    cout << "Waiting for driver to update status...\n";
    Sleep(2000);

    NvFBCStatusEx statusAfter = {};
    statusAfter.dwVersion = NVFBC_STATUS_VER;
    statusAfter.dwAdapterIdx = adapter;
    NVFBCRESULT res = nvfbcLib->getStatus(&statusAfter);

    if (res == NVFBC_SUCCESS && statusAfter.bIsCapturePossible)
    {
        cout << "\nSUCCESS: NvFBC enabled and ready.\n";
    }
    else
    {
        cout << "\nNvFBC enable command succeeded.\n";
        cout << "However, bIsCapturePossible is still false.\n";
        cout << "\nYou may need to RESTART your application for NvFBC to become available.\n";
        cout << "This is normal behavior on some GPU/driver combinations (e.g., RTX 5080).\n";
    }

    PrintStatus(nvfbcLib, adapter);
}

void HandleDisable(NvFBCLibrary* nvfbcLib, int adapter = 0)
{
    cout << "\nAttempting to disable NvFBC on adapter " << adapter << "...\n";

    nvfbcLib->enable(NVFBC_STATE_DISABLE);

    Sleep(1000);

    cout << "NvFBC disable command sent.\n";
    PrintStatus(nvfbcLib, adapter);
}

int main(int argc, char* argv[])
{
    cout << "NvFBCEnable v1.0 - NvFBC Control Utility\n";
    cout << "=========================================\n\n";

    // Load NvFBC library first
    NvFBCLibrary nvfbcLib;
    if (!nvfbcLib.load())
    {
        cerr << "ERROR: Unable to load NvFBC library.\n";
        cerr << "Make sure NVIDIA drivers are installed.\n";
        WaitForEnter();
        return -1;
    }

    cout << "NvFBC library loaded successfully.\n";

    // Interactive mode if no arguments
    if (argc < 2)
    {
        string choice;
        while (true)
        {
            ShowMenu();
            getline(cin, choice);

            if (choice == "1")
            {
                // Check admin for enable
                if (!IsRunningAsAdmin())
                {
                    cerr << "\nERROR: Enabling NvFBC requires Administrator privileges.\n";
                    cerr << "Please run this program as Administrator.\n";
                }
                else
                {
                    HandleEnable(&nvfbcLib, 0);
                }
                cout << "\n";
            }
            else if (choice == "2")
            {
                // Check admin for disable
                if (!IsRunningAsAdmin())
                {
                    cerr << "\nERROR: Disabling NvFBC requires Administrator privileges.\n";
                    cerr << "Please run this program as Administrator.\n";
                }
                else
                {
                    HandleDisable(&nvfbcLib, 0);
                }
                cout << "\n";
            }
            else if (choice == "3")
            {
                PrintStatus(&nvfbcLib, 0);
                cout << "\n";
            }
            else if (choice == "4")
            {
                cout << "\nExiting...\n";
                break;
            }
            else
            {
                cerr << "\nInvalid option. Please select 1-4.\n\n";
            }
        }
        return 0;
    }

    // Command-line mode
    string command = argv[1];

    // Check for admin privileges if enabling/disabling
    if ((command == "-enable" || command == "-disable") && !IsRunningAsAdmin())
    {
        cerr << "ERROR: This operation requires Administrator privileges.\n";
        cerr << "Please run this program as Administrator.\n";
        WaitForEnter();
        return -1;
    }

    if (command == "-status")
    {
        PrintStatus(&nvfbcLib, 0);
    }
    else if (command == "-enable")
    {
        HandleEnable(&nvfbcLib, 0);
    }
    else if (command == "-disable")
    {
        HandleDisable(&nvfbcLib, 0);
    }
    else
    {
        cerr << "ERROR: Unknown command '" << command << "'\n\n";
        PrintUsage();
        WaitForEnter();
        return 1;
    }

    WaitForEnter();
    return 0;
}
