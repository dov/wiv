// Application-wide logging (spdlog). Every log line lands in
// $HOME/.wiv/logs/, detailed enough that the sequence of user actions
// (files opened, tool toggles, navigation, measurements, errors) can be
// reconstructed from the log alone - e.g. by an AI agent asked "what did I
// just do in wiv" without the user having to describe it themselves.
#pragma once

#include <spdlog/spdlog.h>

namespace giv::log
{

// Creates $HOME/.wiv/logs (if needed), opens a new timestamped log file
// there, and installs it as spdlog's default logger. Call once, early in
// main(), before any other logging call.
void init();

} // namespace giv::log
