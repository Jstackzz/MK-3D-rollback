#include "SupportPage.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

SupportPage::SupportPage(QWidget* parent) : Page(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 20, 28, 20);
    root->setSpacing(12);
    root->addWidget(ui::pageHeader(tr("Support"), tr("Support: follow ImTheBlindGuy on YouTube and Twitch.")));

    // the channels as their owner gave them (the YouTube handle is spelled with a small L and a capital i)
    const struct {
        QString title, text, shown, button;
        QUrl url;
    } channels[] = {
        {tr("YouTube"), tr("Videos."), QStringLiteral("youtube.com/@lmTheBIindGuy"), tr("OPEN YOUTUBE"),
         QUrl(QStringLiteral("https://www.youtube.com/@lmTheBIindGuy/videos"))},
        {tr("Twitch"), tr("Live streams."), QStringLiteral("twitch.tv/imtheblindguy"), tr("OPEN TWITCH"),
         QUrl(QStringLiteral("https://www.twitch.tv/imtheblindguy"))},
    };
    auto* row = new QHBoxLayout;
    row->setSpacing(16);
    for (const auto& c : channels) {
        QVBoxLayout* body = nullptr;
        QFrame* card = ui::card(c.title, &body);
        auto* text = ui::label(c.text);
        text->setWordWrap(true);
        body->addWidget(text);
        body->addWidget(ui::label(c.shown, "Muted"));
        auto* open = new QPushButton(c.button);
        open->setObjectName(QStringLiteral("Primary"));
        open->setCursor(Qt::PointingHandCursor);
        open->setToolTip(c.url.toString());
        const QUrl url = c.url;
        connect(open, &QPushButton::clicked, this, [url] { QDesktopServices::openUrl(url); });
        body->addWidget(open, 0, Qt::AlignLeft);
        card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);  // as tall as what it holds
        row->addWidget(card, 1, Qt::AlignTop);
    }
    root->addLayout(row);
    root->addStretch();
}
