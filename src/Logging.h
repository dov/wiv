// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
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
//
// Also reroutes Qt messages (qDebug/qWarning/...) and VSG's logger into
// spdlog; nothing is written to the console by spdlog itself.
void init();

// Redirects fd 1/2 into the log so anything written straight to
// stdout/stderr (e.g. by drivers or C libraries) is logged instead. Call
// after command-line parsing, so --help/usage output still reaches the
// terminal. No-op on Windows.
void captureStdStreams();

} // namespace giv::log
