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

#include "annotationdialog.h"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>

AnnotationDialog::AnnotationDialog(const Annotation &initial, double sampleRate, QWidget *parent)
    : QDialog(parent), current(initial), sampleRate(sampleRate)
{
    setWindowTitle(tr("Annotation"));
    auto *form = new QFormLayout(this);

    // Read-only summary of the bounds. Users tweak these by re-creating the
    // annotation (drag), not by typing numbers in here.
    const double duration = (sampleRate > 0)
        ? (double)(current.sampleRange.maximum - current.sampleRange.minimum + 1) / sampleRate
        : 0.0;
    auto *bounds = new QLabel(this);
    bounds->setText(tr("Time: %1 s   Freq: %2 .. %3 Hz")
                    .arg(duration, 0, 'f', 6)
                    .arg(current.frequencyRange.minimum, 0, 'f', 0)
                    .arg(current.frequencyRange.maximum, 0, 'f', 0));
    form->addRow(tr("Bounds:"), bounds);

    labelEdit = new QLineEdit(current.label, this);
    form->addRow(tr("Label (core:label):"), labelEdit);

    descriptionEdit = new QLineEdit(current.description, this);
    form->addRow(tr("Description (core:description):"), descriptionEdit);

    commentEdit = new QPlainTextEdit(current.comment, this);
    commentEdit->setMinimumHeight(60);
    form->addRow(tr("Comment (core:comment):"), commentEdit);

    colorButton = new QPushButton(this);
    if (!current.boxColor.isValid())
        current.boxColor = QColor("white");
    updateColorButton();
    connect(colorButton, &QPushButton::clicked, this, &AnnotationDialog::pickColor);
    form->addRow(tr("Color:"), colorButton);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        current.label = labelEdit->text();
        current.description = descriptionEdit->text();
        current.comment = commentEdit->toPlainText();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
}

void AnnotationDialog::pickColor()
{
    QColor c = QColorDialog::getColor(current.boxColor, this, tr("Annotation color"),
                                      QColorDialog::ShowAlphaChannel);
    if (c.isValid()) {
        current.boxColor = c;
        updateColorButton();
    }
}

void AnnotationDialog::updateColorButton()
{
    colorButton->setText(current.boxColor.name(QColor::HexArgb));
    QString style = QStringLiteral("background-color: %1; color: %2;")
        .arg(current.boxColor.name())
        .arg(current.boxColor.lightness() < 128 ? "white" : "black");
    colorButton->setStyleSheet(style);
}
