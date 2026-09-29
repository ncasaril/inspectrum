/* Copyright (C) 2026 inspectrum contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <QtTest>
#include <QMenuBar>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>
#include <QCheckBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QLabel>
#include <QProcess>
#include <QClipboard>
#include "mainwindow.h"
#include "aibridge.h"
#include "plotview.h"
#include <QStatusBar>
#include "spectrogramcontrols.h"
#include <QToolTip>
#include "spectrumview.h"
#include <QScrollBar>
#include <QTableWidget>

class AiTests : public QObject
{
    Q_OBJECT
    QTemporaryDir directory;
    QString capture;
    QJsonObject arguments(AiBridge *bridge) {
        const auto s = bridge->state();
        return {{"capture_id", s["capture_id"]}, {"revision", s["revision"]}};
    }
    QJsonArray annotation() {
        return {QJsonObject{{"sample_start", 1000}, {"sample_count", 2000},
            {"freq_low_hz", 5000}, {"freq_high_hz", 7000}, {"label", "Tone near 6 kHz"},
            {"evidence", "Synthetic fixture: unit-amplitude tone at 6000 Hz; measured Hann FFT peak agrees."}}};
    }
private slots:
    void closingSpectrumRemovesMarker() {
        MainWindow window; window.resize(1600, 900);
        window.openFile(capture); window.setSampleRate(48000); window.show();
        auto view = window.findChild<PlotView*>(); QVERIFY(view);
        view->addSpectrumPlot();
        auto spectrum = window.findChild<SpectrumView*>(); QVERIFY(spectrum);
        auto enabled = spectrum->findChild<QCheckBox*>("spectrumDetectPeaks"); QVERIFY(enabled);
        enabled->setChecked(true);
        QVERIFY(view->spectrumMarkerEnabled());
        // The marker is shared: closing one of two views must preserve it.
        view->addSpectrumPlot();
        auto dock = qobject_cast<QDockWidget*>(spectrum->parentWidget()); QVERIFY(dock);
        QPointer<SpectrumView> removed(spectrum);
        dock->close();
        QTRY_VERIFY(removed.isNull());
        QVERIFY(view->spectrumMarkerEnabled());
        spectrum = window.findChild<SpectrumView*>(); QVERIFY(spectrum);
        dock = qobject_cast<QDockWidget*>(spectrum->parentWidget()); QVERIFY(dock);
        dock->setFloating(true);
        removed = spectrum;
        dock->close();
        QTRY_VERIFY(removed.isNull());
        QVERIFY(!view->spectrumMarkerEnabled());
        view->addSpectrumPlot();
        QVERIFY(!view->spectrumMarkerEnabled());
    }
    void peakSpectrumSharedScrolling() {
        const auto path = directory.filePath("low-tone.cf32");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        std::vector<std::complex<float>> samples(131072);
        for (size_t i = 0; i < samples.size(); ++i)
            samples[i] = std::polar(1.0f, float(-2 * std::acos(-1.0) * .375 * i));
        QCOMPARE(file.write(reinterpret_cast<const char*>(samples.data()), samples.size()*sizeof(samples[0])), qint64(samples.size()*sizeof(samples[0])));
        file.close();
        MainWindow window;
        window.findChild<SpectrogramControls*>()->hide();
        window.resize(1000, 700);
        window.openFile(path); window.setSampleRate(48000); window.show();
        auto view = window.findChild<PlotView*>(); QVERIFY(view);
        view->setFFTAndZoom(512, 32);
        view->addSpectrumPlot();
        auto spectrum = window.findChild<SpectrumView*>(); QVERIFY(spectrum);
        auto enabled = spectrum->findChild<QCheckBox*>("spectrumDetectPeaks"); QVERIFY(enabled);
        enabled->setChecked(true);
        auto table = spectrum->findChild<QTableWidget*>("spectrumPeaks"); QVERIFY(table);
        QTRY_VERIFY(table->rowCount() > 0);
        QVERIFY(view->viewport()->height() >= 512); // FFT fits the main view, but not above the table.
        auto bottom = [&]() {
            const auto area = spectrum->sharedFrequencyArea();
            return view->viewport()->mapFromGlobal(area.topLeft()).y()+area.height();
        };
        QVERIFY(bottom() < 448);
        QTRY_COMPARE(view->verticalScrollBar()->maximum(), 512-bottom());
        view->verticalScrollBar()->setValue(0);
        const auto tablePosition = table->mapToGlobal(QPoint(0, 0));
        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                         table->visualItemRect(table->item(0, 0)).center());
        QVERIFY(view->verticalScrollBar()->value() > 0);
        QVERIFY(448-view->verticalScrollBar()->value() < bottom());
        QCOMPARE(table->mapToGlobal(QPoint(0, 0)), tablePosition);
        auto dock = qobject_cast<QDockWidget*>(spectrum->parentWidget()); QVERIFY(dock);
        dock->hide();
        QTRY_COMPARE(view->verticalScrollBar()->maximum(), 0);
        dock->show();
        QTRY_VERIFY(view->verticalScrollBar()->maximum() > 0);
        dock->setFloating(true);
        QTRY_COMPARE(view->verticalScrollBar()->maximum(), 0);
        dock->setFloating(false);
        QTRY_VERIFY(view->verticalScrollBar()->maximum() > 0);
        enabled->setChecked(false);
        QTRY_COMPARE(view->verticalScrollBar()->maximum(), 0);
        enabled->setChecked(true);
        view->setFFTAndZoom(1024, 32);
        QTRY_COMPARE(view->verticalScrollBar()->maximum(), 1024-bottom());
        view->setFFTAndZoom(256, 32);
        QTRY_COMPARE(view->verticalScrollBar()->maximum(), 0);
    }
    void peakSpectrumUsesDraggableMarker() {
        MainWindow window; window.resize(1600, 900);
        window.openFile(capture); window.setSampleRate(48000); window.show();
        auto view = window.findChild<PlotView*>(); QVERIFY(view);
        view->setFFTAndZoom(512, 32);
        view->addSpectrumPlot();
        auto spectrum = window.findChild<SpectrumView*>(); QVERIFY(spectrum);
        auto enabled = spectrum->findChild<QCheckBox*>("spectrumDetectPeaks"); QVERIFY(enabled);
        QVERIFY(!view->spectrumMarkerEnabled());
        enabled->setChecked(true);
        QTest::qWait(100);
        QVERIFY(view->spectrumMarkerEnabled());
        view->centerSpectrumMarker();
        const auto pinned = view->spectrumMarkerPosition();
        QTest::mouseMove(view->viewport(), QPoint(25, 60));
        QCOMPARE(view->spectrumMarkerPosition(), pinned);
        const int x = view->viewport()->width()/2;
        const auto screenshot = view->viewport()->grab().toImage();
        QCOMPARE(screenshot.pixelColor(x, 2), QColor(255, 80, 180));
        QTest::mouseMove(view->viewport(), QPoint(x+8, 6));
        QCOMPARE(view->viewport()->cursor().shape(), Qt::SplitHCursor);
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x+8, 6));
        QMouseEvent move(QEvent::MouseMove, QPointF(x+40, 6), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &move);
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x+40, 6));
        QVERIFY(view->spectrumMarkerPosition() > pinned);
        const auto moved = view->spectrumMarkerPosition();
        QTest::mouseMove(view->viewport(), QPoint(25, 60));
        QCOMPARE(view->spectrumMarkerPosition(), moved);
        view->horizontalScrollBar()->setValue(view->horizontalScrollBar()->value()+10);
        QCOMPARE(view->spectrumMarkerPosition(), moved);
        view->setFFTAndZoom(512, 64);
        QCOMPARE(view->spectrumMarkerPosition(), moved);
        view->centerSpectrumMarker();
        const int newX = view->viewport()->width()/2;
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(newX, 6));
        QMouseEvent farMove(QEvent::MouseMove, QPointF(1000000, 6), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &farMove);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(1000000, 6), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &release);
        QCOMPARE(view->spectrumMarkerPosition(), size_t(131071));
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath()+"/spectrum-marker-test.png"));
    }
    void sliderValueToolTips() {
        SpectrogramControls controls("Controls", nullptr); controls.show();
        controls.sampleRate->setText("48000");
        controls.fftSizeSlider->setValue(10);
        QVERIFY(controls.fftSizeSlider->toolTip().contains("1024 samples"));
        QVERIFY(controls.fftSizeSlider->toolTip().contains("46.875 Hz"));
        controls.zoomLevelSlider->setValue(2);
        QVERIFY(controls.zoomLevelSlider->toolTip().contains("4×"));
        QVERIFY(controls.zoomLevelSlider->toolTip().contains("256 samples per column"));
        controls.zoomLevelSlider->setValue(-2);
        QVERIFY(controls.zoomLevelSlider->toolTip().contains("0.25×"));
        QVERIFY(controls.zoomLevelSlider->toolTip().contains("4096 samples per column"));
        controls.fftSizeSlider->setValue(4);
        controls.zoomLevelSlider->setValue(10);
        QVERIFY(controls.zoomLevelSlider->toolTip().contains("16×")); // effective zoom is capped at FFT size
        QVERIFY(controls.zoomLevelSlider->toolTip().contains("1 samples per column"));
        controls.powerMinSlider->setValue(-95);
        QVERIFY(controls.powerMinSlider->toolTip().contains("-95 dB"));
        controls.reassignmentFloorSlider->setValue(-70);
        QVERIFY(controls.reassignmentFloorSlider->toolTip().contains("-70 dB"));
        controls.powerMaxSlider->setSliderDown(true);
        controls.powerMaxSlider->setValue(-12);
        QCOMPARE(QToolTip::text(), controls.powerMaxSlider->toolTip());
        QVERIFY(QToolTip::text().contains("-12 dB"));
        controls.powerMaxSlider->setSliderDown(false);
        controls.sampleRate->clear();
        QVERIFY(!controls.fftSizeSlider->toolTip().contains("Bin spacing"));
        QToolTip::hideText();
    }
    void absoluteFrequencyAndPowerStatus() {
        const auto metaPath = directory.filePath("frequency.sigmf-meta");
        QFile meta(metaPath); QVERIFY(meta.open(QIODevice::WriteOnly));
        const QByteArray metadata = R"({"global":{"core:datatype":"cf32_le","core:sample_rate":48000,"core:dataset":"tone.cf32"},"captures":[{"core:sample_start":0,"core:frequency":1120500000}],"annotations":[]})";
        QCOMPARE(meta.write(metadata), qint64(metadata.size())); meta.close();
        MainWindow window;
        window.openFile(metaPath);
        auto view = window.findChild<PlotView*>(); QVERIFY(view);
        emit view->mousePositionChanged(.125, 1126500000, {}, "-42.5 dB (relative/bin)");
        auto message = window.statusBar()->currentMessage();
        QVERIFY(message.contains("Freq: 1126500000 Hz"));
        QVERIFY(message.contains("offset 6000000 Hz"));
        QVERIFY(message.contains("Power: -42.5 dB (relative/bin)"));
        emit view->mousePositionChanged(.125, 0, "0.125", {});
        QVERIFY(!window.statusBar()->currentMessage().contains("Power:"));
        QVERIFY(window.statusBar()->currentMessage().contains("Value: 0.125"));
        window.openFile(capture);
        emit view->mousePositionChanged(.125, -6000, {}, "loading…");
        QVERIFY(window.statusBar()->currentMessage().contains("Freq: -6000 Hz"));
        QVERIFY(!window.statusBar()->currentMessage().contains("offset"));
    }
    void initTestCase() {
        QVERIFY(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        QCoreApplication::setApplicationName("inspectrum-ai-test");
        QCoreApplication::setOrganizationName("inspectrum-ai-test");
        capture = directory.filePath("tone.cf32");
        QFile file(capture); QVERIFY(file.open(QIODevice::WriteOnly));
        std::vector<std::complex<float>> samples(131072);
        for (size_t i = 0; i < samples.size(); ++i) samples[i] = std::polar(1.0f, float(2 * std::acos(-1.0) * .125 * i));
        QCOMPARE(file.write(reinterpret_cast<const char*>(samples.data()), samples.size()*sizeof(samples[0])), qint64(samples.size()*sizeof(samples[0])));
    }
    void proposalsAndUndo() {
        MainWindow window; window.resize(1400, 850); window.openFile(capture); window.setSampleRate(48000);
        QString error; QVERIFY2(window.enableAssistant({}, &error), qPrintable(error));
        window.show(); QTest::qWait(100);
        auto bridge = window.findChild<AiBridge*>(); QVERIFY(bridge);
        auto args = arguments(bridge); args["annotations"] = annotation();
        QVERIFY(!bridge->callTool("propose_annotations", args)["isError"].toBool());
        auto apply = window.findChild<QPushButton*>("aiApply"); QVERIFY(apply && apply->isEnabled());
        QVERIFY(bridge->callTool("apply_annotations", arguments(bridge))["isError"].toBool());
        QTest::mouseClick(apply, Qt::LeftButton);
        QCOMPARE(bridge->state()["annotation_count"].toInt(), 1);
        QVERIFY(!apply->isEnabled());
        auto editMenu = window.menuBar()->actions().first()->menu();
        QVERIFY(editMenu); editMenu->actions().first()->trigger();
        QCOMPARE(bridge->state()["annotation_count"].toInt(), 0);
        editMenu->actions().at(1)->trigger();
        QCOMPARE(bridge->state()["annotation_count"].toInt(), 1);
        args = arguments(bridge); args["annotations"] = annotation();
        QVERIFY(!bridge->callTool("propose_annotations", args)["isError"].toBool());
        auto live = window.findChild<QCheckBox*>("aiLiveEdits"); QVERIFY(live); live->setChecked(true);
        QVERIFY(!bridge->callTool("apply_annotations", arguments(bridge))["isError"].toBool());
        QCOMPARE(bridge->state()["annotation_count"].toInt(), 2);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath()+"/ai-dock-test.png"));
    }
    void staleEditsAndReload() {
        MainWindow window; window.openFile(capture); window.setSampleRate(48000);
        QString error; QVERIFY(window.enableAssistant({}, &error));
        auto bridge = window.findChild<AiBridge*>();
        auto old = arguments(bridge); old["annotations"] = annotation();
        QVERIFY(!bridge->callTool("propose_annotations", old)["isError"].toBool());
        auto focus = arguments(bridge); focus["sample_start"] = 5000; focus["sample_count"] = 4000;
        QVERIFY(!bridge->callTool("focus_region", focus)["isError"].toBool());
        QVERIFY(!bridge->applyProposal(&error)); QVERIFY(error.contains("Stale"));
        QVERIFY(bridge->callTool("propose_annotations", old)["isError"].toBool());
        auto current = arguments(bridge); current["annotations"] = annotation();
        window.openFile(capture); // even the same path represents a new capture generation
        QVERIFY(bridge->callTool("propose_annotations", current)["isError"].toBool());
        QCOMPARE(bridge->state()["annotation_count"].toInt(), 0);
    }
    void reviewOnlyControls() {
        MainWindow window; window.openFile(capture); window.setSampleRate(48000);
        QString error; QVERIFY(window.enableAssistant({}, &error));
        auto bridge = window.findChild<AiBridge*>();
        QVERIFY(window.findChild<QDockWidget*>("aiReviewDock"));
        for (const auto name : {"aiPrompt", "aiTranscript", "aiProvider", "aiModel", "aiSend", "aiFollowSelection"})
            QVERIFY(!window.findChild<QObject*>(name));
        QVERIFY(window.findChildren<QProcess*>().isEmpty());
        auto args = arguments(bridge); args["annotations"] = annotation();
        QVERIFY(!bridge->callTool("propose_annotations", args)["isError"].toBool());
        auto proposals = window.findChild<QPlainTextEdit*>("aiProposals");
        QVERIFY(proposals->toPlainText().contains("6000 Hz"));
        window.findChild<QPushButton*>("aiDiscard")->click();
        QVERIFY(proposals->toPlainText().isEmpty());
        QCOMPARE(bridge->state()["pending_proposal_count"].toInt(), 0);
        QCOMPARE(bridge->state()["annotation_count"].toInt(), 0);
        args = arguments(bridge); args["sample_start"] = 5000; args["sample_count"] = 4000;
        QVERIFY(!bridge->callTool("focus_region", args)["isError"].toBool());
        QVERIFY(window.findChild<QLabel*>("aiRegion")->text().contains("5000"));
        window.findChild<QPushButton*>("aiCopyConfig")->click();
        const auto config = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8()).object();
        const auto relay = config["mcpServers"].toObject()["inspectrum"].toObject();
        QVERIFY(relay["args"].toArray().first().toString().endsWith("inspectrum_mcp.py"));
        QCOMPARE(relay["args"].toArray().last().toString(), bridge->endpointPath());
    }
    void canceledJobCannotOverwriteNext() {
        MainWindow window; window.openFile(capture); window.setSampleRate(48000);
        QString error; QVERIFY(window.enableAssistant({}, &error));
        auto bridge = window.findChild<AiBridge*>();
        auto args = arguments(bridge); args["analyzer"] = "measure";
        args["sample_start"] = 1000; args["sample_count"] = 100000;
        auto first = bridge->callTool("start_analysis", args);
        QVERIFY(!first["isError"].toBool());
        const QJsonValue firstId = first["structuredContent"].toObject()["job_id"];
        window.findChild<QPushButton*>("aiCancelAnalysis")->click();
        QVERIFY(bridge->callTool("start_analysis", args)["isError"].toBool());
        QTest::qWait(100);
        auto canceled = bridge->callTool("get_analysis", {{"job_id", firstId}})["structuredContent"].toObject();
        QCOMPARE(canceled["status"].toString(), QString("canceled"));
        args = arguments(bridge); args["analyzer"] = "measure"; args["sample_start"] = 1000; args["sample_count"] = 4096;
        auto second = bridge->callTool("start_analysis", args);
        QVERIFY(!second["isError"].toBool());
        QVERIFY(second["structuredContent"].toObject()["job_id"] != firstId);
        // Destruction during the second job is safe: it only retains copied samples.
    }
};
QTEST_MAIN(AiTests)
#include "ai_tests.moc"
