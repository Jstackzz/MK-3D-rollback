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
// a ranked title's badge (launcher/data/ranks, as the server names the title: "Chosen One"), height pixels tall before
// display scaling, as wide as the badges' box (2.4 times its height; each badge centred in it, so they line up); a null
// pixmap for a title without one
// big: the title's larger picture (<key>-big.png) where it has one, as rankScale above 1 draws it
QPixmap rankBadge(const QString& title, int height, bool big = false);
// the title as the badges' files and the games name it: lower case, dashes for spaces ("chosen-one"); "" for none
QString rankKey(const QString& title);
// how many times the others' size a title's badge is drawn in a game's pages: 1.3 for the highest, Eldest God, in
// Deadly Alliance (as in its fights), 1 for the rest; where a badge goes asks rankBadge for this many times its height
// (big) and makes room for it
qreal rankScale(const QString& title, const QString& gameId);
// a picture without its see-through margins (a badge without the room its box leaves at its sides)
QPixmap trimmed(const QPixmap& pm);
}  // namespace style
