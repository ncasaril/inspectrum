/*
 *  Copyright (C) 2026, Jacob Gilbert
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

#include "spectrumview.h"

#include <QContextMenuEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QTextStream>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

#include "plotview.h"
#include "spectrogramplot.h"
#include "util.h"

SpectrumView::SpectrumView(SpectrogramPlot *spectrogram, PlotView *plotView, QWidget *parent)
    : QWidget(parent), spectrogram(spectrogram), plotView(plotView)
{
    setContextMenuPolicy(Qt::DefaultContextMenu);
    setMinimumWidth(160);
    peakPanel = new QWidget(this);
    peakPanel->setAutoFillBackground(true);
    auto layout = new QVBoxLayout(peakPanel);
    layout->setContentsMargins(4, 4, 4, 4);
    peakEnabled = new QCheckBox(tr("Detect peaks"), peakPanel);
    peakEnabled->setObjectName("spectrumDetectPeaks");
    layout->addWidget(peakEnabled);
    peakDetails = new QWidget(peakPanel);
    auto details = new QVBoxLayout(peakDetails);
    details->setContentsMargins(0, 0, 0, 0);
    auto options = new QHBoxLayout;
    peakThreshold = new QDoubleSpinBox(peakDetails);
    peakThreshold->setObjectName("spectrumPeakThreshold");
    peakThreshold->setRange(1, 60); peakThreshold->setValue(6);
    peakThreshold->setSuffix(tr(" dB"));
    peakThreshold->setToolTip(tr("Minimum height above the median and prominence within four bins on each side."));
    options->addWidget(new QLabel(tr("Threshold:"), peakDetails)); options->addWidget(peakThreshold);
    ratioBasis = new QComboBox(peakDetails); ratioBasis->setObjectName("spectrumRatioBasis");
    ratioBasis->addItems({tr("Offset ratios"), tr("RF ratios")});
    ratioBasis->setToolTip(tr("Frequency ratios use signed offsets from capture center, or absolute RF frequencies. Select a row as reference."));
    options->addWidget(ratioBasis); details->addLayout(options);
    peakRange = new QDoubleSpinBox(peakDetails);
    peakRange->setObjectName("spectrumPeakRange");
    peakRange->setRange(10, 160); peakRange->setValue(60); peakRange->setDecimals(0);
    peakRange->setPrefix(tr("Within ")); peakRange->setSuffix(tr(" dB of strongest bin"));
    details->addWidget(peakRange);
    peakTable = new QTableWidget(0, 6, peakDetails);
    peakTable->setObjectName("spectrumPeaks");
    peakTable->setHorizontalHeaderLabels({tr("#"), tr("Frequency (Hz)"), tr("Offset (Hz)"),
                                        tr("Level (dB)"), tr("Δ ref (dB)"), tr("f / ref")});
    peakTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    peakTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    peakTable->setSelectionMode(QAbstractItemView::SingleSelection);
    peakTable->verticalHeader()->hide();
    peakTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    peakTable->setToolTip(tr("Level is uncalibrated windowed FFT-bin power, not dBm or integrated channel power. Δ ref is the level difference from the reference peak."));
    details->addWidget(peakTable, 1);
    peakInfo = new QLabel(peakDetails); peakInfo->setObjectName("spectrumPeakInfo");
    peakInfo->setWordWrap(true); details->addWidget(peakInfo);
    layout->addWidget(peakDetails);
    peakDetails->hide();
    connect(peakEnabled, &QCheckBox::toggled, this, [this](bool enabled) {
        peakDetails->setVisible(enabled);
        setMinimumWidth(enabled ? 400 : 160);
        setMinimumHeight(enabled ? 420 : 80);
        layoutPeakPanel(); refreshPeaks(); update();
    });
    connect(peakThreshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this]() {
        refreshPeaks(); update();
    });
    connect(peakRange, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this]() {
        refreshPeaks(); update();
    });
    connect(ratioBasis, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        refreshPeaks(); update();
    });
    connect(peakTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (row >= 0 && row < int(peaks.size())) {
            referenceBin = peaks[row].bin;
            refreshPeaks(); update();
        }
    });
    layoutPeakPanel();
    connect(spectrogram, &SpectrogramPlot::spectrumReady, this, [this]() { update(); });
}

int SpectrumView::plotBottom() const { return peakPanel->y(); }

void SpectrumView::layoutPeakPanel()
{
    const int panelHeight = peakEnabled->isChecked() ? 320 : peakEnabled->sizeHint().height()+8;
    peakPanel->setGeometry(0, std::max(0, height()-panelHeight), width(), panelHeight);
}

void SpectrumView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutPeakPanel();
}

void SpectrumView::clearPeaks(const QString &message)
{
    peaks.clear();
    peakTable->setRowCount(0);
    peakInfo->setText(message);
}

void SpectrumView::refreshPeaks()
{
    if (!peakEnabled->isChecked()) { clearPeaks({}); return; }
    if (!spectrogram || !hasSample || !cacheValid || cacheSample != currentSample ||
        cacheFFTSize != spectrogram->getFFTSize() || cacheRenderEpoch != spectrogram->renderEpoch() ||
        cachedLines.empty() || currentSample >= spectrogram->input()->count()) {
        clearPeaks(tr("Waiting for the current spectrum…")); return;
    }
    const int n = cacheFFTSize;
    peaks = detectSpectrumPeaks(cachedLines.back(), spectrogram->isRealSignal() ? n/2 : 0,
                                float(peakThreshold->value()), float(peakRange->value()));
    peakTable->setRowCount(int(peaks.size()));
    if (peaks.empty()) { peakInfo->setText(tr("No resolved peaks above threshold.")); return; }
    auto reference = std::find_if(peaks.begin(), peaks.end(), [this](const SpectrumPeak &p) { return p.bin == referenceBin; });
    if (reference == peaks.end()) { reference = peaks.begin(); referenceBin = reference->bin; }
    const double step = spectrogram->getSampleRate()/n;
    const double center = spectrogram->input()->getFrequency();
    const bool rf = ratioBasis->currentIndex() == 1;
    const double refFrequency = (reference->bin-n/2)*step + (rf ? center : 0);
    const float refPower = reference->powerDb;
    int refRow = int(reference-peaks.begin());
    for (int row = 0; row < int(peaks.size()); ++row) {
        const auto &p = peaks[row];
        const double offset = (p.bin-n/2)*step;
        const QString ratio = std::abs(refFrequency) > step*.5
            ? QString::number((offset+(rf ? center : 0))/refFrequency, 'f', 3) : QStringLiteral("—");
        const QStringList values{QString::number(row+1), QString::number(center+offset, 'f', 1),
            QString::number(offset, 'f', 1), QString::number(p.powerDb, 'f', 1),
            QString::number(p.powerDb-refPower, 'f', 1), ratio};
        for (int col = 0; col < values.size(); ++col) {
            auto item = new QTableWidgetItem(values[col]);
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            peakTable->setItem(row, col, item);
        }
    }
    peakTable->selectRow(refRow);
    peakInfo->setText(tr("Reference #%1 · %2 Hz/bin. Select a row to change reference.\nUncalibrated levels; ratios are harmonic candidates, not proof.")
        .arg(refRow+1).arg(step, 0, 'f', 1));
}

QSize SpectrumView::sizeHint() const
{
    return QSize(240, 512);
}

QDockWidget *SpectrumView::dock() const
{
    return qobject_cast<QDockWidget *>(parentWidget());
}

void SpectrumView::setSample(size_t sample)
{
    // Skip redundant FFT + repaint when the column hasn't moved (e.g. the pointer
    // moved vertically within the same spectrogram column).
    if (hasSample && sample == currentSample)
        return;
    currentSample = sample;
    hasSample = true;
    if (peakEnabled->isChecked()) clearPeaks(tr("Waiting for the current spectrum…"));
    update();
}

void SpectrumView::enableScales(bool enabled)
{
    scalesEnabled = enabled;
    update();
}

void SpectrumView::invalidateCache()
{
    cacheValid = false;
    referenceBin = -1;
    clearPeaks(tr("Waiting for the current spectrum…"));
    update();
}

void SpectrumView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (spectrogram == nullptr)
        return;

    const int n = spectrogram->getFFTSize();
    const float powerMin = spectrogram->getPowerMin();
    const float powerMax = spectrogram->getPowerMax();
    const bool realSignal = spectrogram->isRealSignal();

    // Vertical (frequency) band: while docked, pin to the spectrogram's on-screen
    // extent so the frequency axes line up. The band is resolved fresh here (not
    // cached) so it can never go stale. When floated into its own window, fill the
    // whole widget instead.
    int bandTop = 0;
    int bandH = plotBottom();
    // Local y past which the spectrogram is covered by the derived-plot stack;
    // height() means "nothing covered". The band itself stays the full
    // spectrogram height so the bin -> y mapping is unchanged.
    int clipBottom = plotBottom();
    auto d = dock();
    bool floating = d && d->isFloating();
    if (!floating && plotView != nullptr) {
        int topGlobal, specHeight, visibleBottomGlobal;
        if (plotView->spectrogramScreenBand(topGlobal, specHeight, &visibleBottomGlobal)) {
            bandTop = mapFromGlobal(QPoint(0, topGlobal)).y();
            bandH = specHeight;
            clipBottom = std::min(plotBottom(), mapFromGlobal(QPoint(0, visibleBottomGlobal)).y());
        }
    }

    // The PSD spans the full width (power on X). The frequency axis is omitted
    // here: it duplicates the spectrogram's own scale and the pointer readout.
    QRect plotArea(0, bandTop, width(), bandH);

    if (n <= 0 || powerMax <= powerMin || plotArea.width() <= 0 || plotArea.height() <= 0)
        return;

    // Stop the trace where the derived plots take over, so it only ever spans
    // spectrogram rows the user can actually see beside it.
    if (clipBottom < height())
        painter.setClipRect(QRect(0, 0, width(), std::max(0, clipBottom)));

    painter.setPen(Qt::white);
    QFontMetrics fm(painter.font());

    if (!hasSample) {
        painter.drawText(plotArea, Qt::AlignCenter | Qt::TextWordWrap,
                         "Move the pointer over the spectrogram");
        return;
    }

    // Bins span -Fs/2 (index 0) to +Fs/2 (index n-1). For real signals the
    // spectrogram shows only the positive half; mirror that so the trace matches
    // the spectrogram's layout (DC at the bottom, +Fs/2 at the top).
    const int binLo = realSignal ? n / 2 : 0;
    const int binHi = n - 1;
    if (binHi <= binLo)
        return;

    auto powerToX = [&](float power) {
        float t = (power - powerMin) / (powerMax - powerMin);
        t = clamp(t, 0.0f, 1.0f);
        return plotArea.left() + t * plotArea.width();
    };
    // Lower bins (more negative frequency) sit at the bottom, matching the
    // spectrogram's vertical layout.
    auto fracToY = [&](float frac) {
        return plotArea.bottom() - frac * plotArea.height();
    };

    if (scalesEnabled) {
        // Keep the dB axis labels inside the widget even when the band reaches
        // the bottom edge.
        int dbLabelY = std::min(plotArea.bottom() + fm.ascent() + 2, plotBottom() - 2);

        // --- Power grid + bottom scale (dB) ---
        double dbStep = 10.0;
        while ((powerMax - powerMin) / dbStep > 8.0)
            dbStep *= 2.0;
        for (double db = std::ceil(powerMin / dbStep) * dbStep; db <= powerMax; db += dbStep) {
            int x = (int)powerToX((float)db);
            painter.setPen(QPen(QColor(60, 60, 60), 1));
            painter.drawLine(x, plotArea.top(), x, plotArea.bottom());
            painter.setPen(Qt::white);
            QString label = QString::number((int)std::lround(db));
            int lx = clamp(x - fm.boundingRect(label).width() / 2, 0,
                           width() - fm.boundingRect(label).width());
            painter.drawText(lx, dbLabelY, label);
        }
        QString unit = "dB";
        painter.setPen(Qt::white);
        painter.drawText(plotArea.right() - fm.boundingRect(unit).width(), dbLabelY, unit);
    }

    // --- PSD trace(s) ---
    auto drawTrace = [&](const std::vector<float> &line, qreal opacity) {
        QPainterPath path;
        bool started = false;
        for (int bin = binLo; bin <= binHi; bin++) {
            float power = line[bin];
            if (!std::isfinite(power))
                power = powerMin;
            float x = powerToX(power);
            float frac = (float)(bin - binLo) / (float)(binHi - binLo);
            float y = fracToY(frac);
            if (!started) {
                path.moveTo(x, y);
                started = true;
            } else {
                path.lineTo(x, y);
            }
        }
        painter.setOpacity(opacity);
        painter.drawPath(path);
    };

    // Build the list of (column sample, opacity) traces to draw. The centre column
    // is drawn last at full opacity; with persistence on, neighbouring columns on
    // each side are overlaid starting at firstNeighbourOpacity and rolling off
    // gently outward, so the surrounding spectrum forms a faint persistence fan.
    // Outermost (faintest) is listed first so nearer columns, then the centre, end
    // up on top.
    std::vector<std::pair<size_t, qreal>> traces;
    const int stride = spectrogram->getColumnStride();
    const size_t count = spectrogram->input() ? spectrogram->input()->count() : 0;
    if (persistenceEnabled) {
        const int neighbourCount = 20;
        const qreal firstNeighbourOpacity = 0.4;  // nearest neighbour vs the centre
        const qreal falloff = 0.85;               // gentle roll-off across the span
        for (int k = neighbourCount; k >= 1 && stride > 0; k--) {
            qreal opacity = firstNeighbourOpacity * std::pow(falloff, k - 1);
            if (opacity * 255.0 < 1.0)   // rounds to fully transparent; skip the FFT
                continue;
            size_t off = (size_t)k * stride;
            if (off <= currentSample)
                traces.emplace_back(currentSample - off, opacity);
            if (count == 0 || currentSample + off < count)
                traces.emplace_back(currentSample + off, opacity);
        }
    }
    traces.emplace_back(currentSample, 1.0);

    // Memoise the FFT lines: pure repaints (scroll, resize, power-range changes) at
    // the same column reuse them. Only a column / FFT-size / stride / persistence
    // / render-mode change -- or a file reload (invalidateCache()) -- recomputes.
    const unsigned epoch = spectrogram->renderEpoch();
    if (!cacheValid || cacheSample != currentSample || cacheFFTSize != n
            || cacheStride != stride || cachePersistence != persistenceEnabled
            || cacheRenderEpoch != epoch
            || cachedLines.size() != traces.size()) {
        std::vector<std::vector<float>> readyLines(traces.size());
        bool complete = true;
        for (size_t i = 0; i < traces.size(); ++i)
            if (!spectrogram->requestSpectrumLine(traces[i].first, readyLines[i])) complete = false;
        if (complete) {
            if (cacheFFTSize != n) referenceBin = -1;
            cachedLines = std::move(readyLines);
            cacheValid = true;
            cacheSample = currentSample;
            cacheFFTSize = n;
            cacheStride = stride;
            cachePersistence = persistenceEnabled;
            cacheRenderEpoch = epoch;
            refreshPeaks();
        } else if (!cacheValid || cacheFFTSize != n || cacheRenderEpoch != epoch || cachedLines.size() != traces.size()) {
            clearPeaks(tr("Waiting for the current spectrum…"));
            return;
        }
    }

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0, 220, 0), 0.5));  // thin line; neighbours overlay faintly
    for (size_t i = 0; i < traces.size(); i++)
        drawTrace(cachedLines[i], traces[i].second);
    painter.setOpacity(1.0);
    if (peakEnabled->isChecked() && cacheSample == currentSample && cacheRenderEpoch == epoch) {
        painter.setPen(QPen(Qt::yellow, 1));
        for (size_t i = 0; i < peaks.size(); ++i) {
            const auto &peak = peaks[i];
            QPointF point(powerToX(peak.powerDb), fracToY(float(peak.bin-binLo)/float(binHi-binLo)));
            painter.drawEllipse(point, 3, 3);
            painter.drawText(QPointF(std::min(point.x()+5, double(width()-24)), point.y()-3), QString::number(i+1));
        }
    }

    // Frame the plot area (only when scales are shown).
    if (scalesEnabled) {
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(QColor(120, 120, 120), 1));
        painter.drawRect(plotArea);
    }
}

void SpectrumView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu;

    auto save = new QAction("Save spectrum to file...", &menu);
    connect(save, &QAction::triggered, this, [this]() { saveToFile(); });
    save->setEnabled(hasSample);
    menu.addAction(save);

    // Pointer-following vs a draggable marker line on the spectrogram. The
    // marker is a property of the PlotView (one line, shared by every spectrum
    // view), so this reads and writes it there rather than holding local state.
    if (plotView != nullptr) {
        auto marker = new QAction("Lock to marker line", &menu);
        marker->setCheckable(true);
        marker->setChecked(plotView->spectrumMarkerEnabled());
        connect(marker, &QAction::toggled, this, [this](bool on) {
            plotView->setSpectrumMarkerEnabled(on);
            update();
        });
        menu.addAction(marker);
    }

    // Toggle the neighbouring-column overlay (persistence fan).
    auto persistence = new QAction("Persistence", &menu);
    persistence->setCheckable(true);
    persistence->setChecked(persistenceEnabled);
    connect(persistence, &QAction::toggled, this, [this](bool on) {
        persistenceEnabled = on;
        update();
    });
    menu.addAction(persistence);

    // Detach into a floating window (with a normal title bar) or re-dock. While
    // docked the title bar is hidden so the spectrum lines up with the spectrogram.
    if (auto d = dock()) {
        bool floating = d->isFloating();
        auto detach = new QAction(floating ? "Dock" : "Detach to window", &menu);
        connect(detach, &QAction::triggered, this, [d, floating]() {
            d->setFloating(!floating);
        });
        menu.addAction(detach);
    }

    menu.addSeparator();

    auto remove = new QAction("Remove spectrum plot", &menu);
    connect(remove, &QAction::triggered, this, [this]() {
        // The widget lives inside a QDockWidget (set up by MainWindow); closing
        // it removes and deletes the dock (WA_DeleteOnClose), which deletes us.
        if (auto d = dock())
            d->close();
        else
            deleteLater();
    });
    menu.addAction(remove);

    menu.exec(event->globalPos());
}

void SpectrumView::saveToFile()
{
    if (spectrogram == nullptr || !hasSample)
        return;

    QString fileName = QFileDialog::getSaveFileName(
        this, "Save spectrum", QString(), "CSV files (*.csv);;All files (*)");
    if (fileName.isEmpty())
        return;
    if (QFileInfo(fileName).suffix().isEmpty())
        fileName += ".csv";

    const int n = spectrogram->getFFTSize();
    const double sampleRate = spectrogram->getSampleRate();
    const bool realSignal = spectrogram->isRealSignal();
    std::vector<float> line = spectrogram->getSpectrumLine(currentSample);

    const int binLo = realSignal ? n / 2 : 0;

    std::ofstream os(fileName.toStdString());
    if (!os) {
        QMessageBox::warning(this, "Save spectrum",
                             "Could not open the file for writing.");
        return;
    }

    os << "# frequency_offset_hz,power_db\n";
    for (int bin = binLo; bin < n; bin++) {
        double freqOffset = ((double)bin - n / 2.0) / n * sampleRate;
        os << freqOffset << "," << line[bin] << "\n";
    }
    os.close();
}
