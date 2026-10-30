#include "Style.h"

#include <QGuiApplication>
#include <QPainter>

namespace style {

QPalette palette() {
    QPalette p;
    const QColor bg(0x0c, 0x0c, 0x0e), base(0x10, 0x10, 0x13), text(0xe9, 0xe4, 0xdc), mid(0x24, 0x24, 0x2a);
    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(0x15, 0x15, 0x19));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(0x1e, 0x1e, 0x23));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, QColor(0x8e, 0x0e, 0x1c));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, QColor(0x1a, 0x1a, 0x1f));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(0x6f, 0x69, 0x64));
    p.setColor(QPalette::Mid, mid);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x6a, 0x66, 0x62));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x6a, 0x66, 0x62));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x6a, 0x66, 0x62));
    return p;
}

QString sheet() {
    return QStringLiteral(R"QSS(
* { font-family: "Segoe UI"; font-size: 10pt; color: #E9E4DC; }
QMainWindow, QWidget#Root, QStackedWidget, QScrollArea, QScrollArea > QWidget > QWidget { background: #0C0C0E; }
QDialog { background: #121215; }
QToolTip { background: #1A1A1F; color: #E9E4DC; border: 1px solid #8E0E1C; padding: 4px; }

/* header */
QWidget#Header {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2B0509, stop:0.42 #140A0B, stop:1 #0C0C0E);
    border-bottom: 2px solid #8E0E1C;
}
QLabel#TitleMain { font-family: "Bahnschrift"; font-size: 34pt; font-weight: 800; color: #E31D2B; background: transparent; }
QLabel#TitleSub  { font-family: "Bahnschrift"; font-size: 34pt; font-weight: 300; color: #F4EEE6; background: transparent; }
QLabel#Tagline   { color: #A1958B; font-size: 9pt; background: transparent; }
QLabel#Emblem    { background: transparent; }

QPushButton#GameButton {
    font-family: "Bahnschrift"; font-size: 10pt; font-weight: 600;
    background: #141417; color: #B5ACA3; border: 1px solid #34343C; border-radius: 0px; padding: 8px 14px;
}
QPushButton#GameButton:hover { color: #FFFFFF; border-color: #B3121F; }
QPushButton#GameButton:checked {
    color: #FFFFFF; border: 1px solid #E0303D;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #A1101C, stop:1 #5C0710);
}
QPushButton#AccountButton {
    background: #18181C; border: 1px solid #45454D; border-radius: 15px; padding: 6px 16px; font-weight: 600;
}
QPushButton#AccountButton:hover { border-color: #E0303D; }

/* navigation */
QWidget#NavBar { background: #111114; border-bottom: 1px solid #26262C; }
QPushButton#NavButton {
    font-family: "Bahnschrift"; font-size: 11pt; font-weight: 600;
    background: transparent; color: #A8A19A; border: none; border-bottom: 3px solid transparent;
    padding: 11px 20px 8px 20px;
}
QPushButton#NavButton:hover { color: #FFFFFF; }
QPushButton#NavButton:checked { color: #FFFFFF; border-bottom: 3px solid #D0142A; }

/* page furniture */
QLabel#PageTitle { font-family: "Bahnschrift"; font-size: 22pt; font-weight: 700; color: #F4EEE6; }
QLabel#PageSubtitle { color: #9A928A; font-size: 10pt; }
QFrame#Card { background: #141418; border: 1px solid #24242A; border-radius: 6px; }
QLabel#CardTitle { font-family: "Bahnschrift"; font-size: 11pt; font-weight: 700; color: #E31D2B; background: transparent; }
QLabel#Muted { color: #8E8780; background: transparent; }
QLabel#Big { font-family: "Bahnschrift"; font-size: 16pt; font-weight: 700; color: #FFFFFF; background: transparent; }
QLabel#StatValue { font-family: "Bahnschrift"; font-size: 18pt; font-weight: 700; color: #FFFFFF; background: transparent; }
QLabel#StatLabel { color: #8E8780; font-size: 8pt; background: transparent; }
QLabel#Warning { color: #F0B35A; background: transparent; }
QFrame#Card QLabel, QFrame#Card QCheckBox { background: transparent; }
QFrame#Card QStackedWidget, QFrame#Card QStackedWidget > QWidget { background: transparent; }

/* controls */
QPushButton {
    background: #1E1E23; border: 1px solid #34343C; border-radius: 4px; padding: 7px 14px;
}
QPushButton:hover { border-color: #B3121F; background: #24242A; }
QPushButton:pressed { background: #2C0A0E; }
QPushButton:disabled { color: #5E5A57; border-color: #26262B; background: #16161A; }
QPushButton#Primary {
    font-family: "Bahnschrift"; font-size: 15pt; font-weight: 700; color: #FFFFFF;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #C4141F, stop:1 #6E0912);
    border: 1px solid #F0404C; border-radius: 4px; padding: 12px 30px;
}
QPushButton#Primary:hover { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #DA1B27, stop:1 #820B15); }
QPushButton#Primary:disabled { color: #8E6A6C; background: #2A1416; border-color: #3E2023; }
QPushButton#Primary[searching="true"] {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #34343C, stop:1 #1C1C21); border-color: #6A6A74;
}
QPushButton#Primary[compact="true"] { font-size: 11pt; padding: 6px 14px; }  /* beside plain buttons in a row */
QComboBox, QLineEdit, QSpinBox, QPlainTextEdit, QTextBrowser {
    background: #0F0F12; border: 1px solid #34343C; border-radius: 4px; padding: 5px 8px;
    selection-background-color: #8E0E1C; selection-color: #FFFFFF;
}
QComboBox:hover, QLineEdit:focus, QSpinBox:focus { border-color: #B3121F; }
QComboBox::drop-down { border: none; width: 22px; }
QSpinBox::up-button, QSpinBox::down-button { width: 22px; border: none; background: #1A1A1F; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: #5A0911; }
QComboBox QAbstractItemView { background: #141418; border: 1px solid #8E0E1C; selection-background-color: #5A0911; }
QCheckBox { spacing: 8px; }
QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #4A4A52; background: #0F0F12; border-radius: 3px; }
QCheckBox::indicator:checked { background: #B3121F; border-color: #F0404C; }
QProgressBar { background: #0F0F12; border: 1px solid #34343C; border-radius: 4px; text-align: center; min-height: 18px; }
QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #6E0912, stop:1 #D0142A); border-radius: 3px; }

QListWidget#CharacterGrid { background: transparent; border: none; outline: none; }
QListWidget#CharacterGrid::item {
    background: #1A1A1F; border: 1px solid #2C2C33; border-radius: 4px; color: #DCD5CC; padding: 4px;
}
QListWidget#CharacterGrid::item:hover { border-color: #8E0E1C; color: #FFFFFF; }
QListWidget#CharacterGrid::item:selected {
    color: #FFFFFF; border: 1px solid #F03445;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #7A0C16, stop:1 #3A060B);
}
QListWidget { background: #101013; border: 1px solid #24242A; border-radius: 4px; }
QTableWidget { background: #101013; alternate-background-color: #141418; gridline-color: #1F1F25;
               border: 1px solid #24242A; border-radius: 4px; selection-background-color: #5A0911; }
QHeaderView::section { background: #18181C; color: #C9C1B8; border: none; border-bottom: 2px solid #8E0E1C;
                       padding: 6px 8px; font-weight: 600; }
QTableCornerButton::section { background: #18181C; border: none; }
QTabWidget::pane { border: 1px solid #24242A; border-radius: 4px; top: -1px; }
QTabBar::tab { background: #141418; border: 1px solid #24242A; padding: 7px 16px; color: #A8A19A; }
QTabBar::tab:selected { color: #FFFFFF; border-bottom: 2px solid #D0142A; }
QGroupBox { border: 1px solid #24242A; border-radius: 6px; margin-top: 18px; padding: 12px 10px 10px 10px; background: #141418; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #E31D2B;
                   font-family: "Bahnschrift"; font-weight: 700; }
QScrollBar:vertical { background: #0E0E10; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #2E2E35; border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #8E0E1C; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar:horizontal { background: #0E0E10; height: 10px; }
QScrollBar::handle:horizontal { background: #2E2E35; border-radius: 4px; min-width: 30px; }

/* footer */
QWidget#Footer { background: #09090B; border-top: 1px solid #26262C; }
QWidget#Footer QLabel { color: #8E8780; font-size: 9pt; background: transparent; }
QLabel#Dot { font-size: 12pt; }

/* match found */
QLabel#MatchFound { font-family: "Bahnschrift"; font-size: 28pt; font-weight: 800; color: #E31D2B; }

/* the focus a controller (or Tab) moves: gold, over hover and checked */
QPushButton:focus, QComboBox:focus, QListWidget:focus, QTableWidget:focus, QSpinBox:focus, QLineEdit:focus {
    border-color: #F2C14E;
}
QPushButton#Primary:focus { border: 1px solid #FFE7A3; color: #FFF4D6; }
QPushButton#GameButton:focus { color: #F2C14E; border-color: #F2C14E; }
QPushButton#NavButton:focus { color: #F2C14E; }
QCheckBox:focus { color: #F2C14E; }
QListWidget#CharacterGrid:focus::item:selected { border: 1px solid #F2C14E; }
)QSS");
}

QPixmap linkIcon(const QString& link, int size) {
    const bool wifi = link == QLatin1String("wifi");
    if (!wifi && link != QLatin1String("ethernet")) return {};
    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal s = size;
    if (wifi) {
        const QColor c(0xF0, 0xB3, 0x5A);
        const QPointF o(s / 2, s * 0.84);  // the fan's point
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(o, s * 0.085, s * 0.085);
        p.setPen(QPen(c, s * 0.1, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        for (qreal r : {s * 0.3, s * 0.52, s * 0.74})
            p.drawArc(QRectF(o.x() - r, o.y() - r, 2 * r, 2 * r), 45 * 16, 90 * 16);
    } else {
        const QColor c(0x7C, 0xC6, 0x7A);
        const qreal w = s * 0.16, gap = s * 0.08, left = (s - 4 * w - 3 * gap) / 2, bottom = s * 0.92;
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        for (int i = 0; i < 4; i++) {
            const qreal h = s * (0.3 + 0.2 * i);
            p.drawRoundedRect(QRectF(left + i * (w + gap), bottom - h, w, h), w * 0.25, w * 0.25);
        }
    }
    return pm;
}

QString linkName(const QString& link) {
    if (link == QLatin1String("wifi")) return QObject::tr("Wi-Fi");
    if (link == QLatin1String("ethernet")) return QObject::tr("Wired (Ethernet)");
    if (link == QLatin1String("other")) return QObject::tr("Other connection");
    return QObject::tr("Connection type unknown");
}

QPixmap logo(int height) {
    // drawn at the screen's scale (150 % makes a 76 px logo 114 pixels), from the 320 px tall resource
    const qreal dpr = qGuiApp ? qGuiApp->devicePixelRatio() : 1.0;
    const QImage img(QStringLiteral(":/logo/mk3d-logo.png"));
    QPixmap pm = QPixmap::fromImage(img.scaledToHeight(qRound(height * dpr), Qt::SmoothTransformation));
    pm.setDevicePixelRatio(dpr);
    return pm;
}

QIcon appIcon() {
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/logo/icon-%1.png").arg(size), QSize(size, size));
    return icon;
}

}  // namespace style
