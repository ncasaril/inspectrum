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

public slots:
    void timeSelectionChanged(float time);
    void zoomIn();
    void zoomOut();
    void enableAnnotations(bool enabled);

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
    QCheckBox *cursorsCheckBox;
    QSpinBox *cursorSymbolsSpinBox;
    QLabel *rateLabel;
    QLabel *periodLabel;
    QLabel *symbolRateLabel;
    QLabel *symbolPeriodLabel;
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
};
