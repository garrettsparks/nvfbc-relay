# NVIDIA optical flow headers

`nvOpticalFlowCommon.h` and `nvOpticalFlowD3D11.h` are the interface headers from NVIDIA's
Optical Flow SDK, at interface version 5.0 as their `NV_OF_API_MAJOR_VERSION` and
`NV_OF_API_MINOR_VERSION` state. They are unchanged since they were added to this repository.

Each carries NVIDIA's MIT notice, which applies to that header file only. The runtime they
declare, `nvofapi64.dll`, ships with NVIDIA's driver and is not in this repository.

The relay includes them as `nvof/nvOpticalFlowCommon.h` and `nvof/nvOpticalFlowD3D11.h`,
through the `third_party` include directory in `src/relay/NvFBCR.vcxproj`.
