#pragma once
// A game's own logo: the title image its xbe carries for the Xbox dashboard ($$XTIMAGE, an XPR0 DXT texture, 128x128
// in the three games), read from the player's extracted game, so the release ships none of it. The games show the
// same image as their window icon (runtime patch 0034, app/title_image.cpp).

#include <QByteArray>
#include <QImage>
#include <QString>

namespace titleimage {
QImage fromXbe(const QByteArray& xbe);  // null when it has none in a format read here (DXT1, DXT3, DXT5)
QImage forGame(const QString& gameId);  // from the game's extracted default.xbe; null before it is extracted
}  // namespace titleimage
