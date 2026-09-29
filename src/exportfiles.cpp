/* Copyright (C) 2026 inspectrum contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "exportfiles.h"
#include <QFileInfo>
#include <QSet>

bool ExportFiles::open(const QStringList &paths, QString *error)
{
    QSet<QString> seen;
    for (const auto &path : paths) {
        QFileInfo info(path);
        const QString absolute = info.absoluteFilePath();
        if (seen.contains(absolute) || info.isSymLink() || (info.exists() && !info.isFile())) {
            *error = "Invalid export destination: " + absolute;
            return false;
        }
        seen.insert(absolute);
        Entry entry;
        entry.path = absolute;
        entry.staged.reset(new QTemporaryFile(absolute + ".export-XXXXXX"));
        if (!entry.staged->open()) {
            *error = "Cannot stage " + absolute + ": " + entry.staged->errorString();
            return false;
        }
        entries.push_back(std::move(entry));
    }
    return !entries.empty();
}

void ExportFiles::rollback(QString *error)
{
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->installed && !QFile::remove(it->path))
            *error += "\nCannot remove incomplete replacement: " + it->path;
        if (!it->backup.isEmpty() && !QFile::rename(it->backup, it->path))
            *error += "\nOriginal file retained for recovery at: " + it->backup;
    }
}

bool ExportFiles::commit(QString *error)
{
    for (auto &entry : entries) {
        if (entry.staged->error() != QFile::NoError || !entry.staged->flush()) {
            *error = "Failed writing " + entry.path + ": " + entry.staged->errorString();
            return false;
        }
        entry.staged->close();
    }
    for (auto &entry : entries) {
        QFileInfo info(entry.path);
        if (info.isSymLink() || (info.exists() && !info.isFile())) {
            *error = "Export destination changed: " + entry.path;
            rollback(error);
            return false;
        }
        if (info.exists()) {
            QTemporaryFile backup(entry.path + ".backup-XXXXXX");
            if (!backup.open()) {
                *error = "Cannot reserve backup for " + entry.path;
                rollback(error);
                return false;
            }
            const QString backupPath = backup.fileName();
            if (!backup.remove() || !QFile::rename(entry.path, backupPath)) {
                *error = "Cannot back up " + entry.path;
                rollback(error);
                return false;
            }
            backup.setAutoRemove(false);
            entry.backup = backupPath;
        }
        if (!QFile::rename(entry.staged->fileName(), entry.path)) {
            *error = "Cannot install export: " + entry.path;
            rollback(error);
            return false;
        }
        entry.installed = true;
    }
    for (auto &entry : entries) {
        if (!entry.backup.isEmpty() && !QFile::remove(entry.backup))
            *error += "\nExport saved; old file retained at: " + entry.backup;
    }
    return true;
}
