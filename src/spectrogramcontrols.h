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

#include <QDockWidget>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>

class SpectrogramControls : public QDockWidget
{
    Q_OBJECT

public:
    SpectrogramControls(const QString & title, QWidget * parent);
    void setDefaults();

signals:
    void fftOrZoomChanged(int fftSize, int zoomLevel);
    void openFile(QString fileName);
    void derivedHeightChanged(int height);
    // toggle fast-path (instantaneous) frequency demod in derived trace plot
    void fastDemodChanged(bool enabled);
    /**
     * Requested number of threads to use for tile rendering.
     */
    void threadsChanged(int threads);
    // Post-demod LPF cutoff (Hz) applied to all frequency plots. 0 = disabled.
    void fmLpfChanged(double hz);
    // Post-demod LPF implementation (matches FrequencyDemod::LpfMethod).
    void fmLpfMethodChanged(int method);
    // Block-averaging decimation factor applied after the FM demod. 1 = disabled.
    void fmDecimChanged(int n);
    // Pre-demod IQ decimation factor (1 = off). When set the FM demod
    // chain runs at Fs/M (IQEngine pattern).
    void fmPredemodDecimChanged(int m);
    // User clicked "Auto-tune FM LPF" — PlotView picks values from the
    // current Fs / tuner bandwidth, applies them, then echoes them back
    // to applyAutoLpf so the dock widgets stay in sync.
    void autoLpfRequested();
    // Auto-period analysis on/off. Off = no scan, no label, no on-plot
    // markers. On (default off) = continuous analysis after each view or
    // filter change, label updates, and triangle/line overlay drawn on
    // the FM trace.
    void periodAnalysisChanged(bool enabled);
    // Spectrogram render mode (Standard | Reassigned). Index matches
    // SpectrogramMode enum.
    void spectrogramModeChanged(int mode);
    // Per-bin power floor (dB) below which reassignment is skipped.
    void reassignmentFloorChanged(int floorDb);
    // Reassignment analysis window (Hann | Gaussian). Index matches
    // WindowType enum.
    void reassignmentWindowChanged(int wt);
    // Reassignment splat method (Bilinear | Nearest). Index matches
    // SplatMethod enum.
    void reassignmentSplatChanged(int sm);
    // User clicked "Save annotations". MainWindow handles the actual write.
    void saveAnnotationsRequested();

public slots:
    void timeSelectionChanged(float time);
    void zoomIn();
    void zoomOut();
    void enableAnnotations(bool enabled);
    // Reflect the annotation dirty state — enables / disables the save
    // button and tweaks its label so the user can see when there's pending
    // work without watching the title bar.
    void setAnnotationsDirty(bool dirty);
    void applyAutoLpf(double cutoffHz, int predemodM, int postN);
    // Show the auto-detected period (in seconds) of the visible FM trace.
    // periodSeconds <= 0 clears the label (no signal / not enough data).
    void applyAutoPeriod(double periodSeconds);
    // Show the sample value under the cursor when hovering over a derived
    // plot. Empty string clears the label.
    void applyCursorValue(QString text);

private slots:
    void fftSizeChanged(int value);
    void zoomLevelChanged(int value);
    void powerMinChanged(int value);
    void powerMaxChanged(int value);
    void fileOpenButtonClicked();
    void cursorsStateChanged(int state);

private:
    QWidget *widget;
    QFormLayout *layout;
    void clearCursorLabels();
    void fftOrZoomChanged(void);

public:
    QPushButton *fileOpenButton;
    QLineEdit *sampleRate;
    QSlider *fftSizeSlider;
    QSlider *zoomLevelSlider;
    QSlider *powerMaxSlider;
    QSlider *powerMinSlider;
    // Top spectrogram render mode: Standard |STFT|² (index 0) or
    // Fulop-Fitz reassigned spectrogram (index 1). Default = Standard.
    QComboBox *spectrogramModeCombo;
    // Per-bin power floor (dB) for reassignment. Bins below this threshold
    // are not reassigned — they're rendered at their original (t, ω) so
    // noise context stays visible without speckle.
    QSlider *reassignmentFloorSlider;
    QLabel *reassignmentFloorValueLabel;
    // Reassignment analysis window (default Hann; Gaussian gives sharper
    // ridges as it's the textbook reassignment window).
    QComboBox *reassignmentWindowCombo;
    // Reassignment splat method (default Bilinear; Nearest is ~4× faster
    // on the inner loop with mild aliasing).
    QComboBox *reassignmentSplatCombo;
    QCheckBox *cursorsCheckBox;
    QSpinBox *cursorSymbolsSpinBox;
    QLabel *rateLabel;
    QLabel *periodLabel;
    QLabel *symbolRateLabel;
    QLabel *symbolPeriodLabel;
    // Auto-detected dominant period of the visible FM trace (zero-crossing
    // estimate, updated whenever the view or filter changes).
    QLabel *autoPeriodLabel;
    // Toggle for the period analysis (label + on-plot markers). Off by
    // default — the markers get noisy on broadband signals.
    QCheckBox *periodAnalysisCheckBox;
    // Sample value at the mouse cursor when hovering a derived plot.
    QLabel *cursorValueLabel;
    QCheckBox *scalesCheckBox;
    QCheckBox *annosCheckBox;
    QCheckBox *annoLabelCheckBox;
    QCheckBox *commentsCheckBox;
    QCheckBox *annoColorCheckBox;
    QSpinBox *derivedPlotHeightSpinBox;
    // fast (cheap) demodulation mode for FM traces
    QCheckBox *fastDemodCheckBox;
    /**
     * Spinbox to select number of threads for concurrent tasks.
     */
    QSpinBox   *threadCountSpinBox;
    // FM post-demod LPF implementation (Kaiser FIR / Butterworth IIR / Elliptic IIR)
    QComboBox  *fmLpfMethodCombo;
    // FM post-demod LPF cutoff in Hz (0 disables)
    QLineEdit  *fmLpfLineEdit;
    // FM post-demod block-average decimation factor (1 disables)
    QSpinBox   *fmDecimSpinBox;
    // FM pre-demod IQ decimation (1 disables)
    QSpinBox   *fmPredemodDecimSpinBox;
    // Auto-tune button: PlotView picks reasonable values for cutoff, M, N.
    QPushButton *fmAutoLpfButton;
    // Save annotations to a .sigmf-meta sidecar. Shown disabled when the
    // annotation list hasn't been mutated since the last load/save.
    QPushButton *saveAnnotationsButton;
};
