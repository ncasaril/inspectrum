/* Copyright (C) 2026 inspectrum contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QFile>
#include <QTemporaryFile>
#include <QStringList>
#include <memory>
#include <vector>

// Stage all siblings before touching any destination. A failed replacement
// restores the old files; cancellation simply destroys the staging files.
// Multiple renames cannot provide crash-atomic publication of a SigMF pair.
class ExportFiles
{
public:
    bool open(const QStringList &paths, QString *error);
    QFile &file(size_t index) { return *entries.at(index).staged; }
    bool commit(QString *error);
private:
    struct Entry {
        QString path, backup;
        std::unique_ptr<QTemporaryFile> staged;
        bool installed = false;
    };
    std::vector<Entry> entries;
    void rollback(QString *error);
};
