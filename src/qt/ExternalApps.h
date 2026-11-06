// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
//
// ExternalApps.h - File > Open in External App. One default command plus
// per-file-type overrides (matched on the file name's extension, so ".dcm"
// can go to a DICOM viewer while everything else goes to the default).
// Commands are QSettings-persisted ("externalApps/...") and edited in
// Edit > Preferences > External Apps.
//
// A type is an extension ("dcm", "nii.gz") or, written with a leading "@", a
// file format as wiv's image plugins recognise it ("@dicom"). Formats come
// from what loading the file found out (or a content check), so they work for
// files without an extension. When both match, the extension wins.
//
// A command is a program plus arguments, split like a shell would; "%f" in
// an argument is replaced by the file's path, and the path is appended as the
// last argument when no "%f" appears. An empty command means the system's
// default application for the file.
//
#include <QList>
#include <QString>

namespace givqt::ExternalApps
{

struct Rule
{
    QString extensions; // comma/space separated: "dcm, dicom", "nii.gz", "@dicom"
    QString command;
};

// A named application for File > Open With; slot i (0-based) is bound to
// Ctrl+(i+1) by default.
struct App
{
    QString name;
    QString command;
};
constexpr int kMaxApps = 9;

struct Config
{
    QList<App> apps;
    QString defaultCommand;
    QList<Rule> rules;
};

Config load();
void save(const Config& config);

// The command that handles `path` (whose plugin format is `format`, if known):
// the rule whose extension matches with the longest suffix, else one naming
// the format, else the default. Empty means "system default application".
QString commandFor(const Config& config, const QString& path, const QString& format = {});

// Starts `command` on `path` detached. On failure returns false and sets
// *error (when given).
bool launch(const QString& command, const QString& path, QString* error = nullptr);

} // namespace givqt::ExternalApps
