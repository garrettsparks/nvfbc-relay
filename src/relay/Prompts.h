#pragma once

#include <string>
#include <vector>

#include "Displays.h"
#include "LaunchOptions.h"

// What a launch asks for: the two displays, the mode string (empty for the default) and the
// options, from the command line or from the prompts.
struct LaunchChoice {
    int source = -1;
    int target = -1;
    std::string mode;
    launch::Options options;
};

// Takes the command line when all of it can run. Otherwise opens a console, says why a command
// line that was given was not used, and asks for the displays and the mode, asking again until
// each answer can run. Returns false only when the console's input ended before the answers did.
bool ChooseLaunch(const std::string& commandLine, const std::vector<DisplayInfo>& displays,
                  LaunchChoice* out);
