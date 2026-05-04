/*
 *  Copyright (C) 2026, Niklas Casaril
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

#include <QColor>
#include <QDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>

#include "samplesource.h"

// Modal editor for one annotation. Time/frequency bounds are display-only
// (the user creates them by drag or by the surrounding cursor/tuner state);
// the dialog only edits label, description, comment, and box color.
class AnnotationDialog : public QDialog
{
    Q_OBJECT
public:
    AnnotationDialog(const Annotation &initial, double sampleRate, QWidget *parent = nullptr);
    Annotation result() const { return current; }

private slots:
    void pickColor();

private:
    void updateColorButton();

    Annotation current;
    double sampleRate;
    QLineEdit *labelEdit;
    QLineEdit *descriptionEdit;
    QPlainTextEdit *commentEdit;
    QPushButton *colorButton;
};
