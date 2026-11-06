// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// RemoteClient.h - giv's "--remote cmd" / "--json_method" command-line client:
// sends one json-rpc request to an already running viewer (see RpcServer.h
// for the wire format), prints the raw JSON response on stdout and returns
// the process exit code. Needs no GUI, so main() runs it before creating the
// QApplication.
//
#include <QString>

namespace givqt
{

// `method` and `paramsJson` (a JSON array/value, or empty for null) form the
// request. Returns 0 on success, -1 if no server answered.
int runRemoteClient(const QString& host, int port, const QString& method, const QString& paramsJson);

// Splits giv's "--remote" argument ("method param1 param2 ...") into the
// method and a JSON array of string parameters.
void splitRemoteCommand(const QString& command, QString& method, QString& paramsJson);

} // namespace givqt
