#pragma once
// The launcher's look: dark palette, deep red accent, QSS sheet, and the MK3D logo and icon.

#include <QIcon>
#include <QPalette>
#include <QPixmap>
#include <QString>

namespace style {
QPalette palette();
QString sheet();
QPixmap logo(int height);  // the MK3D logo (launcher/data/logo), height pixels tall before display scaling
// a player's connection as an icon: "wifi" the Wi-Fi fan (a dot under three arcs, orange: wireless drops and delays
// packets now and then), "ethernet" (wired) four rising bars in green; a null pixmap for anything else
QPixmap linkIcon(const QString& link, int size);
QString linkName(const QString& link);  // its tooltip: "Wi-Fi", "Wired (Ethernet)", ...
QIcon appIcon();
}  // namespace style
