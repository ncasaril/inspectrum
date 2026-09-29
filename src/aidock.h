/* Copyright (C) 2026 inspectrum contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QDockWidget>
class AiBridge;

// Review surface only: provider conversations live in external MCP clients.
class AiDock : public QDockWidget
{
    Q_OBJECT
public:
    AiDock(AiBridge *bridge, QWidget *parent = nullptr);
};
