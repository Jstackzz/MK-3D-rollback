#pragma once
// The launcher's look: dark palette, deep red accent, QSS sheet, and a painted emblem/icon.

#include <QIcon>
#include <QPalette>
#include <QPixmap>
#include <QString>

namespace style {
QPalette palette();
QString sheet();
QPixmap emblem(int size);
QIcon appIcon();
}  // namespace style
