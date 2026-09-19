#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Launch-time parsing: the option flags and the capture-mode string, as plain functions over
// explicit structs. No windows.h and no logging, so the policy suite compiles this header and pins
// every rule a launch string is decided by. NvFBCR owns the wiring: it keeps the resolved values
// in its globals, logs any warning returned here, and builds the capture mode a ModeSpec names.

namespace launch {

// Every option a launch string can set. The member initializers are the defaults the relay runs
// with: its globals are initialized from a default-constructed Options, so a default is changed
// here and nowhere else.
struct Options {
    float srcRateHint = 0.0f;       // -src: declared BASE render rate; 0 = not declared
    bool lock = false;              // -lock: the phase comb lock
    bool tint = false;              // -tint: border synthesized frames
    bool etw = false;               // -etw: driver flip timing alongside capture
    bool noJoin = false;            // -nojoin: keep the ETW session, skip the flip join
    bool dejitter = false;          // -dejit: re-stamp late-delivered batches
    bool fgPhase = false;           // -fgphase: the frame-generation phase instrument
    bool phaseKeep = false;         // -phasekeep: phase-aware keep-real
    bool flipEx = false;            // -flipex: D3D9Ex flip-ex swap effect
    bool mark = false;              // -mark: burn the frame-counter marker
    unsigned int markFrames = 0;    // -mark N: first N presents only; 0 = every present
    unsigned int extraLagMs = 0;    // -lag N: extra bracketing delay, 0 to 200 ms
};

// Whitespace tokenizer shared by the command line and the console prompt, so both paths always
// split options identically. Splits on spaces only.
inline std::vector<std::string> SplitTokens(const std::string& text) {
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < text.length()) {
        while (pos < text.length() && text[pos] == ' ') pos++;
        const size_t start = pos;
        while (pos < text.length() && text[pos] != ' ') pos++;
        if (pos > start) tokens.push_back(text.substr(start, pos - start));
    }
    return tokens;
}

// Single fps validation policy for every entry point that accepts a rate (mode strings, -src):
// accept (0, 1000]. std::stof reads the longest numeric prefix, so "60fps" parses as 60.
inline bool ParseFps(const std::string& value, float* outFps) {
    try {
        const float v = std::stof(value);
        if (v > 0.0f && v <= 1000.0f) {
            *outFps = v;
            return true;
        }
    }
    catch (...) {}
    return false;
}

// Applies the option at tokens[i] and returns how many tokens it consumed (0 = not a recognized
// option). A value that fails validation is still consumed, leaves the option as it was, and sets
// *warning to the line the caller should log. Flags match exactly and are case-sensitive.
inline size_t ApplyOption(const std::vector<std::string>& tokens, size_t i, Options* o,
                          std::string* warning) {
    const std::string& t = tokens[i];
    if (t == "-lock")      { o->lock = true;      return 1; }
    if (t == "-tint")      { o->tint = true;      return 1; }
    if (t == "-etw")       { o->etw = true;       return 1; }
    if (t == "-nojoin")    { o->noJoin = true;    return 1; }
    if (t == "-dejit")     { o->dejitter = true;  return 1; }
    if (t == "-fgphase")   { o->fgPhase = true;   return 1; }
    if (t == "-phasekeep") { o->phaseKeep = true; return 1; }
    if (t == "-flipex")    { o->flipEx = true;    return 1; }
    if (t == "-mark") {
        o->mark = true;
        // Optional frame count: consume the next token as N only if it is all digits, so a bare
        // -mark (or -mark followed by another flag) keeps marking every present.
        if (i + 1 < tokens.size() && !tokens[i + 1].empty() &&
            tokens[i + 1].find_first_not_of("0123456789") == std::string::npos) {
            o->markFrames = (unsigned int)std::strtoul(tokens[i + 1].c_str(), NULL, 10);
            return 2;
        }
        return 1;
    }
    if (t == "-lag" && i + 1 < tokens.size()) {
        const long v = std::strtol(tokens[i + 1].c_str(), NULL, 10);
        if (v >= 0 && v <= 200) o->extraLagMs = (unsigned int)v;
        else if (warning) *warning = "-lag value '" + tokens[i + 1] + "' invalid (0-200 ms) - ignored";
        return 2;
    }
    if (t == "-src" && i + 1 < tokens.size()) {
        float v;
        if (ParseFps(tokens[i + 1], &v)) o->srcRateHint = v;
        else if (warning) *warning = "-src value '" + tokens[i + 1] + "' invalid (1-1000) - ignored";
        return 2;
    }
    return 0;
}

enum class ModeKind { Invalid, Vsync, Temporal, Diag, Timer };
enum class Compositor { Nearest, Blend, Interp };

// What a capture-mode string asks for. framerate is the rate a timer present runs at, and the
// nominal 60 that sizes the bracketing lag on a vsync present.
struct ModeSpec {
    ModeKind kind = ModeKind::Invalid;
    Compositor compositor = Compositor::Nearest;
    bool vsyncPresent = false;   // present on vsync; false = QPC timer
    bool d3d11Present = false;   // the D3D11 flip-model swapchain rather than D3D9
    float framerate = 0.0f;
};

inline char LowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

inline bool EqualsNoCase(const std::string& a, const char* b) {
    const size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; i++) {
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    }
    return true;
}

// The capture-mode grammar. The first letter picks the temporal compositor: t nearest selection,
// b blend, o optical-flow interp. The bare letter and X:vsync present on vsync, X:<fps> on a QPC
// timer at that rate.
//
// Two present paths carry the vsync present. t and o run on the D3D9 swapchain, whose windowed
// INTERVAL_ONE present blocks on DWM's compose clock (card-locked 60 Hz under a fullscreen game on
// the source; the DISPLAYED rate under in-game frame generation). The blend mode has both: b and
// b:vsync present through a D3D11 flip-model swapchain on the output window, which Windows
// promotes to independent flip so the present blocks on the SINK's own vblank, and b:dwm is the
// same blend on the D3D9 swapchain. The D3D11 path carries the blend compositor only.
//
// An empty string and "vsync" select the original vsync mode, "diag" and "diag:vsync" the clock
// probes, and a bare number the plain timer mode. Anything else is Invalid, and the caller prints
// the usage list.
inline ModeSpec ParseMode(const std::string& modeStr) {
    ModeSpec s;
    if (modeStr.empty() || EqualsNoCase(modeStr, "vsync")) {
        s.kind = ModeKind::Vsync;
        return s;
    }

    const char c0 = LowerAscii(modeStr[0]);
    Compositor comp = Compositor::Nearest;
    if (c0 == 'b') comp = Compositor::Blend;
    else if (c0 == 'o') comp = Compositor::Interp;

    if (EqualsNoCase(modeStr, "t") || EqualsNoCase(modeStr, "t:vsync") ||
        EqualsNoCase(modeStr, "o") || EqualsNoCase(modeStr, "o:vsync") ||
        EqualsNoCase(modeStr, "b:dwm")) {
        s.kind = ModeKind::Temporal;
        s.compositor = comp;
        s.vsyncPresent = true;
        s.framerate = 60.0f;
        return s;
    }
    if (EqualsNoCase(modeStr, "b") || EqualsNoCase(modeStr, "b:vsync")) {
        s.kind = ModeKind::Temporal;
        s.compositor = comp;
        s.vsyncPresent = true;
        s.d3d11Present = true;
        s.framerate = 60.0f;
        return s;
    }
    if (modeStr.length() > 2 && (c0 == 't' || c0 == 'b' || c0 == 'o') && modeStr[1] == ':') {
        float fps;
        if (ParseFps(modeStr.substr(2), &fps)) {
            s.kind = ModeKind::Temporal;
            s.compositor = comp;
            s.framerate = fps;
            return s;
        }
    }

    if (EqualsNoCase(modeStr, "diag")) {
        s.kind = ModeKind::Diag;
        return s;
    }
    if (EqualsNoCase(modeStr, "diag:vsync")) {
        s.kind = ModeKind::Diag;
        s.vsyncPresent = true;
        return s;
    }

    float fps;
    if (ParseFps(modeStr, &fps)) {
        s.kind = ModeKind::Timer;
        s.framerate = fps;
    }
    return s;
}

}  // namespace launch
