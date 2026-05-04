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

#include <QCache>
#include <QString>
#include <QWidget>
#include "fft.h"
#include "inputsource.h"
#include "plot.h"
#include "tuner.h"
#include "tunertransform.h"

#include <memory>
#include <array>
#include <limits>
#include <math.h>
#include <vector>

class TileCacheKey;
class AnnotationLocation;

// Render mode for the top spectrogram. Standard = |STFT|² with the existing
// per-frame FFT path. Reassigned = Fulop-Fitz reassignment: three FFTs per
// frame (analysis window h, time-weighted t·h, and derivative h') used to
// move each bin's energy to its local centre of mass (t̂, ω̂). See
// SpectrogramPlot::computeReassignedTile() for the maths.
enum class SpectrogramMode {
    Standard = 0,
    Reassigned = 1,
};

class SpectrogramPlot : public Plot
{
    Q_OBJECT

public:
    SpectrogramPlot(std::shared_ptr<SampleSource<std::complex<float>>> src);
    void invalidateEvent() override;
    std::shared_ptr<AbstractSampleSource> output() override;
    void paintFront(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    void paintMid(QPainter &painter, QRect &rect, range_t<size_t> sampleRange) override;
    bool mouseEvent(QEvent::Type type, QMouseEvent event) override;
    std::shared_ptr<SampleSource<std::complex<float>>> input() { return inputSource; };
    void setSampleRate(double sampleRate);
    bool tunerEnabled();
    // Move the tuner so its centre frequency matches the given y-coordinate
    // in plot pixels (top of plot = 0, bottom = height()). Used by the right-
    // click "Add derived plot" menu so the new plot tunes to where the user
    // clicked instead of leaving the tuner at its previous position.
    void setTunerCentreY(int y);
    void enableScales(bool enabled);
    void enableAnnotations(bool enabled);
    void enableAnnoLabels(bool enabled);
    bool isAnnotationsEnabled();
    void enableAnnoColors(bool enabled);
    QString *mouseAnnotationComment(const QMouseEvent *event);

public slots:
    void setFFTSize(int size);
    void setPowerMax(int power);
    void setPowerMin(int power);
    void setZoomLevel(int zoom);
    void setSkip(int skip);
    void tunerMoved();
    // Switch between the standard |STFT|² spectrogram and the Fulop-Fitz
    // reassigned spectrogram. Only the rendering path changes; FFT size,
    // zoom, and tuner state are preserved.
    void setSpectrogramMode(int mode);
    // Per-bin power floor (dB) below which reassignment is skipped — those
    // bins are rendered at their original (t,ω) so the noise floor still
    // shows up but isn't smeared by meaningless reassignment vectors.
    void setReassignmentFloor(int floorDb);

private:
    const int linesPerGraduation = 50;
    static const int tileSize = 65536; // This must be a multiple of the maximum FFT size

    std::shared_ptr<SampleSource<std::complex<float>>> inputSource;
    std::vector<AnnotationLocation> visibleAnnotationLocations;
    std::unique_ptr<FFT> fft;
    // Reassignment companions to `fft` — same size, separate FFTW plans so
    // the three transforms per frame don't trash each other's buffers.
    std::unique_ptr<FFT> fftTimeWeighted;
    std::unique_ptr<FFT> fftDerivative;
    std::unique_ptr<float[]> window;
    // Time-weighted (t·h(n), centred t = n - (N-1)/2) and derivative (h'(n),
    // closed form for Hann) windows used by the reassignment path. Allocated
    // and filled in setFFTSize() alongside the analysis window.
    std::unique_ptr<float[]> windowTimeWeighted;
    std::unique_ptr<float[]> windowDerivative;
    QCache<TileCacheKey, QPixmap> pixmapCache;
    QCache<TileCacheKey, std::array<float, tileSize>> fftCache;
    uint colormap[256];

    int fftSize;
    int zoomLevel;
    int nfftSkip;
    float powerMax;
    float powerMin;
    double sampleRate;
    bool frequencyScaleEnabled;
    bool sigmfAnnotationsEnabled;
    bool sigmfAnnotationLabels;
    bool sigmfAnnotationColors;
    SpectrogramMode mode = SpectrogramMode::Standard;
    // Bins below this power (dB, same scale as powerMax/powerMin) are not
    // reassigned. Default -80 dB matches the Auger-Flandrin recommendation
    // for visualisation and keeps noise speckle out of the reassigned image.
    int reassignmentFloorDb = -80;

    Tuner tuner;
    std::shared_ptr<TunerTransform> tunerTransform;

    // Tap-design memo: liquid_firdes_kaiser is the dominant per-frame cost
    // during a tuner drag (an O(N²) Bessel evaluation for narrow cutoffs)
    // but only depends on the inputs below — pure centre-frequency drags
    // don't change them, so we skip the redesign entirely when nothing's
    // moved. -1 sentinel forces a fresh design on the first call.
    int   lastTapsDeviation_ = -1;
    int   lastTapsFftSize_   = -1;
    float lastTapsPowerMax_  = std::numeric_limits<float>::quiet_NaN();
    // Track the (frequency, deviation) the downstream chain was last told
    // about so we can skip the invalidate fan-out when neither actually
    // moved. Prevents Tuner::tunerMoved emits with no real change (e.g.
    // mouse release within the same pixel after a drag) from triggering
    // a full demod re-render and worker churn.
    float lastNotifiedFrequency_ = std::numeric_limits<float>::quiet_NaN();
    int   lastNotifiedDeviation_ = -1;

    QPixmap* getPixmapTile(size_t tile);
    float* getFFTTile(size_t tile);
    void getLine(float *dest, size_t sample);
    // Compute one full reassigned tile: zero-init the destination, then for
    // each frame run three FFTs (h, t·h, h'), compute (t̂, ω̂) per bin and
    // bilinearly splat |X_h|² into the accumulator. Result is converted to
    // dB so the colormap stage stays unchanged.
    void computeReassignedTile(float *dest, size_t tile);
    int getStride();
    float getTunerPhaseInc();
    std::vector<float> getTunerTaps();
    int linesPerTile();
    void paintFrequencyScale(QPainter &painter, QRect &rect);
    void paintAnnotations(QPainter &painter, QRect &rect, range_t<size_t> sampleRange);
};

class TileCacheKey
{

public:
    TileCacheKey(int fftSize, int zoomLevel, int nfftSkip, size_t sample,
                 SpectrogramMode mode = SpectrogramMode::Standard,
                 int reassignmentFloorDb = 0) {
        this->fftSize = fftSize;
        this->zoomLevel = zoomLevel;
        this->nfftSkip = nfftSkip;
        this->sample = sample;
        this->mode = mode;
        // Threshold only affects reassigned output — collapse to 0 for
        // Standard tiles so changing the slider doesn't invalidate the
        // standard cache.
        this->reassignmentFloorDb =
            (mode == SpectrogramMode::Reassigned) ? reassignmentFloorDb : 0;
    }

    bool operator==(const TileCacheKey &k2) const {
        return (this->fftSize == k2.fftSize) &&
               (this->zoomLevel == k2.zoomLevel) &&
               (this->nfftSkip == k2.nfftSkip) &&
               (this->sample == k2.sample) &&
               (this->mode == k2.mode) &&
               (this->reassignmentFloorDb == k2.reassignmentFloorDb);
    }

    int fftSize;
    int zoomLevel;
    int nfftSkip;
    size_t sample;
    SpectrogramMode mode;
    int reassignmentFloorDb;
};

class AnnotationLocation
{
public:
    Annotation annotation;

    AnnotationLocation(Annotation annotation, int x, int y, int width, int height)
        : annotation(annotation), x(x), y(y), width(width), height(height) {}

    bool isInside(int pos_x, int pos_y) {
        return (x <= pos_x) && (pos_x <= x + width)
            && (y <= pos_y) && (pos_y <= y + height);
    }

private:
    int x;
    int y;
    int width;
    int height;
};
