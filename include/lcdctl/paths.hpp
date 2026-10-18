// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-monitor-controller - filesystem paths for bundled resources.

#pragma once

#include <string>

namespace lcdctl {

// Directory that contains `res/` (fonts...). Looked up next to the executable,
// in the current directory and, for dev builds, in the source tree.
std::string resourceRoot();

// Absolute path of the bundled default font.
std::string defaultFontPath();

}  // namespace lcdctl
