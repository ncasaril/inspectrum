/* Copyright (C) 2026 inspectrum contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "aidock.h"
#include "aibridge.h"
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

AiDock::AiDock(AiBridge *bridge, QWidget *parent)
    : QDockWidget(tr("AI review"), parent)
{
    setObjectName("aiReviewDock");
    auto body = new QWidget(this);
    auto layout = new QVBoxLayout(body);
    auto note = new QLabel(tr("Use your external AI session to analyze the signal. Review its annotation proposals here."), body);
    note->setWordWrap(true); layout->addWidget(note);
    auto region = new QLabel(body); region->setObjectName("aiRegion");
    region->setWordWrap(true); layout->addWidget(region);
    auto updateRegion = [region](const QJsonObject &state) {
        const bool selected = state["selection_enabled"].toBool();
        region->setText(tr("%1: start %2 · %3 samples").arg(selected ? tr("Selection") : tr("View"))
            .arg(state[selected ? "selection_start" : "view_start"].toDouble(), 0, 'f', 0)
            .arg(state[selected ? "selection_count" : "view_count"].toDouble(), 0, 'f', 0));
    };
    connect(bridge, &AiBridge::stateChanged, this, updateRegion);
    updateRegion(bridge->state());
    auto status = new QLabel(tr("Ready for external analysis."), body);
    status->setObjectName("aiStatus"); status->setWordWrap(true); layout->addWidget(status);
    auto cancel = new QPushButton(tr("Cancel local analysis"), body);
    cancel->setObjectName("aiCancelAnalysis");
    cancel->setToolTip(tr("Stops inspectrum's analysis job, not your external AI conversation."));
    layout->addWidget(cancel);
    connect(cancel, &QPushButton::clicked, bridge, &AiBridge::cancelAnalysis);
    connect(bridge, &AiBridge::analysisChanged, status, &QLabel::setText);
    auto proposals = new QPlainTextEdit(body); proposals->setReadOnly(true);
    proposals->setObjectName("aiProposals");
    proposals->setPlaceholderText(tr("Proposed annotations and their supporting measurements appear here."));
    layout->addWidget(proposals, 1);
    auto actions = new QHBoxLayout;
    auto apply = new QPushButton(tr("Apply annotations"), body);
    apply->setObjectName("aiApply"); apply->setEnabled(false);
    auto discard = new QPushButton(tr("Discard"), body); discard->setObjectName("aiDiscard");
    actions->addWidget(apply); actions->addWidget(discard); layout->addLayout(actions);
    connect(apply, &QPushButton::clicked, this, [bridge, status]() {
        QString error;
        status->setText(bridge->applyProposal(&error) ? tr("Annotations applied. Edit → Undo restores the previous state.") : error);
    });
    connect(discard, &QPushButton::clicked, bridge, &AiBridge::discardProposal);
    connect(bridge, &AiBridge::proposalChanged, this, [this, proposals, apply](int count, const QString &description) {
        proposals->setPlainText(description); apply->setEnabled(count > 0);
        if (count) { show(); raise(); }
    });
    auto live = new QCheckBox(tr("Allow live annotation edits"), body);
    live->setObjectName("aiLiveEdits"); layout->addWidget(live);
    connect(live, &QCheckBox::toggled, bridge, &AiBridge::setLiveEdits);
    auto details = new QPushButton(tr("Connection details"), body); details->setCheckable(true);
    layout->addWidget(details);
    auto endpoint = new QLabel(tr("Endpoint: ") + bridge->endpointPath(), body);
    endpoint->setWordWrap(true); endpoint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    endpoint->hide(); layout->addWidget(endpoint);
    connect(details, &QPushButton::toggled, endpoint, &QWidget::setVisible);
    auto config = new QPushButton(tr("Copy MCP configuration"), body);
    config->setObjectName("aiCopyConfig"); layout->addWidget(config);
    connect(config, &QPushButton::clicked, this, [bridge, status]() {
        const auto python = QStandardPaths::findExecutable("python3");
        if (python.isEmpty()) { status->setText(tr("Install python3 to use the MCP relay.")); return; }
        QJsonObject server{{"command", python}, {"args", QJsonArray{bridge->supportFile("inspectrum_mcp.py"), "--endpoint", bridge->endpointPath()}}};
        QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(QJsonObject{{"mcpServers", QJsonObject{{"inspectrum", server}}}}).toJson()));
        status->setText(tr("MCP configuration copied. See doc/ai-integration.md for external client setup."));
    });
    setWidget(body); setMinimumWidth(320);
}
