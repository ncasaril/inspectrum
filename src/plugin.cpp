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

#include "plugin.h"

#include <QColor>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>

// Bound how much child output we accumulate so a runaway/verbose plugin cannot
// exhaust the GUI process's memory, and how many annotations one run may add.
static const int kMaxStdoutBytes = 64 * 1024 * 1024;
static const int kMaxStderrBytes = 1 * 1024 * 1024;
static const size_t kMaxAnnotations = 100000;

namespace {

// Default colour for machine-detected annotations: a distinct cyan so they read as
// plugin output until the user edits them. Annotations with an explicit
// presentation:color override this.
const QColor kDetectedColor(0, 200, 255, 180);

// Convert a SigMF "#RRGGBBAA" colour to a QColor (Qt wants "#AARRGGBB"). Returns an
// invalid QColor when the string isn't a valid colour, mirroring inputsource.cpp.
QColor parseSigmfColor(const QString &s)
{
    QString c = s;
    if ((c.length() == 9) && (c.at(0) == '#'))
        c = "#" + c.mid(7, 2) + c.mid(1, 6);
    if (QColor::isValidColor(c))
        return QColor(c);
    return QColor();
}

} // namespace

QString pluginDirectory()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty())
        base = QDir::homePath() + "/.config";
    return base + "/inspectrum/plugins";
}

PluginManifest parseManifest(const QByteArray &json, const QString &path)
{
    PluginManifest m;
    m.path = path;

    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        m.error = QString("invalid JSON: %1").arg(perr.errorString());
        return m;
    }
    QJsonObject root = doc.object();

    m.name = root["name"].toString();
    m.exec = root["exec"].toString();
    m.sampleType = root["sample_type"].toString("cf32");

    if (m.name.isEmpty()) {
        m.error = "manifest missing \"name\"";
        return m;
    }
    if (m.exec.isEmpty()) {
        m.error = "manifest missing \"exec\"";
        return m;
    }
    // The child is launched with its working directory set to a fresh temp dir, so a
    // relative path containing a separator (e.g. "./plugin.py") would resolve against
    // that empty dir and fail. Resolve such a path against the manifest's own
    // directory so a script can be colocated with its manifest. Absolute paths and
    // bare PATH-resolvable names are left untouched.
    if (QDir::isRelativePath(m.exec) && m.exec.contains('/'))
        m.exec = QFileInfo(path).absoluteDir().absoluteFilePath(m.exec);

    for (const auto &a : root["args"].toArray())
        m.args << a.toString();

    QSet<QString> seenKeys;
    for (const auto &pv : root["params"].toArray()) {
        QJsonObject po = pv.toObject();
        PluginParam p;
        p.key = po["key"].toString();
        if (p.key.isEmpty())
            continue; // a param with no key is useless; skip it
        if (seenKeys.contains(p.key)) {
            // Duplicate keys would collide in the emitted custom_params object
            // (last value wins); skip the later one and warn rather than confuse.
            qWarning() << "inspectrum: plugin manifest" << path
                       << "has duplicate param key" << p.key << "- ignoring the later one";
            continue;
        }
        seenKeys.insert(p.key);
        p.type = po["type"].toString("string");
        p.label = po["label"].toString(p.key);
        p.defaultValue = po["default"];
        for (const auto &c : po["choices"].toArray())
            p.choices << c.toString();
        if (po.contains("min")) { p.hasMin = true; p.minValue = po["min"].toDouble(); }
        if (po.contains("max")) { p.hasMax = true; p.maxValue = po["max"].toDouble(); }
        p.decimals = po["decimals"].toInt(6);
        m.params.push_back(p);
    }

    m.valid = true;
    return m;
}

QVector<PluginManifest> discoverPlugins()
{
    QVector<PluginManifest> result;
    QDir dir(pluginDirectory());
    if (!dir.exists())
        return result;

    const auto entries = dir.entryList(QStringList() << "*.json", QDir::Files, QDir::Name);
    for (const QString &name : entries) {
        const QString path = dir.absoluteFilePath(name);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning() << "inspectrum: cannot read plugin manifest" << path;
            continue;
        }
        PluginManifest m = parseManifest(f.readAll(), path);
        if (!m.valid)
            qWarning() << "inspectrum: skipping plugin manifest" << path << ":" << m.error;
        result.push_back(m);
    }
    return result;
}

bool writeSegmentSigmf(const QString &dir,
                       SampleSource<std::complex<float>> *src,
                       size_t start, size_t count,
                       double sampleRate, double centerFreq,
                       QString *metaPathOut, QString *dataPathOut,
                       QString *errorOut,
                       const std::atomic<bool> *cancel)
{
    auto setErr = [&](const QString &e) { if (errorOut) *errorOut = e; };

    if (src == nullptr) {
        setErr("no sample source");
        return false;
    }
    if (count == 0) {
        setErr("empty segment");
        return false;
    }

    const QString dataName = "segment.sigmf-data";
    const QString metaName = "segment.sigmf-meta";
    const QString dataPath = QDir(dir).absoluteFilePath(dataName);
    const QString metaPath = QDir(dir).absoluteFilePath(metaName);

    // Write the cf32 IQ. std::complex<float> is two contiguous little-endian float32
    // (I then Q), which is exactly the cf32_le on-disk layout inspectrum reads, so we
    // can blit the buffer bytes directly. Pull in chunks to bound memory.
    {
        QFile data(dataPath);
        if (!data.open(QIODevice::WriteOnly)) {
            setErr(QString("cannot open %1 for writing").arg(dataPath));
            return false;
        }
        const size_t chunk = 1u << 20; // 1 Msample = 8 MiB per pull
        for (size_t off = 0; off < count; off += chunk) {
            if (cancel && cancel->load()) {
                setErr("canceled");
                data.close();
                data.remove();
                return false;
            }
            const size_t n = std::min(chunk, count - off);
            auto buf = src->getSamples(start + off, n);
            if (!buf) {
                setErr("sample source returned no data (out of range?)");
                data.close();
                data.remove();
                return false;
            }
            const char *bytes = reinterpret_cast<const char *>(buf.get());
            qint64 want = (qint64)(n * sizeof(std::complex<float>));
            if (data.write(bytes, want) != want) {
                setErr(QString("short write to %1").arg(dataPath));
                data.close();
                data.remove();
                return false;
            }
        }
    }

    // Write the matching .sigmf-meta (global + one capture carrying the absolute
    // centre frequency + an empty annotations array).
    {
        QJsonObject global;
        global.insert("core:datatype", QStringLiteral("cf32_le"));
        if (sampleRate > 0.0)
            global.insert("core:sample_rate", sampleRate);
        global.insert("core:version", QStringLiteral("1.0.0"));
        global.insert("core:dataset", dataName);
        global.insert("core:description",
                      QStringLiteral("Filtered segment extracted by inspectrum"));

        QJsonObject capture;
        capture.insert("core:sample_start", (qint64)0);
        capture.insert("core:frequency", centerFreq);
        QJsonArray captures;
        captures.append(capture);

        QJsonObject root;
        root.insert("global", global);
        root.insert("captures", captures);
        root.insert("annotations", QJsonArray());

        QFile meta(metaPath);
        if (!meta.open(QIODevice::WriteOnly)) {
            setErr(QString("cannot open %1 for writing").arg(metaPath));
            return false;
        }
        const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
        if (meta.write(json) != json.size()) {
            setErr(QString("short write to %1").arg(metaPath));
            meta.close();
            meta.remove();
            return false;
        }
    }

    if (metaPathOut) *metaPathOut = metaPath;
    if (dataPathOut) *dataPathOut = dataPath;
    return true;
}

std::vector<Annotation> parsePluginAnnotations(const QByteArray &json,
                                               size_t segStart, size_t segCount,
                                               double passLo, double passHi,
                                               QString *errorOut)
{
    std::vector<Annotation> out;
    if (errorOut) errorOut->clear();

    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut)
            *errorOut = QString("invalid JSON: %1").arg(perr.errorString());
        return out;
    }

    const QJsonArray annotations = doc.object()["annotations"].toArray();
    for (const auto &av : annotations) {
        if (out.size() >= kMaxAnnotations) {
            qWarning() << "inspectrum: plugin returned more than" << (int)kMaxAnnotations
                       << "annotations; ignoring the rest";
            break;
        }
        QJsonObject a = av.toObject();
        // core:sample_start and core:sample_count are required; skip entries missing
        // either rather than failing the whole batch.
        if (!a.contains("core:sample_start") || !a.contains("core:sample_count"))
            continue;
        const double dStart = a["core:sample_start"].toDouble(-1.0);
        const double dCount = a["core:sample_count"].toDouble(-1.0);
        // Validate the DOUBLES against the segment before any cast to size_t: a
        // hostile/buggy plugin can emit huge or fractional values, and an
        // out-of-range double->size_t conversion is undefined behaviour. Bounds:
        // start must land inside the segment, count must be at least one sample.
        if (!(dStart >= 0.0) || dStart >= (double)segCount)
            continue;
        if (!(dCount >= 1.0))
            continue;

        const size_t localStart = (size_t)dStart;       // safe: 0 <= dStart < segCount
        const size_t maxCnt = segCount - localStart;     // samples left in the segment
        // Clamp the count so the inclusive max can neither wrap nor exceed the last
        // valid file sample (segStart + segCount - 1). Comparing the double avoids
        // casting an over-large value.
        const size_t cnt = (dCount >= (double)maxCnt) ? maxCnt : (size_t)dCount;
        const size_t absStart = segStart + localStart;
        const size_t absMax = absStart + cnt - 1; // inclusive, matches sampleRange

        // Frequency edges are absolute Hz. SigMF requires both-or-neither; fall back
        // to the tuner pass-band when either is absent.
        double fLo = passLo, fHi = passHi;
        if (a.contains("core:freq_lower_edge") && a.contains("core:freq_upper_edge")) {
            fLo = a["core:freq_lower_edge"].toDouble();
            fHi = a["core:freq_upper_edge"].toDouble();
        }
        if (fHi < fLo)
            std::swap(fLo, fHi);

        QColor color = parseSigmfColor(a["presentation:color"].toString());
        if (!color.isValid())
            color = kDetectedColor;

        Annotation ann;
        ann.sampleRange = { absStart, absMax };
        ann.frequencyRange = { fLo, fHi };
        ann.label = a["core:label"].toString();
        ann.description = a["core:description"].toString();
        ann.comment = a["core:comment"].toString();
        ann.boxColor = color;
        out.push_back(ann);
    }

    return out;
}

// ---------------------------------------------------------------------------

PluginRunner::PluginRunner(QObject *parent) : QObject(parent) {}

PluginRunner::~PluginRunner()
{
    cleanup();
}

void PluginRunner::run(const PluginManifest &manifest,
                       std::shared_ptr<SampleSource<std::complex<float>>> src,
                       size_t start, size_t count,
                       double sampleRate, double centerFreq,
                       double passLo, double passHi,
                       const QJsonObject &customParams,
                       int timeoutMs)
{
    if (running_) {
        emit failed("a plugin is already running");
        return;
    }
    if (!src) {
        emit failed("no sample source available");
        return;
    }

    const size_t total = src->count();
    if (start >= total) {
        emit failed("selection starts past the end of the file");
        return;
    }
    if (start + count > total)
        count = total - start;
    if (count == 0) {
        emit failed("empty selection");
        return;
    }

    running_ = true;
    canceling_ = false;
    extractCancel_ = false;
    outBuf_.clear();
    errBuf_.clear();
    segStart_ = start;
    segCount_ = count;   // already clamped to [start, total) above
    passLo_ = passLo;
    passHi_ = passHi;
    timeoutMs_ = timeoutMs;

    // Stash what launchProcess() needs once extraction completes.
    exec_ = manifest.exec;
    args_ = manifest.args;
    QJsonObject ctx;
    ctx.insert("sample_rate", sampleRate);
    ctx.insert("center_freq", centerFreq);
    ctx.insert("custom_params", customParams);
    contextJson_ = QJsonDocument(ctx).toJson(QJsonDocument::Compact);

    tmpDir_.reset(new QTemporaryDir());
    if (!tmpDir_->isValid()) {
        fail("could not create a temporary directory for the segment");
        return;
    }

    // Extract the segment on a worker thread so the GUI event loop keeps running
    // (the busy dialog repaints, Cancel works) even for a whole-file (100GB+) scope.
    const QString dir = tmpDir_->path();
    std::atomic<bool> *cancelPtr = &extractCancel_;
    extractWatcher_ = new QFutureWatcher<SegmentExtract>(this);
    connect(extractWatcher_, &QFutureWatcher<SegmentExtract>::finished,
            this, &PluginRunner::onExtractFinished);
    extractWatcher_->setFuture(QtConcurrent::run(
        [src, start, count, sampleRate, centerFreq, dir, cancelPtr]() -> SegmentExtract {
            SegmentExtract r;
            QString metaPath, err;
            const bool ok = writeSegmentSigmf(dir, src.get(), start, count,
                                              sampleRate, centerFreq,
                                              &metaPath, nullptr, &err, cancelPtr);
            if (!ok && err == "canceled") {
                r.canceled = true;
                return r;
            }
            r.ok = ok;
            r.metaPath = metaPath;
            r.error = err;
            return r;
        }));
}

void PluginRunner::onExtractFinished()
{
    if (!extractWatcher_)
        return;
    const SegmentExtract r = extractWatcher_->result();

    // Cancelled (or already terminal) while extracting: the worker has returned, so
    // it is now safe to delete the temp dir. Emit nothing for a user cancel.
    if (canceling_ || !running_ || r.canceled) {
        canceling_ = false;
        running_ = false;
        cleanup();
        return;
    }
    if (!r.ok) {
        fail(r.error.isEmpty() ? "failed to write segment" : r.error);
        return;
    }
    launchProcess(r.metaPath);
}

void PluginRunner::launchProcess(const QString &metaPath)
{
    proc_ = new QProcess(this);
    proc_->setWorkingDirectory(tmpDir_->path());

    connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus st) {
                onProcFinished(code, (int)st);
            });
    connect(proc_, &QProcess::errorOccurred,
            this, [this](QProcess::ProcessError) { onProcErrorOccurred(); });
    connect(proc_, &QProcess::readyReadStandardOutput, this, &PluginRunner::onReadyStdout);
    connect(proc_, &QProcess::readyReadStandardError, this, &PluginRunner::onReadyStderr);
    // Feed context.json on stdin once the process is up, then signal EOF.
    connect(proc_, &QProcess::started, this, [this]() {
        if (proc_) {
            proc_->write(contextJson_);
            proc_->closeWriteChannel();
        }
    });

    QStringList args = args_;
    args << metaPath;

    if (timeoutMs_ > 0) {
        timeoutTimer_ = new QTimer(this);
        timeoutTimer_->setSingleShot(true);
        connect(timeoutTimer_, &QTimer::timeout, this, &PluginRunner::onTimeout);
        timeoutTimer_->start(timeoutMs_);
    }

    proc_->start(exec_, args);
}

void PluginRunner::onReadyStdout()
{
    if (!proc_)
        return;
    outBuf_.append(proc_->readAllStandardOutput());
    if (outBuf_.size() > kMaxStdoutBytes) {
        proc_->kill();
        fail(QString("plugin produced too much output (> %1 MiB)")
                 .arg(kMaxStdoutBytes / (1024 * 1024)));
    }
}

void PluginRunner::onReadyStderr()
{
    if (!proc_)
        return;
    errBuf_.append(proc_->readAllStandardError());
    if (errBuf_.size() > kMaxStderrBytes)
        errBuf_.truncate(kMaxStderrBytes); // keep the head, stay bounded
}

void PluginRunner::onProcFinished(int exitCode, int exitStatus)
{
    if (!running_)
        return;

    if (proc_) {
        outBuf_.append(proc_->readAllStandardOutput());
        errBuf_.append(proc_->readAllStandardError());
    }
    const QString errStr = QString::fromUtf8(errBuf_);

    if (exitStatus != (int)QProcess::NormalExit) {
        fail(QString("plugin crashed%1").arg(errStr.isEmpty() ? QString() : ":\n" + errStr));
        return;
    }
    if (exitCode != 0) {
        fail(QString("plugin exited with code %1%2")
                 .arg(exitCode).arg(errStr.isEmpty() ? QString() : ":\n" + errStr));
        return;
    }

    // A clean exit with no stdout is a zero-detection run, not a parse failure.
    if (outBuf_.trimmed().isEmpty()) {
        running_ = false;
        cleanup();
        emit finished({});
        return;
    }

    QString perr;
    std::vector<Annotation> annos =
        parsePluginAnnotations(outBuf_, segStart_, segCount_, passLo_, passHi_, &perr);
    if (!perr.isEmpty()) {
        fail(QString("could not parse plugin output: %1%2")
                 .arg(perr).arg(errStr.isEmpty() ? QString() : "\n" + errStr));
        return;
    }

    running_ = false;
    cleanup();
    emit finished(annos);
}

void PluginRunner::onProcErrorOccurred()
{
    if (!running_ || !proc_)
        return;
    // A failed launch never produces finished(); other errors (e.g. Crashed) do, so
    // we only own FailedToStart here.
    if (proc_->error() == QProcess::FailedToStart)
        fail(QString("failed to start plugin: %1").arg(proc_->errorString()));
}

void PluginRunner::onTimeout()
{
    if (!running_)
        return;
    if (proc_)
        proc_->kill();
    fail("plugin timed out");
}

void PluginRunner::cancel()
{
    if (!running_)
        return;
    extractCancel_ = true; // ask any in-flight extraction worker to stop
    if (proc_) {
        // Process phase: killing it + cleanup() (which disconnects before deleteLater)
        // suppresses the finished()/failed() the kill would otherwise trigger.
        running_ = false;
        proc_->kill();
        cleanup();
    } else {
        // Extraction phase: the worker may still be writing tmpDir_, so we cannot
        // delete it yet. Mark cancelling and let onExtractFinished() tidy up once the
        // worker observes extractCancel_ and returns.
        canceling_ = true;
        running_ = false;
    }
}

void PluginRunner::fail(const QString &error)
{
    if (!running_)
        return;
    running_ = false;
    canceling_ = false;
    cleanup();
    emit failed(error);
}

void PluginRunner::cleanup()
{
    if (timeoutTimer_) {
        timeoutTimer_->stop();
        timeoutTimer_->deleteLater();
        timeoutTimer_ = nullptr;
    }
    if (extractWatcher_) {
        // Make sure no worker is still touching tmpDir_ before we remove it. If the
        // future already finished (the common case) this returns immediately;
        // otherwise extractCancel_ makes the worker bail at the next chunk.
        extractCancel_ = true;
        extractWatcher_->waitForFinished();
        extractWatcher_->disconnect(this);
        extractWatcher_->deleteLater();
        extractWatcher_ = nullptr;
    }
    if (proc_) {
        proc_->disconnect(this);
        proc_->deleteLater();
        proc_ = nullptr;
    }
    tmpDir_.reset();
}
