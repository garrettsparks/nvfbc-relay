#include "Prompts.h"

#include <windows.h>
#include <SimpleLogger.h>

#include <cstdio>

namespace {

// A console of this process's own, for the prompts only: the relay is a windowed program, so it
// has none until it asks for one, and it gives it back once the answers are in.
class Console {
public:
    Console() {
        AllocConsole();
        m_in = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        m_out = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }

    ~Console() {
        if (m_in != INVALID_HANDLE_VALUE) CloseHandle(m_in);
        if (m_out != INVALID_HANDLE_VALUE) CloseHandle(m_out);
        FreeConsole();
    }

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    void Write(const std::string& text) {
        DWORD written = 0;
        WriteConsoleA(m_out, text.data(), (DWORD)text.size(), &written, NULL);
    }

    // Writes the text in yellow on the console's own background, then puts the console's own
    // colors back. Plain text when the colors cannot be read. Plain yellow, not the bright one:
    // Windows' default color scheme makes bright yellow a pale cream.
    void WriteWarning(const std::string& text) {
        CONSOLE_SCREEN_BUFFER_INFO info = {};
        if (!GetConsoleScreenBufferInfo(m_out, &info)) {
            Write(text);
            return;
        }
        const WORD background = info.wAttributes & (WORD)0xF0;
        SetConsoleTextAttribute(m_out, (WORD)(background | FOREGROUND_RED | FOREGROUND_GREEN));
        Write(text);
        SetConsoleTextAttribute(m_out, info.wAttributes);
    }

    // One typed line without its line ending. False when the input has ended.
    bool ReadLine(std::string* line) {
        line->clear();
        char buffer[256];
        for (;;) {
            DWORD read = 0;
            if (!ReadConsoleA(m_in, buffer, (DWORD)sizeof(buffer), &read, NULL) || read == 0) {
                return false;
            }
            line->append(buffer, read);
            if (!line->empty() && line->back() == '\n') break;
        }
        while (!line->empty() && (line->back() == '\n' || line->back() == '\r')) {
            line->pop_back();
        }
        return true;
    }

private:
    HANDLE m_in = INVALID_HANDLE_VALUE;
    HANDLE m_out = INVALID_HANDLE_VALUE;
};

// One line per display, in the terms a user picks by: its name, its size, its exact refresh rate
// when it was read, and where it sits on the desktop.
void WriteDisplayList(Console& console, const std::vector<DisplayInfo>& displays) {
    std::string list = "\n";
    for (const DisplayInfo& d : displays) {
        const std::string rate =
            d.exactHz > 0.0 ? " at " + launch::FormatRefreshHz(d.exactHz) + " Hz," : "";
        char line[256];
        snprintf(line, sizeof(line), "[%u] %s, %dx%d%s at (%ld,%ld)\n", d.adapter,
                 d.Name().c_str(), d.Width(), d.Height(), rate.c_str(), d.rect.left, d.rect.top);
        list += line;
    }
    console.Write(list + "\n");
}

bool AskDisplay(Console& console, const char* prompt, int displayCount, int otherDisplay,
                int* index) {
    for (;;) {
        console.Write(prompt);
        std::string answer;
        if (!console.ReadLine(&answer)) return false;
        std::string problem;
        if (launch::CheckDisplayAnswer(answer, displayCount, otherDisplay, index, &problem)) {
            return true;
        }
        LOG("Prompt answer refused: '%s': %s", answer.c_str(), problem.c_str());
        console.Write(problem + "\n");
    }
}

}  // namespace

bool ChooseLaunch(const std::string& commandLine, const std::vector<DisplayInfo>& displays,
                  LaunchChoice* out) {
    const launch::CommandLineCheck check =
        launch::CheckCommandLine(launch::SplitTokens(commandLine), (int)displays.size());
    if (check.usable) {
        out->source = check.line.sourceIndex;
        out->target = check.line.targetIndex;
        out->mode = check.line.mode;
        out->options = check.options;
        LOG("Launch from the command line: game display %d, capture card display %d, mode '%s'",
            out->source, out->target, out->mode.c_str());
        return true;
    }

    Console console;
    if (!check.reason.empty()) {
        LOG("Command line not used: %s", check.reason.c_str());
        console.Write("\nThe command line was not used: " + check.reason +
                      ". Choose below instead.\n");
    }
    WriteDisplayList(console, displays);
    const int count = (int)displays.size();
    if (!AskDisplay(console, "Game display number ? ", count, -1, &out->source) ||
        !AskDisplay(console, "Capture card display number ? ", count, out->source,
                    &out->target)) {
        return false;
    }
    std::string usage = "\n";
    for (const std::string& line : launch::UsageLines()) usage += line + "\n";
    console.Write(usage + "\n");

    // A card display off a whole number of hertz repeats or skips a frame on a schedule. Said
    // after the usage list, right above the mode prompt, so it is the last thing read before the
    // launch and can be fixed first. A command-line launch only logs it.
    const std::string rateNote = launch::CardRateNote(displays[out->target].exactHz);
    if (!rateNote.empty()) {
        LOG("Capture card display rate noted at the prompts: %.6f Hz",
            displays[out->target].exactHz);
        std::string wrapped;
        for (const std::string& line : launch::WrapWords(rateNote)) wrapped += line + "\n";
        console.WriteWarning(wrapped + "\n");
    }
    for (;;) {
        console.Write("Mode and options (press Enter for the default) ? ");
        std::string answer;
        if (!console.ReadLine(&answer)) return false;
        const launch::PromptAnswer parsed = launch::ParsePromptAnswer(answer);
        if (parsed.problem.empty()) {
            out->mode = parsed.mode;
            out->options = parsed.options;
            LOG("Launch from the prompts: game display %d, capture card display %d, mode line "
                "'%s'", out->source, out->target, answer.c_str());
            return true;
        }
        LOG("Prompt answer refused: '%s': %s", answer.c_str(), parsed.problem.c_str());
        console.Write(parsed.problem + "\n\n");
    }
}
