# Nvidia Framebuffer Capture Relay


This is a simple application that captures display to a DirectX 9 surface,
sets up a pseudo-fullscreen window on another display, 
and presents that surface on the window through the same DX9 context.

It is simpler with fewer features than commonly-used capture mechanisms but probably faster -
Except in Horizon Zero Dawn.

---

# Prerequisites

As with any application leveraging NvFBC, this is only officially supported
on Tesla & Quadro professional cards. There are other ways to enable NvFBC
on GeForce which are unsupported and definitely not the intended target of
this program.

HDCP also needs to be disabled. Thanks, DRM.

---


# Use

Download NVFBCR.exe and execute it. It'll pop a console window enumerating
your screens, then ask you to input the index of your capture and target displays.
You'll also have an opportunity here to change the capture/present interval,
but it is set to 60fps by default so you can just hit Enter again to take that.

If you want to make yourself, you should be able to clone the whole repo, open `samples/NvFBC/NvFBCR/NvFBCR_2013.vcxproj` with VS2022 and build it.

Sorry for the weak build instructions, I'm not good with Windows.

---

# Note about Display coordinates

Displays can be oriented in any fashion, but we don't do any automatic checking
of output dimensions during runtime. So starting NvFBC and then starting an application fullscreen in a different resolution may 
cause the coordinate of the output window to break if the target display's coordinate is dependent on the capture display.

In other words, if your target display is to the right/below the capture display, window offset bugs will occur when changing resolutions.

Place the target display to the left of the capture display or keep the capture display at a static resolution.

---

# Why?

TL;DR higher FPS lower latency (mostly)



Assuming you do not want to or cannot use HDMI passthrough on a capture card (personally, I 
need to use DisplayPort to get the most out of my monitor), there are 
basically 3 ways to do screen capture in Windows, and the functions of these exist on two axes.\
The first axis is capture method - NvFBC vs. DXGI duplication. NvFBC is unsupported on modern 
Windows but provides lower capture latency and smaller performance cost in the typical case 
than DXGI.\
The second axis is available output formats. The typical use case is to directly encode output
to a local video file or stream. Using dedicated capture hardware can provide better flexibility
and lessen the performance impact on the capture system.

| Capture Method | Performance Cost | Positives | Negatives |
| ------ | ------ | ------ | ------ |
| OBS | 8-15% | Very flexible outputs, HDR support everywhere | Overall performance cost
| DXGI & Capture Card | 5-7% | Lowest performance impact for HDR streaming | Heavy performance cost for what is essentially just a screen copy
| Shadowplay | ~5% | Lowest performance impact for HDR recording | Cannot stream at all from an HDR source, input latency induced by desktop capture mode, need to login to Nvidia & link accounts to use
| NvFBCR | 2-3% | Lowest total performance impact, requires no application hook, can view an HDR source | Output clamped to SDR can cause HDR sources to look washed out, but restorable in post-processing

### OBS
The first method is to use some dedicated capture or streaming software to 
record or transmit without additional hardware. This is something like OBS
or XSplit, and the most common these days. This works well for many people,
but it's the highest performance impact on the system under capture. \
First, these software suites use DXGI capture.\
Second, directly encoding on the capture system is expensive, though lessened significantly by
dedicated encoding ASICs aboard modern GPUs. \
Still, this performance impact 
as tested on a RTX 4090 can be between 8-10% when using OBS with new NVENC H.264/H.265 for local 
capture. It can be up to 15% when combined with tonemapping to SDR and streaming. These figures were
found using application capture mode, and not all 
applications play nicely with application capture. The 
performance penalty is even greater than this when using 
desktop capture.


### DXGI & capture card
The second way is to use DXGI duplication with a capture card.
This is basically where the capture card acts like a second display output 
and you just use Windows display settings to clone your primary display to
the capture card. This is limited in the fact that HDR cannot be enabled
on either display on this mode, meaning that you are stuck in SDR not only in the capture 
stream but also on the primary viewing monitor. You can get around this by extending 
the capture card as a side display instead of setting it as a clone of the primary, then using 
software like OBS to capture the primary and re-present to the capture card. This nullifies 
the performance penalty of encoding when paired with a separate PC, but still uses DXGI
and presents through the OBS rendering pipeline. This is, however, the only way to get
HDR output to a capture card currently without using HDMI passthrough as far as I can tell.\
About 5-7% performance impact.

### Shadowplay
The third way is Shadowplay. Shadowplay uses NvFBC for capture and can perform encoding without 
the captured frame ever leaving VRAM. It is highly performant and will capture HDR to local 
recordings. It will not, however, automatically tonemap HDR capture to SDR for streaming, instead 
just refusing to start. You are also required to use GeForce Experience to utilize Shadowplay.\
About 5% performance impact for local recording. Slightly higher for streaming. Slightly higher performance impact and much higher input 
latency when using desktop capture.

## NvFBCR
NvFBC-Relay lies somewhere between these other options in features and flexibility while being the
clear-cut performance winner in almost all scenarios. It's incredibly simple in design, as all it 
does is capture direct to a DX9 surface that is already set up as the backbuffer for a window.
It is basically a copy, a pointer swap, and 2 more copies:

* Capture and write to backbuffer - scaling is done in hardware by NvFBC
* Flip backbuffer to front with Present call
* Blt copy front buffer to DWM surface
* DWM renders to screen

No shared memory resources are ever explicitly utilized, everything stays in DX9 context like 
Shadowplay does. The GPU itself does nearly zero work since no rendering resources are utilized;
we don't even initialize a DX9 rendering pipeline, it's just there to hold the surface and swap 
chain. \
This simplicity comes with drawbacks. \
The most obvious one is that we consider it outside our scope to provide an encoder;
we explicitly do not want to use NVENC because it has a nonzero,
though small, performance impact. Encoding the output stream is left to the capture card host.\
DX9 has no capability to output HDR, and while
it does seem possible to utilize a shared surface to pass the NvFBC output to DX11, there doesn't
seem to be any easy way to directly present a surface in DX11 aside from rendering it as a texture,
which both greatly increases complexity and probably throws away all performance benefits at these 
small margins.\
That means that while NvFBCR will capture a display outputting HDR, it will only output on an SDR
surface.\
Frame delivery pacing could be better and tearing is allowed for lowest performance impact.\
Additionally, I have observed a much larger performance impact than is typical using NvFBC in both 
NvFBCR and in Shadowplay specifically in Horizon: Zero Dawn. I've found no other application to 
exhibit this apparent CPU-related regression, but HZD also has a weird bug where ReBAR causes 
CPU-related performance regressions on Intel platforms in this game, so I think this is just an 
outlier where Nvidia's driver really hates this game.


There are some unique benefits to NvFBC-Relay in comparison to the existing options.

* Its performance is the best of the options in most scenarios, typically 2-3% performance impact. 
* It provides great flexibility with the output stream. Since NvFBCR outputs to any arbitrary display, 
a capture card can be used to take this output and manipulate it as desired on a separate system. This removes
some painful limitations like Shadowplay being unable to
stream HDR sources.
* Capture is entirely agnostic of running games and does not rely on application hooks, 
and will capture the desktop without increasing performance impact like OBS or input latency like Shadowplay.




---



# License
GPLv3 for new bits, whatever Nvidia says for the capture SDK stuff

