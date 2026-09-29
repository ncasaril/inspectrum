/*
 * Copyright (C) 2026 inspectrum contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QSemaphore>
#include "plugin.h"
#include "spectrogramplot.h"
#include "spectrumview.h"
#include "spectrumpeaks.h"
#include <QCheckBox>
#include <QComboBox>
#include <QTableWidget>
#include <QLabel>
#include "exportfiles.h"
#include "sampleexport.h"
#include <QBuffer>
#include <QtConcurrent>
#include <QAction>

class TestSource : public SampleSource<std::complex<float>>
{
public:
    bool real = false;
    double tone = -1;
    std::vector<std::pair<double, float>> tones;
    int reads = 0;
    QSemaphore *entered = nullptr, *release = nullptr;
    TestSource() { frequency = 1000000; }
    size_t count() override { return 200000; }
    double rate() override { return 48000; }
    float relativeBandwidth() override { return 1; }
    bool realSignal() override { return real; }
    std::unique_ptr<std::complex<float>[]> getSamples(size_t start, size_t length) override {
        ++reads;
        if (entered) { entered->release(); release->acquire(); }
        if (start > count() || length > count() - start) return nullptr;
        auto data = std::unique_ptr<std::complex<float>[]>(new std::complex<float>[length]);
        for (size_t i = 0; i < length; ++i) {
            data[i] = tone < 0 ? std::complex<float>(float(start + i), 0)
                              : std::polar(1.0f, float(2 * std::acos(-1.0) * tone * (start + i)));
            if (!tones.empty()) {
                data[i] = {};
                for (const auto &t : tones)
                    data[i] += std::polar(t.second, float(2 * std::acos(-1.0) * t.first * (start+i)));
            }
        }
        return data;
    }
};

class CoreTests : public QObject
{
    Q_OBJECT
private slots:
    void peakDetector() {
        std::vector<float> line(128, -100);
        QVERIFY(detectSpectrumPeaks(line, 0, 6).empty());
        line[16] = -10; line[32] = -16; line[48] = -22;
        line[18] = -20; // suppressed by the stronger peak within 3 bins
        line[80] = -97; // below median + threshold
        line[90] = std::numeric_limits<float>::quiet_NaN();
        auto peaks = detectSpectrumPeaks(line, 0, 6);
        QCOMPARE(int(peaks.size()), 3);
        QCOMPARE(peaks[0].bin, 16); QCOMPARE(peaks[1].bin, 32); QCOMPARE(peaks[2].bin, 48);
        QCOMPARE(peaks[1].powerDb-peaks[0].powerDb, -6.0f);
        QVERIFY(detectSpectrumPeaks(line, 64, 6).empty()); // real input hides negative frequencies
        std::fill(line.begin(), line.end(), -std::numeric_limits<float>::infinity());
        QVERIFY(detectSpectrumPeaks(line, 0, 6).empty());
        line[64] = -20;
        QCOMPARE(int(detectSpectrumPeaks(line, 0, 6).size()), 1);
        std::fill(line.begin(), line.end(), -100);
        for (int i = 4; i < 124; i += 4) line[i] = -10;
        QCOMPARE(int(detectSpectrumPeaks(line, 0, 6).size()), 16);
    }
    void spectrumPeakControls() {
        auto source = std::make_shared<TestSource>();
        source->tones = {{.0625, 1.0f}, {.125, .5f}, {.1875, .25f}};
        SpectrogramPlot plot(source); plot.setSampleRate(source->rate());
        SpectrumView view(&plot, nullptr); view.resize(760, 640); view.show();
        auto enabled = view.findChild<QCheckBox*>("spectrumDetectPeaks"); QVERIFY(enabled);
        auto table = view.findChild<QTableWidget*>("spectrumPeaks"); QVERIFY(table);
        QVERIFY(!enabled->isChecked()); QVERIFY(!table->isVisible());
        view.setSample(8192); enabled->setChecked(true);
        QTRY_COMPARE(table->rowCount(), 3);
        QCOMPARE(table->item(0, 1)->text().toDouble(), 1003000.0);
        QCOMPARE(table->item(0, 2)->text().toDouble(), 3000.0);
        QCOMPARE(table->item(1, 2)->text().toDouble(), 6000.0);
        QCOMPARE(table->item(2, 2)->text().toDouble(), 9000.0);
        QVERIFY(std::abs(table->item(1, 4)->text().toDouble()+6.02) < .15);
        QCOMPARE(table->item(1, 5)->text().toDouble(), 2.0);
        QVERIFY(QMetaObject::invokeMethod(table, "cellClicked", Q_ARG(int, 1), Q_ARG(int, 0)));
        QCOMPARE(table->item(0, 5)->text().toDouble(), .5);
        QCOMPARE(table->item(1, 4)->text().toDouble(), 0.0);
        auto basis = view.findChild<QComboBox*>("spectrumRatioBasis");
        basis->setCurrentIndex(1);
        QVERIFY(table->item(0, 5)->text().toDouble() > .99);
        const auto level = table->item(0, 3)->text();
        plot.setPowerMin(-20); view.update(); QTest::qWait(50);
        QCOMPARE(table->item(0, 3)->text(), level);
        view.invalidateCache();
        QCOMPARE(table->rowCount(), 0);
        QTRY_COMPARE(table->rowCount(), 3);
        QVERIFY(view.grab().save(QCoreApplication::applicationDirPath()+"/spectrum-peaks-test.png"));
        enabled->setChecked(false); QVERIFY(!table->isVisible());
        QCOMPARE(table->rowCount(), 0);
        view.setSample(source->count()); enabled->setChecked(true);
        QTest::qWait(100); QCOMPARE(table->rowCount(), 0);
    }
    void saveUndoAndReload() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("capture.cf32");
        QFile data(path);
        QVERIFY(data.open(QIODevice::WriteOnly));
        QCOMPARE(data.write(QByteArray(8192, '\0')), qint64(8192));
        data.close();
        InputSource source;
        source.openFile(path.toUtf8().constData());
        source.setSampleRate(48000);
        source.setGlobalTitle("first");
        std::unique_ptr<QAction> undo(source.undoStack()->createUndoAction(nullptr));
        QVERIFY(undo->isEnabled());
        source.setGlobalDescription("note");
        QString error;
        QVERIFY2(source.saveAnnotations(&error), qPrintable(error));
        QVERIFY(!source.annotationsDirty());
        source.setGlobalTitle("second");
        source.setGlobalDescription({});
        QVERIFY(source.saveAnnotations(&error));
        QVERIFY(!QFile::exists(dir.filePath("capture.sigmf-meta")));
        source.undoStack()->undo();
        QVERIFY(source.annotationsDirty());
        QCOMPARE(source.globalDescription(), QString("note"));
        source.undoStack()->redo();
        QVERIFY(!source.annotationsDirty());
        QVERIFY(source.isOpenFilePath(path));
        QVERIFY(source.isOpenFilePath(path + ".sigmf-meta"));
        source.openFile((path + ".sigmf-meta").toUtf8().constData());
        QCOMPARE(source.globalTitle(), QString("second"));
        QVERIFY(source.globalDescription().isEmpty());
        QVERIFY(!source.undoStack()->canUndo());
        QVERIFY(!undo->isEnabled());
        QVERIFY(source.isOpenFilePath(path));
        QVERIFY(!source.annotationsDirty());
    }
    void exportWriteFailure() {
        class FailingOutput : public QIODevice {
            qint64 readData(char *, qint64) override { return -1; }
            qint64 writeData(const char *, qint64) override {
                setErrorString("simulated disk full"); return -1;
            }
        } output;
        output.open(QIODevice::WriteOnly);
        TestSource source;
        QString error;
        QVERIFY(!writeSampleRange(output, source, 0, 100, 1, &error));
        QVERIFY(error.contains("simulated disk full"));
    }
    void pluginExtractionUsesSharedWriter() {
        QTemporaryDir dir;
        TestSource source;
        source.tone = 0.02;
        QString meta, data, error;
        QVERIFY(writeSegmentSigmf(dir.path(), &source, 1000, 10003, 7,
                                  48000.0 / 7, 1000000, &meta, &data, &error));
        QBuffer expected; expected.open(QIODevice::ReadWrite);
        QVERIFY(writeSampleRange(expected, source, 1000, 10003, 7, &error, {}, 997));
        QFile exported(data); QVERIFY(exported.open(QIODevice::ReadOnly));
        QCOMPARE(exported.readAll(), expected.data());
        QFile metadata(meta); QVERIFY(metadata.open(QIODevice::ReadOnly));
        auto root = QJsonDocument::fromJson(metadata.readAll()).object();
        QCOMPARE(root["global"].toObject()["core:sample_rate"].toDouble(), 48000.0 / 7);
    }
    void closeSpectrumDuringWork() {
        QThreadPool *pool = QThreadPool::globalInstance();
        const int oldThreads = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        QSemaphore entered, release;
        auto blocker = QtConcurrent::run([&]() { entered.release(); release.acquire(); });
        entered.acquire();
        std::weak_ptr<TestSource> lifetime;
        {
            auto source = std::make_shared<TestSource>();
            lifetime = source;
            SpectrogramPlot plot(source);
            std::vector<float> line;
            plot.requestSpectrumLine(8192, line);
            QCoreApplication::processEvents();
        }
        bool released = lifetime.expired();
        release.release();
        blocker.waitForFinished();
        pool->waitForDone();
        pool->setMaxThreadCount(oldThreads);
        QVERIFY(released); // worker does not retain or dereference the live source
    }
    void asynchronousSpectrum() {
        for (int mode : {0, 1}) {
            auto source = std::make_shared<TestSource>();
            source->tone = 0.02;
            SpectrogramPlot expectedPlot(source), plot(source);
            expectedPlot.setSpectrogramMode(mode);
            plot.setSpectrogramMode(mode);
            auto expected = expectedPlot.getSpectrumLine(8192);
            source->reads = 0;
            std::vector<float> line;
            QSignalSpy ready(&plot, &SpectrogramPlot::spectrumReady);
            QVERIFY(!plot.requestSpectrumLine(8192, line));
            QCOMPARE(source->reads, 0); // paint-time lookup does no source I/O or FFT
            QTRY_VERIFY(ready.count() > 0);
            QVERIFY(plot.requestSpectrumLine(8192, line));
            QVERIFY(line == expected);
        }
    }
    void cursorBinPower() {
        for (bool real : {false, true}) for (int mode : {0, 1}) {
            auto source = std::make_shared<TestSource>();
            source->real = real; source->tone = .125;
            SpectrogramPlot plot(source);
            plot.setSampleRate(source->rate());
            plot.setSpectrogramMode(mode);
            float power;
            QVERIFY(!plot.requestPowerAt(source->count(), 0, power));
            QVERIFY(std::isnan(power));
            QVERIFY(!plot.requestPowerAt(8192, -1, power));
            QVERIFY(!plot.requestPowerAt(8192, plot.height(), power));
            QSignalSpy ready(&plot, &SpectrogramPlot::spectrumReady);
            QVERIFY(!plot.requestPowerAt(8192, 0, power));
            QTRY_VERIFY(ready.count() > 0);
            const auto expected = plot.getSpectrumLine(8192);
            for (int y : {0, plot.height()/2, plot.height()-1}) {
                QVERIFY(plot.requestPowerAt(8192, y, power));
                const int bin = plot.getFFTSize()-1-int(double(y)*plot.getFFTSize()/plot.spectrumHeight());
                QCOMPARE(power, expected[bin]);
            }
            const int peakBin = int(std::max_element(expected.begin(), expected.end())-expected.begin());
            const int y = plot.getFFTSize()-1-peakBin;
            QVERIFY(plot.requestPowerAt(8192, y, power));
            QVERIFY(std::isfinite(power));
            const float original = power;
            plot.setPowerMin(-20); plot.setPowerMax(10);
            QVERIFY(plot.requestPowerAt(8192, y, power));
            QCOMPARE(power, original); // display gain must not alter measured bin power
            QVERIFY(plot.freqAtPlotY(y) > source->getFrequency());
        }
    }
    void discardStaleSpectrum() {
        auto source = std::make_shared<TestSource>();
        source->tone = 0.02;
        SpectrogramPlot plot(source);
        QThreadPool *pool = QThreadPool::globalInstance();
        const int oldThreads = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        QSemaphore entered, release;
        auto blocker = QtConcurrent::run([&]() { entered.release(); release.acquire(); });
        entered.acquire();
        std::vector<float> line;
        QSignalSpy ready(&plot, &SpectrogramPlot::spectrumReady);
        plot.requestSpectrumLine(8192, line);
        QCoreApplication::processEvents(); // copies frames, queues worker behind blocker
        const bool captured = source->reads > 0;
        source->tone = 0.2;
        plot.invalidateEvent();
        release.release();
        blocker.waitForFinished();
        pool->setMaxThreadCount(oldThreads);
        QVERIFY(captured);
        QTRY_VERIFY(ready.count() > 0);
        QVERIFY(!plot.requestSpectrumLine(8192, line)); // old result was rejected
        ready.clear();
        QTRY_VERIFY(ready.count() > 0);
        QVERIFY(plot.requestSpectrumLine(8192, line));
        SpectrogramPlot expected(source);
        QVERIFY(line == expected.getSpectrumLine(8192));
    }
    void annotationHistory() {
        InputSource source;
        int notifications = 0;
        source.addAnnotationCallback([&]() { ++notifications; });
        Annotation a({10, 20}, {100, 200}, "burst", {}, {}, Qt::cyan);
        source.addAnnotations(std::vector<Annotation>(1000, a));
        QCOMPARE(notifications, 1);
        QCOMPARE(source.undoStack()->count(), 1);
        source.undoStack()->undo();
        QVERIFY(source.annotationList.empty());
        QVERIFY(!source.annotationsDirty());
        source.undoStack()->redo();
        QCOMPARE(source.annotationList.size(), size_t(1000));
        source.undoStack()->setClean();
        QVERIFY(!source.annotationsDirty());
        a.sampleRange = {30, 40};
        source.updateAnnotation(0, a);
        QVERIFY(source.annotationsDirty());
        source.undoStack()->undo();
        QCOMPARE(source.annotationList[0].sampleRange.minimum, size_t(10));
        QVERIFY(!source.annotationsDirty());
        source.removeAnnotation(0);
        QCOMPARE(source.annotationList.size(), size_t(999));
        source.undoStack()->undo();
        QCOMPARE(source.annotationList.size(), size_t(1000));
        source.setGlobalTitle("title");
        source.setGlobalDescription("description");
        source.undoStack()->undo();
        QVERIFY(source.globalDescription().isEmpty());
        source.undoStack()->undo();
        QVERIFY(source.globalTitle().isEmpty());
        QVERIFY(!source.annotationsDirty());
    }
    void exportDecimation() {
        TestSource source;
        QString error;
        QBuffer first, second, direct;
        first.open(QIODevice::ReadWrite);
        second.open(QIODevice::ReadWrite);
        direct.open(QIODevice::ReadWrite);
        QVERIFY(writeSampleRange(first, source, 1000, 10003, 7, &error, {}, 997));
        QVERIFY(writeSampleRange(second, source, 1000, 10003, 7, &error, {}, 65536));
        QCOMPARE(first.data(), second.data());
        QCOMPARE(first.size(), qint64((1 + 10002/7) * sizeof(std::complex<float>)));
        // A linear ramp is unchanged by a centred, unity-gain symmetric FIR.
        for (int k : {0, 100, 1428}) {
            std::complex<float> value;
            std::memcpy(&value, first.data().constData() + k * sizeof(value), sizeof(value));
            QVERIFY(std::abs(value.real() - (1000 + k * 7)) < 0.02f);
        }
        QVERIFY(writeSampleRange(direct, source, 11, 333, 1, &error));
        auto original = source.getSamples(11, 333);
        QCOMPARE(direct.data(), QByteArray(reinterpret_cast<const char*>(original.get()), 333 * sizeof(std::complex<float>)));
        // Exporting the file edges must remain bounded and keep ceil(N/D) samples.
        QBuffer edges; edges.open(QIODevice::ReadWrite);
        QVERIFY(writeSampleRange(edges, source, source.count()-3, 3, 7, &error));
        QCOMPARE(edges.size(), qint64(sizeof(std::complex<float>)));
        QVERIFY(!writeSampleRange(edges, source, source.count()-3, 4, 7, &error));
    }
    void suppressExportAliases() {
        TestSource source;
        for (double tone : {0.02, 0.20}) {
            source.tone = tone;
            QBuffer buffer; buffer.open(QIODevice::ReadWrite);
            QString error;
            QVERIFY(writeSampleRange(buffer, source, 1000, 4000, 4, &error));
            double power = 0;
            for (int k = 0; k < 1000; ++k) {
                std::complex<float> value;
                std::memcpy(&value, buffer.data().constData() + k*sizeof(value), sizeof(value));
                power += std::norm(value) / 1000.0;
            }
            if (tone < 0.1) QVERIFY(power > 0.99 && power < 1.01);
            else QVERIFY2(power < 1e-6, qPrintable(QString::number(power)));
        }
    }
    void stagedExports() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString data = dir.filePath("capture.sigmf-data");
        QString meta = dir.filePath("capture.sigmf-meta");
        auto write = [](const QString &path, const QByteArray &bytes) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        };
        auto read = [](const QString &path) {
            QFile f(path); f.open(QIODevice::ReadOnly); return f.readAll();
        };
        QVERIFY(write(data, "old IQ"));
        QVERIFY(write(meta, "old metadata"));
        QString error;
        {
            ExportFiles exportFiles;
            QVERIFY(exportFiles.open({data, meta}, &error));
            exportFiles.file(0).write("canceled IQ");
        }
        QCOMPARE(read(data), QByteArray("old IQ"));
        QCOMPARE(read(meta), QByteArray("old metadata"));
        {
            ExportFiles exportFiles;
            QVERIFY(exportFiles.open({data, meta}, &error));
            exportFiles.file(0).write("new IQ");
            exportFiles.file(1).write("new metadata");
            QVERIFY(QFile::remove(meta));
            QVERIFY(QDir().mkdir(meta)); // second sibling becomes unwritable
            QVERIFY(!exportFiles.commit(&error));
            QCOMPARE(read(data), QByteArray("old IQ"));
            QVERIFY(QDir().rmdir(meta));
        }
        error.clear();
        QVERIFY(write(meta, "old metadata"));
        {
            ExportFiles exportFiles;
            QVERIFY(exportFiles.open({data, meta}, &error));
            exportFiles.file(0).write("new IQ");
            exportFiles.file(1).write("new metadata");
            QVERIFY2(exportFiles.commit(&error), qPrintable(error));
        }
        QCOMPARE(read(data), QByteArray("new IQ"));
        QCOMPARE(read(meta), QByteArray("new metadata"));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 2);
    }
    void frequencyCoordinates() {
        for (bool real : {false, true}) {
            auto source = std::make_shared<TestSource>();
            source->real = real;
            SpectrogramPlot plot(source);
            plot.setSampleRate(source->rate());
            QCOMPARE(plot.freqAtPlotY(0), 1024000.0);
            QCOMPARE(plot.freqAtPlotY(plot.height()), real ? 1000000.0 : 976000.0);
            for (int y : {0, plot.height()/2, plot.height()})
                QCOMPARE(plot.plotYAtFreq(plot.freqAtPlotY(y)), y);
        }
    }
    void renderCacheKeys() {
        TileCacheKey base(512, 1, 1, 0, SpectrogramMode::Reassigned, -80);
        QVERIFY(!(base == TileCacheKey(512, 1, 1, 0, SpectrogramMode::Reassigned, -60)));
        QVERIFY(!(base == TileCacheKey(512, 1, 1, 0, SpectrogramMode::Reassigned, -80, WindowType::Gaussian)));
        QVERIFY(!(base == TileCacheKey(512, 1, 1, 0, SpectrogramMode::Reassigned, -80, WindowType::Hann, SplatMethod::Nearest)));
        auto source = std::make_shared<TestSource>();
        SpectrogramPlot plot(source);
        unsigned epoch = plot.renderEpoch();
        plot.setSpectrogramMode(1);
        QVERIFY(plot.renderEpoch() != epoch);
    }
    void pluginCoordinates() {
        QString error;
        auto annotations = parsePluginAnnotations(R"({"annotations":[
            {"core:sample_start":2,"core:sample_count":100},
            {"core:sample_start":100,"core:sample_count":1}]})",
            1000, 40, 4, 10, 20, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(annotations.size(), size_t(1));
        QCOMPARE(annotations[0].sampleRange.minimum, size_t(1008));
        QCOMPARE(annotations[0].sampleRange.maximum, size_t(1039));
    }
    void cancelExtraction() {
        auto source = std::make_shared<TestSource>();
        QSemaphore entered, release;
        source->entered = &entered;
        source->release = &release;
        PluginRunner runner;
        int completions = 0;
        connect(&runner, &PluginRunner::finished, this, [&] { ++completions; });
        connect(&runner, &PluginRunner::failed, this, [&] { ++completions; });
        PluginManifest manifest;
        manifest.valid = true;
        manifest.exec = "unused-after-cancel";
        runner.run(manifest, source, 0, 100, 48000, 0, -24000, 24000, {});
        bool started = entered.tryAcquire(1, 5000);
        runner.cancel();
        bool guarded = runner.busy();
        release.release();
        QVERIFY(started);
        QVERIFY(guarded);
        QTRY_VERIFY(!runner.busy());
        QCOMPARE(completions, 0);
    }
};
QTEST_MAIN(CoreTests)
#include "core_tests.moc"
