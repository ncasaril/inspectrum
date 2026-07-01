/*
 *  Copyright (C) 2026, Niklas Casaril <niklas@casaril.com>
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

#include <QObject>
#include <QByteArray>
#include <QFutureWatcher>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <QJsonValue>
#include <atomic>
#include <complex>
#include <memory>
#include <vector>
#include "samplesource.h"

class QProcess;
class QTimer;
class QTemporaryDir;

// Result of the (worker-thread) segment extraction.
struct SegmentExtract {
    bool ok = false;
    bool canceled = false;
    QString metaPath;
    QString error;
};

// One declared parameter of a plugin (drives the auto-generated param dialog).
struct PluginParam {
    QString key;            // JSON key handed to the plugin in custom_params
    QString type;           // "float" | "int" | "bool" | "string" | "enum"
    QString label;          // dialog label (falls back to key)
    QJsonValue defaultValue;
    QStringList choices;    // for type == "enum"
    // Optional numeric bounds / precision for int/float widgets. When a bound is
    // absent the dialog uses a wide default; declaring them avoids silent clamping
    // or rounding of out-of-range / small-magnitude values.
    bool hasMin = false;
    bool hasMax = false;
    double minValue = 0.0;
    double maxValue = 0.0;
    int decimals = 6;       // float precision
};

// A discovered plugin manifest (~/.config/inspectrum/plugins/*.json).
struct PluginManifest {
    QString name;
    QString exec;           // absolute path or PATH-resolvable executable
    QStringList args;       // fixed args prepended before the meta-file path
    QString sampleType;     // accepted input sample type, e.g. "cf32"
    QVector<PluginParam> params;
    QString path;           // manifest file path (for diagnostics)
    bool valid = false;
    QString error;          // why it's invalid, if !valid
};

// Directory plugins are discovered from: ~/.config/inspectrum/plugins
QString pluginDirectory();

// Parse a single manifest blob. Always returns a manifest; check .valid/.error.
PluginManifest parseManifest(const QByteArray &json, const QString &path);

// Discover and parse every *.json manifest in pluginDirectory().
QVector<PluginManifest> discoverPlugins();

// Write a temporary SigMF segment (cf32_le .sigmf-data + .sigmf-meta) for samples
// [start, start+count) pulled from `src`. The meta carries core:sample_rate and
// captures[0].core:frequency = centerFreq (absolute Hz). Returns false + *errorOut
// on failure. metaPathOut/dataPathOut may be null. Safe to call off the GUI thread.
// If `cancel` is non-null and becomes true, the write aborts between chunks,
// removes the partial data file, and returns false with *errorOut == "canceled".
bool writeSegmentSigmf(const QString &dir,
                       SampleSource<std::complex<float>> *src,
                       size_t start, size_t count,
                       double sampleRate, double centerFreq,
                       QString *metaPathOut, QString *dataPathOut,
                       QString *errorOut,
                       const std::atomic<bool> *cancel = nullptr);

// Parse an IQEngine-style { "annotations": [...] } blob into inspectrum Annotations,
// mapping segment-local sample indices to absolute file indices (abs = segStart +
// local; inclusive max = abs + count - 1). Frequency edges are absolute Hz and pass
// through; when omitted, [passLo, passHi] (the tuner pass-band) is substituted.
//
// segCount is the number of samples in the extracted segment: returned indices are
// validated and clamped against it, so a buggy or hostile plugin cannot produce an
// out-of-range / inverted range or trigger an out-of-range double->size_t cast.
// Annotations whose start falls outside the segment, or with count < 1, are skipped.
// Returns the parsed annotations; on malformed JSON returns empty and sets *errorOut.
std::vector<Annotation> parsePluginAnnotations(const QByteArray &json,
                                               size_t segStart, size_t segCount,
                                               double passLo, double passHi,
                                               QString *errorOut);

// Runs a plugin over an extracted segment as an async child process. One run at a
// time per instance (busy() guards — stays true across a cancelled extraction until
// its worker is joined). Emits finished() with mapped annotations or failed() with a
// human-readable error; exactly one is emitted per run(), and neither on cancel.
class PluginRunner : public QObject
{
    Q_OBJECT

public:
    explicit PluginRunner(QObject *parent = nullptr);
    ~PluginRunner() override;

    // Extract [start,count) from src, write the temp segment, and launch the plugin.
    // centerFreq/passLo/passHi are absolute Hz. customParams is forwarded verbatim as
    // the context.json "custom_params" object. Errors before launch are reported via
    // failed(). timeoutMs <= 0 disables the timeout.
    void run(const PluginManifest &manifest,
             std::shared_ptr<SampleSource<std::complex<float>>> src,
             size_t start, size_t count,
             double sampleRate, double centerFreq,
             double passLo, double passHi,
             const QJsonObject &customParams,
             int timeoutMs = 120000);

    // busy() (not running_) is the single-flight guard: running_ goes false the
    // instant cancel() is called, but during an extraction cancel the worker thread
    // is still alive until onExtractFinished() joins it. busy() stays true across
    // that window so callers can't start a new run and stomp the in-flight one.
    bool busy() const { return running_ || canceling_; }

public slots:
    // Kill the process if running; no signal is emitted for a user cancel.
    void cancel();

signals:
    void finished(std::vector<Annotation> annotations);
    void failed(QString error);

private slots:
    void onExtractFinished();
    void onProcFinished(int exitCode, int exitStatus);
    void onProcErrorOccurred();
    void onReadyStdout();
    void onReadyStderr();
    void onTimeout();

private:
    void cleanup();
    void fail(const QString &error);
    void launchProcess(const QString &metaPath);

    QProcess *proc_ = nullptr;
    QTimer *timeoutTimer_ = nullptr;
    std::unique_ptr<QTemporaryDir> tmpDir_;
    QFutureWatcher<SegmentExtract> *extractWatcher_ = nullptr;
    std::atomic<bool> extractCancel_{false};
    bool running_ = false;
    bool canceling_ = false;      // cancel requested while extraction is in flight
    int timeoutMs_ = 0;
    size_t segStart_ = 0;
    size_t segCount_ = 0;
    double passLo_ = 0.0;
    double passHi_ = 0.0;
    // Stashed at run() time; used to launch the process once extraction completes.
    QString exec_;
    QStringList args_;
    QByteArray contextJson_;
    // Incrementally accumulated child output, capped so a runaway plugin can't
    // exhaust the GUI process's memory.
    QByteArray outBuf_;
    QByteArray errBuf_;
};
