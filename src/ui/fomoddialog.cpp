#include "fomoddialog.h"

#include "core/util.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTextBrowser>
#include <QVBoxLayout>

FomodDialog::FomodDialog(const Fomod::Script &script, Fomod::Environment env, const QString &rootDir,
                         QWidget *parent)
    : QDialog(parent), m_session(script, std::move(env)), m_root(rootDir)
{
    setWindowTitle(script.moduleName.isEmpty() ? QStringLiteral("Install") : script.moduleName);
    resize(980, 640);

    auto *layout = new QVBoxLayout(this);
    m_title = new QLabel;
    m_title->setObjectName(QStringLiteral("heading"));
    layout->addWidget(m_title);

    auto *split = new QSplitter(Qt::Horizontal);
    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    split->addWidget(m_scroll);

    auto *side = new QWidget;
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(8, 0, 0, 0);
    m_image = new QLabel;
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setMinimumHeight(220);
    m_description = new QTextBrowser;
    sideLayout->addWidget(m_image, 3);
    sideLayout->addWidget(m_description, 2);
    split->addWidget(side);
    split->setSizes({520, 440});
    layout->addWidget(split, 1);

    auto *buttons = new QHBoxLayout;
    auto *cancel = new QPushButton(QStringLiteral("Cancel"));
    m_back = new QPushButton(QStringLiteral("Back"));
    m_next = new QPushButton(QStringLiteral("Next"));
    m_next->setObjectName(QStringLiteral("suggested"));
    m_next->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addStretch(1);
    buttons->addWidget(m_back);
    buttons->addWidget(m_next);
    layout->addLayout(buttons);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_back, &QPushButton::clicked, this, [this] {
        const int prev = nextVisible(m_step - 1, -1);
        if (prev >= 0)
            showStep(prev);
    });
    connect(m_next, &QPushButton::clicked, this, [this] {
        const int next = nextVisible(m_step + 1, 1);
        if (next < 0)
            accept();
        else
            showStep(next);
    });

    const int first = nextVisible(0, 1);
    if (first >= 0)
        showStep(first);
}

int FomodDialog::nextVisible(int from, int direction) const
{
    for (int s = from; s >= 0 && s < int(m_session.script().steps.size()); s += direction)
        if (m_session.stepVisible(s))
            return s;
    return -1;
}

void FomodDialog::showStep(int step)
{
    m_step = step;
    /* Fomod::Session::preselect leaves a step that already has choices alone,
     * so coming back to a step keeps what was picked. */
    m_session.preselectDefaults(step);
    buildPage();
    m_back->setEnabled(nextVisible(step - 1, -1) >= 0);
    m_next->setText(nextVisible(step + 1, 1) < 0 ? QStringLiteral("Install") : QStringLiteral("Next"));
}

void FomodDialog::buildPage()
{
    const Fomod::Step &step = m_session.script().steps[size_t(m_step)];
    m_title->setText(step.name);
    m_entries.clear();

    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    for (int g = 0; g < int(step.groups.size()); ++g) {
        const Fomod::Group &group = step.groups[size_t(g)];
        auto *box = new QGroupBox(group.name);
        auto *boxLayout = new QVBoxLayout(box);
        const bool exclusive = group.type == Fomod::GroupType::ExactlyOne
                            || group.type == Fomod::GroupType::AtMostOne;
        QButtonGroup *buttons = nullptr;
        if (exclusive) {
            buttons = new QButtonGroup(box);
            /* At-most-one has to be able to go back to none. */
            buttons->setExclusive(group.type == Fomod::GroupType::ExactlyOne);
        }
        for (int o = 0; o < int(group.options.size()); ++o) {
            const Fomod::Option &option = group.options[size_t(o)];
            QAbstractButton *button;
            if (exclusive)
                button = new QRadioButton(option.name);
            else
                button = new QCheckBox(option.name);
            if (buttons)
                buttons->addButton(button);
            button->setChecked(m_session.isSelected(m_step, g, o));
            const Fomod::OptionType type = m_session.optionType(m_step, g, o);
            button->setEnabled(type != Fomod::OptionType::Required
                               && type != Fomod::OptionType::NotUsable
                               && group.type != Fomod::GroupType::All);
            connect(button, &QAbstractButton::clicked, this, [this, g, button, o, exclusive, buttons] {
                /* Making the radios of an at-most-one group behave like one. */
                if (exclusive && !buttons->exclusive() && button->isChecked())
                    for (QAbstractButton *other : buttons->buttons())
                        if (other != button)
                            other->setChecked(false);
                describe(m_session.script().steps[size_t(m_step)].groups[size_t(g)].options[size_t(o)]);
                groupChanged(g);
            });
            connect(button, &QAbstractButton::pressed, this, [this, g, o] {
                describe(m_session.script().steps[size_t(m_step)].groups[size_t(g)].options[size_t(o)]);
            });
            boxLayout->addWidget(button);
            m_entries.push_back({g, o, button});
        }
        layout->addWidget(box);
    }
    layout->addStretch(1);
    m_scroll->setWidget(page);

    for (const Entry &e : m_entries)
        if (e.button->isChecked()) {
            describe(step.groups[size_t(e.group)].options[size_t(e.option)]);
            return;
        }
    if (!m_entries.empty())
        describe(step.groups[size_t(m_entries.front().group)].options[size_t(m_entries.front().option)]);
}

void FomodDialog::describe(const Fomod::Option &option)
{
    m_description->setPlainText(option.description);
    m_image->clear();
    if (option.image.isEmpty())
        return;
    /* The path comes from the mod: look only inside its own folder, in any case. */
    const QString wanted = Util::foldKey(Util::posixPath(option.image));
    for (const QString &f : Util::walkFiles(m_root))
        if (Util::foldKey(f) == wanted) {
            const QPixmap pixmap(m_root + QLatin1Char('/') + f);
            if (!pixmap.isNull())
                m_image->setPixmap(pixmap.scaled(m_image->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            return;
        }
}

void FomodDialog::groupChanged(int group)
{
    QList<int> chosen;
    for (const Entry &e : m_entries)
        if (e.group == group && e.button->isChecked())
            chosen << e.option;
    m_session.select(m_step, group, chosen);
    /* A flag can flip what's available here or which steps come next. */
    m_next->setText(nextVisible(m_step + 1, 1) < 0 ? QStringLiteral("Install") : QStringLiteral("Next"));
    for (const Entry &e : m_entries) {
        const Fomod::OptionType type = m_session.optionType(m_step, e.group, e.option);
        const bool isAll = m_session.script().steps[size_t(m_step)].groups[size_t(e.group)].type == Fomod::GroupType::All;
        e.button->setEnabled(type != Fomod::OptionType::Required && type != Fomod::OptionType::NotUsable && !isAll);
    }
}
