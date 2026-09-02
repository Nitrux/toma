// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C. <hello@nxos.org>

#pragma once
#include <string>

struct TomaConfig {
    std::string screenshotsPath;
    std::string recordingsPath;
    std::string recordingFormat = "mp4";
    std::string recordingPreset = "balanced";
    unsigned int recordingFramerate = 60;
    unsigned int recordingCountdown = 0;
    bool audioEnabled = false;
    std::string audioDevice = "default";
};

TomaConfig loadTomaConfig();
