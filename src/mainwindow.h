/*
 *  Copyright (C) 2015, Mike Walters <mike@flomp.net>
 *
 *  This file is part of inspectrum.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <QMainWindow>
#include <QScrollArea>
#include "spectrogramcontrols.h"
#include "plotview.h"

class QMenu;

class MainWindow : public QMainWindow, Subscriber
{
    Q_OBJECT

public:
    MainWindow();
    void changeSampleRate(double rate);

public slots:
    void openFile(QString fileName);
    void setSampleRate(QString rate);
    void setSampleRate(double rate);
    void setFormat(QString fmt);
    void invalidateEvent() override;
    // Persist the current annotation list to a .sigmf-meta sidecar via
    // InputSource::saveAnnotations. Triggered by the dock button.
    void saveAnnotations();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void onAnnotationsChanged();
    void refreshWindowTitle();
    // (Re)populate the Tools → Run plugin menu from the manifests in
    // ~/.config/inspectrum/plugins. Called at startup and after each open.
    void rebuildPluginMenu();

    SpectrogramControls *dock;
    PlotView *plots;
    InputSource *input;
    // The app's only menu — built lazily; "Run plugin" submenu lives under it.
    QMenu *pluginMenu = nullptr;
    // Remembered base title (no dirty marker); refreshWindowTitle appends
    // "*" when annotations are unsaved.
    QString baseTitle;
};
