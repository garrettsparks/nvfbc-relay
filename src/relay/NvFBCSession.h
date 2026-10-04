#pragma once

#include <windows.h>
#include <NvFBCApi.h>
#include <NvFBCLoader.h>

#include <string>

#include "Failure.h"
#include "RelayContext.h"

// The capture state NvFBC reports for GPU 0, the one GPU the relay drives.
struct CaptureStatus {
    bool read = false;          // the status call itself succeeded
    bool possible = false;      // NvFBC is enabled on this system
    bool canCreateNow = false;  // no other session stands in the way right now
};

MaybeFailure LoadNvFBC(NvFBCLoader* nvfbc);

// Reads and logs the status.
CaptureStatus ReadCaptureStatus(NvFBCLoader* nvfbc);

// Turns NvFBC on so a relaunch can capture. A relaunched process that still finds capture
// impossible gets a failure here instead, so the relay relaunches at most once.
MaybeFailure TurnOnCapture(NvFBCLoader* nvfbc, bool relaunched);

// Starts this exe again with the given arguments (launch::RelaunchArguments, which mark it as
// the relaunch so it continues this one's log), and hands it the single-instance lock. Logs
// nothing further when the new process starts.
MaybeFailure Relaunch(const std::string& arguments, HANDLE* instanceLock);

// The session the shell creates on the present device, set up to write into its back buffer,
// and the grab parameters for it. The temporal modes replace it with their own.
MaybeFailure CreateStartupSession(RelayContext* ctx, NVFBC_TODX9VID_GRAB_FRAME_PARAMS* grab,
                                  NvFBCFrameGrabInfo* grabInfo);

// The failure for a session NvFBC refused to create: "in use" when another session stands in
// the way, otherwise "could not start".
Failure SessionRefused(NvFBCLoader* nvfbc, const char* where);
