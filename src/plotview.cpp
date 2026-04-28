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
#include "frequencydemod.h"
#include <QPixmapCache>
#include <iostream>
#include <fstream>
#include <QtGlobal>
#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QWheelEvent>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QMenu>
#include <QPainter>
#include <QProgressDialog>
#include <QRadioButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QToolTip>
#include <QVBoxLayout>
#include "plots.h"
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
    // clear any cached trace tiles so new demod data is used
    QPixmapCache::clear();
    // walk all derived TracePlot instances and update their demod mode
    for (auto &plt : plots) {
        if (auto tp = dynamic_cast<TracePlot*>(plt.get())) {
            auto src = tp->source();
            if (auto fd = dynamic_cast<FrequencyDemod*>(src.get())) {
                fd->setCheapDemod(enabled);
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
            if (auto fd = dynamic_cast<FrequencyDemod*>(tp->source().get())) {
                fd->setPostLpfCutoff(hz);
            }
        }
    }
    QPixmapCache::clear();
    viewport()->update();
    if (periodTimer) periodTimer->start();
}

void PlotView::setFmDecimation(int n)
{
    if (n < 1) n = 1;
    fmDecim = n;
    for (auto &plt : plots) {
        if (auto tp = dynamic_cast<TracePlot*>(plt.get())) {
            if (auto fd = dynamic_cast<FrequencyDemod*>(tp->source().get())) {
                fd->setPostDecimation(n);
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
            if (auto fd = dynamic_cast<FrequencyDemod*>(tp->source().get())) {
                fd->setPostLpfMethod(m);
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
            if (auto fd = dynamic_cast<FrequencyDemod*>(tp->source().get())) {
                fd->setPredemodDecimation(m);
            }
        }
    }
    QPixmapCache::clear();
    viewport()->update();
    if (periodTimer) periodTimer->start();
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

void PlotView::analyzeVisiblePeriod()
{
    // Find the first derived float-source plot (FM trace by convention) and
    // estimate its dominant period over the currently-visible sample range
    // by counting zero crossings around the trace's mean. Cheap, robust to
    // amplitude scale, and works for any roughly-periodic signal — won't
    // give meaningful numbers for noise or a flat trace, which the
    // applyAutoPeriod() consumer handles by displaying "—".
    if (sampleRate <= 0.0) {
        emit autoPeriodChanged(0.0);
        return;
    }
    SampleSource<float> *fsrc = nullptr;
    for (auto &plt : plots) {
        auto tp = dynamic_cast<TracePlot*>(plt.get());
        if (!tp) continue;
        auto typed = std::dynamic_pointer_cast<SampleSource<float>>(tp->source());
        if (typed) { fsrc = typed.get(); break; }
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
        emit autoPeriodChanged(0.0);
        return;
    }
    if (n > kMaxAnalyseSamples) n = kMaxAnalyseSamples;

    auto data = fsrc->getSamples(viewRange.minimum, n);
    if (!data) {
        emit autoPeriodChanged(0.0);
        return;
    }

    double sum = 0.0;
    size_t valid = 0;
    for (size_t i = 0; i < n; ++i) {
        if (std::isfinite(data[i])) { sum += data[i]; ++valid; }
    }
    if (valid < 256) {
        emit autoPeriodChanged(0.0);
        return;
    }
    const double mean = sum / valid;

    // Hysteresis around the mean to avoid counting noise-level wobbles as
    // crossings. Use a small fraction of the trace's span as a deadband.
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(data[i])) continue;
        if (data[i] < mn) mn = data[i];
        if (data[i] > mx) mx = data[i];
    }
    const double span = mx - mn;
    if (span <= 0.0) {
        emit autoPeriodChanged(0.0);
        return;
    }
    const double hyst = 0.1 * span;

    // Count low-to-high crossings of the (data - mean) signal, with the
    // value first having to dip below mean - hyst before a crossing back
    // above mean + hyst counts. Each such crossing = one full period.
    int periods = 0;
    bool armedLow = false;
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(data[i])) continue;
        const double d = data[i] - mean;
        if (d < -hyst) armedLow = true;
        else if (armedLow && d > hyst) {
            ++periods;
            armedLow = false;
        }
    }
    if (periods < 2) {
        emit autoPeriodChanged(0.0);
        return;
    }
    const double duration = static_cast<double>(n) / sampleRate;
    const double period = duration / periods;
    emit autoPeriodChanged(period);
}

void PlotView::addPlot(Plot *plot)
{
    plots.emplace_back(plot);
    // If this is a derived plot (not the main spectrogram), set its height
    if (plots.size() > 1) {
        plot->setPlotHeight(derivedPlotHeight);
    }
    // Propagate the currently-configured FM settings to any newly added FM plot.
    if (auto tp = dynamic_cast<TracePlot*>(plot)) {
        if (auto fd = dynamic_cast<FrequencyDemod*>(tp->source().get())) {
            fd->setPostLpfMethod(static_cast<FrequencyDemod::LpfMethod>(fmLpfMethod));
            fd->setPostLpfCutoff(fmLpfCutoffHz);
            fd->setPostDecimation(fmDecim);
            fd->setPredemodDecimation(fmPredemodDecim);
        }
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
    // plot — figure out which one and read its value at this x.
    const int derivedCount = static_cast<int>(plots.size()) - 1;
    const int derivedTotalH = derivedCount * derivedPlotHeight;
    const int derivedTop = viewportH - derivedTotalH;
    if (derivedCount > 0 && y >= derivedTop) {
        const int posInDerived = y - derivedTop;
        const int plotIdx = 1 + posInDerived / derivedPlotHeight;
        if (plotIdx >= 1 && static_cast<size_t>(plotIdx) < plots.size()) {
            valueText = sampleValueText(plots[plotIdx].get(), sampleIdx);
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

    emit mousePositionChanged(timePos, freqPos, valueText);
    QGraphicsView::mouseMoveEvent(event);
}

QString PlotView::sampleValueText(Plot *plot, size_t sampleIdx)
{
    auto tp = dynamic_cast<TracePlot*>(plot);
    if (!tp) return QString();
    auto src = tp->source();
    if (!src) return QString();

    // Float source (FM, AM, threshold): read one sample and format.
    if (auto fsrc = std::dynamic_pointer_cast<SampleSource<float>>(src)) {
        auto data = fsrc->getSamples(sampleIdx, 1);
        if (!data) return QString();
        float v = data[0];
        if (!std::isfinite(v)) return QStringLiteral("NaN");
        return QString::number(v, 'g', 6);
    }
    // Complex source (IQ plot): show I and Q separately plus magnitude.
    if (auto csrc = std::dynamic_pointer_cast<SampleSource<std::complex<float>>>(src)) {
        auto data = csrc->getSamples(sampleIdx, 1);
        if (!data) return QString();
        float i = data[0].real();
        float q = data[0].imag();
        float mag = std::hypot(i, q);
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

void PlotView::cursorsMoved()
{
    selectedSamples = {
        columnToSample(horizontalScrollBar()->value() + cursors.selection().minimum),
        columnToSample(horizontalScrollBar()->value() + cursors.selection().maximum)
    };

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
    }
    viewport()->update();
}

bool PlotView::viewportEvent(QEvent *event) {
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

template<typename SOURCETYPE>
void PlotView::exportSamples(std::shared_ptr<AbstractSampleSource> src)
{
    auto sampleSrc = std::dynamic_pointer_cast<SampleSource<SOURCETYPE>>(src);
    if (!sampleSrc) {
        return;
    }

    QFileDialog dialog(this);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilter(getFileNameFilter<SOURCETYPE>());
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
    decimation.setValue(1 / sampleSrc->relativeBandwidth());

    QVBoxLayout vbox2;
    vbox2.addWidget(&decimation);

    groupBox2.setLayout(&vbox2);
    l->addWidget(&groupBox2, 4, 2);

    if (dialog.exec()) {
        QStringList fileNames = dialog.selectedFiles();

        size_t start, end;
        if (cursorSelection.isChecked()) {
            start = selectedSamples.minimum;
            end = start + selectedSamples.length();
        } else if(currentView.isChecked()) {
            start = viewRange.minimum;
            end = start + viewRange.length();
        } else {
            start = 0;
            end = sampleSrc->count();
        }

        std::ofstream os (fileNames[0].toStdString(), std::ios::binary);

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

    // Draw derived plots in a fixed area at the bottom (always visible)
    if (derivedHeight > 0) {
        // Back layer
        int y = viewRect.height() - derivedHeight;
        for (size_t i = 1; i < plots.size(); ++i) {
            Plot *plot = plots[i].get();
            QRect rect(0, y, width(), plot->height());
            plot->paintBack(painter, rect, viewRange);
            y += plot->height();
        }
        // Mid layer
        y = viewRect.height() - derivedHeight;
        for (size_t i = 1; i < plots.size(); ++i) {
            Plot *plot = plots[i].get();
            QRect rect(0, y, width(), plot->height());
            plot->paintMid(painter, rect, viewRange);
            y += plot->height();
        }
        // Front layer
        y = viewRect.height() - derivedHeight;
        for (size_t i = 1; i < plots.size(); ++i) {
            Plot *plot = plots[i].get();
            QRect rect(0, y, width(), plot->height());
            plot->paintFront(painter, rect, viewRange);
            y += plot->height();
        }
    }
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
