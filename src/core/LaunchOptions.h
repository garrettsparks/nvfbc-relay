#pragma once

#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Launch-time parsing: the option flags, the capture-mode string, the command line and the prompt
// answers, as plain functions over explicit structs. No windows.h and no logging, so the policy
// suite compiles this header and pins every rule a launch is decided by. The relay's shell owns
// the wiring: it runs a launch through the checks here, logs and shows what they refuse, and
// builds the capture mode a ModeSpec names.

namespace launch {

// Every option a launch string can set. The member initializers are the defaults the relay runs
// with: a launch starts from a default-constructed Options, so a default is changed here and
// nowhere else. The comb lock, the extra lag, flip timing and delivery-lateness
// correction are on by default because the relay-cost ladder measured no cost for any of them;
// -nolock, -lag 0, -noetw and -nodejit turn them off.
struct Options {
    float srcRateHint = 0.0f;       // -src: declared BASE render rate; 0 = not declared
    bool lock = true;               // -lock / -nolock: the phase comb lock
    bool tint = false;              // -tint: border synthesized frames
    bool etw = true;                // -etw / -noetw: driver flip timing alongside capture
    bool noJoin = false;            // -nojoin: keep the ETW session, skip the flip join
    bool dejitter = true;           // -dejit / -nodejit: re-stamp late-delivered batches
    bool dejitterRequested = false; // -dejit was typed, rather than on by default
    bool fgPhase = false;           // -fgphase: the frame-generation phase instrument
    bool phaseKeep = false;         // -phasekeep: phase-aware keep-real
    bool flipEx = false;            // -flipex: D3D9Ex flip-ex swap effect
    bool mark = false;              // -mark: burn the frame-counter marker
    unsigned int markFrames = 0;    // -mark N: first N presents only; 0 = every present
    unsigned int extraLagMs = 75;   // -lag N: extra bracketing delay, 0 to 200 ms
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

// Integer validation for the display indices. The whole string must be an integer, optionally
// signed and surrounded by spaces; anything else, including a value outside int, returns false
// and leaves *out as it was. It never throws: nothing on a launch path catches an exception, so
// a mistyped index has to come back as a value the caller can refuse.
inline bool ParseInt(const std::string& text, int* out) {
    size_t b = text.find_first_not_of(' ');
    if (b == std::string::npos) return false;
    const size_t e = text.find_last_not_of(' ') + 1;
    bool negative = false;
    if (text[b] == '-' || text[b] == '+') {
        negative = text[b] == '-';
        b++;
    }
    if (b == e) return false;
    long long v = 0;
    for (size_t i = b; i < e; i++) {
        if (text[i] < '0' || text[i] > '9') return false;
        v = v * 10 + (text[i] - '0');
        if (v > (long long)INT_MAX + 1) return false;
    }
    if (negative) v = -v;
    if (v < INT_MIN || v > INT_MAX) return false;
    *out = (int)v;
    return true;
}

// One row of the usage list, and the registry of every flag a launch string can carry: a flag
// without a row is unknown to ApplyOption, and the suite holds every row to being parsed, so the
// list a user reads and the parser cannot drift apart. arg is a sample value, shown after the
// flag; one in brackets is optional. A row that is not shown is a development flag: parsed, and
// left off the list a release user reads.
struct UsageRow {
    const char* flag;
    const char* arg;
    const char* text;
    bool shown;
    bool commandLineOnly;   // read by ParseCommandLine alone; the mode prompt does not take it
};

inline const std::vector<UsageRow>& OptionRows() {
    static const std::vector<UsageRow> rows = {
        {"-src", "60", "The game's own frame rate, not counting frames added by DLSS Frame "
                       "Generation or Smooth Motion. E.g. if frame generation doubles a game to "
                       "120 FPS, use -src 60. If frame rate varies, bias lower. E.g. for a game "
                       "running mostly 75 to 90 FPS, use -src 80, not 90. Default 60.",
         true, false},
        {"-nolock", "", "Turn the phase comb lock off (on by default)", false, false},
        {"-lag", "75", "Extra delay in ms added to the relayed video (0-200, default 75). More "
                       "delay means fewer repeated frames. The player never feels it. The stream "
                       "just runs slightly later.", true, false},
        {"-noetw", "", "Do not read the display driver's scanout times (read by default; -dejit "
                       "needs them)", false, false},
        {"-nodejit", "", "Do not re-stamp late-delivered capture batches onto the flip grid (on by "
                         "default; needs flip timing and the comb lock)", false, false},
        {"-mark", "[N]", "Burn the frame-counter marker for offline analysis; N = first N presents "
                         "only, else every present", false, false},
        {"-lock", "", "The comb lock; on by default, accepted so older launch strings keep working",
         false, false},
        {"-etw", "", "Flip timing; on by default, accepted so older launch strings keep working",
         false, false},
        {"-dejit", "", "Late-batch correction; on by default, and typed it refuses loudly when a "
                       "prerequisite is off", false, false},
        {"-nojoin", "", "Keep the ETW session and its flip lines, skip the per-present flip join",
         false, false},
        {"-tint", "", "Border every synthesized frame", false, false},
        {"-fgphase", "", "Frame-generation phase instrument; stalls the capture thread every wake",
         false, false},
        {"-phasekeep", "", "Phase-aware keep-real, the x3 rotation vote", false, false},
        {"-flipex", "", "D3D9Ex flip-ex swap effect on the D3D9 present path", false, false},
        {"-source", "0", "Capture display index, for a launch without the prompts", false, true},
        {"-target", "1", "Output display index, for a launch without the prompts", false, true},
        {"-framerate", "b:vsync", "Capture mode, for a launch without the prompts", false, true},
    };
    return rows;
}

// The capture modes, one row per line of the list; flag holds the spellings, comma-separated.
inline const std::vector<UsageRow>& ModeRows() {
    static const std::vector<UsageRow> rows = {
        {"b, b:vsync", "", "Blend compositor on a D3D11 flip-model swapchain, presented on the "
                           "SINK's vblank (the default: a blank answer selects it)", true, false},
        {"b:dwm, b:60", "", "The same blend compositor on the D3D9 swapchain: DWM's compose clock "
                            "(b:dwm) or a timer at the given fps", true, false},
        {"t, t:vsync", "", "Temporal frame selection, presented on vsync (DWM compose clock)",
         true, false},
        {"t:59.94", "", "Temporal frame selection, presented on a timer at the given fps", true,
         false},
        {"vsync", "", "The original relay: VSync-driven presentation (matches target display "
                      "refresh)", true, false},
        {"60", "", "Timer mode (simple timer-driven at the given fps)", true, false},
        {"o, o:vsync, o:60", "", "Optical-flow interp compositor on the D3D9 swapchain", false,
         false},
        {"diag, diag:vsync", "", "Clock probes: DWM compose timing and the card's raster", false,
         false},
    };
    return rows;
}

inline const UsageRow* FindOptionRow(const std::string& flag) {
    for (const UsageRow& r : OptionRows()) {
        if (flag == r.flag) return &r;
    }
    return NULL;
}

// The usage list the mode prompt prints, one string per line.
inline std::vector<std::string> UsageLines() {
    std::vector<std::string> lines;
    auto add = [&lines](const UsageRow& r) {
        std::string spelling = r.flag;
        if (r.arg[0]) spelling += std::string(" ") + r.arg;
        if (spelling.size() < 15) spelling.resize(15, ' ');
        lines.push_back("  " + spelling + "- " + r.text);
    };
    lines.push_back("Capture modes:");
    for (const UsageRow& r : ModeRows()) {
        if (r.shown) add(r);
    }
    lines.push_back("Options, typed after the mode (b:vsync -src 60):");
    for (const UsageRow& r : OptionRows()) {
        if (r.shown) add(r);
    }
    return lines;
}

// Applies the option at tokens[i] and returns how many tokens it consumed (0 = not a recognized
// option). A value that fails validation is still consumed, leaves the option as it was, and sets
// *warning to what was wrong with it, in words the console can show. Flags match exactly and are
// case-sensitive.
inline size_t ApplyOption(const std::vector<std::string>& tokens, size_t i, Options* o,
                          std::string* warning) {
    const std::string& t = tokens[i];
    // The usage rows are the registry: a flag with no row is not an option, whatever below
    // would have done with it.
    const UsageRow* row = FindOptionRow(t);
    if (!row || row->commandLineOnly) return 0;
    // The positive spellings of the default-on options are still accepted, so a launch string
    // written before they became defaults keeps working unchanged.
    if (t == "-lock")      { o->lock = true;      return 1; }
    if (t == "-nolock")    { o->lock = false;     return 1; }
    if (t == "-tint")      { o->tint = true;      return 1; }
    if (t == "-etw")       { o->etw = true;       return 1; }
    if (t == "-noetw")     { o->etw = false;      return 1; }
    if (t == "-nojoin")    { o->noJoin = true;    return 1; }
    if (t == "-dejit")     { o->dejitter = true;  o->dejitterRequested = true; return 1; }
    if (t == "-nodejit")   { o->dejitter = false; o->dejitterRequested = false; return 1; }
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
        else if (warning) *warning = "-lag takes a whole number from 0 to 200, not '" + tokens[i + 1] + "'";
        return 2;
    }
    if (t == "-src" && i + 1 < tokens.size()) {
        float v;
        if (ParseFps(tokens[i + 1], &v)) o->srcRateHint = v;
        else if (warning) *warning = "-src takes a frame rate above 0 and up to 1000, not '" + tokens[i + 1] + "'";
        return 2;
    }
    return 0;
}

// What was wrong with a token that neither the option dispatch nor the command line took. A
// registered flag that takes a value was typed without one; anything else is not an option
// where it was typed. The display and mode flags are options on the command line only, so at
// the mode prompt they are unknown like any other stray token.
inline std::string RefusedToken(const std::string& token, bool atPrompt) {
    const UsageRow* row = FindOptionRow(token);
    const bool takesValue = row && row->arg[0] && row->arg[0] != '[';
    if (takesValue && !(atPrompt && row->commandLineOnly)) {
        return token + " needs a value after it";
    }
    return "'" + token + "' is not one of the options" + (atPrompt ? " listed above" : "");
}

// Settles an option that depends on others, once every token is in. -dejit needs flip timing
// with the flip join on, and the comb lock. When it is on only as a default and an opt-out
// removed one of those, it steps aside and this returns the line to log. When it was typed it
// stays on, and the capture mode refuses it loudly, as it does any request that contradicts
// itself.
inline std::string ResolveDependencies(Options* o) {
    if (!o->dejitter || o->dejitterRequested) return std::string();
    const char* reason = NULL;
    if (!o->etw) reason = "flip timing, and -noetw turned that off";
    else if (o->noJoin) reason = "the flip join, and -nojoin turned that off";
    else if (!o->lock) reason = "the comb lock, and -nolock turned that off";
    if (!reason) return std::string();
    o->dejitter = false;
    return std::string("Delivery-lateness correction off: -dejit needs ") + reason;
}

// What a command line names besides the options: the display pair and the capture mode, for a
// scripted launch that skips the prompts. An index of -1 and an empty mode mean not given.
struct CommandLine {
    int sourceIndex = -1;
    int targetIndex = -1;
    std::string mode;
    bool foundAny = false;   // at least one token was recognized
    bool hasSource = false;  // -source was given with a number
    bool hasTarget = false;  // -target was given with a number
};

// Parses a whole command line: -source, -target and -framerate here, every other token through
// ApplyOption. Each rejected value appends what was wrong to *warnings, and each token nothing
// took goes to *unknown. An index that is not a number is reported and left as it was.
inline CommandLine ParseCommandLine(const std::vector<std::string>& args, Options* o,
                                    std::vector<std::string>* warnings,
                                    std::vector<std::string>* unknown = NULL) {
    CommandLine c;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& t = args[i];
        const bool hasValue = i + 1 < args.size();
        if ((t == "-source" || t == "-target") && hasValue) {
            const bool source = t == "-source";
            int* index = source ? &c.sourceIndex : &c.targetIndex;
            if (ParseInt(args[i + 1], index)) {
                if (source) c.hasSource = true;
                else c.hasTarget = true;
            } else if (warnings) {
                warnings->push_back(t + " must be a display number, not '" + args[i + 1] + "'");
            }
            c.foundAny = true;
            i++;
        } else if (t == "-framerate" && hasValue) {
            c.mode = args[i + 1];
            c.foundAny = true;
            i++;
        } else {
            std::string w;
            const size_t consumed = ApplyOption(args, i, o, &w);
            if (!w.empty() && warnings) warnings->push_back(w);
            if (consumed > 0) {
                c.foundAny = true;
                i += consumed - 1;
            } else if (unknown) {
                unknown->push_back(t);
            }
        }
    }
    return c;
}

// The shortest decimal spelling ParseFps reads back as exactly this rate: 59.94 stays
// "59.94", and a value with more digits than that gets as many as it needs.
inline std::string FormatFps(float fps) {
    char text[32];
    for (int digits = 6; digits <= 9; digits++) {
        snprintf(text, sizeof(text), "%.*g", digits, (double)fps);
        if (std::strtof(text, NULL) == fps) break;
    }
    return text;
}

// The option tokens that rebuild *o when parsed over the defaults, so a relaunch runs with the
// options this launch resolved to. Only what differs from a default is written, and a
// default launch writes nothing.
inline std::string FormatOptions(const Options& o) {
    const Options d;
    std::string s;
    auto add = [&s](const std::string& token) {
        if (!s.empty()) s += ' ';
        s += token;
    };
    if (o.srcRateHint != d.srcRateHint) add("-src " + FormatFps(o.srcRateHint));
    if (o.lock != d.lock) add(o.lock ? "-lock" : "-nolock");
    if (o.etw != d.etw) add(o.etw ? "-etw" : "-noetw");
    if (o.noJoin) add("-nojoin");
    // A typed -dejit is written as typed, so the relaunch refuses it as loudly as this launch
    // did; a default one that stepped aside is written as off.
    if (o.dejitterRequested) {
        add("-dejit");
    } else if (o.dejitter != d.dejitter) {
        add(o.dejitter ? "-dejit" : "-nodejit");
    }
    if (o.extraLagMs != d.extraLagMs) add("-lag " + std::to_string(o.extraLagMs));
    if (o.mark) add(o.markFrames ? "-mark " + std::to_string(o.markFrames) : "-mark");
    if (o.tint) add("-tint");
    if (o.fgPhase) add("-fgphase");
    if (o.phaseKeep) add("-phasekeep");
    if (o.flipEx) add("-flipex");
    return s;
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
// An empty string is a bare launch and selects b:vsync, the release path. "vsync" selects the
// original vsync mode, "diag" and "diag:vsync" the clock probes, and a bare number the plain timer
// mode. Anything else is Invalid, and the caller prints the usage list.
inline ModeSpec ParseMode(const std::string& modeStr) {
    ModeSpec s;
    if (modeStr.empty()) return ParseMode("b:vsync");
    if (EqualsNoCase(modeStr, "vsync")) {
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

// Whether a command line can run as it stands. It can when it names two different displays that
// exist, a mode ParseMode accepts or none (the default), and nothing but registered options with
// valid values. Otherwise reason holds what was wrong, in the words the console shows. An empty
// command line is not refused, it is simply not usable: asking is the normal path then.
struct CommandLineCheck {
    bool usable = false;
    std::string reason;
    CommandLine line;
    Options options;
};

inline CommandLineCheck CheckCommandLine(const std::vector<std::string>& args, int displayCount) {
    CommandLineCheck c;
    if (args.empty()) return c;
    std::vector<std::string> warnings;
    std::vector<std::string> unknown;
    c.line = ParseCommandLine(args, &c.options, &warnings, &unknown);
    const auto noDisplay = [displayCount](int index) {
        return index < 0 || index >= displayCount;
    };
    if (!warnings.empty()) {
        c.reason = warnings[0];
    } else if (!unknown.empty()) {
        c.reason = RefusedToken(unknown[0], false);
    } else if (!c.line.hasSource) {
        c.reason = "-source is missing";
    } else if (!c.line.hasTarget) {
        c.reason = "-target is missing";
    } else if (noDisplay(c.line.sourceIndex)) {
        c.reason = "there is no display " + std::to_string(c.line.sourceIndex);
    } else if (noDisplay(c.line.targetIndex)) {
        c.reason = "there is no display " + std::to_string(c.line.targetIndex);
    } else if (c.line.sourceIndex == c.line.targetIndex) {
        c.reason = "the game display and the capture card display must be different";
    } else if (!c.line.mode.empty() && ParseMode(c.line.mode).kind == ModeKind::Invalid) {
        c.reason = "'" + c.line.mode + "' is not one of the modes";
    } else {
        c.usable = true;
    }
    return c;
}

// An answer at a display prompt. Returns true with *index set when it names a display in the
// list that is not otherDisplay (-1 when there is none to avoid); otherwise *problem holds what
// the console says before asking again.
inline bool CheckDisplayAnswer(const std::string& answer, int displayCount, int otherDisplay,
                               int* index, std::string* problem) {
    int n = -1;
    if (!ParseInt(answer, &n)) {
        *problem = "Type the number in brackets beside the display.";
    } else if (n < 0 || n >= displayCount) {
        *problem = "There is no display " + std::to_string(n) + ". Type a number from the list.";
    } else if (n == otherDisplay) {
        *problem = "The capture card display must be a different display from the game display.";
    } else {
        *index = n;
        return true;
    }
    return false;
}

// An answer at the mode prompt: a mode, then options, or options alone for the default mode.
// Options start from the defaults, so what was typed is what runs. problem is empty when the
// answer can run, and otherwise what the console says before asking again.
struct PromptAnswer {
    std::string mode;       // as typed; empty selects the default
    Options options;
    std::string problem;
};

inline PromptAnswer ParsePromptAnswer(const std::string& line) {
    PromptAnswer a;
    const std::vector<std::string> tokens = SplitTokens(line);
    size_t i = 0;
    if (!tokens.empty() && tokens[0][0] != '-') {
        a.mode = tokens[0];
        if (ParseMode(a.mode).kind == ModeKind::Invalid) {
            a.problem = "'" + a.mode + "' is not one of the modes listed above. Type one of "
                        "them, or press Enter for the default.";
            return a;
        }
        i = 1;
    }
    while (i < tokens.size()) {
        std::string warning;
        const size_t consumed = ApplyOption(tokens, i, &a.options, &warning);
        if (!warning.empty()) {
            a.problem = warning + ".";
            return a;
        }
        if (consumed == 0) {
            a.problem = RefusedToken(tokens[i], true) + ".";
            return a;
        }
        i += consumed;
    }
    return a;
}

// The startup line that records every resolved option, including those entered at the prompt,
// which appear nowhere else. mktrace.py reads its "src rate hint <n> fps" and "extra lag <n> ms"
// phrases, and relaylog.py echoes it as a header, so its wording and field order stay fixed.
inline std::string ResolvedOptionsLine(const Options& o, bool d3d11Present) {
    char mark[48];
    if (!o.mark) snprintf(mark, sizeof(mark), "off");
    else if (o.markFrames) snprintf(mark, sizeof(mark), "on (first %u presents)", o.markFrames);
    else snprintf(mark, sizeof(mark), "on (every present)");
    const bool joinOn = o.etw && !o.noJoin;
    const char* join = "on";
    if (!o.etw) join = "off (-noetw)";
    else if (o.noJoin) join = "OFF (-nojoin)";
    const char* dejitter = "off";
    if (o.dejitter && joinOn && o.lock) dejitter = "ON (-dejit)";
    else if (o.dejitter) dejitter = "REFUSED (-dejit needs -etw with the join on, and the comb lock)";
    const char* phaseKeep = "off";
    if (o.phaseKeep && joinOn) phaseKeep = "ON (-phasekeep)";
    else if (o.phaseKeep) phaseKeep = "REFUSED (-phasekeep needs -etw with the join on)";
    char line[768];
    snprintf(line, sizeof(line),
             "Resolved options: src rate hint %.1f fps%s, comb lock %s, frame marker %s, blend "
             "tint %s, etw flip capture %s, flip join %s, dejitter %s, fgphase %s, phasekeep %s, "
             "flip mode %s, extra lag %u ms, present path %s",
             (double)o.srcRateHint, o.srcRateHint > 0.0f ? "" : " (not declared; 60 assumed)",
             o.lock ? "on" : "off", mark, o.tint ? "on" : "off", o.etw ? "on" : "off", join,
             dejitter,
             o.fgPhase ? "requested (-fgphase; ACTIVE only when the instrument line follows)"
                       : "off",
             phaseKeep, o.flipEx ? "FLIPEX (-flipex)" : "bitblt (DISCARD)", o.extraLagMs,
             d3d11Present ? "D3D11 flip-model swapchain (b:vsync)" : "D3D9 swapchain");
    return line;
}

}  // namespace launch
