#pragma once

#define WINDOWS_LEAN_AND_MEAN
#include <windows.h>

#include "NvFBC/nvFBC.h"
#include "SimpleLogger.h"
#include <string>

#define NVFBC64_LIBRARY_NAME "NvFBC64.dll"
#define NVFBC_LIBRARY_NAME "NvFBC.dll"

// Wraps loading and using NvFBC
class NvFBCLibrary
{
    NvFBCLibrary(const NvFBCLibrary &);
    NvFBCLibrary &operator=(const NvFBCLibrary &);

public:
    NvFBCLibrary()
        : m_handle(NULL)
        , pfn_get_status(NULL)
        , pfn_set_global_flags(NULL)
        , pfn_create(NULL)
        , pfn_enable(NULL)
        , fnIsWow64Process(NULL)
    {}

    ~NvFBCLibrary()
    {
        if(NULL != m_handle)
            close();
    }

    // Attempts to load NvFBC from system directory.
    // on 32-bit OS: looks for NvFBC.dll in system32
    // for 32-bit app on 64-bit OS: looks for NvFBC.dll in syswow64
    // for 64-bit app on 64-bit OS: looks for NvFBC64.dll in system32
    bool load(std::string fileName = std::string())
    {
        if(NULL != m_handle)
        {
            LOG("NvFBC library already loaded");
            return true;
        }

        if(!fileName.empty())
        {
            LOG("Attempting to load NvFBC from: %s", fileName.c_str());
            m_handle = ::LoadLibraryA(fileName.c_str());
        }

        if(NULL == m_handle)
        {
            std::string defaultPath = getDefaultPath();
            LOG("Attempting to load NvFBC from default path: %s", defaultPath.c_str());
            m_handle = ::LoadLibraryA(defaultPath.c_str());
        }

        if(NULL == m_handle)
        {
            LOGERR("Unable to load NvFBC (error: %d)", GetLastError());
            return false;
        }

        // Load the three functions exported by NvFBC
        pfn_create = (NvFBC_CreateFunctionExType)::GetProcAddress(m_handle, "NvFBC_CreateEx");
        pfn_set_global_flags = (NvFBC_SetGlobalFlagsType)::GetProcAddress(m_handle, "NvFBC_SetGlobalFlags");
        pfn_get_status = (NvFBC_GetStatusExFunctionType)::GetProcAddress(m_handle, "NvFBC_GetStatusEx");
        pfn_enable = (NvFBC_EnableFunctionType)::GetProcAddress(m_handle,"NvFBC_Enable");

        if((NULL == pfn_create) || (NULL == pfn_set_global_flags) || (NULL == pfn_get_status) || (NULL == pfn_enable))
        {
            LOGERR("Unable to load NvFBC function pointers (create:%p, flags:%p, status:%p, enable:%p)", 
                   pfn_create, pfn_set_global_flags, pfn_get_status, pfn_enable);
            close();

            return false;
        }

        LOG("NvFBC library loaded successfully");
        return true;
    }

    // Close the NvFBC dll
    void close()
    {
        if(NULL != m_handle)
        {
            FreeLibrary(m_handle);
            LOG("NvFBC library closed");
        }

        m_handle = NULL;
        pfn_create = NULL;
        pfn_get_status = NULL;
        pfn_enable  = NULL;
    }

    // Get the status for the provided adapter, if no adapter is 
    // provided the default adapter is used.
    NVFBCRESULT getStatus(NvFBCStatusEx *status)
    {
        return pfn_get_status((void*)status);
    }

    // Sets the global flags for the provided adapter, if 
    // no adapter is provided the default adapter is used
    void setGlobalFlags(DWORD flags, int adapter = 0)
    {
        setTargetAdapter(adapter);
        pfn_set_global_flags(flags);
        LOG("Set NvFBC global flags: 0x%X (adapter: %d)", flags, adapter);
    }

    // Creates an instance of the provided NvFBC type if possible
    NVFBCRESULT createEx(NvFBCCreateParams *pParams)
    {
        return pfn_create((void *)pParams);
    }
    // Creates an instance of the provided NvFBC type if possible.  
    void *create(DWORD type, DWORD *maxWidth, DWORD *maxHeight, int adapter = 0, void *devicePtr = NULL)
    {
        if(NULL == m_handle)
        {
            LOGERR("Cannot create NvFBC instance - library not loaded");
            return NULL;
        }

        NVFBCRESULT res = NVFBC_SUCCESS;
        NvFBCStatusEx status = {0};
        status.dwVersion = NVFBC_STATUS_VER;
        status.dwAdapterIdx = adapter;
        res = getStatus(&status);

        if (res != NVFBC_SUCCESS)
        {
            LOGERR("NvFBC getStatus failed (result: 0x%X)", res);
            return NULL;
        }

        // Check to see if the device and driver are supported
        if(!status.bIsCapturePossible)
        {
            LOGERR("NvFBC not enabled (bIsCapturePossible=false)");
            return NULL;
        }

        // Check to see if an instance can be created
        if(!status.bCanCreateNow)
        {
            LOGERR("NvFBC not enabled (bCanCreateNow=false)");
            return NULL;
        }

        NvFBCCreateParams createParams;
        memset(&createParams, 0, sizeof(createParams));
        createParams.dwVersion = NVFBC_CREATE_PARAMS_VER;
        createParams.dwInterfaceType = type;
        createParams.pDevice = devicePtr;
        createParams.dwAdapterIdx = adapter;

        res = pfn_create(&createParams);
        
        if(res == NVFBC_SUCCESS)
        {
            LOG("NvFBC instance created successfully (type: 0x%X, adapter: %d, maxRes: %dx%d)", 
                type, adapter, createParams.dwMaxDisplayWidth, createParams.dwMaxDisplayHeight);
        }
        else
        {
            LOGERR("Failed to create NvFBC instance (result: 0x%X)", res);
        }

        *maxWidth = createParams.dwMaxDisplayWidth;
        *maxHeight = createParams.dwMaxDisplayHeight;
        
        return createParams.pNvFBC;
    }

    // enable/disable NVFBC
    void enable(NVFBC_STATE nvFBCState)
    {
        NVFBCRESULT res = NVFBC_SUCCESS;
        res = pfn_enable(nvFBCState);

        if (res != NVFBC_SUCCESS)
        {
            LOGERR("Failed to %s NvFBC - insufficient privilege (result: 0x%X)", 
                   nvFBCState == 0 ? "disable" : "enable", res);
            return;
        }
        else
        {
            LOG("NvFBC is %s", nvFBCState == 0 ? "disabled" : "enabled");
            return;
        }
    }

protected:
    // Get the default NvFBC library path
    typedef BOOL (WINAPI *pfnIsWow64Process) (HANDLE, PBOOL);
    pfnIsWow64Process fnIsWow64Process = NULL;

    BOOL IsWow64()
    {
        BOOL bIsWow64 = FALSE;

        fnIsWow64Process = (pfnIsWow64Process) GetProcAddress(
            GetModuleHandle(TEXT("kernel32.dll")),"IsWow64Process");
      
        if (NULL != fnIsWow64Process)
        {
            if (!fnIsWow64Process(GetCurrentProcess(),&bIsWow64))
            {
                bIsWow64 = false;
            }
        }
        return bIsWow64;
    }

    std::string getDefaultPath()
    {
        std::string defaultPath;

        size_t pathSize;
        char *libPath;

        if(0 != _dupenv_s(&libPath, &pathSize, "SystemRoot"))
        {
            LOGERR("Unable to get the SystemRoot environment variable");
            return defaultPath;
        }

        if(0 == pathSize)
        {
            LOGERR("The SystemRoot environment variable is not set");
            return defaultPath;
        }
#ifdef _WIN64
        defaultPath = std::string(libPath) + "\\System32\\" + NVFBC64_LIBRARY_NAME;
#else
        if (IsWow64())
        {
            defaultPath = std::string(libPath) + "\\Syswow64\\" + NVFBC_LIBRARY_NAME;
        }
        else
        {
            defaultPath = std::string(libPath) + "\\System32\\" + NVFBC_LIBRARY_NAME;            
        }
#endif
        return defaultPath;
    }

    void setTargetAdapter(int adapter = 0)
    {
        char targetAdapter[10] = {0};
        _snprintf_s(targetAdapter, 10, 9, "%d", adapter);
        SetEnvironmentVariableA("NVFBC_TARGET_ADAPTER", targetAdapter);
    }


protected:
    HMODULE                       m_handle = NULL;
    NvFBC_GetStatusExFunctionType pfn_get_status = NULL;
    NvFBC_SetGlobalFlagsType      pfn_set_global_flags = NULL;
    NvFBC_CreateFunctionExType    pfn_create = NULL;
    NvFBC_EnableFunctionType      pfn_enable = NULL;
};
