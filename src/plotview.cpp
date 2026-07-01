/*
 *  Copyright (C) 2015-2016, Mike Walters <mike@flomp.net>
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

#include "plotview.h"
#include "amplitudedemod.h"
#include "annotationdialog.h"
#include "frequencydemod.h"
#include "fskdemod.h"
#include "fskpolarplot.h"
#include "histogramplot.h"
#include "util.h"
#include <QPixmapCache>
#include <algorithm>
#include <climits>
#include <cmath>
#include <iostream>
#include <fstream>
#include <type_traits>
#include <QtGlobal>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QWheelEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QProgressDialog>
#include <QRadioButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QToolTip>
#include <QVBoxLayout>
#include "plots.h"
#include "latencylog.h"
#include <QThreadPool>

PlotView::PlotView(InputSource *input) : cursors(this), viewRange({0, 0}), derivedPlotHeight(200)
{
    mainSampleSource = input;
    setDragMode(QGraphicsView::ScrollHandDrag);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setMouseTracking(true);
    enableCursors(false);
    connect(&cursors, &Cursors::cursorsMoved, this, &PlotView::cursorsMoved);

    spectrogramPlot = new SpectrogramPlot(std::shared_ptr<SampleSource<std::complex<float>>>(mainSampleSource));
    auto tunerOutput = std::dynamic_pointer_cast<SampleSource<std::complex<float>>>(spectrogramPlot->output());

    enableScales(true);

    enableAnnotations(true);
    enableAnnoLabels(true);
    enableAnnoColors(true);
    enableAnnotationCommentsTooltips(true);

    addPlot(spectrogramPlot);

    mainSampleSource->subscribe(this);

    // Repaint annotation overlays whenever the input source's annotation
    // list changes (add / edit / delete / save). Title-bar dirty handling
    // lives in MainWindow, which sets its own callback after construction.
    static_cast<InputSource*>(mainSampleSource)
        ->addAnnotationCallback([this]() { onAnnotationsChanged(); });

    // Debounced re-analysis of the visible FM trace's dominant period.
    // Restarted from updateView() and from every FM-filter setter so the
    // dock label stays in sync with what the user sees, but only one scan
    // runs per idle period.
    periodTimer = new QTimer(this);
    periodTimer->setSingleShot(true);
    periodTimer->setInterval(200);
    connect(periodTimer, &QTimer::timeout,
            this, &PlotView::analyzeVisiblePeriod);
}

void PlotView::enableFastDemod(bool enabled)
{
    fmFastDemod = enabled;
    // clear any cached trace tiles so new demod data is used
    QPixmapCache::clear();
    // walk all derived TracePlot instances and update their demod mode
    for (auto &plt : plots) {
        if (auto tp = dynamic_cast<TracePlot*>(plt.get())) {
            auto src = tp->source();
            if (auto fd = dynamic_cast<FrequencyDemod*>(src.get())) {
                fd->setCheapDemod(enabled);
            }
            if (auto fsk = dynamic_cast<FskDemod*>(src.get())) {
                fsk->setCheapDemod(enabled);
            }
        }
    }
    // repaint everything
    viewport()->update();
}
 
void PlotView::setMaxThreads(int threads)
{
    // configure the global QThreadPool for QtConcurrent::run
    QThreadPool::globalInstance()->setMaxThreadCount(threads);
}

void PlotView::setFmLpfCutoff(double hz)
{
    fmLpfCutoffHz = hz;
    for (auto &plt : plots) {
        if (auto tp = dynamic_cast<TracePlot*>(plt.get())) {
            auto src = tp->source();
            if (auto fd = dynamic_cast<FrequencyDemod*>(src.get())) {
                fd->setPostLpfCutoff(hz);
            }
            if (auto fsk = dynamic_cast<FskDemod*>(src.get())) {
                fsk->setPostLpfCutoff(hz);
            }
        }
    }
    QPixmapCache::clear();
    viewport()->update();
    if (periodTimer) periodTimer->start();
}

// Symbol rate (baud) for the differential constellation. Kept separate from
// the FM LPF cutoff: the polar delay is a symbol-period quantity, not a filter
// bandwidth, and conflating them reshaped the FM trace as a side effect.
void PlotView::setSymbolRate(double baud)
{
    symbolRateHz = baud;
    for (auto &plt : plots) {
        if (auto fskp = dynamic_cast<FskPolarPlot*>(plt.get())) {
            fskp->setSymbolRate(baud);
        }
    }
}

// Signal-strength gate (% of window peak) for the FSK polar plot's scope.
void PlotView::setConstellationGate(int pct)
{
    constellationGatePct = pct;
    for (auto &plt : plots) {
        if (auto fskp = dynamic_cast<FskPolarPlot*>(plt.get())) {
            fskp->setLevelGate(pct);
        }
    }
}

// Toggle symbol-timed (1 point/symbol) vs full-rate constellation rendering.
void PlotView::setConstellationSymbolTimed(bool on)
{
    constellationSymbolTimed = on;
    for (auto &plt : plots) {
        if (auto fskp = dynamic_cast<FskPolarPlot*>(plt.get())) {
            fskp->setSymbolTimed(on);
        }
    }
}

void PlotView::setFmDecimation(int n)
{
    if (n < 1) n = 1;
    fmDecim = n;
    for (auto &plt : plots) {
        if (auto tp = dynamic_cast<TracePlot*>(plt.get())) {
            auto src = tp->source();
            if (auto fd = dynamic_cast<FrequencyDemod*>(src.get())) {
                fd->setPostDecimation(n);
            }
            if (auto fsk = dynamic_cast<FskDemod*>(src.get())) {
                fsk->setPostDecimation(n);
            }
        }
    }
    QPixmapCache::clear();
    viewport()->update();
    if (periodTimer) periodTimer->start();
}

void PlotView::setFmLpfMethod(int method)
{
    fmLpfMethod = method;
    auto m = static_cast<FrequencyDemod::LpfMethod>(method);
    for (auto &plt : plots) {
        if (auto tp = dynamic_cast<TracePlot*>(plt.get())) {
            auto src = tp->source();
            if (auto fd = dynamic_cast<FrequencyDemod*>(src.get())) {
                fd->setPostLpfMethod(m);
            }
            if (auto fsk = dynamic_cast<FskDemod*>(src.get())) {
                fsk->setPostLpfMethod(m);
            }
        }
    }
    QPixmapCache::clear();
    viewport()->update();
    if (periodTimer) periodTimer->start();
}

void PlotView::setFmPredemodDecimation(int m)
{
    if (m < 1) m = 1;
    fmPredemodDecim = m;
    for (auto &plt : plots) {
        if (auto tp = dynamic_cast<TracePlot*>(plt.get())) {
            auto src = tp->source();
            if (auto fd = dynamic_cast<FrequencyDemod*>(src.get())) {
                fd->setPredemodDecimation(m);
            }
            if (auto fsk = dynamic_cast<FskDemod*>(src.get())) {
                fsk->setPredemodDecimation(m);
            }
        }
    }
    QPixmapCache::clear();
    viewport()->update();
    if (periodTimer) periodTimer->start();
}

// Amplitude squelch (% of window-peak |IQ|) for every FM plot. Blanks the
// discriminator output in the noise gaps so it stops dominating the autoscale.
void PlotView::setFmSquelch(int pct)
{
    fmSquelchPct = pct;
    for (auto &plt : plots) {
        if (auto tp = dynamic_cast<TracePlot*>(plt.get())) {
            if (auto fd = dynamic_cast<FrequencyDemod*>(tp->source().get()))
                fd->setAmplitudeSquelch(pct / 100.0);
        }
    }
    QPixmapCache::clear();
    viewport()->update();
    if (periodTimer) periodTimer->start();
}

// Switch every AM (AmplitudeDemod) plot between linear power and dB.
void PlotView::setAmDbMode(bool on)
{
    amDbMode = on;
    for (auto &plt : plots) {
        if (auto tp = dynamic_cast<TracePlot*>(plt.get())) {
            if (auto am = dynamic_cast<AmplitudeDemod*>(tp->source().get()))
                am->setDbMode(on);
        }
    }
    QPixmapCache::clear();
    viewport()->update();
}

// Full-scale reference (dBm) added to AM plots in dB mode. 0 = dBFS.
void PlotView::setAmReferenceLevel(double dbm)
{
    amRefLevelDbm = dbm;
    for (auto &plt : plots) {
        if (auto tp = dynamic_cast<TracePlot*>(plt.get())) {
            if (auto am = dynamic_cast<AmplitudeDemod*>(tp->source().get()))
                am->setReferenceLevelDbm(dbm);
        }
    }
    QPixmapCache::clear();
    viewport()->update();
}

void PlotView::autoTuneFmLpf()
{
    if (!spectrogramPlot || sampleRate <= 0.0) return;
    auto src = spectrogramPlot->output();
    if (!src) return;
    // relativeBandwidth is the tuner span as a fraction of Fs. It lives on
    // SampleSource<T>, not AbstractSampleSource, so we have to cast — the
    // tuner output is always complex<float>.
    auto typed = std::dynamic_pointer_cast<SampleSource<std::complex<float>>>(src);
    double relBw = typed ? typed->relativeBandwidth() : 1.0;
    if (relBw <= 0.0 || relBw > 1.0) relBw = 1.0;
    const double tunerBw = relBw * sampleRate;

    // Heuristic cutoff: tunerBw / 50 — captures the slowly-varying envelope
    // of typical FM modulation while clamping to a usable range so very
    // wide or very narrow tuners still get a sane number.
    double cutoff = tunerBw / 50.0;
    if (cutoff < 500.0)   cutoff = 500.0;
    if (cutoff > 50000.0) cutoff = 50000.0;

    // Pre-demod decimation must keep the freqdem's effective Nyquist above
    // tunerBw — i.e. M ≤ 1/(2·relBw). Pick 80% of that for safety. Also
    // bound by a useful-cutoff target (Fs/(4·fc)) so we don't decimate
    // past what the post-LPF can take advantage of, and a hard ceiling to
    // keep msresamp's per-stage halfband cost bounded.
    int M_alias = std::max(1, static_cast<int>(std::floor(0.8 / (2.0 * relBw))));
    int M_lpf   = std::max(1, static_cast<int>(std::floor(sampleRate / (4.0 * cutoff))));
    int M = std::min({M_alias, M_lpf, 100});
    int N = 1; // post-decim adds nothing once predemod decim is on

    setFmLpfCutoff(cutoff);
    setFmPredemodDecimation(M);
    setFmDecimation(N);
    emit fmAutoLpfComputed(cutoff, M, N);
}

void PlotView::setPeriodAnalysisEnabled(bool enabled)
{
    periodAnalysisEnabled = enabled;
    if (enabled) {
        if (periodTimer) periodTimer->start();
    } else {
        // Clear any existing markers and the dock label.
        for (auto &plt : plots) {
            if (auto tp = dynamic_cast<TracePlot*>(plt.get())) {
                tp->setPeriodMarkers({});
            }
        }
        emit autoPeriodChanged(0.0);
    }
}

void PlotView::setSpectrogramMode(int mode)
{
    if (spectrogramPlot) {
        spectrogramPlot->setSpectrogramMode(mode);
    }
}

void PlotView::setReassignmentFloor(int floorDb)
{
    if (spectrogramPlot) {
        spectrogramPlot->setReassignmentFloor(floorDb);
    }
}

void PlotView::setReassignmentWindow(int wt)
{
    if (spectrogramPlot) {
        spectrogramPlot->setWindowType(wt);
    }
}

void PlotView::setReassignmentSplat(int sm)
{
    if (spectrogramPlot) {
        spectrogramPlot->setSplatMethod(sm);
    }
}

void PlotView::analyzeVisiblePeriod()
{
    // Find the first derived float-source plot (FM trace by convention) and
    // estimate its dominant period over the currently-visible sample range
    // by counting upward crossings of the trace's mean (with hysteresis).
    // Records each detected crossing point so TracePlot::paintFront can
    // overlay markers + a connecting line.
    if (!periodAnalysisEnabled || sampleRate <= 0.0) {
        emit autoPeriodChanged(0.0);
        return;
    }
    TracePlot *targetPlot = nullptr;
    SampleSource<float> *fsrc = nullptr;
    for (auto &plt : plots) {
        auto tp = dynamic_cast<TracePlot*>(plt.get());
        if (!tp) continue;
        auto typed = std::dynamic_pointer_cast<SampleSource<float>>(tp->source());
        if (typed) { fsrc = typed.get(); targetPlot = tp; break; }
    }
    if (!fsrc) {
        emit autoPeriodChanged(0.0);
        return;
    }

    // Limit the analysis size — the FFT-free zero-crossing pass is O(N) and
    // we just need a reasonable estimate, not microsecond precision.
    constexpr size_t kMaxAnalyseSamples = 200000;
    size_t n = viewRange.maximum > viewRange.minimum
             ? (viewRange.maximum - viewRange.minimum) : 0;
    if (n < 256) {
        if (targetPlot) targetPlot->setPeriodMarkers({});
        emit autoPeriodChanged(0.0);
        return;
    }
    if (n > kMaxAnalyseSamples) n = kMaxAnalyseSamples;

    auto data = fsrc->getSamples(viewRange.minimum, n);
    if (!data) {
        if (targetPlot) targetPlot->setPeriodMarkers({});
        emit autoPeriodChanged(0.0);
        return;
    }

    double sum = 0.0;
    size_t valid = 0;
    for (size_t i = 0; i < n; ++i) {
        if (std::isfinite(data[i])) { sum += data[i]; ++valid; }
    }
    if (valid < 256) {
        if (targetPlot) targetPlot->setPeriodMarkers({});
        emit autoPeriodChanged(0.0);
        return;
    }
    const double mean = sum / valid;

    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(data[i])) continue;
        if (data[i] < mn) mn = data[i];
        if (data[i] > mx) mx = data[i];
    }
    const double span = mx - mn;
    if (span <= 0.0) {
        if (targetPlot) targetPlot->setPeriodMarkers({});
        emit autoPeriodChanged(0.0);
        return;
    }
    const double hyst = 0.1 * span;

    // Each upward crossing of (data - mean) = 0 with hysteresis = one
    // period boundary. Record the absolute sample index of each crossing
    // so the plot can render markers.
    std::vector<size_t> peaks;
    bool armedLow = false;
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(data[i])) continue;
        const double d = data[i] - mean;
        if (d < -hyst) armedLow = true;
        else if (armedLow && d > hyst) {
            peaks.push_back(viewRange.minimum + i);
            armedLow = false;
        }
    }
    if (peaks.size() < 2) {
        if (targetPlot) targetPlot->setPeriodMarkers({});
        emit autoPeriodChanged(0.0);
        return;
    }
    // Period = (last peak time - first peak time) / (peaks - 1) — uses the
    // span between detected peaks rather than the visible duration so an
    // off-by-one near the edges doesn't bias the estimate.
    const double dt = static_cast<double>(peaks.back() - peaks.front())
                    / (peaks.size() - 1) / sampleRate;
    if (targetPlot) targetPlot->setPeriodMarkers(std::move(peaks));
    emit autoPeriodChanged(dt);
}

void PlotView::addPlot(Plot *plot)
{
    plots.emplace_back(plot);
    // If this is a derived plot (not the main spectrogram), set its height
    if (plots.size() > 1) {
        plot->setPlotHeight(derivedPlotHeight);
    }
    // Propagate the currently-configured FM settings to newly added FM/FSK plots.
    if (auto tp = dynamic_cast<TracePlot*>(plot)) {
        if (auto fd = dynamic_cast<FrequencyDemod*>(tp->source().get())) {
            fd->setPostLpfMethod(static_cast<FrequencyDemod::LpfMethod>(fmLpfMethod));
            fd->setPostLpfCutoff(fmLpfCutoffHz);
            fd->setPostDecimation(fmDecim);
            fd->setPredemodDecimation(fmPredemodDecim);
            fd->setCheapDemod(fmFastDemod);
            fd->setAmplitudeSquelch(fmSquelchPct / 100.0);
        }
        if (auto fsk = dynamic_cast<FskDemod*>(tp->source().get())) {
            fsk->setPostLpfMethod(static_cast<FrequencyDemod::LpfMethod>(fmLpfMethod));
            fsk->setPostLpfCutoff(fmLpfCutoffHz);
            fsk->setPostDecimation(fmDecim);
            fsk->setPredemodDecimation(fmPredemodDecim);
            fsk->setCheapDemod(fmFastDemod);
        }
        if (auto am = dynamic_cast<AmplitudeDemod*>(tp->source().get())) {
            am->setDbMode(amDbMode);
            am->setReferenceLevelDbm(amRefLevelDbm);
        }
    }
    if (auto fskp = dynamic_cast<FskPolarPlot*>(plot)) {
        fskp->setSymbolRate(symbolRateHz);
        fskp->setLevelGate(constellationGatePct);
        fskp->setSymbolTimed(constellationSymbolTimed);
        fskp->setSelection(cursorsEnabled, selectedSamples);
    }
    if (auto hist = dynamic_cast<HistogramPlot*>(plot)) {
        hist->setSelection(cursorsEnabled, selectedSamples);
    }
    connect(plot, &Plot::repaint, this, &PlotView::repaint);
}

void PlotView::mouseMoveEvent(QMouseEvent *event)
{
    updateAnnotationTooltip(event);

    int x = event->pos().x();
    int y = event->pos().y();
    int hScroll = horizontalScrollBar()->value();
    size_t sampleIdx = columnToSample(x + hScroll);
    double timePos = (sampleRate > 0.0) ? (sampleIdx / sampleRate) : 0.0;
    int viewportH = viewport()->height();

    double freqPos = 0.0;
    QString valueText;

    // Derived plots are stacked at the bottom of the viewport (each
    // `derivedPlotHeight` tall). Top of the stack is at viewportH minus
    // their total height. If the cursor is below that, it's over a derived
    // plot — figure out which one, read its value, and push a hover-cursor
    // overlay to that plot. Clear hover on every other derived plot so
    // only the active one shows the marker.
    const int derivedCount = static_cast<int>(plots.size()) - 1;
    const int derivedTotalH = derivedCount * derivedPlotHeight;
    const int derivedTop = viewportH - derivedTotalH;
    int activePlotIdx = -1;
    double hoverValue = std::numeric_limits<double>::quiet_NaN();
    if (derivedCount > 0 && y >= derivedTop) {
        const int posInDerived = y - derivedTop;
        const int plotIdx = 1 + posInDerived / derivedPlotHeight;
        if (plotIdx >= 1 && static_cast<size_t>(plotIdx) < plots.size()) {
            activePlotIdx = plotIdx;
            valueText = sampleValueText(plots[plotIdx].get(), sampleIdx, &hoverValue);
        }
    } else {
        // Cursor is over the spectrogram (scrollable) — compute frequency
        // offset from Y so the existing status-bar field stays correct.
        int vScroll = verticalScrollBar()->value();
        int contentY = y + vScroll;
        int plotH = spectrogramPlot->height();
        if (contentY >= 0 && contentY < plotH && sampleRate > 0.0) {
            double hzPerPixel = sampleRate / plotH;
            freqPos = ((plotH / 2.0) - contentY) * hzPerPixel;
        }
    }

    // Push hover state onto the active derived plot, clear all others.
    for (size_t i = 1; i < plots.size(); ++i) {
        if (auto tp = dynamic_cast<TracePlot*>(plots[i].get())) {
            if (static_cast<int>(i) == activePlotIdx) {
                tp->setHoverCursor(true, sampleIdx, hoverValue, valueText);
            } else {
                tp->setHoverCursor(false, 0, 0.0, QString());
            }
        }
    }

    emit mousePositionChanged(timePos, freqPos, valueText);
    QGraphicsView::mouseMoveEvent(event);
}

QString PlotView::sampleValueText(Plot *plot, size_t sampleIdx, double *rawValueOut)
{
    if (rawValueOut) *rawValueOut = std::numeric_limits<double>::quiet_NaN();
    auto tp = dynamic_cast<TracePlot*>(plot);
    if (!tp) return QString();
    auto src = tp->source();
    if (!src) return QString();

    // Float source. FrequencyDemod now scales its output to Hz at the
    // source (see fillBatchCache / work in frequencydemod.cpp), so all the
    // hover formatter has to do is detect "this is the FM trace" and
    // append "Hz". The Y-axis labels read in Hz too — same value either
    // way. AM and threshold are dimensionless w.r.t. the IQ scale, so
    // show the raw float.
    if (auto fsrc = std::dynamic_pointer_cast<SampleSource<float>>(src)) {
        auto data = fsrc->getSamples(sampleIdx, 1);
        if (!data) return QString();
        float v = data[0];
        if (rawValueOut) *rawValueOut = v;
        if (!std::isfinite(v)) return QStringLiteral("NaN");
        if (dynamic_cast<FrequencyDemod*>(src.get())) {
            return QStringLiteral("%1Hz")
                .arg(QString::fromStdString(formatSIValue(v)));
        }
        // AM in dB mode reads in dBFS, or dBm once a full-scale reference is
        // set (ref 0 = dBFS by convention).
        if (auto am = dynamic_cast<AmplitudeDemod*>(src.get())) {
            if (am->dbMode()) {
                const char *unit = (am->referenceLevelDbm() != 0.0) ? " dBm" : " dBFS";
                return QString::number(v, 'f', 1) + QLatin1String(unit);
            }
        }
        return QString::number(v, 'g', 6);
    }
    // Complex source (IQ plot): show I and Q plus magnitude.
    if (auto csrc = std::dynamic_pointer_cast<SampleSource<std::complex<float>>>(src)) {
        auto data = csrc->getSamples(sampleIdx, 1);
        if (!data) return QString();
        float i = data[0].real();
        float q = data[0].imag();
        float mag = std::hypot(i, q);
        if (rawValueOut) *rawValueOut = mag;
        return QStringLiteral("I=%1 Q=%2 |·|=%3")
            .arg(i, 0, 'g', 4)
            .arg(q, 0, 'g', 4)
            .arg(mag, 0, 'g', 4);
    }
    return QString();
}

void PlotView::mouseReleaseEvent(QMouseEvent *event)
{
    // This is used to show the tooltip again on drag release if the mouse is
    // hovering over an annotation.
    updateAnnotationTooltip(event);
    QGraphicsView::mouseReleaseEvent(event);
}

void PlotView::updateAnnotationTooltip(QMouseEvent *event)
{
    // If there are any mouse buttons pressed, we assume
    // that the plot is being dragged and hide the tooltip.
    bool isDrag = event->buttons() != Qt::NoButton;
    if (!annotationCommentsEnabled
        || !spectrogramPlot->isAnnotationsEnabled()
        || isDrag)  {
        QToolTip::hideText();
    } else {
        QString* comment = spectrogramPlot->mouseAnnotationComment(event);
        if (comment != nullptr) {
            QToolTip::showText(event->globalPos(), *comment);
        } else {
            QToolTip::hideText();
        }
    }
}

void PlotView::contextMenuEvent(QContextMenuEvent * event)
{
    QMenu menu;

    // Determine which plot was clicked: spectrogram (index 0) or a derived plot (index >=1)
    int clickX = event->pos().x();
    int clickY = event->pos().y();
    int hscroll = horizontalScrollBar()->value();
    size_t clickSample = columnToSample(clickX + hscroll);
    int viewportH = viewport()->height();

    Plot *selectedPlot = nullptr;
    size_t plotIndex = 0;
    // Tuner Y for any new derived plot: -1 means "leave tuner alone" (e.g.
    // the click was in an existing derived plot, not the spectrogram).
    int tunerCentreY = -1;
    // Check if click is in derived plot area (fixed at bottom)
    if (plots.size() > 1 && clickY >= viewportH - derivedPlotHeight) {
        int posInDerived = clickY - (viewportH - derivedPlotHeight);
        plotIndex = 1 + posInDerived / derivedPlotHeight;
        if (plotIndex >= plots.size())
            return;
        selectedPlot = plots[plotIndex].get();
    } else {
        // Spectrogram area (scrollable)
        int contentY = clickY + verticalScrollBar()->value();
        if (contentY < 0 || contentY >= spectrogramPlot->height())
            return;
        plotIndex = 0;
        selectedPlot = plots[0].get();
        tunerCentreY = contentY;
    }

    // Compute center position for recentering
    int centerX = viewport()->width() / 2;
    // Add actions to add derived plots
    // that are compatible with selectedPlot's output
    QMenu *plotsMenu = menu.addMenu("Add derived plot");
    auto src = selectedPlot->output();
    auto compatiblePlots = as_range(Plots::plots.equal_range(src->sampleType()));
    // Shortcut: add sample/amplitude/frequency as a single stacked set.
    // Only offered when the source is complex<float> (the combo only makes
    // sense for that sample type — the three creators all assume it).
    if (src->sampleType() == typeid(std::complex<float>)) {
        auto trioAction = new QAction(QStringLiteral("Add IQ + AM + FM"), plotsMenu);
        connect(
            trioAction, &QAction::triggered,
            this, [=]() {
                // Tune the spectrogram tuner to the click position so the
                // new derived plots see the signal under the cursor.
                if (tunerCentreY >= 0)
                    spectrogramPlot->setTunerCentreY(tunerCentreY);
                // Order: sample (IQ) first, amplitude (AM) second, frequency (FM) last
                addPlot(Plots::samplePlot(src));
                addPlot(Plots::amplitudePlot(src));
                addPlot(Plots::frequencyPlot(src));
                this->zoomSample = clickSample;
                this->zoomPos = centerX;
                this->updateView(true);
            }
        );
        plotsMenu->addAction(trioAction);
        plotsMenu->addSeparator();
    }
    for (auto p : compatiblePlots) {
        auto plotInfo = p.second;
        auto action = new QAction(QString("Add %1").arg(plotInfo.name), plotsMenu);
        auto plotCreator = plotInfo.creator;
        connect(
            action, &QAction::triggered,
            this, [=]() {
                // Tune the spectrogram tuner to the click Y first; the new
                // plot subscribes to tunerTransform so it picks up the new
                // centre frequency on its first paint.
                if (tunerCentreY >= 0)
                    spectrogramPlot->setTunerCentreY(tunerCentreY);
                // Add the new derived plot
                addPlot(plotCreator(src));
                // Re-center view so clickSample is at center
                this->zoomSample = clickSample;
                this->zoomPos = centerX;
                this->updateView(true);
            }
        );
        plotsMenu->addAction(action);
    }

    // Add submenu for extracting symbols
    QMenu *extractMenu = menu.addMenu("Extract symbols");
    // Add action to extract symbols from selected plot to stdout
    auto extract = new QAction("To stdout", extractMenu);
    connect(
        extract, &QAction::triggered,
        this, [=]() {
            extractSymbols(src, false);
        }
    );
    extract->setEnabled(cursorsEnabled && (src->sampleType() == typeid(float)));
    extractMenu->addAction(extract);

    // Add action to extract symbols from selected plot to clipboard
    auto extractClipboard = new QAction("Copy to clipboard", extractMenu);
    connect(
        extractClipboard, &QAction::triggered,
        this, [=]() {
            extractSymbols(src, true);
        }
    );
    extractClipboard->setEnabled(cursorsEnabled && (src->sampleType() == typeid(float)));
    extractMenu->addAction(extractClipboard);

    // Add action to export the selected samples into a file
    auto save = new QAction("Export samples to file...", &menu);
    connect(
        save, &QAction::triggered,
        this, [=]() {
            if (selectedPlot == spectrogramPlot) {
                exportSamples(spectrogramPlot->tunerEnabled() ? spectrogramPlot->output() : spectrogramPlot->input());
            } else {
                exportSamples(src);
            }
        }
    );
    menu.addAction(save);

    // Add submenu to run external analysis plugins over a region of the tuned
    // (filtered) signal. Plugins detect bursts / calls / sync etc. and return
    // annotations. Discovered fresh each right-click so newly-installed manifests
    // appear without a restart.
    {
        QMenu *pluginMenu = menu.addMenu("Run plugin");
        buildPluginMenu(pluginMenu, [this](const PluginManifest &mf) { runPlugin(mf); });
    }

    // Annotation actions: edit/delete if the click landed on one, or "Add
    // annotation here" otherwise (only on the spectrogram). The list is
    // accessed via the InputSource so saves and dirty-tracking flow through.
    if (selectedPlot == spectrogramPlot) {
        int hitIdx = spectrogramPlot->annotationIndexAt(clickX, clickY);
        if (hitIdx >= 0) {
            auto editAct = new QAction("Edit annotation...", &menu);
            connect(editAct, &QAction::triggered, this, [=]() {
                promptEditAnnotation(hitIdx);
            });
            menu.addAction(editAct);

            auto delAct = new QAction("Delete annotation", &menu);
            connect(delAct, &QAction::triggered, this, [=]() {
                deleteAnnotation(hitIdx);
            });
            menu.addAction(delAct);
        } else {
            auto addAct = new QAction("Add annotation here", &menu);
            connect(addAct, &QAction::triggered, this, [=]() {
                // Pick bounds from the surrounding state so the user gets a
                // sensible starting region without dragging:
                //   time:  cursor selection if any, else current view
                //   freq:  tuner pass-band if enabled, else ±5% of Fs around
                //          the click y
                size_t s0, s1;
                if (cursorsEnabled) {
                    s0 = selectedSamples.minimum;
                    s1 = selectedSamples.minimum + selectedSamples.length();
                } else {
                    s0 = viewRange.minimum;
                    s1 = s0 + viewRange.length();
                }
                double fLo, fHi;
                if (spectrogramPlot->tunerEnabled()) {
                    auto *inputSrc = static_cast<InputSource*>(mainSampleSource);
                    double centre = inputSrc->getFrequency()
                                  + spectrogramPlot->tunerOffsetHz();
                    double half   = 0.5 * spectrogramPlot->tunerBandwidthHz();
                    fLo = centre - half;
                    fHi = centre + half;
                } else {
                    int yLocal = clickY + verticalScrollBar()->value();
                    double centre = spectrogramPlot->freqAtPlotY(yLocal);
                    double half   = 0.05 * sampleRate;
                    fLo = centre - half;
                    fHi = centre + half;
                }
                Annotation a;
                a.sampleRange = {s0, s1 > s0 ? s1 - 1 : s0};
                a.frequencyRange = {fLo, fHi};
                a.boxColor = QColor(255, 200, 0, 180);
                AnnotationDialog dlg(a, sampleRate, this);
                if (dlg.exec() == QDialog::Accepted) {
                    auto *inputSrc = static_cast<InputSource*>(mainSampleSource);
                    inputSrc->addAnnotation(dlg.result());
                }
            });
            menu.addAction(addAct);
        }
        menu.addSeparator();
    }

    // Add action to remove the selected plot (only for derived plots)
    auto rem = new QAction("Remove plot", &menu);
    connect(
        rem, &QAction::triggered,
        this, [=]() {
            if (plotIndex > 0 && plotIndex < plots.size())
                plots.erase(plots.begin() + plotIndex);
        }
    );
    rem->setEnabled(plotIndex > 0);
    menu.addAction(rem);

    updateViewRange(false);
    if(menu.exec(event->globalPos()))
        updateView(false);
}


bool PlotView::choosePluginScope(size_t &start, size_t &count)
{
    QDialog dlg(this);
    dlg.setWindowTitle("Run plugin - region");
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel("Run the plugin over:", &dlg));

    auto *selRadio = new QRadioButton("Cursor selection", &dlg);
    auto *viewRadio = new QRadioButton("Current view", &dlg);
    auto *wholeRadio = new QRadioButton("Whole file", &dlg);
    selRadio->setEnabled(cursorsEnabled);
    if (cursorsEnabled)
        selRadio->setChecked(true);
    else
        viewRadio->setChecked(true);
    layout->addWidget(selRadio);
    layout->addWidget(viewRadio);
    layout->addWidget(wholeRadio);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    if (selRadio->isChecked()) {
        start = selectedSamples.minimum;
        count = selectedSamples.length();
    } else if (viewRadio->isChecked()) {
        start = viewRange.minimum;
        count = viewRange.length();
    } else {
        start = 0;
        count = mainSampleSource->count();
    }
    return true;
}

bool PlotView::collectPluginParams(const PluginManifest &manifest, QJsonObject &out)
{
    out = QJsonObject();
    if (manifest.params.isEmpty())
        return true;

    QDialog dlg(this);
    dlg.setWindowTitle(QString("%1 - parameters").arg(manifest.name));
    auto *form = new QFormLayout(&dlg);

    struct Field { const PluginParam *p; QWidget *w; };
    std::vector<Field> fields;
    for (const auto &p : manifest.params) {
        QWidget *w = nullptr;
        if (p.type == "int") {
            auto *sb = new QSpinBox(&dlg);
            // Saturate the (double) manifest bounds into int range before casting;
            // an out-of-int32 bound would otherwise be an out-of-range double->int
            // conversion (UB). INT_MIN/INT_MAX are exactly representable as double.
            auto toIntBound = [](double v) {
                v = std::max((double)INT_MIN, std::min((double)INT_MAX, v));
                return (int)v;
            };
            sb->setRange(p.hasMin ? toIntBound(p.minValue) : -1000000000,
                         p.hasMax ? toIntBound(p.maxValue) : 1000000000);
            sb->setValue(p.defaultValue.toInt());
            w = sb;
        } else if (p.type == "float") {
            auto *sb = new QDoubleSpinBox(&dlg);
            sb->setDecimals(p.decimals > 0 ? p.decimals : 6);
            sb->setRange(p.hasMin ? p.minValue : -1e12,
                         p.hasMax ? p.maxValue : 1e12);
            sb->setValue(p.defaultValue.toDouble());
            w = sb;
        } else if (p.type == "bool") {
            auto *cb = new QCheckBox(&dlg);
            cb->setChecked(p.defaultValue.toBool());
            w = cb;
        } else if (p.type == "enum") {
            auto *combo = new QComboBox(&dlg);
            combo->addItems(p.choices);
            int idx = p.choices.indexOf(p.defaultValue.toString());
            if (idx >= 0)
                combo->setCurrentIndex(idx);
            w = combo;
        } else { // string (default)
            auto *le = new QLineEdit(&dlg);
            le->setText(p.defaultValue.toString());
            w = le;
        }
        form->addRow(p.label, w);
        fields.push_back({ &p, w });
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    for (const auto &f : fields) {
        const PluginParam &p = *f.p;
        if (p.type == "int")
            out.insert(p.key, static_cast<QSpinBox*>(f.w)->value());
        else if (p.type == "float")
            out.insert(p.key, static_cast<QDoubleSpinBox*>(f.w)->value());
        else if (p.type == "bool")
            out.insert(p.key, static_cast<QCheckBox*>(f.w)->isChecked());
        else if (p.type == "enum")
            out.insert(p.key, static_cast<QComboBox*>(f.w)->currentText());
        else
            out.insert(p.key, static_cast<QLineEdit*>(f.w)->text());
    }
    return true;
}

void PlotView::buildPluginMenu(QMenu *menu,
                               const std::function<void(const PluginManifest &)> &onTrigger)
{
    menu->setToolTipsVisible(true);
    int added = 0;
    for (const auto &m : discoverPlugins()) {
        if (!m.valid)
            continue;
        PluginManifest mf = m;
        QAction *act = menu->addAction(mf.name);
        added++;
        // The extracted segment is always complex cf32; gate other declared types so
        // a plugin can't be handed a format it didn't ask for.
        if (mf.sampleType.compare("cf32", Qt::CaseInsensitive) != 0) {
            act->setEnabled(false);
            act->setToolTip(QObject::tr("unsupported sample_type \"%1\" (only cf32)")
                                .arg(mf.sampleType));
            continue;
        }
        QObject::connect(act, &QAction::triggered, menu, [onTrigger, mf]() { onTrigger(mf); });
    }
    if (added == 0) {
        QAction *none = menu->addAction(QObject::tr("No plugins in %1").arg(pluginDirectory()));
        none->setEnabled(false);
    }
}

void PlotView::runPlugin(const PluginManifest &manifest)
{
    if (mainSampleSource == nullptr || mainSampleSource->count() == 0) {
        QMessageBox::information(this, "Run plugin", "No file is open.");
        return;
    }
    if (pluginRunner && pluginRunner->busy()) {
        QMessageBox::information(this, "Run plugin",
            "A plugin is already running. Wait for it to finish or cancel it.");
        return;
    }

    // Source = the tuned/filtered IQ when the tuner is on, else the raw input
    // (mirrors the "Export samples" behaviour). The pass-band gives default
    // frequency bounds for annotations that don't specify their own.
    auto *inputSrc = static_cast<InputSource*>(mainSampleSource);
    std::shared_ptr<SampleSource<std::complex<float>>> src;
    double centerFreq, passLo, passHi;
    if (spectrogramPlot && spectrogramPlot->tunerEnabled()) {
        src = std::dynamic_pointer_cast<SampleSource<std::complex<float>>>(spectrogramPlot->output());
        const double centre = inputSrc->getFrequency() + spectrogramPlot->tunerOffsetHz();
        const double half = 0.5 * spectrogramPlot->tunerBandwidthHz();
        centerFreq = centre;
        passLo = centre - half;
        passHi = centre + half;
    } else {
        src = spectrogramPlot ? spectrogramPlot->input() : nullptr;
        centerFreq = inputSrc->getFrequency();
        passLo = centerFreq - 0.5 * sampleRate;
        passHi = centerFreq + 0.5 * sampleRate;
    }
    if (!src) {
        QMessageBox::warning(this, "Run plugin", "Could not access the sample source.");
        return;
    }

    size_t start = 0, count = 0;
    if (!choosePluginScope(start, count))
        return;
    if (count == 0) {
        QMessageBox::information(this, "Run plugin", "The chosen region is empty.");
        return;
    }

    // Warn before extracting a very large segment (written to a temp file on a
    // worker thread before the plugin runs; cancellable from the busy dialog).
    const double bytes = (double)count * sizeof(std::complex<float>);
    if (bytes > 512.0 * 1024 * 1024) {
        auto r = QMessageBox::question(this, "Run plugin",
            QString("This extracts %1 MiB of IQ to a temporary file. Continue?")
                .arg(bytes / (1024.0 * 1024.0), 0, 'f', 0));
        if (r != QMessageBox::Yes)
            return;
    }

    QJsonObject params;
    if (!collectPluginParams(manifest, params))
        return;

    if (!pluginRunner) {
        pluginRunner = new PluginRunner(this);
        connect(pluginRunner, &PluginRunner::finished, this,
            [this](std::vector<Annotation> annos) {
                if (pluginProgress)
                    pluginProgress->reset();
                auto *in = static_cast<InputSource*>(mainSampleSource);
                for (const auto &a : annos)
                    in->addAnnotation(a);
                const QString msg = annos.empty()
                    ? QStringLiteral("Plugin finished: no annotations returned.")
                    : QString("Plugin added %1 annotation%2.")
                          .arg(annos.size()).arg(annos.size() == 1 ? "" : "s");
                QMessageBox::information(this, "Run plugin", msg);
            });
        connect(pluginRunner, &PluginRunner::failed, this,
            [this](QString err) {
                if (pluginProgress)
                    pluginProgress->reset();
                QMessageBox::warning(this, "Run plugin", "Plugin failed:\n" + err);
            });
    }

    if (!pluginProgress) {
        pluginProgress = new QProgressDialog(this);
        pluginProgress->setWindowTitle("Run plugin");
        pluginProgress->setRange(0, 0); // indeterminate / busy
        pluginProgress->setMinimumDuration(0);
        pluginProgress->setWindowModality(Qt::WindowModal);
        connect(pluginProgress, &QProgressDialog::canceled, this, [this]() {
            if (pluginRunner)
                pluginRunner->cancel();
        });
    }
    pluginProgress->setLabelText(QString("Running %1...").arg(manifest.name));
    pluginProgress->show();

    pluginRunner->run(manifest, src, start, count, sampleRate, centerFreq,
                      passLo, passHi, params);
}

void PlotView::updateSelectionPlots()
{
    // Plots that summarise the cursor selection (or the visible range when
    // cursors are off) rather than tracking time: keep their range in sync.
    for (auto &plt : plots) {
        if (auto fskp = dynamic_cast<FskPolarPlot*>(plt.get())) {
            fskp->setSelection(cursorsEnabled, selectedSamples);
        }
        if (auto hist = dynamic_cast<HistogramPlot*>(plt.get())) {
            hist->setSelection(cursorsEnabled, selectedSamples);
        }
    }
}

void PlotView::cursorsMoved()
{
    selectedSamples = {
        columnToSample(horizontalScrollBar()->value() + cursors.selection().minimum),
        columnToSample(horizontalScrollBar()->value() + cursors.selection().maximum)
    };

    updateSelectionPlots();
    emitTimeSelection();
    viewport()->update();
}

void PlotView::emitTimeSelection()
{
    size_t sampleCount = selectedSamples.length();
    float selectionTime = sampleCount / (float)mainSampleSource->rate();
    emit timeSelectionChanged(selectionTime);
}

void PlotView::enableCursors(bool enabled)
{
    cursorsEnabled = enabled;
    if (enabled) {
        int margin = viewport()->rect().width() / 3;
        cursors.setSelection({viewport()->rect().left() + margin, viewport()->rect().right() - margin});
        cursorsMoved();
    } else {
        updateSelectionPlots();
    }
    viewport()->update();
}

bool PlotView::isOverSpectrogram(int viewportY) const
{
    if (!spectrogramPlot) return false;
    int top = -verticalScrollBar()->value();
    int bottom = top + spectrogramPlot->height();
    return viewportY >= top && viewportY < bottom;
}

bool PlotView::startAnnotationDrag(QMouseEvent *event)
{
    if (!isOverSpectrogram(event->pos().y()))
        return false;
    annotDragging = true;
    annotDragOrigin = event->pos();
    if (!annotRubber)
        annotRubber = new QRubberBand(QRubberBand::Rectangle, viewport());
    annotRubber->setGeometry(QRect(annotDragOrigin, QSize()));
    annotRubber->show();
    return true;
}

void PlotView::updateAnnotationDrag(QMouseEvent *event)
{
    if (!annotDragging || !annotRubber) return;
    annotRubber->setGeometry(QRect(annotDragOrigin, event->pos()).normalized());
}

void PlotView::finishAnnotationDrag(QMouseEvent *event)
{
    if (!annotDragging) return;
    annotDragging = false;
    QRect rect = QRect(annotDragOrigin, event->pos()).normalized();
    if (annotRubber)
        annotRubber->hide();
    // Demand a minimum extent so an accidental click doesn't pop the dialog.
    if (rect.width() < 4 || rect.height() < 4)
        return;
    promptNewAnnotation(rect);
}

void PlotView::promptNewAnnotation(QRect viewportRect)
{
    if (!spectrogramPlot) return;
    int hscroll = horizontalScrollBar()->value();
    size_t s0 = columnToSample(viewportRect.left()  + hscroll);
    size_t s1 = columnToSample(viewportRect.right() + hscroll);
    if (s1 < s0) std::swap(s0, s1);
    int specTop = -verticalScrollBar()->value();
    int yTopLocal    = viewportRect.top()    - specTop;
    int yBottomLocal = viewportRect.bottom() - specTop;
    double fHi = spectrogramPlot->freqAtPlotY(yTopLocal);
    double fLo = spectrogramPlot->freqAtPlotY(yBottomLocal);
    if (fHi < fLo) std::swap(fHi, fLo);

    Annotation a;
    a.sampleRange = {s0, s1};
    a.frequencyRange = {fLo, fHi};
    a.boxColor = QColor(255, 200, 0, 180);
    AnnotationDialog dlg(a, sampleRate, this);
    if (dlg.exec() == QDialog::Accepted) {
        auto inputSrc = static_cast<InputSource*>(mainSampleSource);
        inputSrc->addAnnotation(dlg.result());
    }
}

void PlotView::promptEditAnnotation(int index)
{
    auto inputSrc = static_cast<InputSource*>(mainSampleSource);
    if (index < 0 || index >= (int)inputSrc->annotationList.size())
        return;
    AnnotationDialog dlg(inputSrc->annotationList[index], sampleRate, this);
    if (dlg.exec() == QDialog::Accepted) {
        inputSrc->updateAnnotation(index, dlg.result());
    }
}

void PlotView::deleteAnnotation(int index)
{
    auto inputSrc = static_cast<InputSource*>(mainSampleSource);
    inputSrc->removeAnnotation(index);
}

void PlotView::onAnnotationsChanged()
{
    viewport()->update();
}

QRect PlotView::annotationViewportRect(const Annotation &a)
{
    if (!spectrogramPlot) return QRect();
    const int hscroll = horizontalScrollBar()->value();
    const int vscroll = verticalScrollBar()->value();
    const int x0 = sampleToColumn(a.sampleRange.minimum) - hscroll;
    const int x1 = sampleToColumn(a.sampleRange.maximum) - hscroll;
    // Higher frequency = smaller y, so freqMax is the top edge.
    const int yTop = spectrogramPlot->plotYAtFreq(a.frequencyRange.maximum) - vscroll;
    const int yBot = spectrogramPlot->plotYAtFreq(a.frequencyRange.minimum) - vscroll;
    return QRect(QPoint(x0, yTop), QPoint(x1, yBot)).normalized();
}

PlotView::AnnoGrab PlotView::grabForRect(const QRect &r, QPoint p) const
{
    const int M = 6; // edge/handle grab margin in px
    const bool inX = p.x() >= r.left() - M && p.x() <= r.right() + M;
    const bool inY = p.y() >= r.top() - M && p.y() <= r.bottom() + M;
    if (!inX || !inY) return AnnoGrab::None;
    const bool nL = std::abs(p.x() - r.left())   <= M;
    const bool nR = std::abs(p.x() - r.right())  <= M;
    const bool nT = std::abs(p.y() - r.top())    <= M;
    const bool nB = std::abs(p.y() - r.bottom()) <= M;
    if (nT && nL) return AnnoGrab::TopLeft;
    if (nT && nR) return AnnoGrab::TopRight;
    if (nB && nL) return AnnoGrab::BottomLeft;
    if (nB && nR) return AnnoGrab::BottomRight;
    if (nL) return AnnoGrab::Left;
    if (nR) return AnnoGrab::Right;
    if (nT) return AnnoGrab::Top;
    if (nB) return AnnoGrab::Bottom;
    return AnnoGrab::Move; // inside the body
}

Qt::CursorShape PlotView::cursorForGrab(AnnoGrab g) const
{
    switch (g) {
        case AnnoGrab::Left:
        case AnnoGrab::Right:        return Qt::SizeHorCursor;
        case AnnoGrab::Top:
        case AnnoGrab::Bottom:       return Qt::SizeVerCursor;
        case AnnoGrab::TopLeft:
        case AnnoGrab::BottomRight:  return Qt::SizeFDiagCursor;
        case AnnoGrab::TopRight:
        case AnnoGrab::BottomLeft:   return Qt::SizeBDiagCursor;
        case AnnoGrab::Move:         return Qt::SizeAllCursor;
        default:                     return Qt::ArrowCursor;
    }
}

int PlotView::annotationGrabAt(QPoint pos, AnnoGrab *grabOut)
{
    if (!spectrogramPlot || !isOverSpectrogram(pos.y())) {
        if (grabOut) *grabOut = AnnoGrab::None;
        return -1;
    }
    auto inputSrc = static_cast<InputSource*>(mainSampleSource);
    // Topmost (last drawn) wins, so iterate in reverse.
    for (int i = (int)inputSrc->annotationList.size() - 1; i >= 0; --i) {
        QRect r = annotationViewportRect(inputSrc->annotationList[i]);
        AnnoGrab g = grabForRect(r, pos);
        if (g != AnnoGrab::None) {
            if (grabOut) *grabOut = g;
            return i;
        }
    }
    if (grabOut) *grabOut = AnnoGrab::None;
    return -1;
}

long long PlotView::sampleAtViewportX(int vx)
{
    long long col = (long long)vx + horizontalScrollBar()->value();
    if (col < 0) col = 0;
    return col * (long long)samplesPerColumn();
}

double PlotView::freqAtViewportY(int vy) const
{
    if (!spectrogramPlot) return 0.0;
    return spectrogramPlot->freqAtPlotY(vy + verticalScrollBar()->value());
}

void PlotView::updateAnnotationHover(QMouseEvent *event)
{
    AnnoGrab g = AnnoGrab::None;
    int idx = annotationGrabAt(event->pos(), &g);
    // Only claim the cursor while over an annotation; otherwise release it so
    // the tuner / default cursor handling still works.
    if (g != AnnoGrab::None)
        viewport()->setCursor(cursorForGrab(g));
    else
        viewport()->unsetCursor();
    if (idx != hoveredAnnotation) {
        hoveredAnnotation = idx;
        spectrogramPlot->setActiveAnnotation(idx);
        viewport()->update();
    }
}

bool PlotView::beginAnnotationEdit(QMouseEvent *event)
{
    AnnoGrab g = AnnoGrab::None;
    int idx = annotationGrabAt(event->pos(), &g);
    if (idx < 0 || g == AnnoGrab::None)
        return false;
    auto inputSrc = static_cast<InputSource*>(mainSampleSource);
    editingAnnotation = idx;
    editGrab = g;
    editPressPos = event->pos();
    editOrig = inputSrc->annotationList[idx];
    spectrogramPlot->setActiveAnnotation(idx);
    viewport()->setCursor(cursorForGrab(g));
    return true;
}

void PlotView::updateAnnotationEdit(QMouseEvent *event)
{
    if (editingAnnotation < 0) return;
    auto inputSrc = static_cast<InputSource*>(mainSampleSource);
    if (editingAnnotation >= (int)inputSrc->annotationList.size()) {
        editingAnnotation = -1;
        return;
    }

    const long long count = (long long)inputSrc->count();
    long long dS = sampleAtViewportX(event->pos().x()) - sampleAtViewportX(editPressPos.x());
    double    dF = freqAtViewportY(event->pos().y()) - freqAtViewportY(editPressPos.y());

    long long oMin = (long long)editOrig.sampleRange.minimum;
    long long oMax = (long long)editOrig.sampleRange.maximum;
    double    oLo  = editOrig.frequencyRange.minimum;
    double    oHi  = editOrig.frequencyRange.maximum;
    long long nMin = oMin, nMax = oMax;
    double    nLo  = oLo,  nHi  = oHi;

    switch (editGrab) {
        case AnnoGrab::Move:
            // Clamp the shift so the whole box stays within the file.
            if (oMin + dS < 0) dS = -oMin;
            if (count > 0 && oMax + dS > count - 1) dS = (count - 1) - oMax;
            nMin = oMin + dS; nMax = oMax + dS;
            nLo = oLo + dF;   nHi = oHi + dF;
            break;
        case AnnoGrab::Left:        nMin = oMin + dS; break;
        case AnnoGrab::Right:       nMax = oMax + dS; break;
        case AnnoGrab::Top:         nHi  = oHi + dF;  break; // top edge = higher freq
        case AnnoGrab::Bottom:      nLo  = oLo + dF;  break;
        case AnnoGrab::TopLeft:     nMin = oMin + dS; nHi = oHi + dF; break;
        case AnnoGrab::TopRight:    nMax = oMax + dS; nHi = oHi + dF; break;
        case AnnoGrab::BottomLeft:  nMin = oMin + dS; nLo = oLo + dF; break;
        case AnnoGrab::BottomRight: nMax = oMax + dS; nLo = oLo + dF; break;
        default: break;
    }

    if (nMin < 0) nMin = 0;
    if (nMax < 0) nMax = 0;
    if (count > 0) {
        if (nMin > count - 1) nMin = count - 1;
        if (nMax > count - 1) nMax = count - 1;
    }
    if (nMin > nMax) std::swap(nMin, nMax);
    if (nLo > nHi)   std::swap(nLo, nHi);

    Annotation &a = inputSrc->annotationList[editingAnnotation];
    a.sampleRange = { (size_t)nMin, (size_t)nMax };
    a.frequencyRange = { nLo, nHi };
    viewport()->update();
}

void PlotView::finishAnnotationEdit(QMouseEvent *event)
{
    if (editingAnnotation < 0) return;
    updateAnnotationEdit(event);
    auto inputSrc = static_cast<InputSource*>(mainSampleSource);
    const int idx = editingAnnotation;
    editingAnnotation = -1;
    editGrab = AnnoGrab::None;
    if (idx >= 0 && idx < (int)inputSrc->annotationList.size()) {
        // Commit through updateAnnotation so the dirty flag + change callbacks
        // fire (enables Save, marks the title bar). The value is the one we
        // edited live in place.
        inputSrc->updateAnnotation(idx, inputSrc->annotationList[idx]);
    }
}

bool PlotView::viewportEvent(QEvent *event) {
    if (event->type() == QEvent::MouseMove) {
        LatencyLog::mark("MouseMove ----------");
    } else if (event->type() == QEvent::Wheel) {
        LatencyLog::mark("Wheel ----------");
    }
    // An in-progress annotation move/resize drag owns the mouse until release.
    if (editingAnnotation >= 0) {
        if (event->type() == QEvent::MouseMove) {
            updateAnnotationEdit(static_cast<QMouseEvent*>(event));
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            finishAnnotationEdit(static_cast<QMouseEvent*>(event));
            return true;
        }
    }

    // Shift+left-drag on the spectrogram → new annotation; plain left-press on
    // an existing annotation's box/handles → move/resize it. Both run before the
    // per-plot dispatch so they don't drag the tuner. Hover (no button) shows
    // the resize handles + cursor.
    if (event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton && spectrogramPlot) {
            if (me->modifiers() & Qt::ShiftModifier) {
                if (startAnnotationDrag(me))
                    return true;
            } else if (beginAnnotationEdit(me)) {
                return true;
            }
        }
    } else if (event->type() == QEvent::MouseMove && annotDragging) {
        updateAnnotationDrag(static_cast<QMouseEvent*>(event));
        return true;
    } else if (event->type() == QEvent::MouseButtonRelease && annotDragging) {
        finishAnnotationDrag(static_cast<QMouseEvent*>(event));
        return true;
    } else if (event->type() == QEvent::MouseMove && spectrogramPlot) {
        auto *me = static_cast<QMouseEvent*>(event);
        if (me->buttons() == Qt::NoButton)
            updateAnnotationHover(me);
    }
    // Handle wheel events for zooming (before the parent's handler to stop normal scrolling)
    if (event->type() == QEvent::Wheel) {
        QWheelEvent *wheelEvent = static_cast<QWheelEvent*>(event);
        // Ctrl+wheel: horizontal zoom
        if (QApplication::keyboardModifiers() & Qt::ControlModifier) {
            bool canZoomIn = zoomLevel < fftSize;
            bool canZoomOut = zoomLevel > -64;
            int delta = wheelEvent->angleDelta().y();
            if ((delta > 0 && canZoomIn) || (delta < 0 && canZoomOut)) {
                scrollZoomStepsAccumulated += delta;
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
                zoomPos = wheelEvent->position().x();
#else
                zoomPos = wheelEvent->pos().x();
#endif
                zoomSample = columnToSample(horizontalScrollBar()->value() + zoomPos);
                if (scrollZoomStepsAccumulated >= 120) {
                    scrollZoomStepsAccumulated -= 120;
                    emit zoomIn();
                } else if (scrollZoomStepsAccumulated <= -120) {
                    scrollZoomStepsAccumulated += 120;
                    emit zoomOut();
                }
            }
            return true;
        }
        // No modifier: forward to plots for vertical zoom
        {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
            int viewY = int(wheelEvent->position().y());
#else
            int viewY = wheelEvent->pos().y();
#endif
            int plotY = -verticalScrollBar()->value();
            for (auto&& plot : plots) {
                int ph = plot->height();
                if (viewY >= plotY && viewY < plotY + ph) {
                    if (plot->wheelEvent(wheelEvent))
                        return true;
                    break;
                }
                plotY += ph;
            }
        }
    }

    // Pass mouse events to individual plot objects
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonRelease ||
        event->type() == QEvent::Leave) {

        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);

        int plotY = -verticalScrollBar()->value();
        for (auto&& plot : plots) {
            bool result = plot->mouseEvent(
                event->type(),
                QMouseEvent(
                    event->type(),
                    QPoint(mouseEvent->pos().x(), mouseEvent->pos().y() - plotY),
                    mouseEvent->button(),
                    mouseEvent->buttons(),
                    QApplication::keyboardModifiers()
                )
            );
            if (result)
                return true;
            plotY += plot->height();
        }

        if (cursorsEnabled)
            if (cursors.mouseEvent(event->type(), *mouseEvent))
                return true;
    }

    // Handle parent eveents
    return QGraphicsView::viewportEvent(event);
}

void PlotView::extractSymbols(std::shared_ptr<AbstractSampleSource> src,
                              bool toClipboard)
{
    if (!cursorsEnabled)
        return;
    auto floatSrc = std::dynamic_pointer_cast<SampleSource<float>>(src);
    if (!floatSrc)
        return;
    auto samples = floatSrc->getSamples(selectedSamples.minimum, selectedSamples.length());
    auto step = (float)selectedSamples.length() / cursors.segments();
    auto symbols = std::vector<float>();
    for (auto i = step / 2; i < selectedSamples.length(); i += step)
    {
        symbols.push_back(samples[i]);
    }
    if (!toClipboard) {
        for (auto f : symbols)
            std::cout << f << ", ";
        std::cout << std::endl << std::flush;
    } else {
        QClipboard *clipboard = QGuiApplication::clipboard();
        QString symbolText;
        QTextStream symbolStream(&symbolText);
        for (auto f : symbols)
            symbolStream << f << ", ";
        clipboard->setText(symbolText);
    }
}

void PlotView::exportSamples(std::shared_ptr<AbstractSampleSource> src)
{
    if (src->sampleType() == typeid(std::complex<float>)) {
        exportSamples<std::complex<float>>(src);
    } else {
        exportSamples<float>(src);
    }
}

// Tightest integer power of two ≤ 1/relBw, clamped to ≥ 1. Decimation by this
// keeps the tuner pass-band inside the new Nyquist (the TunerTransform's FIR
// already constrains energy to ±relBw·Fs/2 around DC).
static int defaultPow2DecimFor(float relBw)
{
    if (!(relBw > 0.0f) || relBw >= 1.0f)
        return 1;
    int decim = 1;
    while ((decim * 2) <= (int)std::floor(1.0f / relBw))
        decim *= 2;
    return decim;
}

template<typename SOURCETYPE>
void PlotView::exportSamples(std::shared_ptr<AbstractSampleSource> src)
{
    auto sampleSrc = std::dynamic_pointer_cast<SampleSource<SOURCETYPE>>(src);
    if (!sampleSrc) {
        return;
    }

    const bool sigmfSupported = std::is_same<SOURCETYPE, std::complex<float>>::value;
    const QString rawFilter = QString::fromUtf8(getFileNameFilter<SOURCETYPE>());
    const QString sigmfFilter = QStringLiteral("SigMF tuned IQ (*.sigmf-meta)");

    QFileDialog dialog(this);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    QStringList filters;
    filters << rawFilter;
    if (sigmfSupported)
        filters << sigmfFilter;
    dialog.setNameFilters(filters);
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);

    QGroupBox groupBox("Selection To Export", &dialog);
    QVBoxLayout vbox(&groupBox);

    QRadioButton cursorSelection("Cursor Selection", &groupBox);
    QRadioButton currentView("Current View", &groupBox);
    QRadioButton completeFile("Complete File (Experimental)", &groupBox);

    if (cursorsEnabled) {
        cursorSelection.setChecked(true);
    } else {
        currentView.setChecked(true);
        cursorSelection.setEnabled(false);
    }

    vbox.addWidget(&cursorSelection);
    vbox.addWidget(&currentView);
    vbox.addWidget(&completeFile);
    vbox.addStretch(1);

    groupBox.setLayout(&vbox);

    QGridLayout *l = dialog.findChild<QGridLayout*>();
    l->addWidget(&groupBox, 4, 1);

    QGroupBox groupBox2("Decimation");
    QSpinBox decimation(&groupBox2);
    decimation.setMinimum(1);
    const int rawDefaultDecim = std::max(1, (int)(1.0f / sampleSrc->relativeBandwidth()));
    decimation.setValue(rawDefaultDecim);

    QVBoxLayout vbox2;
    vbox2.addWidget(&decimation);

    groupBox2.setLayout(&vbox2);
    l->addWidget(&groupBox2, 4, 2);

    // When the user switches to SigMF, default the decim to the tightest
    // power-of-two so the new sample rate hugs the tuner pass-band. We don't
    // *force* power-of-two — the user can override if they want to keep the
    // existing rate or pick a non-2 factor.
    if (sigmfSupported) {
        connect(&dialog, &QFileDialog::filterSelected, this, [&](const QString &f) {
            if (f == sigmfFilter)
                decimation.setValue(defaultPow2DecimFor(sampleSrc->relativeBandwidth()));
            else
                decimation.setValue(rawDefaultDecim);
        });
    }

    if (!dialog.exec())
        return;

    QStringList fileNames = dialog.selectedFiles();
    if (fileNames.isEmpty())
        return;

    size_t start, end;
    if (cursorSelection.isChecked()) {
        start = selectedSamples.minimum;
        end = start + selectedSamples.length();
    } else if (currentView.isChecked()) {
        start = viewRange.minimum;
        end = start + viewRange.length();
    } else {
        start = 0;
        end = sampleSrc->count();
    }

    // Trust either the filter dropdown or an explicit .sigmf-* extension in
    // the typed name — typing the extension is a discoverable shortcut and
    // would otherwise silently fall back to a raw-write of an oddly-named file.
    const bool typedSigmfName =
        fileNames[0].endsWith(".sigmf-meta", Qt::CaseInsensitive) ||
        fileNames[0].endsWith(".sigmf-data", Qt::CaseInsensitive);
    const bool wantSigmf = sigmfSupported &&
        (dialog.selectedNameFilter() == sigmfFilter || typedSigmfName);
    if (wantSigmf) {
        // Reachable only when SOURCETYPE == complex<float>; the cast below
        // succeeds because sigmfSupported was the gate for offering SigMF.
        auto cplxSrc = std::dynamic_pointer_cast<SampleSource<std::complex<float>>>(src);
        if (!cplxSrc)
            return;
        QString metaPath = fileNames[0];
        if (!metaPath.endsWith(".sigmf-meta", Qt::CaseInsensitive)) {
            // Strip a .sigmf-data sibling extension if present, then append.
            if (metaPath.endsWith(".sigmf-data", Qt::CaseInsensitive))
                metaPath.chop(QStringLiteral(".sigmf-data").size());
            metaPath += QStringLiteral(".sigmf-meta");
        }
        writeSigmf(cplxSrc, metaPath, start, end, std::max(1, decimation.value()));
        return;
    }

    std::ofstream os(fileNames[0].toStdString(), std::ios::binary);

    size_t index;
    // viewRange.length() is used as some less arbitrary step value
    size_t step = viewRange.length();

    QProgressDialog progress("Exporting samples...", "Cancel", start, end, this);
    progress.setWindowModality(Qt::WindowModal);
    for (index = start; index < end; index += step) {
        progress.setValue(index);
        if (progress.wasCanceled())
            break;

        size_t length = std::min(step, end - index);
        auto samples = sampleSrc->getSamples(index, length);
        if (samples != nullptr) {
            for (auto i = 0; i < length; i += decimation.value()) {
                os.write((const char*)&samples[i], sizeof(SOURCETYPE));
            }
        }
    }
}

// Encode a QColor as the SigMF presentation-color string "#RRGGBBAA". Inverse
// of the read path in InputSource which rotates Qt's "#AARRGGBB" form.
static QString sigmfColorString(const QColor &c)
{
    return QStringLiteral("#%1%2%3%4")
        .arg(c.red(),   2, 16, QChar('0'))
        .arg(c.green(), 2, 16, QChar('0'))
        .arg(c.blue(),  2, 16, QChar('0'))
        .arg(c.alpha(), 2, 16, QChar('0'))
        .toUpper();
}

bool PlotView::writeSigmf(std::shared_ptr<SampleSource<std::complex<float>>> src,
                          const QString &metaPath,
                          size_t start, size_t end, int decim)
{
    if (decim < 1) decim = 1;
    if (end <= start) {
        QMessageBox::warning(this, "SigMF export", "Empty selection — nothing to write.");
        return false;
    }

    QString dataPath = metaPath;
    dataPath.chop(QStringLiteral(".sigmf-meta").size());
    dataPath += QStringLiteral(".sigmf-data");

    std::ofstream os(dataPath.toStdString(), std::ios::binary);
    if (!os) {
        QMessageBox::warning(this, "SigMF export",
                             QStringLiteral("Could not open %1 for writing.").arg(dataPath));
        return false;
    }

    // Read in the same chunk size as the raw exporter so progress and memory
    // behaviour match. With decim>1 we still read a full chunk and write
    // every Nth sample — keeps the loop simple and lets the upstream FIR see
    // contiguous input.
    const size_t step = std::max<size_t>(viewRange.length(), 65536);
    QProgressDialog progress("Exporting SigMF samples...", "Cancel",
                             (int)std::min<size_t>(start, INT_MAX),
                             (int)std::min<size_t>(end,   INT_MAX), this);
    progress.setWindowModality(Qt::WindowModal);

    size_t writtenSamples = 0;
    for (size_t index = start; index < end; index += step) {
        if (index <= (size_t)INT_MAX)
            progress.setValue((int)index);
        if (progress.wasCanceled()) {
            os.close();
            QFile::remove(dataPath);
            return false;
        }
        size_t length = std::min(step, end - index);
        auto samples = src->getSamples(index, length);
        if (!samples) continue;
        // Pick samples whose absolute offset from `start` is a multiple of
        // decim — keeps the every-Nth pattern aligned across chunk boundaries
        // without explicit phase carry.
        const size_t relStart = index - start;
        const size_t chunkPhase = (decim - (relStart % decim)) % decim;
        for (size_t i = chunkPhase; i < length; i += decim) {
            os.write((const char*)&samples[i], sizeof(std::complex<float>));
            ++writtenSamples;
        }
    }
    os.close();

    // Provenance: pull the source filename and capture frequency from the
    // InputSource at the head of the chain. mainSampleSource is always an
    // InputSource (set in the constructor), so the cast is sound.
    auto inputSrc = static_cast<InputSource*>(mainSampleSource);
    const double oldRate = sampleRate;
    const double tunerOffset = spectrogramPlot ? spectrogramPlot->tunerOffsetHz() : 0.0;
    const double tunerBw    = spectrogramPlot ? spectrogramPlot->tunerBandwidthHz() : oldRate;
    const double oldCenter  = inputSrc ? inputSrc->getFrequency() : 0.0;
    const double newRate    = (decim > 0) ? oldRate / (double)decim : oldRate;
    const double newCenter  = oldCenter + tunerOffset;

    const QString nowIso = QDateTime::currentDateTimeUtc()
                               .toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzzZ"));
    const QString sourceFile = inputSrc ? inputSrc->filePath() : QString();

    QJsonObject global;
    global.insert("core:datatype", QStringLiteral("cf32_le"));
    global.insert("core:sample_rate", newRate);
    global.insert("core:version", QStringLiteral("1.0.0"));
    global.insert("core:datetime", nowIso);
    global.insert("core:description",
                  QStringLiteral("Tuned IQ export from %1")
                      .arg(QFileInfo(sourceFile).fileName()));
    global.insert("inspectrum:source_file", sourceFile);
    global.insert("inspectrum:source_sample_rate", oldRate);
    global.insert("inspectrum:source_center_frequency", oldCenter);
    global.insert("inspectrum:tuner_offset_hz", tunerOffset);
    global.insert("inspectrum:tuner_bandwidth_hz", tunerBw);
    global.insert("inspectrum:export_start_sample", (qint64)start);
    global.insert("inspectrum:export_end_sample", (qint64)end);
    global.insert("inspectrum:decimation", decim);

    QJsonObject capture;
    capture.insert("core:sample_start", 0);
    capture.insert("core:frequency", newCenter);
    capture.insert("core:datetime", nowIso);
    QJsonArray captures;
    captures.append(capture);

    // Annotation translation: keep entries that overlap both the export time
    // window AND the new pass-band. Frequencies are absolute Hz in SigMF, so
    // they don't need rewriting; sample indices do (relative to the new
    // start, scaled by decim).
    const double passLo = newCenter - newRate * 0.5;
    const double passHi = newCenter + newRate * 0.5;
    QJsonArray annotations;
    if (inputSrc) {
        for (const Annotation &a : inputSrc->annotationList) {
            if (a.sampleRange.maximum < start || a.sampleRange.minimum >= end)
                continue;
            if (a.frequencyRange.maximum < passLo || a.frequencyRange.minimum > passHi)
                continue;

            const size_t clipStart = std::max<size_t>(a.sampleRange.minimum, start);
            const size_t clipEnd   = std::min<size_t>(a.sampleRange.maximum, end - 1);
            // ceil for start / floor for end so the new range strictly covers
            // every original sample of the annotation that survived the clip.
            const size_t newStart = (clipStart - start + (size_t)decim - 1) / (size_t)decim;
            const size_t newEnd   = (clipEnd   - start) / (size_t)decim;
            const size_t newCount = (newEnd >= newStart) ? (newEnd - newStart + 1) : 1;

            QJsonObject ann;
            ann.insert("core:sample_start", (qint64)newStart);
            ann.insert("core:sample_count", (qint64)newCount);
            ann.insert("core:freq_lower_edge", a.frequencyRange.minimum);
            ann.insert("core:freq_upper_edge", a.frequencyRange.maximum);
            if (!a.label.isEmpty())
                ann.insert("core:label", a.label);
            if (!a.description.isEmpty())
                ann.insert("core:description", a.description);
            if (!a.comment.isEmpty())
                ann.insert("core:comment", a.comment);
            if (a.boxColor.isValid() && a.boxColor != QColor("white"))
                ann.insert("presentation:color", sigmfColorString(a.boxColor));
            annotations.append(ann);
        }
    }

    QJsonObject root;
    root.insert("global", global);
    root.insert("captures", captures);
    root.insert("annotations", annotations);

    QFile metaFile(metaPath);
    if (!metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this, "SigMF export",
                             QStringLiteral("Wrote %1 but could not open %2 for writing.")
                                 .arg(dataPath).arg(metaPath));
        return false;
    }
    metaFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    metaFile.close();

    qDebug() << "SigMF export:" << writtenSamples << "samples to" << dataPath
             << "metadata to" << metaPath
             << "newRate=" << newRate << "newCenter=" << newCenter
             << "decim=" << decim << "annotations=" << annotations.size();
    return true;
}

void PlotView::invalidateEvent()
{
    horizontalScrollBar()->setMinimum(0);
    horizontalScrollBar()->setMaximum(sampleToColumn(mainSampleSource->count()));
}

void PlotView::repaint()
{
    viewport()->update();
}

void PlotView::setCursorSegments(int segments)
{
    // Calculate number of samples per segment
    float sampPerSeg = (float)selectedSamples.length() / cursors.segments();

    // Alter selection to keep samples per segment the same
    selectedSamples.maximum = selectedSamples.minimum + (segments * sampPerSeg + 0.5f);

    cursors.setSegments(segments);

    // Keep any FSK polar plot's selected range in sync — cursorsMoved() and
    // enableCursors() both do this, and resegmenting mutates selectedSamples too.
    updateSelectionPlots();
    updateView();
    emitTimeSelection();
}

void PlotView::setFFTAndZoom(int size, int zoom)
{
    auto oldSamplesPerColumn = samplesPerColumn();
    float oldPlotCenter = (verticalScrollBar()->value() + viewport()->height() / 2.0) / plotsHeight();
    if (verticalScrollBar()->maximum() == 0)
        oldPlotCenter = 0.5;

    // Set new FFT size
    fftSize = size;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setFFTSize(size);

    // Set new zoom level
    zoomLevel = std::max(1,zoom);
    nfftSkip = std::max(1,-1*zoom);
    if (spectrogramPlot != nullptr) {
        spectrogramPlot->setZoomLevel(zoomLevel);
        spectrogramPlot->setSkip(nfftSkip);
    }

    // Update horizontal (time) scrollbar
    horizontalScrollBar()->setSingleStep(10);
    horizontalScrollBar()->setPageStep(100);

    updateView(true, samplesPerColumn() < oldSamplesPerColumn);

    // maintain the relative position of the vertical scroll bar
    if (verticalScrollBar()->maximum())
        verticalScrollBar()->setValue((int )(oldPlotCenter * plotsHeight() - viewport()->height() / 2.0 + 0.5f));
}

void PlotView::setPowerMin(int power)
{
    powerMin = power;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setPowerMin(power);
    updateView();
}

void PlotView::setPowerMax(int power)
{
    powerMax = power;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setPowerMax(power);
    updateView();
}

void PlotView::paintEvent(QPaintEvent *event)
{
    if (mainSampleSource == nullptr) return;
    LatencyLog::mark("paintEvent start");

    // Full viewport rectangle
    QRect viewRect(0, 0, width(), height());
    QPainter painter(viewport());
    painter.fillRect(viewRect, Qt::black);

    // Determine heights: spectrogram and derived plots
    if (plots.empty()) return;
    Plot *specPlot = plots.front().get();
    int specHeight = specPlot->height();
    int derivedHeight = 0;
    for (size_t i = 1; i < plots.size(); ++i) {
        derivedHeight += plots[i]->height();
    }

    // Draw spectrogram plot in top region with vertical scrolling
    int yOffset = -verticalScrollBar()->value();
    QRect specRect(0, yOffset, width(), specHeight);
    specPlot->paintBack(painter, specRect, viewRange);
    specPlot->paintMid(painter, specRect, viewRange);
    specPlot->paintFront(painter, specRect, viewRange);

    // Draw cursors and time scale over spectrogram
    if (cursorsEnabled) {
        cursors.paintFront(painter, specRect, viewRange);
    }
    if (timeScaleEnabled) {
        paintTimeScale(painter, specRect, viewRange);
    }

    // Draw derived plots in a fixed area at the bottom (always visible).
    // When the file ends before the viewport's right edge (zoomed-out short
    // capture, or panned past EOF), viewRange is clamped to the file length
    // by updateViewRange — so the actual content width is the pixel span of
    // viewRange at the current zoom, not the full viewport. Stretching N
    // samples across the full width here makes the derived plots disagree
    // with the spectrogram column-for-column.
    int contentWidth = std::min<int>(width(), sampleToColumn(viewRange.length()));
    if (contentWidth < 1) contentWidth = 1;
    if (derivedHeight > 0) {
        // Back layer
        int y = viewRect.height() - derivedHeight;
        for (size_t i = 1; i < plots.size(); ++i) {
            Plot *plot = plots[i].get();
            QRect rect(0, y, contentWidth, plot->height());
            plot->paintBack(painter, rect, viewRange);
            y += plot->height();
        }
        // Mid layer
        y = viewRect.height() - derivedHeight;
        for (size_t i = 1; i < plots.size(); ++i) {
            Plot *plot = plots[i].get();
            QRect rect(0, y, contentWidth, plot->height());
            plot->paintMid(painter, rect, viewRange);
            y += plot->height();
        }
        // Front layer
        y = viewRect.height() - derivedHeight;
        for (size_t i = 1; i < plots.size(); ++i) {
            Plot *plot = plots[i].get();
            QRect rect(0, y, contentWidth, plot->height());
            plot->paintFront(painter, rect, viewRange);
            y += plot->height();
        }
    }
    LatencyLog::mark("paintEvent end");
}

void PlotView::paintTimeScale(QPainter &painter, QRect &rect, range_t<size_t> sampleRange)
{
    float startTime = (float)sampleRange.minimum / sampleRate;
    float stopTime = (float)sampleRange.maximum / sampleRate;
    float duration = stopTime - startTime;

    if (duration <= 0)
        return;

    painter.save();

    QPen pen(Qt::white, 1, Qt::SolidLine);
    painter.setPen(pen);
    QFontMetrics fm(painter.font());

    int tickWidth = 80;
    int maxTicks = rect.width() / tickWidth;

    double durationPerTick = 10 * pow(10, floor(log(duration / maxTicks) / log(10)));

    double firstTick = int(startTime / durationPerTick) * durationPerTick;

    double tick = firstTick;

    while (tick <= stopTime) {

        size_t tickSample = tick * sampleRate;
        int tickLine = sampleToColumn(tickSample - sampleRange.minimum);

        char buf[128];
        snprintf(buf, sizeof(buf), "%.06f", tick);
        painter.drawLine(tickLine, 0, tickLine, 30);
        painter.drawText(tickLine + 2, 25, buf);

        tick += durationPerTick;
    }

    // Draw small ticks
    durationPerTick /= 10;
    firstTick = int(startTime / durationPerTick) * durationPerTick;
    tick = firstTick;
    while (tick <= stopTime) {

        size_t tickSample = tick * sampleRate;
        int tickLine = sampleToColumn(tickSample - sampleRange.minimum);

        painter.drawLine(tickLine, 0, tickLine, 10);
        tick += durationPerTick;
    }

    painter.restore();
}

int PlotView::plotsHeight()
{
    int height = 0;
    for (auto&& plot : plots) {
        height += plot->height();
    }
    return height;
}

void PlotView::resizeEvent(QResizeEvent * event)
{
    updateView();
}

size_t PlotView::samplesPerColumn()
{
    return fftSize * nfftSkip / zoomLevel;
}

void PlotView::scrollContentsBy(int dx, int dy)
{
    LatencyLog::markf("scrollContentsBy dx=%d dy=%d", dx, dy);
    updateView();
}

void PlotView::showEvent(QShowEvent *event)
{
    // Intentionally left blank. See #171
}

void PlotView::updateViewRange(bool reCenter)
{
    // Update current view
    auto start = columnToSample(horizontalScrollBar()->value());
    viewRange = {start, std::min(start + columnToSample(width()), mainSampleSource->count())};

    // Adjust time offset to zoom around central sample
    if (reCenter) {
        horizontalScrollBar()->setValue(
            sampleToColumn(zoomSample) - zoomPos
        );
    }
    zoomSample = viewRange.minimum + viewRange.length() / 2;
    zoomPos = width() / 2;
}

void PlotView::updateView(bool reCenter, bool expanding)
{
    if (!expanding) {
        updateViewRange(reCenter);
    }
    // Horizontal scroll based on total samples
    horizontalScrollBar()->setMaximum(
        std::max(0, sampleToColumn(mainSampleSource->count()) - width())
    );
    // Vertical scroll only for spectrogram area; derived plots are fixed at bottom
    // Compute total height of derived plots
    int derivedHeight = 0;
    for (size_t i = 1; i < plots.size(); ++i) {
        derivedHeight += plots[i]->height();
    }
    // Effective viewport height available to spectrogram
    int specViewHeight = std::max(0, viewport()->height() - derivedHeight);
    // Spectrogram plot height (first plot)
    int specHeight = plots.empty() ? 0 : plots.front()->height();
    verticalScrollBar()->setMaximum(
        std::max(0, specHeight - specViewHeight)
    );

    if (expanding) {
        updateViewRange(reCenter);
    }

    // Update cursors
    range_t<int> newSelection = {
        sampleToColumn(selectedSamples.minimum) - horizontalScrollBar()->value(),
        sampleToColumn(selectedSamples.maximum) - horizontalScrollBar()->value()
    };
    cursors.setSelection(newSelection);

    // Re-paint
    viewport()->update();
    // Re-analyse the visible FM trace's period after the view settles.
    if (periodTimer) periodTimer->start();
}

void PlotView::setSampleRate(double rate)
{
    sampleRate = rate;

    if (spectrogramPlot != nullptr)
        spectrogramPlot->setSampleRate(rate);

    emitTimeSelection();
}

void PlotView::enableScales(bool enabled)
{
    timeScaleEnabled = enabled;

    if (spectrogramPlot != nullptr)
        spectrogramPlot->enableScales(enabled);

    viewport()->update();
}

void PlotView::enableAnnotations(bool enabled)
{
    if (spectrogramPlot != nullptr)
        spectrogramPlot->enableAnnotations(enabled);

    viewport()->update();
}

void PlotView::enableAnnoLabels(bool enabled)
{
    if (spectrogramPlot != nullptr)
        spectrogramPlot->enableAnnoLabels(enabled);

    viewport()->update();
}

void PlotView::enableAnnotationCommentsTooltips(bool enabled)
{
    annotationCommentsEnabled = enabled;

    viewport()->update();
}

void PlotView::enableAnnoColors(bool enabled)
{
    if (spectrogramPlot != nullptr)
        spectrogramPlot->enableAnnoColors(enabled);
    
    viewport()->update();
}
// Adjust the height of derived plots (all except the primary spectrogram)
void PlotView::setDerivedPlotHeight(int height)
{
    derivedPlotHeight = height;
    for (size_t i = 1; i < plots.size(); ++i) {
        plots[i]->setPlotHeight(derivedPlotHeight);
    }
    updateView();
}

int PlotView::sampleToColumn(size_t sample)
{
    return sample / samplesPerColumn();
}

size_t PlotView::columnToSample(int col)
{
    return col * samplesPerColumn();
}
