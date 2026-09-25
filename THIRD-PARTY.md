# Third-party material

This project started as the NvFBC sample from the NVIDIA Capture SDK. The files
listed below are NVIDIA's, not ours, and the MIT license in `LICENSE` does not
cover them.

## Inventory

| Path | Origin | Terms |
| ---- | ------ | ----- |
| `third_party/nvof/nvOpticalFlowCommon.h`, `nvOpticalFlowD3D11.h` | NVIDIA Optical Flow SDK interface headers | MIT, with NVIDIA's notice in each file. |

Everything under `src/`, `tools/` and `tests/` is ours and carries no NVIDIA
notice.

## The original relay

This project began as NvFBC-Relay by Collin Blakley:

    https://gitlab.com/DonnerPartyOf1/nvfbc-relay

That repository has no license file. None of its code or text remains here:
the parts that had survived into this project were rewritten in September 2026,
and his commits stay in the history.

## Redistribution

The NvOFFRUC library used by the optical flow work on other branches is not in
this repository and must not be committed. The DesignWorks license it ships
under grants no redistribution right, so it has to be placed on a machine by
hand.

## The NvFBC interface

`src/common/NvFBCApi.h` declares the parts of NVIDIA's frame buffer capture
interface that this project uses. It gives the names, values, struct layouts
and method order the driver expects, written for this project, with no
comments or documentation copied from NVIDIA's headers. Those headers are not
in this repository. The driver's `NvFBC64.dll` is loaded at run time and is
not redistributed.
