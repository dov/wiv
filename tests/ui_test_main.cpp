// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
// Custom main for the GUI tests: one QApplication for the whole process, with
// HOME / XDG_* redirected to a scratch directory first so that preferences,
// recent files, the image-state DB and the log never touch the user's own.
#include "Logging.h"
#include "qt/PreferencesDialog.h"

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>

#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    QTemporaryDir scratch;
    if (!scratch.isValid()) return 2;
    const QByteArray root = scratch.path().toLocal8Bit();
    qputenv("HOME", root);
    qputenv("XDG_CONFIG_HOME", root + "/config");
    qputenv("XDG_DATA_HOME", root + "/data");
    qputenv("XDG_CACHE_HOME", root + "/cache");
    qputenv("WIV_TEST_SCRATCH", root);

    giv::log::init();
    QApplication app(argc, argv);
    QApplication::setOrganizationName("wiv");
    QApplication::setApplicationName("wiv");
    givqt::PreferencesDialog::applyTheme();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
