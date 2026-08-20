# Toma | [![License](https://img.shields.io/badge/License-BSD_3--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)

Screenshot and screen recording utility for Nitrux.

Toma is a rewrite of [our Grimshot script](https://github.com/Nitrux/nitrux-desktop-settings/blob/41a8e78a2ad3c11dbf84e688088df52ac208187d/usr/bin/grimshot), implemented in C++, and now also supports screen recording.

## Usage

```bash
toma screenshot -f   # Full-screen screenshot
toma screenshot -s   # Select a screenshot region
toma screenshot -w   # Select a window for the screenshot
toma record -f       # Full-screen recording
toma record -s       # Select a recording region
toma record -stop    # Stop the active recording
toma record -w       # Select a window for the recording
```

## Requirements

- Nitrux 7.0.0 and newer.

## Runtime Requirements

```
grim
wf-recorder
slurp
wl-clipboard
libnotify
```

# Licensing

This repository and its contents are licensed under **BSD-3-Clause**.

# Issues

If you find problems with the contents of this repository, please create an issue and use the **🐞 Bug report** template.

## Submitting a bug report

Before submitting a bug, you should look at the [existing bug reports](https://github.com/Nitrux/toma/issues) to verify that no one has reported the bug already.

©2026 Nitrux Latinoamericana S.C.
