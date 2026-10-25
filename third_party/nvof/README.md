# NVIDIA optical flow headers

`nvOpticalFlowCommon.h`, `nvOpticalFlowD3D11.h` and `nvOpticalFlowD3D12.h` are the interface
headers from NVIDIA's Optical Flow SDK, at interface version 5.0 as their
`NV_OF_API_MAJOR_VERSION` and `NV_OF_API_MINOR_VERSION` state. They are unchanged apart from line
endings (the SDK ships them with CR LF; this repository stores text with LF).

Each carries NVIDIA's MIT notice, which applies to that header file only. The runtime they
declare, `nvofapi64.dll`, ships with NVIDIA's driver and is not in this repository.

The relay includes them as `nvof/<header>`, through the `third_party` include directory in
`src/relay/NvFBCR.vcxproj`.
