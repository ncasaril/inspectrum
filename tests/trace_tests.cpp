#include <QtTest>
#include <QSemaphore>
#include <QSignalSpy>
#include "tracesummary.h"
#include "traceplot.h"
#include "tunertransform.h"
#include "amplitudedemod.h"
#include "frequencydemod.h"
#include "inputsource.h"
#include "spectrogramplot.h"

class TraceSource : public SampleSource<std::complex<float>> {
public:
    size_t total = 30000000;
    bool modulated = false;
    std::atomic<size_t> reads{0}, largest{0};
    std::atomic<bool> *cancelOnRead = nullptr;
    QSemaphore *entered = nullptr, *release = nullptr;
    size_t count() override { return total; }
    double rate() override { return 48000; }
    float relativeBandwidth() override { return 1; }
    std::unique_ptr<std::complex<float>[]> getSamples(size_t start, size_t length) override {
        ++reads;
        auto old = largest.load();
        while (old < length && !largest.compare_exchange_weak(old, length)) {}
        if (entered) { entered->release(); release->acquire(); }
        if (cancelOnRead) cancelOnRead->store(true);
        if (start > total || length > total-start) return nullptr;
        auto data = std::make_unique<std::complex<float>[]>(length);
        for (size_t i = 0; i < length; ++i) {
            const auto n = start+i;
            // Periodic phase without large-angle float loss; narrow pulses at chunk seams.
            const float amp = n%65536 == 0 ? 2 : .5f;
            const double phase = (n%64)*2*std::acos(-1.0)/64 +
                (modulated ? 3*std::sin((n%4096)*2*std::acos(-1.0)/4096) : 0);
            data[i] = std::polar(amp, float(phase));
        }
        return data;
    }
};

class TraceTests : public QObject {
    Q_OBJECT
private slots:
    void boundedReadAndExactEnvelope() {
        TraceSource source; std::atomic<bool> cancel{false};
        const size_t start = 13, count = 200013;
        auto summary = summarizeTrace(source, start, count, 103, cancel);
        QVERIFY(summary.complete); QVERIFY(!summary.dense);
        QVERIFY(source.largest <= TraceSummary::chunkSize);
        QCOMPARE(summary.columns[0].size(), size_t(103));
        auto reference = source.getSamples(start, count);
        double minimum = 1e9, maximum = -1e9;
        for (int c = 0; c < 2; ++c) for (int x = 0; x < 103; ++x) {
            TraceBucket bucket;
            for (size_t i = size_t(x)*count/103; i < size_t(x+1)*count/103; ++i)
                bucket.add(traceComponent(reference[i], c));
            QCOMPARE(summary.columns[c][x].low, bucket.low);
            QCOMPARE(summary.columns[c][x].high, bucket.high);
            minimum = std::min(minimum, double(bucket.low));
            maximum = std::max(maximum, double(bucket.high));
        }
        QCOMPARE(summary.minimum, minimum); QCOMPARE(summary.maximum, maximum);
    }
    void denseAndEndOfFile() {
        TraceSource source; source.total = 100;
        std::atomic<bool> cancel{false};
        auto summary = summarizeTrace(source, 90, 20, 20, cancel);
        QVERIFY(summary.complete); QVERIFY(summary.dense);
        QCOMPARE(summary.points[0].size(), size_t(20));
        QVERIFY(std::isfinite(summary.points[0][9]));
        QVERIFY(std::isnan(summary.points[0][10]));
    }
    void cancellationStopsAfterOneRead() {
        TraceSource source; std::atomic<bool> cancel{false}; source.cancelOnRead = &cancel;
        auto summary = summarizeTrace(source, 0, source.total, 1000, cancel);
        QVERIFY(!summary.complete); QCOMPARE(source.reads.load(), size_t(1));
        QVERIFY(source.largest <= TraceSummary::chunkSize);
    }
    void fmChunkBoundaries_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("raw") << 0;
        QTest::newRow("kaiser") << 1;
        QTest::newRow("butterworth") << 2;
        QTest::newRow("predecimation") << 3;
    }
    void fmChunkBoundaries() {
        QFETCH(int, mode);
        auto raw = std::make_shared<TraceSource>();
        raw->modulated = true;
        auto tuner = std::make_shared<TunerTransform>(raw);
        auto makeDemod = [&] {
            auto demod = std::make_shared<FrequencyDemod>(tuner);
            if (mode) demod->setPostLpfCutoff(2000);
            if (mode == 2) demod->setPostLpfMethod(FrequencyDemod::LpfMethod::ButterworthIir);
            if (mode == 3) demod->setPredemodDecimation(4);
            return demod;
        };
        auto whole = makeDemod(), chunked = makeDemod();
        const size_t start = 10000, count = 1200000;
        auto reference = whole->getSamples(start, count); QVERIFY(reference);
        double largestError = 0;
        for (size_t offset = 0; offset < count; offset += TraceSummary::chunkSize) {
            const auto length = std::min(TraceSummary::chunkSize, count-offset);
            auto chunk = chunked->getSamples(start+offset, length); QVERIFY(chunk);
            for (size_t i = 0; i < length; ++i) {
                QVERIFY(std::isfinite(chunk[i]));
                largestError = std::max(largestError, double(std::abs(chunk[i]-reference[offset+i])));
            }
        }
        QVERIFY2(largestError < 1.0, qPrintable(QString("max FM boundary error: %1 Hz").arg(largestError)));
    }
    void latestViewWinsAndCloseIsSafe() {
        auto source = std::make_shared<TraceSource>();
        QSemaphore entered, release; source->entered = &entered; source->release = &release;
        auto plot = std::make_unique<TracePlot>(source);
        QSignalSpy ready(plot.get(), &Plot::repaint);
        QImage image(100, 200, QImage::Format_ARGB32); image.fill(Qt::transparent);
        QPainter painter(&image); QRect rect(0, 0, 100, 200);
        plot->paintMid(painter, rect, {0, 20000000});
        QVERIFY(entered.tryAcquire(1, 5000));
        plot->paintMid(painter, rect, {0, 10});
        source->entered = nullptr; release.release();
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 2, 5000);
        QCOMPARE(source->reads.load(), size_t(2)); // one cancelled chunk + latest view
        plot->paintMid(painter, rect, {0, 10}); // cached repaint must not read again
        QCOMPARE(source->reads.load(), size_t(2));
        source->entered = &entered;
        plot->paintMid(painter, rect, {100, 20000000});
        QVERIFY(entered.tryAcquire(1, 5000));
        plot.reset(); // must not block or let a worker dereference the deleted plot
        source->entered = nullptr; release.release();
        QTest::qWait(100);
    }
    void wideIqAmFm() {
        auto raw = std::make_shared<TraceSource>();
        auto tuner = std::make_shared<TunerTransform>(raw);
        auto am = std::make_shared<AmplitudeDemod>(tuner);
        auto fm = std::make_shared<FrequencyDemod>(tuner);
        TracePlot iqPlot(tuner), amPlot(am), fmPlot(fm);
        QSignalSpy iqReady(&iqPlot, &Plot::repaint), amReady(&amPlot, &Plot::repaint), fmReady(&fmPlot, &Plot::repaint);
        QImage image(3499, 200, QImage::Format_ARGB32); image.fill(Qt::transparent);
        QPainter painter(&image); QRect rect(0, 0, 3499, 200);
        for (auto plot : {&iqPlot, &amPlot, &fmPlot}) plot->paintMid(painter, rect, {0, 28663808});
        QTRY_VERIFY_WITH_TIMEOUT(iqReady.count() && amReady.count() && fmReady.count(), 60000);
        QVERIFY(raw->largest <= TraceSummary::chunkSize+512);
        const auto reads = raw->reads.load();
        for (auto plot : {&iqPlot, &amPlot, &fmPlot}) plot->paintMid(painter, rect, {0, 28663808});
        QCOMPARE(raw->reads.load(), reads);
    }
    void captureStress() {
        const auto path = qgetenv("INSPECTRUM_STRESS_CAPTURE");
        if (path.isEmpty()) QSKIP("Set INSPECTRUM_STRESS_CAPTURE to exercise a real capture");
        auto input = std::make_shared<InputSource>(); input->openFile(path.constData());
        QVERIFY(input->count() >= 28663808);
        SpectrogramPlot spectrum(input); spectrum.setSampleRate(input->rate());
        spectrum.setFFTSize(2048); spectrum.setZoomLevel(1);
        auto tuner = std::dynamic_pointer_cast<SampleSource<std::complex<float>>>(spectrum.output());
        QVERIFY(tuner);
        TracePlot iq(tuner), am(std::make_shared<AmplitudeDemod>(tuner)),
            fm(std::make_shared<FrequencyDemod>(tuner));
        QSignalSpy iqDone(&iq, &Plot::repaint), amDone(&am, &Plot::repaint), fmDone(&fm, &Plot::repaint);
        QImage image(3499, 2048, QImage::Format_ARGB32); image.fill(Qt::transparent);
        QPainter painter(&image); QRect fftRect(0, 0, 3499, 2048), traceRect(0, 0, 3499, 200);
        for (size_t length : {size_t(28663808), input->count()}) {
            iqDone.clear(); amDone.clear(); fmDone.clear();
            spectrum.setSkip(std::max<size_t>(1, length/(3499*2048)));
            spectrum.paintMid(painter, fftRect, {0, length});
            for (auto plot : {&iq, &am, &fm}) plot->paintMid(painter, traceRect, {0, length});
            QTRY_VERIFY_WITH_TIMEOUT(iqDone.count() && amDone.count() && fmDone.count(), 90000);
            for (auto plot : {&iq, &am, &fm}) plot->paintMid(painter, traceRect, {0, length});
            qInfo("Completed FFT + IQ/AM/FM range: %zu samples", length);
        }
    }
};

QTEST_MAIN(TraceTests)
#include "trace_tests.moc"
