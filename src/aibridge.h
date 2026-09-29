/* Copyright (C) 2026 inspectrum contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <atomic>
#include "inputsource.h"
#include "plugin.h"

class PlotView;

// One live GUI session. Local authenticated JSONL transport carries MCP; the
// stdio adapter relays it for Codex/Claude without a public listener.
class AiBridge : public QObject
{
    Q_OBJECT
public:
    AiBridge(InputSource *input, PlotView *view, QObject *parent = nullptr);
    ~AiBridge() override;
    bool start(const QString &endpointPath = {}, QString *error = nullptr);
    QString endpointPath() const { return endpointPath_; }
    QJsonObject state();
    QJsonArray tools() const;
    QJsonObject callTool(const QString &name, const QJsonObject &args);
    bool applyProposal(QString *error);
    void discardProposal();
    void cancelAnalysis();
    void setLiveEdits(bool enabled) { liveEdits_ = enabled; }
    QString supportFile(const QString &name) const;
signals:
    void stateChanged(QJsonObject state);
    void proposalChanged(int count, QString description);
    void analysisChanged(QString status);
private:
    struct Client { QByteArray buffer; bool authenticated = false, initialized = false, subscribed = false; };
    struct Waiter { QPointer<QTcpSocket> socket; QJsonValue id; };
    InputSource *input_;
    PlotView *view_;
    QTcpServer server_;
    QTemporaryDir directory_;
    QString endpointPath_, token_, sessionId_;
    QHash<QTcpSocket*, Client> clients_;
    QTimer poll_;
    QJsonObject lastState_;
    quint64 revision_ = 0, annotationRevision_ = 0;
    bool liveEdits_ = false;
    std::vector<Annotation> proposal_;
    QJsonObject proposalContext_;
    QJsonObject job_;
    QString jobId_;
    QFutureWatcher<QJsonObject> measurement_;
    bool measurementBusy_ = false;
    std::shared_ptr<std::atomic<bool>> cancel_;
    PluginRunner runner_;
    QList<Waiter> waiters_;
    void receive(QTcpSocket *socket);
    void dispatch(QTcpSocket *socket, const QJsonObject &message);
    void send(QTcpSocket *socket, const QJsonObject &message);
    bool matches(const QJsonObject &args, QString *error);
    QJsonObject startAnalysis(const QJsonObject &args);
    void finishAnalysis(const QJsonObject &result, const QString &error = {});
    QJsonObject jobResult();
};
