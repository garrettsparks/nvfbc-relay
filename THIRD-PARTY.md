# Third-party material

This project started as the NvFBC sample from the NVIDIA Capture SDK and keeps
the SDK's directory layout. Parts of the tree are therefore NVIDIA's, not
ours, and the MIT license in `LICENSE` does not cover them.

## Inventory

| Path | Origin | Terms |
| ---- | ------ | ----- |
| `inc/NvFBC/*.h` | NVIDIA Capture SDK headers | "Copyright 1993-2018 NVIDIA Corporation. All rights reserved." Subject to the applicable NVIDIA license agreement. |
| `samples/Util/` | NVIDIA Capture SDK sample helpers | Same notice. The four files without a header comment (`NvFBCLibrary.h`, `ReadMe.txt`, and the two project files) are SDK sample material as well. |
| `inc/NvAPI` | git submodule, https://github.com/NVIDIA/nvapi | Governed by that repository's own license. Referenced, not vendored. |
| `lib/NvAPI/` | NVIDIA NvAPI import libraries, prebuilt | NVIDIA's terms. |
| `lib/DirectX_Nov2008/` | `ddraw.lib` from the November 2008 DirectX SDK | Microsoft DirectX SDK EULA. |

Everything under `samples/NvFBC/NvFBCR/`, `samples/NvFBC/NvFBCEnable/` and
`samples/Common/` is ours and carries no NVIDIA notice.

## The original relay

This project began as NvFBC-Relay by Collin Blakley:

    https://gitlab.com/DonnerPartyOf1/nvfbc-relay

That repository has no license file, so his work carries no grant and he
retains all rights to it. 404 lines of his code survive in the current tree,
all of them in `samples/NvFBC/NvFBCR/NvFBCR.cpp` (measured with `git blame -w`
on `dev`). Every other file under the paths listed above is entirely ours.

`NvFBCR.cpp` is therefore excluded from the MIT license in `LICENSE` until its
original author agrees to relicense his contributions. The comparison writeup
in the README's `Why?` section is also his.

## Redistribution

Use of the NvFBC headers and the SDK sample helpers is governed by the NVIDIA
license agreement accepted when downloading the Capture SDK. Read that
agreement before redistributing this repository or anything built from it.

The NvOFFRUC library used by the optical flow work on other branches is not in
this repository and must not be committed. The DesignWorks license it ships
under grants no redistribution right, so it has to be placed on a machine by
hand.

## Direction

The intent is to remove third-party code from the tree entirely:

- `inc/NvFBC/nvFBCCuda.h` and `inc/NvFBC/nvFBCToSys.h` cover capture paths this
  project does not use and can be deleted outright.
- The CUDA helpers in `samples/Util/` (`helper_cuda.h`, `helper_cuda_gl.h`,
  `helper_cuda_drvapi.h`, `helper_string.h`, `drvapi_error_string.h`) are not
  included by anything here.
- `NvFBCLibrary.h` is a LoadLibrary and GetProcAddress shim and can be
  rewritten from scratch.
- The remaining NvFBC headers declare the ABI. This project uses 15 `NVFBC_*`
  constants and a small number of structs, all on the DX9Vid path, so an
  independent declaration of that subset is feasible. It has to land before the
  vendored headers are removed, because the Capture SDK is behind an NVIDIA
  login and CI cannot fetch it.
- The prebuilt libraries in `lib/` appear in no `AdditionalDependencies` entry
  and are candidates for deletion once a link test confirms it.
