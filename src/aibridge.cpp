/* Copyright (C) 2026 inspectrum contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "aibridge.h"
#include "plotview.h"
#include "fft.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>

namespace {
constexpr int maxFrame = 1024 * 1024;
constexpr size_t maxSamples = 1024 * 1024;

QJsonObject result(const QJsonObject &data) {
    return {{"content", QJsonArray{QJsonObject{{"type", "text"},
        {"text", QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact))}}}},
        {"structuredContent", data}, {"isError", false}};
}
QJsonObject failure(const QString &error) {
    auto r = result({{"error", error}}); r["isError"] = true; return r;
}
bool integer(const QJsonValue &v, size_t &out) {
    if (!v.isDouble()) return false;
    const double n = v.toDouble();
    if (!std::isfinite(n) || n < 0 || n > 9007199254740991.0 || std::floor(n) != n || n > double(SIZE_MAX)) return false;
    out = size_t(n); return true;
}
QJsonObject annotationJson(const Annotation &a, int index) {
    return {{"index", index}, {"sample_start", double(a.sampleRange.minimum)},
        {"sample_count", double(a.sampleRange.maximum - a.sampleRange.minimum + 1)},
        {"freq_low_hz", a.frequencyRange.minimum}, {"freq_high_hz", a.frequencyRange.maximum},
        {"label", a.label}, {"description", a.description}, {"evidence", a.comment}};
}
class SnapshotSource : public SampleSource<std::complex<float>> {
public:
    std::vector<std::complex<float>> samples;
    double sampleRate;
    SnapshotSource(std::vector<std::complex<float>> data, double rate, double centre)
        : samples(std::move(data)), sampleRate(rate) { frequency = centre; }
    size_t count() override { return samples.size(); }
    double rate() override { return sampleRate; }
    float relativeBandwidth() override { return 1; }
    std::unique_ptr<std::complex<float>[]> getSamples(size_t start, size_t count) override {
        if (start > samples.size() || count > samples.size()-start) return nullptr;
        auto out = std::make_unique<std::complex<float>[]>(count);
        std::copy_n(samples.data()+start, count, out.get()); return out;
    }
};
QJsonObject measure(const SnapshotSource &source, const std::atomic<bool> &cancel) {
    const auto &samples = source.samples;
    double power = 0, peak = 0;
    size_t valid = 0;
    for (auto sample : samples) {
        if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag())) continue;
        const double p = std::norm(std::complex<double>(sample));
        power += p; peak = std::max(peak, p); ++valid;
    }
    const int n = 1024;
    FFT fft(n);
    std::vector<std::complex<float>> in(n), out(n);
    std::vector<double> psd(n, 0);
    size_t frames = 0;
    for (size_t offset = 0; offset + n <= samples.size(); offset += n/2) {
        if (cancel.load()) return {};
        bool finite = true;
        for (int i = 0; i < n; ++i) {
            const auto sample = samples[offset+i];
            finite &= std::isfinite(sample.real()) && std::isfinite(sample.imag());
            in[i] = sample * float(0.5 - 0.5 * std::cos(2 * std::acos(-1.0) * i / (n-1)));
        }
        if (!finite) continue;
        fft.process(out.data(), in.data());
        for (int i = 0; i < n; ++i) psd[i] += std::norm(std::complex<double>(out[i ^ (n/2)])) / (n*n);
        ++frames;
    }
    auto db = [](double p) -> QJsonValue { return p > 0 ? QJsonValue(10 * std::log10(p)) : QJsonValue(); };
    QJsonObject r{{"sample_count", double(samples.size())}, {"finite_samples", double(valid)},
        {"mean_power_dbfs", db(valid ? power/valid : 0)}, {"peak_power_dbfs", db(peak)},
        {"sample_rate", source.sampleRate}, {"fft_size", n}, {"fft_frames", double(frames)},
        {"note", "Uncalibrated normalized sample power. Spectrum is mean Hann-windowed bin power, not dBm or dB/Hz. Occupied bandwidth includes noise/interference; it is not a modulation identification."}};
    if (frames) {
        double total = 0;
        for (auto &p : psd) { p /= frames; total += p; }
        QJsonArray bins;
        for (auto p : psd) bins.append(db(p));
        r["spectrum_db"] = bins;
        r["first_bin_offset_hz"] = -source.sampleRate / 2;
        r["bin_width_hz"] = source.sampleRate / n;
        if (total > 0) {
            int lo = 0, hi = n-1; double acc = 0;
            while (lo < n-1 && acc + psd[lo] < total * .005) acc += psd[lo++];
            acc = 0;
            while (hi > lo && acc + psd[hi] < total * .005) acc += psd[hi--];
            r["occupied_bandwidth_99_hz"] = (hi-lo+1) * source.sampleRate/n;
            r["peak_offset_hz"] = (std::max_element(psd.begin(), psd.end())-psd.begin()-n/2) * source.sampleRate/n;
        }
    }
    return r;
}
}

AiBridge::AiBridge(InputSource *input, PlotView *view, QObject *parent)
    : QObject(parent), input_(input), view_(view),
      directory_(QDir::tempPath()+"/inspectrum-ai-XXXXXX"),
      sessionId_(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    QPointer<AiBridge> self(this);
    input_->addAnnotationCallback([self]() { if (self) ++self->annotationRevision_; });
    poll_.setInterval(250);
    connect(&poll_, &QTimer::timeout, this, [this]() { state(); });
    connect(&server_, &QTcpServer::newConnection, this, [this]() {
        while (auto socket = server_.nextPendingConnection()) {
            if (clients_.size() >= 8) { socket->abort(); socket->deleteLater(); continue; }
            clients_.insert(socket, {});
            socket->setReadBufferSize(maxFrame + 1);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { receive(socket); });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                clients_.remove(socket); socket->deleteLater();
            });
            QTimer::singleShot(5000, socket, [this, socket]() {
                if (clients_.contains(socket) && !clients_[socket].authenticated) socket->abort();
            });
        }
    });
    connect(&measurement_, &QFutureWatcher<QJsonObject>::finished, this, [this]() {
        measurementBusy_ = false;
        finishAnalysis(measurement_.result());
    });
    connect(&runner_, &PluginRunner::finished, this, [this](std::vector<Annotation> annotations) {
        QJsonArray values;
        const size_t start = size_t(job_["sample_start"].toDouble());
        for (auto &a : annotations) {
            a.sampleRange.minimum += start; a.sampleRange.maximum += start;
            values.append(annotationJson(a, values.size()));
        }
        finishAnalysis({{"annotations", values}, {"analyzer", job_["analyzer"]}});
    });
    connect(&runner_, &PluginRunner::failed, this, [this](QString error) { finishAnalysis({}, error); });
    connect(&runner_, &PluginRunner::progress, this, [this](QString text) { emit analysisChanged(text.left(1024)); });
}

AiBridge::~AiBridge()
{
    cancelAnalysis();
    server_.close();
    // Disconnect callbacks before members disappear; queued workers only own snapshots.
    for (auto socket : clients_.keys()) { socket->disconnect(this); socket->abort(); }
    if (!endpointPath_.isEmpty()) QFile::remove(endpointPath_);
}

bool AiBridge::start(const QString &endpointPath, QString *error)
{
    if (server_.isListening()) return true;
    if (!directory_.isValid() || !server_.listen(QHostAddress::LocalHost, 0)) {
        if (error) *error = "Cannot start local AI bridge: " + server_.errorString(); return false;
    }
    token_ = QUuid::createUuid().toString(QUuid::WithoutBraces) + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString path = endpointPath.isEmpty() ? directory_.filePath("endpoint.json") : endpointPath;
    // Never replace another running instance's descriptor.
    if (QFileInfo::exists(path)) {
        if (error) *error = "AI endpoint file already exists: " + path;
        server_.close(); return false;
    }
    QSaveFile file(path);
    const QByteArray json = QJsonDocument(QJsonObject{{"host", "127.0.0.1"},
        {"port", int(server_.serverPort())}, {"token", token_}, {"pid", double(QCoreApplication::applicationPid())}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        file.write(json) != json.size() || !file.commit()) {
        if (error) *error = "Cannot write private AI endpoint: " + file.errorString();
        server_.close(); return false;
    }
    endpointPath_ = QFileInfo(path).absoluteFilePath();
    poll_.start(); state(); return true;
}

QString AiBridge::supportFile(const QString &name) const
{
    const QString installed = QCoreApplication::applicationDirPath()+"/../share/inspectrum/ai/"+name;
    if (QFileInfo::exists(installed)) return QFileInfo(installed).absoluteFilePath();
    const QString source = QStringLiteral(INSPECTRUM_AI_SOURCE_DIR) + "/" + name;
    if (QFileInfo::exists(source)) return source;
    return QStringLiteral(INSPECTRUM_AI_SOURCE_DIR) + "/../../examples/plugins/" + name;
}

QJsonObject AiBridge::state()
{
    QJsonObject s = view_->analysisViewState();
    s["capture_id"] = sessionId_ + ":" + QString::number(input_->captureGeneration());
    s["file_name"] = QFileInfo(input_->filePath()).fileName();
    s["sample_count"] = double(input_->count());
    s["sample_rate"] = input_->rate();
    s["center_frequency_hz"] = input_->getFrequency();
    s["real_signal"] = input_->realSignal();
    s["annotation_revision"] = double(annotationRevision_);
    s["annotation_count"] = double(input_->annotationList.size());
    s["annotation_edit_active"] = view_->annotationEditActive();
    if (s != lastState_) {
        lastState_ = s; ++revision_;
        QJsonObject changed = s; changed["revision"] = double(revision_);
        emit stateChanged(changed);
        for (auto socket : clients_.keys()) if (clients_[socket].subscribed)
            send(socket, {{"jsonrpc", "2.0"}, {"method", "notifications/resources/updated"},
                {"params", QJsonObject{{"uri", "inspectrum://session"}}}});
    }
    s["revision"] = double(revision_);
    // Proposal/permission status does not invalidate the evidence it describes.
    s["pending_proposal_count"] = int(proposal_.size());
    s["live_edits_enabled"] = liveEdits_;
    return s;
}

bool AiBridge::matches(const QJsonObject &args, QString *error)
{
    auto current = state();
    if (!args["capture_id"].isString() || args["capture_id"] != current["capture_id"] ||
        !args["revision"].isDouble() || args["revision"] != current["revision"]) {
        *error = "Stale or missing capture_id/revision. Read get_state and analyze the current region again."; return false;
    }
    if (view_->annotationEditActive()) { *error = "An annotation drag is in progress."; return false; }
    return true;
}

QJsonArray AiBridge::tools() const
{
    QJsonArray list;
    const QJsonObject context{{"capture_id", QJsonObject{{"type", "string"}}},
                             {"revision", QJsonObject{{"type", "integer"}, {"minimum", 0}}}};
    auto add = [&](QString name, QString description, QJsonObject props, QJsonArray required, bool readOnly) {
        list.append(QJsonObject{{"name", name}, {"description", description},
            {"inputSchema", QJsonObject{{"type", "object"}, {"properties", props}, {"required", required}, {"additionalProperties", false}}},
            {"annotations", QJsonObject{{"readOnlyHint", readOnly}, {"destructiveHint", false}, {"openWorldHint", false}}}});
    };
    add("get_state", "Read the live capture, selection, tuner and revision. Sample bounds use absolute capture indices; frequencies use absolute Hz.", {}, {}, true);
    add("get_annotations", "Read annotations in pages of at most 500.",
        {{"offset", QJsonObject{{"type", "integer"}, {"minimum", 0}}}}, {}, true);
    add("get_spectrogram", "Get a PNG of the current plot viewport with its exact capture/view context.", context, {"capture_id", "revision"}, true);
    auto region = context;
    region["sample_start"] = QJsonObject{{"type", "integer"}, {"minimum", 0}};
    region["sample_count"] = QJsonObject{{"type", "integer"}, {"minimum", 1}};
    add("focus_region", "Select an absolute sample region and centre it in the GUI. Returns the new revision.", region,
        {"capture_id", "revision", "sample_start", "sample_count"}, false);
    auto analysis = region;
    analysis["analyzer"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"measure", "fsk", "energy"}}};
    analysis["source"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"raw", "tuned"}}};
    analysis["parameters"] = QJsonObject{{"type", "object"}};
    add("start_analysis", "Analyze an immutable snapshot of at most 1048576 samples locally. Returns job_id; call get_analysis with wait=true. Omitted region uses the current selection/view. FSK/energy need Python+NumPy.", analysis,
        {"capture_id", "revision", "analyzer"}, true);
    add("get_analysis", "Read a job result and its original context. wait=true waits up to 25 seconds without blocking the GUI. Never apply a stale result.",
        {{"job_id", QJsonObject{{"type", "string"}}}, {"wait", QJsonObject{{"type", "boolean"}}}}, {"job_id"}, true);
    add("cancel_analysis", "Cancel the current local analysis job.", {}, {}, false);
    auto proposal = context;
    proposal["annotations"] = QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 1000},
        {"items", QJsonObject{{"type", "object"}, {"properties", QJsonObject{
            {"sample_start", QJsonObject{{"type", "integer"}, {"minimum", 0}}},
            {"sample_count", QJsonObject{{"type", "integer"}, {"minimum", 1}}},
            {"freq_low_hz", QJsonObject{{"type", "number"}}}, {"freq_high_hz", QJsonObject{{"type", "number"}}},
            {"label", QJsonObject{{"type", "string"}, {"maxLength", 256}}},
            {"description", QJsonObject{{"type", "string"}, {"maxLength", 4096}}},
            {"evidence", QJsonObject{{"type", "string"}, {"maxLength", 4096}}}}},
            {"required", QJsonArray{"sample_start", "sample_count", "freq_low_hz", "freq_high_hz", "label", "evidence"}},
            {"additionalProperties", false}}}};
    add("propose_annotations", "Preview an annotation batch in the AI review panel. Include supporting measurements in evidence. Does not change or save the capture.", proposal,
        {"capture_id", "revision", "annotations"}, false);
    add("apply_annotations", "Apply the pending proposal as one undoable batch, only if the user enabled live annotation edits. Otherwise the user applies it in the dock. Does not save to disk.", context,
        {"capture_id", "revision"}, false);
    return list;
}

QJsonObject AiBridge::callTool(const QString &name, const QJsonObject &args)
{
    QString error;
    if (name == "get_state") return result(state());
    if (name == "get_annotations") {
        size_t offset = 0;
        if (args.contains("offset") && !integer(args["offset"], offset)) return failure("Invalid offset.");
        QJsonArray annotations;
        for (size_t i = offset; i < input_->annotationList.size() && i-offset < 500; ++i)
            annotations.append(annotationJson(input_->annotationList[i], int(i)));
        return result({{"context", state()}, {"annotations", annotations}});
    }
    if (name == "cancel_analysis") { cancelAnalysis(); return jobResult(); }
    if (name == "get_analysis") {
        if (args["job_id"].toString() != jobId_ || jobId_.isEmpty()) return failure("Unknown job_id.");
        return jobResult();
    }
    if (!matches(args, &error)) return failure(error);
    if (name == "get_spectrogram") {
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
        if (!view_->viewport()->grab().scaled(1600, 1200, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "PNG"))
            return failure("Could not capture the viewport.");
        auto r = result({{"context", state()}, {"description", "Current plot viewport; axes and derived plots may be included."}});
        auto content = r["content"].toArray();
        content.append(QJsonObject{{"type", "image"}, {"mimeType", "image/png"}, {"data", QString::fromLatin1(png.toBase64())}});
        r["content"] = content; return r;
    }
    if (name == "focus_region") {
        size_t start, count;
        if (!integer(args["sample_start"], start) || !integer(args["sample_count"], count) || !count ||
            start >= input_->count() || count > input_->count()-start) return failure("Invalid sample range.");
        view_->focusAnalysisRange(start, count); return result(state());
    }
    if (name == "start_analysis") return startAnalysis(args);
    if (name == "propose_annotations") {
        if (!args["annotations"].isArray()) return failure("annotations must be an array.");
        const auto list = args["annotations"].toArray();
        if (list.isEmpty() || list.size() > 1000) return failure("Propose 1–1000 annotations.");
        std::vector<Annotation> proposed;
        for (auto value : list) {
            const auto a = value.toObject(); size_t start, count;
            if (!integer(a["sample_start"], start) || !integer(a["sample_count"], count) || !count ||
                start >= input_->count() || count > input_->count()-start ||
                !a["freq_low_hz"].isDouble() || !a["freq_high_hz"].isDouble() ||
                !std::isfinite(a["freq_low_hz"].toDouble()) || !std::isfinite(a["freq_high_hz"].toDouble()) ||
                a["freq_low_hz"].toDouble() > a["freq_high_hz"].toDouble() ||
                a["label"].toString().isEmpty() || a["label"].toString().size() > 256 ||
                a["evidence"].toString().isEmpty() || a["evidence"].toString().size() > 4096 ||
                a["description"].toString().size() > 4096) return failure("Invalid annotation bounds, label or evidence.");
            proposed.emplace_back(range_t<size_t>{start, start+count-1},
                range_t<double>{a["freq_low_hz"].toDouble(), a["freq_high_hz"].toDouble()},
                a["label"].toString(), a["description"].toString(), "AI suggestion: " + a["evidence"].toString(), QColor(0,200,255,180));
        }
        proposal_ = std::move(proposed); proposalContext_ = state();
        QString text;
        for (const auto &a : proposal_) text += QString("%1 · samples %2–%3\n%4\n\n")
            .arg(a.label).arg(qulonglong(a.sampleRange.minimum)).arg(qulonglong(a.sampleRange.maximum)).arg(a.comment);
        emit proposalChanged(int(proposal_.size()), text);
        return result({{"proposal_count", int(proposal_.size())}, {"context", proposalContext_}, {"live_edits_enabled", liveEdits_}});
    }
    if (name == "apply_annotations") {
        if (!liveEdits_) return failure("Live edits are off. The user can apply the proposal in the AI review panel.");
        if (!applyProposal(&error)) return failure(error);
        return result(state());
    }
    return failure("Unknown tool: " + name);
}

bool AiBridge::applyProposal(QString *error)
{
    if (proposal_.empty()) { *error = "No pending proposal."; return false; }
    if (!matches(proposalContext_, error)) return false;
    input_->addAnnotations(proposal_); discardProposal(); state(); return true;
}
void AiBridge::discardProposal() { proposal_.clear(); proposalContext_ = {}; emit proposalChanged(0, {}); }

QJsonObject AiBridge::startAnalysis(const QJsonObject &args)
{
    if (runner_.busy() || measurementBusy_) return failure("An analysis is still running; cancel or wait for it.");
    const auto context = state();
    const bool selected = context["selection_enabled"].toBool();
    size_t start, count;
    if (!integer(args.contains("sample_start") ? args["sample_start"] : context[selected ? "selection_start" : "view_start"], start) ||
        !integer(args.contains("sample_count") ? args["sample_count"] : context[selected ? "selection_count" : "view_count"], count) ||
        !count || count > maxSamples || start >= input_->count() || count > input_->count()-start)
        return failure("Choose a valid region of 1–1048576 samples.");
    if (!(input_->rate() > 0)) return failure("Set the capture sample rate first.");
    const QString analyzer = args["analyzer"].toString();
    if (analyzer != "measure" && analyzer != "fsk" && analyzer != "energy") return failure("Unknown analyzer.");
    const QString basis = args["source"].toString("raw");
    if (basis != "raw" && basis != "tuned") return failure("source must be raw or tuned.");
    auto source = view_->analysisSource(basis == "tuned");
    if (!source) return failure("No analysis source.");
    std::unique_ptr<std::complex<float>[]> samples;
    try { samples = source->getSamples(start, count); }
    catch (const std::exception &e) { return failure(QString::fromUtf8(e.what())); }
    if (!samples) return failure("Could not read the selected samples.");
    const double centre = input_->getFrequency() + (basis == "tuned" && context["tuner_enabled"].toBool() ? context["tuner_offset_hz"].toDouble() : 0);
    auto snapshot = std::make_shared<SnapshotSource>(std::vector<std::complex<float>>(samples.get(), samples.get()+count), source->rate(), centre);
    jobId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job_ = {{"job_id", jobId_}, {"status", "running"}, {"context", context}, {"sample_start", double(start)},
        {"sample_count", double(count)}, {"source", basis}, {"center_frequency_hz", centre}, {"analyzer", analyzer}};
    emit analysisChanged("Running " + analyzer);
    if (analyzer == "measure") {
        measurementBusy_ = true;
        cancel_ = std::make_shared<std::atomic<bool>>(false);
        const auto cancel = cancel_;
        measurement_.setFuture(QtConcurrent::run([snapshot, cancel]() { return measure(*snapshot, *cancel); }));
    } else {
        PluginManifest manifest; manifest.valid = true; manifest.name = analyzer;
        manifest.exec = QStandardPaths::findExecutable("python3");
        manifest.args = QStringList{supportFile(analyzer == "fsk" ? "fsk-analyze.py" : "energy-detect.py")};
        runner_.run(manifest, snapshot, 0, count, source->rate(), centre,
                    centre-source->rate()/2, centre+source->rate()/2, args["parameters"].toObject());
    }
    return jobResult();
}

QJsonObject AiBridge::jobResult()
{
    auto job = job_;
    if (!job.isEmpty()) {
        auto current = state(); const auto context = job["context"].toObject();
        job["stale"] = current["capture_id"] != context["capture_id"] || current["revision"] != context["revision"];
    }
    return result(job);
}
void AiBridge::finishAnalysis(const QJsonObject &data, const QString &error)
{
    if (job_["status"] == "running") {
        job_["status"] = error.isEmpty() ? "completed" : "failed";
        job_["result"] = data;
        if (!error.isEmpty()) job_["error"] = error;
    }
    emit analysisChanged(job_["status"].toString());
    const auto waiting = waiters_; waiters_.clear();
    for (auto &w : waiting) if (w.socket) send(w.socket, {{"jsonrpc", "2.0"}, {"id", w.id}, {"result", jobResult()}});
}
void AiBridge::cancelAnalysis()
{
    if (cancel_) cancel_->store(true);
    runner_.cancel();
    if (job_["status"] == "running") { job_["status"] = "canceled"; finishAnalysis({}); }
}

void AiBridge::send(QTcpSocket *socket, const QJsonObject &message)
{
    if (socket->bytesToWrite() > 8 * maxFrame) { socket->abort(); return; }
    socket->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}
void AiBridge::receive(QTcpSocket *socket)
{
    if (!clients_.contains(socket)) return;
    clients_[socket].buffer += socket->readAll();
    if (clients_[socket].buffer.size() > maxFrame) { socket->abort(); return; }
    while (clients_.contains(socket) && clients_[socket].buffer.contains('\n')) {
        auto &client = clients_[socket];
        const int end = client.buffer.indexOf('\n');
        const auto line = client.buffer.left(end); client.buffer.remove(0, end+1);
        QJsonParseError error; auto doc = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) { socket->abort(); return; }
        if (!client.authenticated) {
            if (doc.object()["token"].toString() != token_) { socket->abort(); return; }
            client.authenticated = true; send(socket, {{"authenticated", true}}); continue;
        }
        dispatch(socket, doc.object());
        if (!clients_.contains(socket)) return;
    }
}
void AiBridge::dispatch(QTcpSocket *socket, const QJsonObject &message)
{
    const auto id = message["id"]; const auto method = message["method"].toString();
    const auto params = message["params"].toObject();
    auto reply = [&](QJsonObject data) { send(socket, {{"jsonrpc", "2.0"}, {"id", id}, {"result", data}}); };
    auto rpcError = [&](int code, QString text) { send(socket, {{"jsonrpc", "2.0"}, {"id", id},
        {"error", QJsonObject{{"code", code}, {"message", text}}}}); };
    if (message["jsonrpc"] != "2.0") { rpcError(-32600, "Expected JSON-RPC 2.0."); return; }
    if (!message.contains("id")) {
        if (method == "notifications/cancelled") {
            for (int i = waiters_.size()-1; i >= 0; --i)
                if (waiters_[i].socket == socket && waiters_[i].id == params["requestId"]) waiters_.removeAt(i);
        }
        return;
    }
    if (method == "initialize") {
        if (clients_[socket].initialized) { rpcError(-32600, "Already initialized."); return; }
        clients_[socket].initialized = true;
        const QString requested = params["protocolVersion"].toString();
        const QString version = QStringList{"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"}.contains(requested) ? requested : "2025-06-18";
        reply({{"protocolVersion", version}, {"serverInfo", QJsonObject{{"name", "inspectrum"}, {"version", "0.1.0"}}},
            {"capabilities", QJsonObject{{"tools", QJsonObject{}}, {"resources", QJsonObject{{"subscribe", true}}}}},
            {"instructions", "Analyze the live capture using measurements. Read get_state before analysis and mutations; retain the original context when proposing findings. Never relabel an old result with a newer revision. Use get_analysis wait=true. Proposals need measured evidence; identification is tentative unless decoded/verified."}});
        return;
    }
    if (!clients_[socket].initialized) { rpcError(-32000, "Initialize first."); return; }
    if (method == "ping") { reply({}); return; }
    if (method == "tools/list") { reply({{"tools", tools()}}); return; }
    if (method == "resources/list") {
        reply({{"resources", QJsonArray{QJsonObject{{"uri", "inspectrum://session"}, {"name", "Live capture state"}, {"mimeType", "application/json"}}}}}); return;
    }
    if (method.startsWith("resources/")) {
        if (params["uri"] != "inspectrum://session") { rpcError(-32602, "Unknown resource."); return; }
        if (method == "resources/read") {
            reply({{"contents", QJsonArray{QJsonObject{{"uri", "inspectrum://session"}, {"mimeType", "application/json"},
                {"text", QString::fromUtf8(QJsonDocument(state()).toJson(QJsonDocument::Compact))}}}}}); return;
        }
        if (method == "resources/subscribe" || method == "resources/unsubscribe") {
            clients_[socket].subscribed = method == "resources/subscribe"; reply({}); return;
        }
    }
    if (method == "tools/call") {
        const auto args = params["arguments"].toObject();
        if (params["name"] == "get_analysis" && args["wait"].toBool() && args["job_id"] == jobId_ && job_["status"] == "running") {
            if (waiters_.size() >= 16) { reply(failure("Too many waiting requests.")); return; }
            waiters_.append({socket, id});
            QPointer<QTcpSocket> weak(socket);
            QTimer::singleShot(25000, this, [this, weak, id]() {
                for (int i = 0; i < waiters_.size(); ++i) if (waiters_[i].socket == weak && waiters_[i].id == id) {
                    waiters_.removeAt(i); if (weak) send(weak, {{"jsonrpc", "2.0"}, {"id", id}, {"result", jobResult()}}); break;
                }
            }); return;
        }
        reply(callTool(params["name"].toString(), args)); return;
    }
    rpcError(-32601, "Unknown method.");
}
