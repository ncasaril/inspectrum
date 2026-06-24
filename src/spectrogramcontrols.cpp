/*
 *  Copyright (C) 2015, Mike Walters <mike@flomp.net>
 *  Copyright (C) 2015, Jared Boone <jared@sharebrained.com>
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

#include "spectrogramcontrols.h"
#include <QIntValidator>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QSettings>
#include <QLabel>
#include <QSizePolicy>
#include <cmath>
#include <string>
#include "util.h"

namespace {
// Stop a label with dynamic, variable-length text from driving the dock's
// minimum width (and thus reflowing the central plot view) as its content
// changes. Ignored horizontal policy => 0 contribution to the layout minimum.
void makeWidthStable(QLabel *label)
{
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setMinimumWidth(0);
}
} // namespace

SpectrogramControls::SpectrogramControls(const QString & title, QWidget * parent)
    : QDockWidget::QDockWidget(title, parent)
{
    widget = new QWidget(this);
    layout = new QFormLayout(widget);

    fileOpenButton = new QPushButton("Open file...", widget);
    layout->addRow(fileOpenButton);

    sampleRate = new QLineEdit();
    auto double_validator = new QDoubleValidator(this);
    double_validator->setBottom(0.0);
    sampleRate->setValidator(double_validator);
    layout->addRow(new QLabel(tr("Sample rate:")), sampleRate);

    // Spectrogram settings
    layout->addRow(new QLabel()); // TODO: find a better way to add an empty row?
    layout->addRow(new QLabel(tr("<b>Spectrogram</b>")));

    fftSizeSlider = new QSlider(Qt::Horizontal, widget);
    fftSizeSlider->setRange(4, 16);
    fftSizeSlider->setPageStep(1);

    layout->addRow(new QLabel(tr("FFT size:")), fftSizeSlider);

    zoomLevelSlider = new QSlider(Qt::Horizontal, widget);
    zoomLevelSlider->setRange(-6, 10);
    zoomLevelSlider->setPageStep(1);

    layout->addRow(new QLabel(tr("Zoom:")), zoomLevelSlider);

    powerMaxSlider = new QSlider(Qt::Horizontal, widget);
    powerMaxSlider->setRange(-140, 10);
    layout->addRow(new QLabel(tr("Power max:")), powerMaxSlider);

    powerMinSlider = new QSlider(Qt::Horizontal, widget);
    powerMinSlider->setRange(-140, 10);
    layout->addRow(new QLabel(tr("Power min:")), powerMinSlider);

    scalesCheckBox = new QCheckBox(widget);
    scalesCheckBox->setCheckState(Qt::Checked);
    layout->addRow(new QLabel(tr("Scales:")), scalesCheckBox);

    // Spectrogram render mode: Standard (the existing |STFT|² path) or
    // Reassigned (Fulop-Fitz time-frequency reassignment). Default is
    // Standard so behaviour is unchanged for users who don't touch it.
    spectrogramModeCombo = new QComboBox(widget);
    spectrogramModeCombo->addItem(tr("Standard"));
    spectrogramModeCombo->addItem(tr("Reassigned"));
    spectrogramModeCombo->setCurrentIndex(0);
    spectrogramModeCombo->setToolTip(tr(
        "Render mode for the top spectrogram. Reassigned uses Fulop-Fitz "
        "time-frequency reassignment (3× FFT cost) to sharpen tonal and "
        "chirp ridges. Below the noise floor threshold, bins are left at "
        "their original location."));
    layout->addRow(new QLabel(tr("Render mode:")), spectrogramModeCombo);
    connect(spectrogramModeCombo,
            static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &SpectrogramControls::spectrogramModeChanged);

    // Reassignment noise floor (dB). Default -80 follows the Auger-Flandrin
    // recommendation; below this threshold the bin is rendered at its
    // original (t,ω) instead of being reassigned, which would otherwise
    // produce meaningless speckle in noise regions.
    reassignmentFloorSlider = new QSlider(Qt::Horizontal, widget);
    reassignmentFloorSlider->setRange(-140, -20);
    reassignmentFloorSlider->setValue(-80);
    reassignmentFloorSlider->setToolTip(tr(
        "Bins with power below this threshold are not reassigned. -80 dB "
        "matches the Auger-Flandrin recommendation; lower values reassign "
        "more (and may speckle), higher values reassign less."));
    reassignmentFloorValueLabel = new QLabel(QStringLiteral("-80 dB"), widget);
    {
        auto *floorRow = new QWidget(widget);
        auto *floorLayout = new QHBoxLayout(floorRow);
        floorLayout->setContentsMargins(0, 0, 0, 0);
        floorLayout->addWidget(reassignmentFloorSlider, 1);
        floorLayout->addWidget(reassignmentFloorValueLabel);
        layout->addRow(new QLabel(tr("Reassign floor:")), floorRow);
    }
    connect(reassignmentFloorSlider, &QSlider::valueChanged,
            this, [this](int v) {
        reassignmentFloorValueLabel->setText(QString("%1 dB").arg(v));
        emit reassignmentFloorChanged(v);
    });

    // Reassignment analysis window: Gaussian gives sharper ridges than Hann
    // for tonal/chirp signals at the same FFT cost. Order matches WindowType.
    reassignmentWindowCombo = new QComboBox(widget);
    reassignmentWindowCombo->addItem(tr("Hann"));
    reassignmentWindowCombo->addItem(tr("Gaussian"));
    reassignmentWindowCombo->setCurrentIndex(0);
    reassignmentWindowCombo->setToolTip(tr(
        "Analysis window for the reassigned spectrogram. Gaussian is the "
        "textbook reassignment window — its own Fourier eigenfunction — "
        "and gives sharper ridges. Standard mode always uses Hann."));
    layout->addRow(new QLabel(tr("Reassign window:")), reassignmentWindowCombo);
    connect(reassignmentWindowCombo,
            static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &SpectrogramControls::reassignmentWindowChanged);

    // Reassignment splat method. Order matches SplatMethod.
    reassignmentSplatCombo = new QComboBox(widget);
    reassignmentSplatCombo->addItem(tr("Bilinear"));
    reassignmentSplatCombo->addItem(tr("Nearest"));
    reassignmentSplatCombo->setCurrentIndex(0);
    reassignmentSplatCombo->setToolTip(tr(
        "How |X_h|² is distributed onto the reassigned grid. Bilinear "
        "(default) writes to the 4 nearest pixels; Nearest writes to one "
        "and is ~4× cheaper on the inner loop."));
    layout->addRow(new QLabel(tr("Reassign splat:")), reassignmentSplatCombo);
    connect(reassignmentSplatCombo,
            static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &SpectrogramControls::reassignmentSplatChanged);

    // Time selection settings
    layout->addRow(new QLabel()); // TODO: find a better way to add an empty row?
    layout->addRow(new QLabel(tr("<b>Time selection</b>")));

    cursorsCheckBox = new QCheckBox(widget);
    layout->addRow(new QLabel(tr("Enable cursors:")), cursorsCheckBox);

    cursorSymbolsSpinBox = new QSpinBox();
    cursorSymbolsSpinBox->setMinimum(1);
    cursorSymbolsSpinBox->setMaximum(99999);
    layout->addRow(new QLabel(tr("Symbols:")), cursorSymbolsSpinBox);

    rateLabel = new QLabel();
    layout->addRow(new QLabel(tr("Rate:")), rateLabel);

    periodLabel = new QLabel();
    layout->addRow(new QLabel(tr("Period:")), periodLabel);

    symbolRateLabel = new QLabel();
    layout->addRow(new QLabel(tr("Symbol rate:")), symbolRateLabel);

    symbolPeriodLabel = new QLabel();
    layout->addRow(new QLabel(tr("Symbol period:")), symbolPeriodLabel);

    // SigMF selection settings
    layout->addRow(new QLabel()); // TODO: find a better way to add an empty row?
    layout->addRow(new QLabel(tr("<b>SigMF Control</b>")));

    annosCheckBox = new QCheckBox(widget);
    layout->addRow(new QLabel(tr("Display Annotations:")), annosCheckBox);
    annoLabelCheckBox = new QCheckBox(widget);
    layout->addRow(new QLabel(tr("Annotation Labels:")), annoLabelCheckBox);
    commentsCheckBox = new QCheckBox(widget);
    layout->addRow(new QLabel(tr("Annotation comments:")), commentsCheckBox);
    annoColorCheckBox = new QCheckBox(widget);
    layout->addRow(new QLabel(tr("Annotation Colors:")), annoColorCheckBox);

    saveAnnotationsButton = new QPushButton(tr("Save annotations"), widget);
    saveAnnotationsButton->setEnabled(false);
    saveAnnotationsButton->setToolTip(tr(
        "Write the current annotation list to a .sigmf-meta sidecar next to "
        "the data file. Becomes enabled after add/edit/delete."));
    layout->addRow(saveAnnotationsButton);
    connect(saveAnnotationsButton, &QPushButton::clicked,
            this, &SpectrogramControls::saveAnnotationsRequested);

    // Derived plots settings
    layout->addRow(new QLabel()); // spacer
    layout->addRow(new QLabel(tr("<b>Derived Plots</b>")));
    autoPeriodLabel = new QLabel(QStringLiteral("—"));
    autoPeriodLabel->setToolTip(tr(
        "Auto-detected dominant period of the visible FM trace, estimated "
        "from zero-crossings around the trace's mean. Updates as you pan, "
        "zoom or change filter settings."));
    // These two labels are rewritten with variable-width text on every mouse
    // move / analysis pass. A plain QLabel reports its full text width as its
    // minimum size, so a non-wrapping label here would grow the dock's minimum
    // width and reflow the central spectrogram ("everything jumps") whenever
    // the readout got longer. Ignored horizontal policy makes them contribute
    // 0 to the dock's minimum width — the column stays fixed and long values
    // just clip (the full value is still in the tooltip and the status bar).
    makeWidthStable(autoPeriodLabel);
    layout->addRow(new QLabel(tr("Auto period:")), autoPeriodLabel);
    periodAnalysisCheckBox = new QCheckBox(widget);
    periodAnalysisCheckBox->setCheckState(Qt::Unchecked);
    periodAnalysisCheckBox->setToolTip(tr(
        "Run the period analyser and draw peak markers on the FM trace. "
        "Disabled by default because the markers can be hectic on noisy "
        "signals."));
    layout->addRow(new QLabel(tr("Period markers:")), periodAnalysisCheckBox);
    connect(periodAnalysisCheckBox, &QCheckBox::toggled,
            this, &SpectrogramControls::periodAnalysisChanged);
    cursorValueLabel = new QLabel(QStringLiteral("—"));
    cursorValueLabel->setToolTip(tr(
        "Sample value at the mouse cursor when hovering over a derived "
        "trace plot. Float for FM/AM/threshold, I+Q+|·| for IQ."));
    makeWidthStable(cursorValueLabel);
    layout->addRow(new QLabel(tr("Cursor value:")), cursorValueLabel);
    derivedPlotHeightSpinBox = new QSpinBox(widget);
    derivedPlotHeightSpinBox->setRange(20, 1000);
    derivedPlotHeightSpinBox->setValue(200);
    layout->addRow(new QLabel(tr("Plot height:")), derivedPlotHeightSpinBox);
    connect(derivedPlotHeightSpinBox, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &SpectrogramControls::derivedHeightChanged);
    // Fast-path instantaneous-frequency FM demodulation
    fastDemodCheckBox = new QCheckBox(widget);
    fastDemodCheckBox->setCheckState(Qt::Unchecked);
    layout->addRow(new QLabel(tr("Fast-path FM demod:")), fastDemodCheckBox);
    connect(fastDemodCheckBox, &QCheckBox::toggled,
            this, &SpectrogramControls::fastDemodChanged);
    // Thread count for concurrent tile rendering
    threadCountSpinBox = new QSpinBox(widget);
    threadCountSpinBox->setRange(1, 64);
    threadCountSpinBox->setValue(8);
    layout->addRow(new QLabel(tr("Threads:")), threadCountSpinBox);
    connect(threadCountSpinBox, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &SpectrogramControls::threadsChanged);
    // FM post-demod LPF method. Kept as a picker so the slow-but-accurate
    // Kaiser FIR is still available for A/B comparison against the IIR
    // alternatives. Order here must match FrequencyDemod::LpfMethod.
    fmLpfMethodCombo = new QComboBox(widget);
    fmLpfMethodCombo->addItem(tr("Kaiser FIR"));
    fmLpfMethodCombo->addItem(tr("Butterworth IIR (filtfilt)"));
    fmLpfMethodCombo->setCurrentIndex(0); // default = Kaiser FIR (linear-phase, accurate)
    layout->addRow(new QLabel(tr("FM LPF method:")), fmLpfMethodCombo);
    connect(fmLpfMethodCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &SpectrogramControls::fmLpfMethodChanged);
    // FM post-demod LPF cutoff (Hz). 0 = disabled.
    fmLpfLineEdit = new QLineEdit(widget);
    auto fmLpfValidator = new QDoubleValidator(0.0, 1e9, 3, this);
    fmLpfLineEdit->setValidator(fmLpfValidator);
    fmLpfLineEdit->setText("0");
    layout->addRow(new QLabel(tr("FM LPF cutoff (Hz):")), fmLpfLineEdit);
    // Fire only on editingFinished (Enter pressed or focus leaves the field)
    // so we don't rebuild the Kaiser LPF on every keystroke.
    connect(fmLpfLineEdit, &QLineEdit::editingFinished, this, [this]() {
        bool ok;
        double hz = fmLpfLineEdit->text().toDouble(&ok);
        if (ok) emit fmLpfChanged(hz);
    });
    // FM pre-demod IQ decimation factor (M). 1 = off; M>1 routes the chain
    // through a polyphase decimator → freqdem at Fs/M → LPF at Fs/M → hold.
    fmPredemodDecimSpinBox = new QSpinBox(widget);
    fmPredemodDecimSpinBox->setRange(1, 1000);
    fmPredemodDecimSpinBox->setValue(1);
    fmPredemodDecimSpinBox->setToolTip(tr(
        "Decimate the IQ stream by M before the FM demod (IQEngine pattern). "
        "M=1 disables; M>1 keeps the post-LPF in a numerically clean regime."));
    layout->addRow(new QLabel(tr("FM predemod decim (M):")), fmPredemodDecimSpinBox);
    connect(fmPredemodDecimSpinBox,
            static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &SpectrogramControls::fmPredemodDecimChanged);
    // FM post-demod block-average decimation factor. 1 = disabled.
    fmDecimSpinBox = new QSpinBox(widget);
    fmDecimSpinBox->setRange(1, 4096);
    fmDecimSpinBox->setValue(1);
    layout->addRow(new QLabel(tr("FM decim (N):")), fmDecimSpinBox);
    connect(fmDecimSpinBox, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &SpectrogramControls::fmDecimChanged);
    // FM amplitude squelch: blank the discriminator output where the carrier
    // amplitude is below this % of the window peak, so noise in the gaps
    // between bursts stops dominating the FM trace's autoscale. 0 = off.
    fmSquelchSpinBox = new QSpinBox(widget);
    fmSquelchSpinBox->setRange(0, 100);
    fmSquelchSpinBox->setValue(0);
    fmSquelchSpinBox->setSuffix("%");
    fmSquelchSpinBox->setToolTip(tr(
        "Blank the FM trace where the carrier amplitude |IQ| is below this "
        "fraction of the window peak, so receiver noise between bursts doesn't "
        "blow out the y-axis. 0 disables."));
    layout->addRow(new QLabel(tr("FM squelch:")), fmSquelchSpinBox);
    connect(fmSquelchSpinBox, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &SpectrogramControls::fmSquelchChanged);

    // Auto-tune button: ask PlotView to pick reasonable values for cutoff,
    // predemod M, and post N. PlotView computes from current Fs and tuner
    // bandwidth, applies via the existing setters, and echoes the values
    // back via applyAutoLpf so these widgets stay in sync.
    fmAutoLpfButton = new QPushButton(tr("Auto-tune FM LPF"), widget);
    fmAutoLpfButton->setToolTip(tr(
        "Pick reasonable values for FM LPF cutoff, predemod decimation (M), "
        "and post decimation (N) from the current sample rate and tuner "
        "bandwidth."));
    layout->addRow(fmAutoLpfButton);
    connect(fmAutoLpfButton, &QPushButton::clicked,
            this, &SpectrogramControls::autoLpfRequested);

    // Symbol rate (baud) for the FSK polar plot's differential delay. The
    // delay is round(Fs / baud) ≈ one symbol period — the delay at which a
    // differential constellation collapses to clusters. 0 = unset (the polar
    // plot hides the scatter and prompts instead of drawing a meaningless
    // smear). Decoupled from the FM LPF cutoff on purpose.
    symbolRateLineEdit = new QLineEdit(widget);
    auto symbolRateValidator = new QDoubleValidator(0.0, 1e9, 3, this);
    symbolRateLineEdit->setValidator(symbolRateValidator);
    symbolRateLineEdit->setText("0");
    symbolRateLineEdit->setToolTip(tr(
        "Symbol rate (baud) for the FSK polar plot. Sets the differential "
        "delay to one symbol period; 0 hides the constellation."));
    layout->addRow(new QLabel(tr("Symbol rate (Bd):")), symbolRateLineEdit);
    connect(symbolRateLineEdit, &QLineEdit::editingFinished, this, [this]() {
        bool ok;
        double baud = symbolRateLineEdit->text().toDouble(&ok);
        if (ok) emit symbolRateChanged(baud);
    });
    // Copy the baud measured by the cursor selection (Symbols ÷ selection
    // length) into the field and apply it — the manual "estimate" path.
    useMeasuredBaudButton = new QPushButton(tr("Use measured baud"), widget);
    useMeasuredBaudButton->setToolTip(tr(
        "Copy the symbol rate measured by the cursor selection into the field "
        "above. Set Symbols to the number of symbols spanned, drag the cursors "
        "across them, then click."));
    layout->addRow(useMeasuredBaudButton);
    connect(useMeasuredBaudButton, &QPushButton::clicked, this, [this]() {
        if (lastMeasuredSymbolRate_ > 0.0) {
            symbolRateLineEdit->setText(QString::number(lastMeasuredSymbolRate_, 'f', 3));
            emit symbolRateChanged(lastMeasuredSymbolRate_);
        }
    });

    // Signal-strength gate for the FSK polar constellation: only samples whose
    // differential magnitude exceeds this % of the window peak are plotted, so
    // noise/dead-air between bursts stays off the scope.
    constellationGateSpinBox = new QSpinBox(widget);
    constellationGateSpinBox->setRange(0, 100);
    constellationGateSpinBox->setValue(15);
    constellationGateSpinBox->setSuffix("%");
    constellationGateSpinBox->setToolTip(tr(
        "Drop samples below this fraction of the window's peak level from the "
        "FSK polar plot, so only strong (in-burst) parts form the constellation. "
        "0 disables the gate."));
    layout->addRow(new QLabel(tr("Constellation min level:")), constellationGateSpinBox);
    connect(constellationGateSpinBox, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &SpectrogramControls::constellationGateChanged);

    // Symbol-timed constellation: resample to one point per symbol (using the
    // symbol rate above) so the inter-symbol trajectory spokes collapse into
    // clean decision-point clusters. Off shows the full-rate trajectory.
    constellationSymbolTimedCheckBox = new QCheckBox(widget);
    constellationSymbolTimedCheckBox->setChecked(true);
    constellationSymbolTimedCheckBox->setToolTip(tr(
        "Sample the FSK polar plot once per symbol (timing recovered) so the "
        "constellation shows decision-point clusters instead of the full-rate "
        "trajectory. Needs a valid symbol rate."));
    layout->addRow(new QLabel(tr("Constellation symbol-timed:")), constellationSymbolTimedCheckBox);
    connect(constellationSymbolTimedCheckBox, &QCheckBox::toggled,
            this, &SpectrogramControls::constellationSymbolTimedChanged);

    widget->setLayout(layout);
    setWidget(widget);

    connect(fftSizeSlider, &QSlider::valueChanged, this, &SpectrogramControls::fftSizeChanged);
    connect(zoomLevelSlider, &QSlider::valueChanged, this, &SpectrogramControls::zoomLevelChanged);
    connect(fileOpenButton, &QPushButton::clicked, this, &SpectrogramControls::fileOpenButtonClicked);
    connect(cursorsCheckBox, &QCheckBox::stateChanged, this, &SpectrogramControls::cursorsStateChanged);
    connect(powerMinSlider, &QSlider::valueChanged, this, &SpectrogramControls::powerMinChanged);
    connect(powerMaxSlider, &QSlider::valueChanged, this, &SpectrogramControls::powerMaxChanged);
}

void SpectrogramControls::clearCursorLabels()
{
    periodLabel->setText("");
    rateLabel->setText("");
    symbolPeriodLabel->setText("");
    symbolRateLabel->setText("");
}

void SpectrogramControls::cursorsStateChanged(int state)
{
    if (state == Qt::Unchecked) {
        clearCursorLabels();
    }
}

void SpectrogramControls::setDefaults()
{
    fftOrZoomChanged();

    cursorsCheckBox->setCheckState(Qt::Unchecked);
    cursorSymbolsSpinBox->setValue(1);

    annosCheckBox->setCheckState(Qt::Checked);
    annoLabelCheckBox->setCheckState(Qt::Checked);
    annoColorCheckBox->setCheckState(Qt::Checked);

    // Try to set the sample rate from the last-used value. Sanity-check the
    // loaded value: a missing/zero setting means "no useful rate was ever
    // saved", in which case fall back to 8 MHz rather than starting at 0
    // (FrequencyDemod skips its Hz scaling at rate=0, but downstream UI
    // — time scale, hover, period analyser — are all degraded with rate=0,
    // and an obvious sane default lets the user see something instead of a
    // flat trace).
    QSettings settings;
    int savedSampleRate = settings.value("SampleRate", 8000000).toInt();
    if (savedSampleRate <= 0) savedSampleRate = 8000000;
    sampleRate->setText(QString::number(savedSampleRate));
    fftSizeSlider->setValue(settings.value("FFTSize", 9).toInt());
    powerMaxSlider->setValue(settings.value("PowerMax", 0).toInt());
    powerMinSlider->setValue(settings.value("PowerMin", -100).toInt());
    zoomLevelSlider->setValue(settings.value("ZoomLevel", 0).toInt());
}

void SpectrogramControls::fftOrZoomChanged(void)
{
    int fftSize = pow(2, fftSizeSlider->value());
    int zoomLevel = zoomLevelSlider->value();
    if (zoomLevel >= 0)
        // zooming in by power-of-two steps
        zoomLevel = std::min(fftSize, (int)pow(2, zoomLevel));
    else
        // zooming out (skipping FFTs) by power-of-two steps
        zoomLevel = -1*std::min(fftSize, (int)pow(2, -1*zoomLevel));
    emit fftOrZoomChanged(fftSize, zoomLevel);
}

void SpectrogramControls::fftSizeChanged(int value)
{
    QSettings settings;
    settings.setValue("FFTSize", value);
    fftOrZoomChanged();
}

void SpectrogramControls::zoomLevelChanged(int value)
{
    QSettings settings;
    settings.setValue("ZoomLevel", value);
    fftOrZoomChanged();
}

void SpectrogramControls::powerMinChanged(int value)
{
    QSettings settings;
    settings.setValue("PowerMin", value);
}

void SpectrogramControls::powerMaxChanged(int value)
{
    QSettings settings;
    settings.setValue("PowerMax", value);
}

void SpectrogramControls::fileOpenButtonClicked()
{
    QSettings settings;
    QString fileName;
    QFileDialog fileSelect(this);
    fileSelect.setNameFilter(tr("All files (*);;"
                "complex<float> file (*.cfile *.cf32 *.fc32);;"
                "complex<int8> HackRF file (*.cs8 *.sc8 *.c8);;"
                "complex<int16> Fancy file (*.cs16 *.sc16 *.c16);;"
                "complex<uint8> RTL-SDR file (*.cu8 *.uc8);;"
                "Rohde & Schwarz iq.tar (*.iq.tar)"));

    // Try and load a saved state
    {
        QByteArray savedState = settings.value("OpenFileState").toByteArray();
        fileSelect.restoreState(savedState);

        // Filter doesn't seem to be considered part of the saved state
        QString lastUsedFilter = settings.value("OpenFileFilter").toString();
        if(lastUsedFilter.size())
            fileSelect.selectNameFilter(lastUsedFilter);
    }

    if(fileSelect.exec())
    {
        fileName = fileSelect.selectedFiles()[0];

        // Remember the state of the dialog for the next time
        QByteArray dialogState = fileSelect.saveState();
        settings.setValue("OpenFileState", dialogState);
        settings.setValue("OpenFileFilter", fileSelect.selectedNameFilter());
    }

    if (!fileName.isEmpty())
        emit openFile(fileName);
}

void SpectrogramControls::timeSelectionChanged(float time)
{
    if (cursorsCheckBox->checkState() == Qt::Checked) {
        periodLabel->setText(QString::fromStdString(formatSIValue(time)) + "s");
        rateLabel->setText(QString::fromStdString(formatSIValue(1 / time)) + "Hz");

        int symbols = cursorSymbolsSpinBox->value();
        // Remember the raw baud so "Use measured baud" can push it to the FSK
        // polar plot without re-parsing the SI-formatted label text.
        lastMeasuredSymbolRate_ = (time > 0.0f) ? (symbols / time) : 0.0;
        symbolPeriodLabel->setText(QString::fromStdString(formatSIValue(time / symbols)) + "s");
        symbolRateLabel->setText(QString::fromStdString(formatSIValue(symbols / time)) + "Bd");
    }
}

void SpectrogramControls::zoomIn()
{
    zoomLevelSlider->setValue(zoomLevelSlider->value() + 1);
}

void SpectrogramControls::zoomOut()
{
    zoomLevelSlider->setValue(zoomLevelSlider->value() - 1);
}

void SpectrogramControls::enableAnnotations(bool enabled) {
    // disable annotation comments checkbox when annotations are disabled
    commentsCheckBox->setEnabled(enabled);
}

void SpectrogramControls::setAnnotationsDirty(bool dirty)
{
    saveAnnotationsButton->setEnabled(dirty);
    saveAnnotationsButton->setText(dirty ? tr("Save annotations *") : tr("Save annotations"));
}

void SpectrogramControls::applyAutoLpf(double cutoffHz, int predemodM, int postN)
{
    // Mirror the PlotView-computed values back into the widgets. The
    // QSpinBox setValue calls fire valueChanged → re-apply via PlotView's
    // setters; that's idempotent (the setter sees the same value and is
    // a no-op). For the QLineEdit we have to emit fmLpfChanged manually
    // because editingFinished doesn't fire on programmatic setText().
    fmLpfLineEdit->setText(QString::number(cutoffHz, 'f', 0));
    emit fmLpfChanged(cutoffHz);
    fmPredemodDecimSpinBox->setValue(predemodM);
    fmDecimSpinBox->setValue(postN);
}

void SpectrogramControls::applyAutoPeriod(double periodSeconds)
{
    if (periodSeconds <= 0.0 || !std::isfinite(periodSeconds)) {
        autoPeriodLabel->setText(QStringLiteral("—"));
        return;
    }
    const double freq = 1.0 / periodSeconds;
    autoPeriodLabel->setText(
        QString::fromStdString(formatSIValue(periodSeconds)) + QStringLiteral("s  (")
        + QString::fromStdString(formatSIValue(freq)) + QStringLiteral("Hz)"));
}

void SpectrogramControls::applyCursorValue(QString text)
{
    cursorValueLabel->setText(text.isEmpty() ? QStringLiteral("—") : text);
}
