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

#include <QMessageBox>
#include <QtWidgets>
#include <QPixmapCache>
#include <QRubberBand>
#include <sstream>

#include "mainwindow.h"
#include "util.h"

MainWindow::MainWindow()
{
    baseTitle = tr("inspectrum - jacobagilbert edition");
    setWindowTitle(baseTitle);

    QPixmapCache::setCacheLimit(40960);

    dock = new SpectrogramControls(tr("Controls"), this);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, dock);

    input = new InputSource();
    input->subscribe(this);
    // Mirror annotation changes into the title bar dirty marker and dock
    // button state. Wired before PlotView's own callback so the first add
    // doesn't slip through.
    input->addAnnotationCallback([this]() { onAnnotationsChanged(); });

    plots = new PlotView(input);
    setCentralWidget(plots);

    connect(dock, &SpectrogramControls::saveAnnotationsRequested,
            this, &MainWindow::saveAnnotations);

    // Connect dock inputs
    connect(dock, &SpectrogramControls::openFile, this, &MainWindow::openFile);
    connect(dock->sampleRate, static_cast<void (QLineEdit::*)(const QString&)>(&QLineEdit::textChanged), this, static_cast<void (MainWindow::*)(QString)>(&MainWindow::setSampleRate));
    connect(dock, static_cast<void (SpectrogramControls::*)(int, int)>(&SpectrogramControls::fftOrZoomChanged), plots, &PlotView::setFFTAndZoom);
    connect(dock->powerMaxSlider, &QSlider::valueChanged, plots, &PlotView::setPowerMax);
    connect(dock->powerMinSlider, &QSlider::valueChanged, plots, &PlotView::setPowerMin);
    connect(dock->cursorsCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableCursors);
    connect(dock->scalesCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableScales);
    connect(dock->annosCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableAnnotations);
    connect(dock->annosCheckBox, &QCheckBox::stateChanged, dock, &SpectrogramControls::enableAnnotations);
    connect(dock->annoLabelCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableAnnoLabels);
    connect(dock->commentsCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableAnnotationCommentsTooltips);
    connect(dock->annoColorCheckBox, &QCheckBox::stateChanged, plots, &PlotView::enableAnnoColors);
    // Connect derived plot height adjustment
    connect(dock, &SpectrogramControls::derivedHeightChanged, plots, &PlotView::setDerivedPlotHeight);
    // fast-path FM demodulation toggle
    connect(dock, &SpectrogramControls::fastDemodChanged, plots, &PlotView::enableFastDemod);
    // allow user to control number of threads in the Qt thread pool
    connect(dock, &SpectrogramControls::threadsChanged, plots, &PlotView::setMaxThreads);
    // FM post-demod LPF cutoff, method, and block-average decimation
    connect(dock, &SpectrogramControls::fmLpfChanged, plots, &PlotView::setFmLpfCutoff);
    connect(dock, &SpectrogramControls::fmLpfMethodChanged, plots, &PlotView::setFmLpfMethod);
    connect(dock, &SpectrogramControls::fmDecimChanged, plots, &PlotView::setFmDecimation);
    connect(dock, &SpectrogramControls::fmPredemodDecimChanged, plots, &PlotView::setFmPredemodDecimation);
    // Auto-tune button: dock asks PlotView, PlotView picks values and applies
    // them, then echoes them back so the dock widgets reflect the new state.
    connect(dock, &SpectrogramControls::autoLpfRequested, plots, &PlotView::autoTuneFmLpf);
    connect(plots, &PlotView::fmAutoLpfComputed, dock, &SpectrogramControls::applyAutoLpf);
    // Auto period-detection on the visible FM trace, fed into the dock label.
    connect(plots, &PlotView::autoPeriodChanged, dock, &SpectrogramControls::applyAutoPeriod);
    connect(dock, &SpectrogramControls::periodAnalysisChanged, plots, &PlotView::setPeriodAnalysisEnabled);
    // Spectrogram render mode (Standard / Reassigned) and the reassignment
    // noise-floor threshold. Both end up on SpectrogramPlot via PlotView.
    connect(dock, &SpectrogramControls::spectrogramModeChanged, plots, &PlotView::setSpectrogramMode);
    connect(dock, &SpectrogramControls::reassignmentFloorChanged, plots, &PlotView::setReassignmentFloor);
    connect(dock, &SpectrogramControls::reassignmentWindowChanged, plots, &PlotView::setReassignmentWindow);
    connect(dock, &SpectrogramControls::reassignmentSplatChanged, plots, &PlotView::setReassignmentSplat);
    connect(dock->cursorSymbolsSpinBox, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), plots, &PlotView::setCursorSegments);

    // Connect dock outputs
    connect(plots, &PlotView::timeSelectionChanged, dock, &SpectrogramControls::timeSelectionChanged);
    connect(plots, &PlotView::zoomIn, dock, &SpectrogramControls::zoomIn);
    connect(plots, &PlotView::zoomOut, dock, &SpectrogramControls::zoomOut);

    // Set defaults after making connections so everything is in sync
    dock->setDefaults();
    // Show cursor (mouse) position in time and frequency in the status bar
    // *and* mirror the sample value under the cursor into the dock's
    // "Cursor value:" label (the dock entry is the discoverable one; the
    // status bar text is just a bonus for users used to looking down).
    connect(plots, &PlotView::mousePositionChanged,
            this, [this](double timePos, double freqPos, QString valueText) {
        QString msg = QString("Time: %1 s   Freq: %2 Hz")
            .arg(timePos, 0, 'f', 6)
            .arg(freqPos, 0, 'f', 0);
        if (!valueText.isEmpty()) {
            msg += QStringLiteral("   Value: ") + valueText;
        }
        statusBar()->showMessage(msg, 0);
        this->dock->applyCursorValue(valueText);
    });

}

void MainWindow::openFile(QString fileName)
{
    QString title="%1 jacobagilbert edition: %2";
    baseTitle = title.arg(QApplication::applicationName(), fileName.section('/',-1,-1));
    refreshWindowTitle();

    // Try to parse osmocom_fft filenames and extract the sample rate and center frequency.
    // Example file name: "name-f2.411200e+09-s5.000000e+06-t20160807180210.cfile"
    QRegExp rx("(.*)-f(.*)-s(.*)-.*\\.cfile");
    QString basename = fileName.section('/',-1,-1);

    if (rx.exactMatch(basename)) {
        QString centerfreq = rx.cap(2);
        QString samplerate = rx.cap(3);

        std::stringstream ss(samplerate.toUtf8().constData());

        // Needs to be a double as the number is in scientific format
        double rate;
        ss >> rate;
        if (!ss.fail()) {
            setSampleRate(rate);
        }
    }

    // gqrx capture filename: gqrx_<YYYYMMDD>_<HHMMSS>_<centerfreqHz>_<sampleRateHz>_fc.raw
    // Example: gqrx_20260429_031912_251580000_10000000_fc.raw
    // Both fields are integer Hz, so a clean regex+toDouble round-trip works.
    QRegExp gqrxRx("gqrx_\\d{8}_\\d{6}_(\\d+)_(\\d+)_fc\\.raw");
    if (gqrxRx.exactMatch(basename)) {
        bool ok = false;
        double centerHz = gqrxRx.cap(1).toDouble(&ok);
        if (ok && centerHz > 0.0) {
            input->setCenterFrequency(centerHz);
        }
        double rateHz = gqrxRx.cap(2).toDouble(&ok);
        if (ok && rateHz > 0.0) {
            setSampleRate(rateHz);
        }
    }

    try
    {
        input->openFile(fileName.toUtf8().constData());
    }
    catch (const std::exception &ex)
    {
        QMessageBox msgBox(QMessageBox::Critical, "Inspectrum openFile error", QString("%1: %2").arg(fileName).arg(ex.what()));
        msgBox.exec();
    }
}

void MainWindow::invalidateEvent()
{
    plots->setSampleRate(input->rate());

    // Only update the text box if it is not already representing
    // the current value. Otherwise the cursor might jump or the
    // representation might change (e.g. to scientific).
    double currentValue = dock->sampleRate->text().toDouble();
    if(QString::number(input->rate()) != QString::number(currentValue)) {
        setSampleRate(input->rate());
    }
}

void MainWindow::setSampleRate(QString rate)
{
    auto sampleRate = rate.toDouble();
    input->setSampleRate(sampleRate);
    plots->setSampleRate(sampleRate);

    // Save the sample rate in settings as we're likely to be opening the same
    // file across multiple runs. Skip the save if the value is bogus (≤ 0):
    // a transient zero (cleared text field, in-flight typing) used to poison
    // QSettings and turn FrequencyDemod's Hz scaling into "multiply by 0",
    // which made the FM plot a flat line on the next launch with no obvious
    // cause.
    if (sampleRate > 0.0) {
        QSettings settings;
        settings.setValue("SampleRate", sampleRate);
    }
}

void MainWindow::setSampleRate(double rate)
{
    // 'g' format (the QString::number default) switches to scientific at 7+
    // digits, so 10 MHz renders as "1e+07" and looks to the user like the
    // rate didn't parse. Display sample rates as plain integers in Hz.
    dock->sampleRate->setText(QString::number(rate, 'f', 0));
}

void MainWindow::setFormat(QString fmt)
{
    input->setFormat(fmt.toUtf8().constData());
}

void MainWindow::onAnnotationsChanged()
{
    refreshWindowTitle();
    if (dock)
        dock->setAnnotationsDirty(input->annotationsDirty());
}

void MainWindow::refreshWindowTitle()
{
    if (input && input->annotationsDirty())
        setWindowTitle(baseTitle + QStringLiteral(" *"));
    else
        setWindowTitle(baseTitle);
}

void MainWindow::saveAnnotations()
{
    QString err;
    if (!input->saveAnnotations(&err)) {
        QMessageBox::warning(this, tr("Save annotations"),
                             tr("Could not save: %1").arg(err));
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (input && input->annotationsDirty()) {
        auto choice = QMessageBox::question(
            this, tr("Unsaved annotations"),
            tr("Annotations have been edited but not saved. Save before closing?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (choice == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
        if (choice == QMessageBox::Save) {
            QString err;
            if (!input->saveAnnotations(&err)) {
                QMessageBox::warning(this, tr("Save annotations"),
                                     tr("Could not save: %1").arg(err));
                event->ignore();
                return;
            }
        }
    }
    QMainWindow::closeEvent(event);
}
