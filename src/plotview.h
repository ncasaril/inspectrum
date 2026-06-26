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

#pragma once

#include <QGraphicsView>
#include <QPaintEvent>
#include <QPoint>
#include <QRubberBand>

#include <QTimer>

#include "cursors.h"
#include "inputsource.h"
#include "plot.h"
#include "samplesource.h"
#include "spectrogramplot.h"
#include "traceplot.h"

class PlotView : public QGraphicsView, Subscriber
{
    Q_OBJECT

public:
    PlotView(InputSource *input);
    void setSampleRate(double rate);

signals:
    void timeSelectionChanged(float time);
    void zoomIn();
    void zoomOut();
    /**
     * Emitted when the mouse moves over the plot area.
     * @param time     Time position in seconds corresponding to mouse X coordinate
     * @param frequency  Frequency offset in Hz when the cursor is over the spectrogram (else 0)
     * @param valueText  Pre-formatted sample-value string when the cursor is over a derived
     *                   trace plot (e.g. "0.0042" for a float plot, "I=… Q=…" for IQ); empty
     *                   when over the spectrogram or no readable plot.
     */
    void mousePositionChanged(double time, double frequency, QString valueText);
    // Echoed after autoTuneFmLpf() picks values, so the dock widgets can
    // be updated to reflect what was applied.
    void fmAutoLpfComputed(double cutoffHz, int predemodM, int postN);
    // Auto-detected dominant period (seconds) of the visible FM trace.
    // Emitted after the analysis debounce timer fires. periodSeconds<=0
    // means "no signal / not enough data".
    void autoPeriodChanged(double periodSeconds);

public slots:
    void cursorsMoved();
    void enableCursors(bool enabled);
    void enableScales(bool enabled);
    void enableAnnotations(bool enabled);
    void enableAnnoLabels(bool enabled);
    void enableAnnotationCommentsTooltips(bool enabled);
    void enableAnnoColors(bool enabled);
    void invalidateEvent() override;
    void repaint();
    void setCursorSegments(int segments);
    void setFFTAndZoom(int fftSize, int zoomLevel);
    void setPowerMin(int power);
    void setPowerMax(int power);
    void setDerivedPlotHeight(int height);
    /**
     * Enable or disable the fast-path (cheap) FM demodulation mode
     */
    void enableFastDemod(bool enabled);
    /**
     * Set maximum threads for background tile rendering.
     */
    void setMaxThreads(int threads);
    // Cutoff (Hz) for the post-demod LPF on every FM plot. 0 disables.
    void setFmLpfCutoff(double hz);
    // LPF backend (matches FrequencyDemod::LpfMethod).
    void setFmLpfMethod(int method);
    // Block-average decimation factor on every FM plot. 1 disables.
    void setFmDecimation(int n);
    // Pre-demod IQ decimation factor (1 = off).
    void setFmPredemodDecimation(int m);
    // Amplitude squelch (% of window-peak |IQ|) for every FM plot. 0 = off.
    void setFmSquelch(int pct);
    // Switch every AM plot between linear power and a dB scale.
    void setAmDbMode(bool on);
    // Full-scale reference (dBm) for AM plots in dB mode. 0 = dBFS.
    void setAmReferenceLevel(double dbm);
    // Symbol rate (baud) for the FSK polar plot's differential delay. 0 = unset.
    void setSymbolRate(double baud);
    // Signal-strength gate (% of window peak) for the FSK polar constellation.
    void setConstellationGate(int pct);
    // Symbol-timed (1 point/symbol) vs full-rate FSK polar constellation.
    void setConstellationSymbolTimed(bool on);
    // Pick reasonable LPF cutoff, predemod M, and post N from the current
    // sample rate and tuner bandwidth. Applies them and emits
    // fmAutoLpfComputed() so the dock widgets can mirror.
    void autoTuneFmLpf();
    // Toggle the period analyser. When off, no scan runs and any existing
    // markers / period label are cleared.
    void setPeriodAnalysisEnabled(bool enabled);
    // Forward dock changes to the spectrogram render mode / reassignment
    // floor. The spectrogram plot owns the actual rendering state; these
    // are pure pass-throughs.
    void setSpectrogramMode(int mode);
    void setReassignmentFloor(int floorDb);
    void setReassignmentWindow(int wt);
    void setReassignmentSplat(int sm);

protected:
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent * event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent * event) override;
    void scrollContentsBy(int dx, int dy) override;
    bool viewportEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    Cursors cursors;
    SampleSource<std::complex<float>> *mainSampleSource = nullptr;
    SpectrogramPlot *spectrogramPlot = nullptr;
    std::vector<std::unique_ptr<Plot>> plots;
    range_t<size_t> viewRange;
    range_t<size_t> selectedSamples;
    int zoomPos;
    size_t zoomSample;

    int fftSize = 1024;
    int zoomLevel = 1;
    int nfftSkip = 1;
    int powerMin;
    int powerMax;
    bool cursorsEnabled;
    double sampleRate = 0.0;
    bool timeScaleEnabled;
    int scrollZoomStepsAccumulated = 0;
    bool annotationCommentsEnabled;

    void addPlot(Plot *plot);
    void emitTimeSelection();
    // Read a single-sample value from a derived plot's source (FM/AM = float,
    // IQ = complex<float>) and format for display in the status bar.
    // rawValueOut, if non-null, also receives the unscaled float used to
    // place the hover dot on the trace (NaN if no readable value).
    QString sampleValueText(Plot *plot, size_t sampleIdx,
                            double *rawValueOut = nullptr);
    void extractSymbols(std::shared_ptr<AbstractSampleSource> src, bool toClipboard);
    void exportSamples(std::shared_ptr<AbstractSampleSource> src);
    template<typename SOURCETYPE> void exportSamples(std::shared_ptr<AbstractSampleSource> src);
    // Writes a SigMF pair (.sigmf-meta + .sigmf-data) of the tuned IQ over
    // [start..end) with integer decimation. metaPath must end in .sigmf-meta.
    // Returns true on success. Annotations on the input source that intersect
    // both the time window and the resulting passband are translated and
    // included; their absolute Hz frequencies are preserved.
    bool writeSigmf(std::shared_ptr<SampleSource<std::complex<float>>> src,
                    const QString &metaPath,
                    size_t start, size_t end, int decim);
    int plotsHeight();
    size_t samplesPerColumn();
    void updateViewRange(bool reCenter);
    void updateView(bool reCenter = false, bool expanding = false);
    void paintTimeScale(QPainter &painter, QRect &rect, range_t<size_t> sampleRange);
    void updateAnnotationTooltip(QMouseEvent *event);

    int sampleToColumn(size_t sample);
    size_t columnToSample(int col);
    int derivedPlotHeight;
    // Debounced auto-period analyser: bumped from updateView() and from
    // every FM-filter setter; fires once after a short idle so we don't
    // re-scan the visible region on every scroll tick. Gated by
    // periodAnalysisEnabled (off by default).
    QTimer *periodTimer = nullptr;
    bool   periodAnalysisEnabled = false;
    void analyzeVisiblePeriod();
    void updateSelectionPlots();
    // Latest-applied FM post-demod settings; re-applied when new FM plots are added.
    double fmLpfCutoffHz = 0.0;
    int    fmLpfMethod = 0; // FrequencyDemod::LpfMethod::KaiserFir
    int    fmDecim = 1;
    int    fmPredemodDecim = 1;
    bool   fmFastDemod = false;
    int    fmSquelchPct = 0; // amplitude squelch (% of window-peak |IQ|), 0 = off
    // AM-plot display: dB scale toggle and full-scale reference (dBm; 0 = dBFS),
    // re-applied to AM plots as they're added.
    bool   amDbMode = false;
    double amRefLevelDbm = 0.0;
    // Symbol rate (baud) re-applied to FSK polar plots as they're added. 0 = unset.
    double symbolRateHz = 0.0;
    // Constellation signal-strength gate (% of window peak), re-applied likewise.
    int    constellationGatePct = 15;
    // Symbol-timed constellation rendering, re-applied to new FSK polar plots.
    bool   constellationSymbolTimed = true;
    // Shift+left-drag annotation rubber-banding on the spectrogram. The
    // rectangle is in viewport coords; on release we map x→sample range and
    // y→absolute Hz range, then open AnnotationDialog.
    QRubberBand *annotRubber = nullptr;
    bool         annotDragging = false;
    QPoint       annotDragOrigin;
    bool isOverSpectrogram(int viewportY) const;
    bool startAnnotationDrag(QMouseEvent *event);
    void updateAnnotationDrag(QMouseEvent *event);
    void finishAnnotationDrag(QMouseEvent *event);

    // --- Interactive bounds editing of an existing annotation ------------
    // Which part of an annotation's box a point is grabbing: an edge/corner
    // resizes that side, the body moves the whole box.
    enum class AnnoGrab {
        None, Move, Left, Right, Top, Bottom,
        TopLeft, TopRight, BottomLeft, BottomRight
    };
    int      hoveredAnnotation = -1;   // drawn with handles; drives hover cursor
    int      editingAnnotation = -1;   // currently being dragged (-1 = none)
    AnnoGrab editGrab = AnnoGrab::None;
    QPoint   editPressPos;             // viewport pos where the drag began
    Annotation editOrig;               // annotation snapshot at drag start
    // Viewport rectangle of an annotation's box at the current scroll/zoom.
    QRect annotationViewportRect(const Annotation &a);
    // Which grab region (if any) `pos` is over for box `r` (handle margin px).
    AnnoGrab grabForRect(const QRect &r, QPoint pos) const;
    Qt::CursorShape cursorForGrab(AnnoGrab g) const;
    // Topmost annotation whose box/handles `pos` grabs; fills *grabOut. -1 none.
    int annotationGrabAt(QPoint pos, AnnoGrab *grabOut);
    // Map a viewport coordinate back to a sample index / absolute Hz.
    long long sampleAtViewportX(int vx);
    double freqAtViewportY(int vy) const;
    // Hover (no button): set the active annotation + resize cursor.
    void updateAnnotationHover(QMouseEvent *event);
    // Press/drag/release on an annotation box to move or resize it.
    bool beginAnnotationEdit(QMouseEvent *event);
    void updateAnnotationEdit(QMouseEvent *event);
    void finishAnnotationEdit(QMouseEvent *event);
    // Compose an annotation from a viewport rectangle and open the editor.
    // On accept, appends to the input source.
    void promptNewAnnotation(QRect viewportRect);
    // Right-click "Add annotation here" / "Edit annotation" / "Delete
    // annotation" wiring lives in contextMenuEvent; these helpers are the
    // shared logic.
    void promptEditAnnotation(int index);
    void deleteAnnotation(int index);
    // Connected to InputSource::setAnnotationCallback in the constructor —
    // handles repaint, dirty title bar, dock save-button enable.
    void onAnnotationsChanged();
};
