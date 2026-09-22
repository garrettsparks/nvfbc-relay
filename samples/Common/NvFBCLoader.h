/*!
 * \brief Loads the NVIDIA frame buffer capture library at run time
 */

#pragma once

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "NvFBC/nvFBC.h"
#include "SimpleLogger.h"

#ifndef _WIN64
#error NvFBCLoader loads NvFBC64.dll and builds for x64 only
#endif

// Loads NvFBC64.dll and forwards to its exports. The DLL is loaded by its full System32 path,
// never by bare name, so a DLL of that name placed beside the exe cannot load in its place.
// Call load() before anything else.
class NvFBCLoader
{
public:
    NvFBCLoader() {}
    ~NvFBCLoader() { close(); }

    NvFBCLoader(const NvFBCLoader &) = delete;
    NvFBCLoader &operator=(const NvFBCLoader &) = delete;

    // Loads the DLL and resolves its exports. Returns false, with a log line, when the DLL or
    // any export is missing.
    bool load()
    {
        if (m_module)
        {
            LOG("NvFBC library already loaded");
            return true;
        }

        const std::string path = systemPath();
        if (path.empty())
        {
            return false;
        }

        LOG("Attempting to load NvFBC from default path: %s", path.c_str());
        m_module = ::LoadLibraryA(path.c_str());
        if (!m_module)
        {
            LOGERR("Unable to load NvFBC (error: %d)", GetLastError());
            return false;
        }

        m_create = reinterpret_cast<NvFBC_CreateFunctionExType>(::GetProcAddress(m_module, "NvFBC_CreateEx"));
        m_getStatus = reinterpret_cast<NvFBC_GetStatusExFunctionType>(::GetProcAddress(m_module, "NvFBC_GetStatusEx"));
        m_enable = reinterpret_cast<NvFBC_EnableFunctionType>(::GetProcAddress(m_module, "NvFBC_Enable"));
        // Nothing here calls NvFBC_SetGlobalFlags, but a DLL without it is refused like one
        // missing any other export.
        const FARPROC setGlobalFlags = ::GetProcAddress(m_module, "NvFBC_SetGlobalFlags");

        if (!m_create || !setGlobalFlags || !m_getStatus || !m_enable)
        {
            LOGERR("Unable to load NvFBC function pointers (create:%p, flags:%p, status:%p, enable:%p)",
                   m_create, setGlobalFlags, m_getStatus, m_enable);
            close();
            return false;
        }

        LOG("NvFBC library loaded successfully");
        return true;
    }

    // Unloads the DLL. Does nothing when it is not loaded.
    void close()
    {
        if (m_module)
        {
            ::FreeLibrary(m_module);
            LOG("NvFBC library closed");
        }

        m_module = NULL;
        m_create = NULL;
        m_getStatus = NULL;
        m_enable = NULL;
    }

    // Fills in the status of the adapter that status->dwAdapterIdx names.
    NVFBCRESULT getStatus(NvFBCStatusEx *status)
    {
        return m_getStatus(status);
    }

    // Turns NvFBC on or off.
    NVFBCRESULT enable(NVFBC_STATE state)
    {
        const NVFBCRESULT res = m_enable(state);

        if (res != NVFBC_SUCCESS)
        {
            LOGERR("Failed to %s NvFBC - insufficient privilege (result: 0x%X)",
                   state == NVFBC_STATE_DISABLE ? "disable" : "enable", res);
        }
        else
        {
            LOG("NvFBC is %s", state == NVFBC_STATE_DISABLE ? "disabled" : "enabled");
        }

        return res;
    }

    // Creates a capture session of the given interface type on an adapter, bound to a device.
    // Refuses, with a log line, unless the driver reports capture possible and a session
    // creatable now. Returns the session and the largest display size it captures, or NULL.
    void *create(DWORD type, DWORD *maxWidth, DWORD *maxHeight, int adapter, void *device)
    {
        if (!m_module)
        {
            LOGERR("Cannot create NvFBC instance - library not loaded");
            return NULL;
        }

        NvFBCStatusEx status = {};
        status.dwVersion = NVFBC_STATUS_VER;
        status.dwAdapterIdx = adapter;
        const NVFBCRESULT statusRes = getStatus(&status);

        if (statusRes != NVFBC_SUCCESS)
        {
            LOGERR("NvFBC getStatus failed (result: 0x%X)", statusRes);
            return NULL;
        }

        if (!status.bIsCapturePossible)
        {
            LOGERR("NvFBC not enabled (bIsCapturePossible=false)");
            return NULL;
        }

        if (!status.bCanCreateNow)
        {
            LOGERR("NvFBC not enabled (bCanCreateNow=false)");
            return NULL;
        }

        NvFBCCreateParams params;
        memset(&params, 0, sizeof(params));
        params.dwVersion = NVFBC_CREATE_PARAMS_VER;
        params.dwInterfaceType = type;
        params.pDevice = device;
        params.dwAdapterIdx = adapter;

        const NVFBCRESULT res = m_create(&params);

        if (res != NVFBC_SUCCESS)
        {
            LOGERR("Failed to create NvFBC instance (result: 0x%X)", res);
            return NULL;
        }

        LOG("NvFBC instance created successfully (type: 0x%X, adapter: %d, maxRes: %dx%d)",
            type, adapter, params.dwMaxDisplayWidth, params.dwMaxDisplayHeight);

        *maxWidth = params.dwMaxDisplayWidth;
        *maxHeight = params.dwMaxDisplayHeight;
        return params.pNvFBC;
    }

private:
    // %SystemRoot%\System32\NvFBC64.dll, or empty, with a log line, when SystemRoot is unset.
    static std::string systemPath()
    {
        char *root = NULL;
        size_t size = 0;

        if (_dupenv_s(&root, &size, "SystemRoot") != 0)
        {
            LOGERR("Unable to get the SystemRoot environment variable");
            return std::string();
        }

        if (!root || size == 0)
        {
            free(root);
            LOGERR("The SystemRoot environment variable is not set");
            return std::string();
        }

        const std::string path = std::string(root) + "\\System32\\NvFBC64.dll";
        free(root);
        return path;
    }

    HMODULE m_module = NULL;
    NvFBC_CreateFunctionExType m_create = NULL;
    NvFBC_GetStatusExFunctionType m_getStatus = NULL;
    NvFBC_EnableFunctionType m_enable = NULL;
};
