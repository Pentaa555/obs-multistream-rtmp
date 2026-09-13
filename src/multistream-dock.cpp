
// SPDX-License-Identifier: GPL-2.0-or-later

#include "multistream-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QFont>
#include <QDockWidget>
#include <QDialog>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMetaObject>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScreen>
#include <QSplitter>
#include <QSvgRenderer>
#include <QStringList>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <vector>

namespace multistream {
namespace {

QString text(const char *key)
{
    return QString::fromUtf8(obs_module_text(key));
}

QString state_text(DestinationState state)
{
    return text(state_text_key(state));
}

const char *state_badge(DestinationState state)
{
    switch (state) {
    case DestinationState::Streaming:
        return "streaming";
    case DestinationState::Starting:
    case DestinationState::Connecting:
    case DestinationState::Reconnecting:
        return "active";
    case DestinationState::Stopping:
        return "stopping";
    case DestinationState::Error:
        return "error";
    case DestinationState::Configuring:
        return "configuring";
    case DestinationState::Stopped:
    default:
        return "stopped";
    }
}

QString display_url(const std::string &url)
{
    return QString::fromUtf8(url.c_str());
}

QPixmap resource_pixmap(const char *resource_path, const QSize &size)
{
    const QString path = QString::fromUtf8(resource_path);
    if (!path.endsWith(".svg", Qt::CaseInsensitive))
        return QIcon(path).pixmap(size);

    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);
    QSvgRenderer renderer(path);
    if (renderer.isValid()) {
        QPainter painter(&pixmap);
        renderer.render(&painter);
    }
    return pixmap;
}

constexpr int kFacebookTargetRole = Qt::UserRole + 1;

class ToggleSwitch final : public QCheckBox {
public:
    explicit ToggleSwitch(QWidget *parent = nullptr) : QCheckBox(parent)
    {
        setTristate(false);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        animation_.setDuration(160);
        animation_.setEasingCurve(QEasingCurve::InOutCubic);
        connect(&animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            handle_position_ = value.toReal();
            update();
        });
        connect(this, &QCheckBox::toggled, this, [this](bool checked) {
            animation_.stop();
            animation_.setStartValue(handle_position_);
            animation_.setEndValue(checked ? 1.0 : 0.0);
            animation_.start();
        });
    }

    QSize sizeHint() const override
    {
        return QSize(64, 32);
    }

protected:
    bool hitButton(const QPoint &position) const override
    {
        return rect().contains(position);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
            setFocus(Qt::MouseFocusReason);
            click();
            event->accept();
            return;
        }

        QCheckBox::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        constexpr qreal track_width = 52.0;
        constexpr qreal track_height = 24.0;
        constexpr qreal handle_radius = 9.0;
        const qreal left = (width() - track_width) / 2.0;
        const qreal top = (height() - track_height) / 2.0;
        const QRectF track(left, top, track_width, track_height);

        const auto blend = [](const QColor &from, const QColor &to, qreal amount) {
            const qreal t = qBound(0.0, amount, 1.0);
            return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * t,
                                    from.greenF() + (to.greenF() - from.greenF()) * t,
                                    from.blueF() + (to.blueF() - from.blueF()) * t,
                                    from.alphaF() + (to.alphaF() - from.alphaF()) * t);
        };

        QColor track_color;
        QColor handle_color;
        if (!isEnabled()) {
            track_color = QColor("#34313d");
            handle_color = QColor("#686573");
        } else {
            track_color = blend(QColor("#4b4656"), QColor("#59d6c7"), handle_position_);
            handle_color = blend(QColor("#eeeaf2"), QColor("#24212c"), handle_position_);
        }

        painter.setPen(Qt::NoPen);
        painter.setBrush(track_color);
        painter.drawRoundedRect(track, track_height / 2.0, track_height / 2.0);

        const qreal handle_x = track.left() + handle_radius + 3.0 +
                               handle_position_ * (track_width - 2.0 * (handle_radius + 3.0));
        painter.setBrush(handle_color);
        painter.drawEllipse(QPointF(handle_x, track.center().y()), handle_radius, handle_radius);

        if (hasFocus()) {
            QPen focus_pen(QColor("#7be5d7"));
            focus_pen.setWidth(1);
            painter.setPen(focus_pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(track.adjusted(-2, -2, 2, 2), 11, 11);
        }
    }

private:
    qreal handle_position_ = 0.0;
    QVariantAnimation animation_;
};

void style_switch(QCheckBox *checkbox)
{
    checkbox->setObjectName("destinationSwitch");
    checkbox->setFixedSize(64, 32);
    checkbox->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

bool is_native_stream_start_button(QObject *object)
{
    QWidget *candidate = qobject_cast<QWidget *>(object);
    while (candidate) {
        auto *button = qobject_cast<QAbstractButton *>(candidate);
        if (button) {
            QStringList labels{button->text(), button->toolTip(), button->accessibleName(), button->objectName()};
            if (auto *tool_button = qobject_cast<QToolButton *>(button)) {
                if (QAction *action = tool_button->defaultAction())
                    labels.append({action->text(), action->toolTip(), action->objectName()});
            }

            for (QString label : labels) {
                label.remove('&');
                label = label.simplified().toLower();
                const bool says_start = label.contains("start") || label.contains("iniciar") ||
                                        label.contains("comenzar");
                const bool says_stream = label.contains("stream") || label.contains("transmision") ||
                                         label.contains(QString::fromUtf8("transmisión"));
                if (says_start && says_stream)
                    return true;
            }

            QString object_name = button->objectName().toLower();
            const bool is_stream_control =
                (object_name.contains("stream") || object_name.contains("transmision") ||
                 object_name.contains(QString::fromUtf8("transmisión"))) &&
                (object_name.contains("button") || object_name.contains("control"));
            if (is_stream_control)
                return true;
        }
        candidate = candidate->parentWidget();
    }
    return false;
}

} // namespace

MultistreamDock::MultistreamDock(QWidget *parent) : QWidget(parent), manager_(std::make_unique<DestinationManager>())
{
    setObjectName("obs-multistream-rtmp-dock");
    setWindowTitle(text("DockTitle"));
    setMinimumSize(720, 440);
    build_ui();

    manager_->set_state_callback([this](const std::string &id, DestinationState state, const std::string &error,
                                         DestinationManager::OperationId generation) {
        handle_state_change(id, state, error, generation);
    });
    const DestinationManager::LoadStatus load_status = manager_->load();
    if (load_status == DestinationManager::LoadStatus::InvalidFile)
        QMessageBox::warning(this, text("DockTitle"), text("LoadError"));
    else if (load_status == DestinationManager::LoadStatus::UnsupportedVersion)
        QMessageBox::warning(this, text("DockTitle"), text("UnsupportedConfigVersion"));
    obs_frontend_add_event_callback(&MultistreamDock::frontend_event_callback, this);
    if (qApp)
        qApp->installEventFilter(this);
    refresh_list(false);
    restore_facebook_session();
    restore_twitch_session();
    restore_youtube_session();
}

MultistreamDock::~MultistreamDock()
{
    if (qApp)
        qApp->removeEventFilter(this);
    obs_frontend_remove_event_callback(&MultistreamDock::frontend_event_callback, this);
    manager_->set_state_callback({});
    facebook_provider_.cancel_pending_requests();
    twitch_provider_.cancel_pending_requests();
    youtube_provider_.cancel_pending_requests();
    restore_native_service();
    manager_->shutdown();
    if (floating_window_) {
        setParent(nullptr);
        delete floating_window_;
        floating_window_ = nullptr;
    }
}

void MultistreamDock::open_dock()
{
    show_and_raise_dock();
}

void MultistreamDock::build_ui()
{
    auto *root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(12, 12, 12, 12);
    root_layout->setSpacing(10);

    auto *summary_card = new QFrame(this);
    summary_card->setObjectName("summaryCard");
    auto *summary_layout = new QHBoxLayout(summary_card);
    summary_layout->setContentsMargins(18, 14, 18, 14);
    summary_layout->setSpacing(18);

    auto *brand_layout = new QVBoxLayout();
    brand_layout->setSpacing(2);
    auto *title = new QLabel(text("PluginName"), summary_card);
    title->setObjectName("pageTitle");
    auto *subtitle = new QLabel(text("BandwidthNotice"), summary_card);
    subtitle->setObjectName("pageSubtitle");
    subtitle->setWordWrap(true);
    brand_layout->addWidget(title);
    brand_layout->addWidget(subtitle);
    summary_layout->addLayout(brand_layout, 1);

    const auto add_metric = [summary_card, summary_layout](const QString &caption, QLabel *&value) {
        auto *metric = new QFrame(summary_card);
        metric->setObjectName("summaryMetric");
        auto *metric_layout = new QVBoxLayout(metric);
        metric_layout->setContentsMargins(12, 4, 12, 4);
        metric_layout->setSpacing(1);
        auto *caption_label = new QLabel(caption, metric);
        caption_label->setObjectName("summaryLabel");
        value = new QLabel("0", metric);
        value->setObjectName("summaryValue");
        metric_layout->addWidget(caption_label);
        metric_layout->addWidget(value);
        summary_layout->addWidget(metric);
    };
    add_metric(text("Destinations"), summary_destinations_value_);
    add_metric(text("Enabled"), summary_enabled_value_);
    add_metric(text("Streaming"), summary_live_value_);
    add_metric(text("Status"), summary_status_value_);

    auto *columns = new QSplitter(Qt::Horizontal, this);
    columns->setChildrenCollapsible(false);
    columns->setHandleWidth(6);

    auto *left_panel = new QFrame(columns);
    left_panel->setObjectName("destinationsCard");
    auto *left_layout = new QVBoxLayout(left_panel);
    left_layout->setContentsMargins(12, 12, 12, 12);
    left_layout->setSpacing(10);

    auto *title_row = new QHBoxLayout();
    auto *destinations_label = new QLabel(text("Destinations"), left_panel);
    QFont title_font = destinations_label->font();
    title_font.setBold(true);
    title_font.setPointSize(title_font.pointSize() + 1);
    destinations_label->setFont(title_font);
    title_row->addWidget(destinations_label);
    title_row->addStretch();
    left_layout->addLayout(title_row);

    destination_list_ = new QListWidget(left_panel);
    destination_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    destination_list_->setDragDropMode(QAbstractItemView::NoDragDrop);
    destination_list_->setSpacing(5);
    destination_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    destination_list_->setContextMenuPolicy(Qt::CustomContextMenu);
    destination_list_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    left_layout->addWidget(destination_list_);

    start_all_button_ = new QPushButton(text("StartAll"), this);
    start_all_button_->setObjectName("primaryButton");
    start_all_button_->setMinimumWidth(156);
    start_all_button_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto *platforms_layout = new QVBoxLayout();
    platforms_layout->setSpacing(8);
    const auto add_platform = [this, left_panel, platforms_layout](const char *name_key, const char *icon_path,
                                                                     const char *provider_id) -> QWidget * {
        auto *row = new QFrame(left_panel);
        row->setObjectName("platformRow");
        row->setMinimumHeight(50);
        auto *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(10, 6, 10, 6);
        row_layout->setSpacing(12);

        auto *connect_button = new QPushButton(text("Connect"), row);
        connect_button->setObjectName("platformConnectButton");
        connect_button->setMinimumWidth(86);
        row_layout->addWidget(connect_button);

        auto *icon = new QLabel(row);
        icon->setObjectName("platformIcon");
        icon->setFixedSize(30, 30);
        icon->setAlignment(Qt::AlignCenter);
        icon->setPixmap(resource_pixmap(icon_path, QSize(28, 28)));
        row_layout->addWidget(icon);

        auto *name = new QLabel(text(name_key), row);
        name->setObjectName("platformName");
        row_layout->addWidget(name, 1);

        if (std::string(provider_id) == "facebook") {
            connect(connect_button, &QPushButton::clicked, this, [this]() { add_destination_kind("facebook"); });
        } else if (std::string(provider_id) == "twitch") {
            connect(connect_button, &QPushButton::clicked, this, [this]() { add_destination_kind("twitch"); });
        } else if (std::string(provider_id) == "youtube") {
            connect(connect_button, &QPushButton::clicked, this, [this]() { add_destination_kind("youtube"); });
        } else {
            const QString platform_name = text(name_key);
            connect(connect_button, &QPushButton::clicked, this, [this, platform_name]() {
                QMessageBox::information(this, text("DockTitle"),
                                         text("PlatformUnavailable").arg(platform_name));
            });
        }
        platforms_layout->addWidget(row);
        return row;
    };
    facebook_platform_row_ = add_platform("Facebook", ":/icons/facebook_logo.png", "facebook");
    twitch_platform_row_ = add_platform("Twitch", ":/icons/twitch_logo.svg", "twitch");
    youtube_platform_row_ = add_platform("YouTube", ":/icons/youtube_logo.svg", "youtube");
    add_platform("TikTok", ":/icons/tiktok_logo.svg", "tiktok");
    add_platform("XTwitter", ":/icons/x_logo.svg", "x");
    add_platform("Kick", ":/icons/kick_logo.svg", "kick");
    add_platform("Patreon", ":/icons/patreon_logo.svg", "patreon");
    left_layout->addLayout(platforms_layout);

    auto *add_button = new QPushButton(text("AddCustomDestination"), left_panel);
    add_button->setIcon(QIcon(":/icons/add_destination.svg"));
    add_button->setObjectName("secondaryButton");
    add_button->setToolTip(text("AddCustomDestination"));
    left_layout->addWidget(add_button);

    auto *notice = new QLabel(text("BandwidthNotice"), left_panel);
    notice->setWordWrap(true);
    notice->setStyleSheet("color: #d9ebe5; background-color: #302d39; border: 1px solid #474252; border-radius: 4px; padding: 7px;");
    left_layout->addWidget(notice);

    auto *right_panel = new QFrame(columns);
    right_panel->setObjectName("editorPanel");
    auto *right_layout = new QVBoxLayout(right_panel);
    right_layout->setContentsMargins(12, 12, 12, 12);
    right_layout->setSpacing(10);

    settings_group_ = new QGroupBox(right_panel);
    settings_group_->setObjectName("settingsCard");
    auto *settings_form = new QFormLayout(settings_group_);
    settings_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    name_edit_ = new QLineEdit(settings_group_);
    name_label_ = new QLabel(text("Name"), settings_group_);
    name_edit_->setPlaceholderText(text("DefaultDestinationName"));
    settings_form->addRow(name_label_, name_edit_);

    provider_edit_ = new QComboBox(settings_group_);
    provider_label_ = new QLabel(text("Provider"), settings_group_);
    provider_edit_->addItem(text("GenericRtmp"), "rtmp_custom");
    provider_edit_->addItem(text("Facebook"), "facebook");
    provider_edit_->addItem(text("Twitch"), "twitch");
    provider_edit_->addItem(text("YouTube"), "youtube");
    settings_form->addRow(provider_label_, provider_edit_);

    facebook_auth_row_ = new QWidget(settings_group_);
    auto *facebook_auth_layout = new QHBoxLayout(facebook_auth_row_);
    facebook_auth_layout->setContentsMargins(0, 0, 0, 0);
    connect_facebook_button_ = new QPushButton(text("ConnectFacebook"), facebook_auth_row_);
    disconnect_facebook_button_ = new QPushButton(text("DisconnectFacebook"), facebook_auth_row_);
    disconnect_facebook_button_->hide();
    facebook_auth_layout->addWidget(connect_facebook_button_);
    settings_form->addRow(QString(), facebook_auth_row_);

    page_label_ = new QLabel(text("FacebookTarget"), settings_group_);
    page_edit_ = new QComboBox(settings_group_);
    page_edit_->setEditable(false);
    page_edit_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    page_edit_->setMinimumContentsLength(18);
    settings_form->addRow(page_label_, page_edit_);

    facebook_title_label_ = new QLabel(text("FacebookTitle"), settings_group_);
    facebook_title_edit_ = new QLineEdit(settings_group_);
    facebook_title_edit_->setPlaceholderText(text("FacebookTitlePlaceholder"));
    settings_form->addRow(facebook_title_label_, facebook_title_edit_);

    facebook_description_label_ = new QLabel(text("FacebookDescription"), settings_group_);
    facebook_description_edit_ = new QTextEdit(settings_group_);
    facebook_description_edit_->setPlaceholderText(text("FacebookDescriptionPlaceholder"));
    facebook_description_edit_->setMaximumHeight(70);
    settings_form->addRow(facebook_description_label_, facebook_description_edit_);

    facebook_privacy_label_ = new QLabel(text("FacebookPrivacy"), settings_group_);
    facebook_privacy_edit_ = new QComboBox(settings_group_);
    facebook_privacy_edit_->addItem(text("PrivacyPublic"), "PUBLIC");
    facebook_privacy_edit_->addItem(text("PrivacyFriends"), "ALL_FRIENDS");
    facebook_privacy_edit_->addItem(text("PrivacyPrivate"), "SELF");
    settings_form->addRow(facebook_privacy_label_, facebook_privacy_edit_);

    twitch_auth_row_ = new QWidget(settings_group_);
    auto *twitch_auth_layout = new QHBoxLayout(twitch_auth_row_);
    twitch_auth_layout->setContentsMargins(0, 0, 0, 0);
    connect_twitch_button_ = new QPushButton(text("ConnectTwitch"), twitch_auth_row_);
    disconnect_twitch_button_ = new QPushButton(text("DisconnectTwitch"), twitch_auth_row_);
    disconnect_twitch_button_->hide();
    twitch_auth_layout->addWidget(connect_twitch_button_);
    twitch_auth_layout->addWidget(disconnect_twitch_button_);
    settings_form->addRow(QString(), twitch_auth_row_);

    twitch_title_label_ = new QLabel(text("TwitchTitle"), settings_group_);
    twitch_title_edit_ = new QLineEdit(settings_group_);
    twitch_title_edit_->setPlaceholderText(text("TwitchTitlePlaceholder"));
    settings_form->addRow(twitch_title_label_, twitch_title_edit_);

    youtube_auth_row_ = new QWidget(settings_group_);
    auto *youtube_auth_layout = new QHBoxLayout(youtube_auth_row_);
    youtube_auth_layout->setContentsMargins(0, 0, 0, 0);
    connect_youtube_button_ = new QPushButton(text("ConnectYouTube"), youtube_auth_row_);
    disconnect_youtube_button_ = new QPushButton(text("DisconnectYouTube"), youtube_auth_row_);
    disconnect_youtube_button_->hide();
    youtube_auth_layout->addWidget(connect_youtube_button_);
    youtube_auth_layout->addWidget(disconnect_youtube_button_);
    settings_form->addRow(QString(), youtube_auth_row_);

    youtube_title_label_ = new QLabel(text("YouTubeTitle"), settings_group_);
    youtube_title_edit_ = new QLineEdit(settings_group_);
    youtube_title_edit_->setPlaceholderText(text("YouTubeTitlePlaceholder"));
    settings_form->addRow(youtube_title_label_, youtube_title_edit_);

    youtube_description_label_ = new QLabel(text("YouTubeDescription"), settings_group_);
    youtube_description_edit_ = new QTextEdit(settings_group_);
    youtube_description_edit_->setPlaceholderText(text("YouTubeDescriptionPlaceholder"));
    youtube_description_edit_->setMaximumHeight(70);
    settings_form->addRow(youtube_description_label_, youtube_description_edit_);

    youtube_privacy_label_ = new QLabel(text("YouTubePrivacy"), settings_group_);
    youtube_privacy_edit_ = new QComboBox(settings_group_);
    youtube_privacy_edit_->addItem(text("PrivacyPublic"), "public");
    youtube_privacy_edit_->addItem(text("PrivacyUnlisted"), "unlisted");
    youtube_privacy_edit_->addItem(text("PrivacyPrivate"), "private");
    settings_form->addRow(youtube_privacy_label_, youtube_privacy_edit_);

    url_label_ = new QLabel(text("RtmpUrl"), settings_group_);
    url_edit_ = new QLineEdit(settings_group_);
    url_edit_->setPlaceholderText("rtmp[s]://server/app");
    settings_form->addRow(url_label_, url_edit_);

    key_label_ = new QLabel(text("StreamKey"), settings_group_);
    key_edit_ = new QLineEdit(settings_group_);
    key_edit_->setEchoMode(QLineEdit::Password);
    settings_form->addRow(key_label_, key_edit_);

    status_label_ = new QLabel(settings_group_);
    status_label_->setObjectName("stateBadge");
    status_label_->setProperty("state", "stopped");

    error_label_ = new QLabel(settings_group_);
    error_label_->setObjectName("errorLabel");
    error_label_->setWordWrap(true);
    error_label_->setStyleSheet("color: #d85b5b;");
    error_label_->hide();
    settings_form->addRow(QString(), error_label_);
    right_layout->addWidget(settings_group_);

    empty_editor_label_ = new QLabel(text("SelectDestination"), right_panel);
    empty_editor_label_->setObjectName("emptyEditorLabel");
    empty_editor_label_->setAlignment(Qt::AlignCenter);
    empty_editor_label_->setWordWrap(true);
    right_layout->addWidget(empty_editor_label_, 1);

    auto *recovery_row = new QVBoxLayout();
    recovery_label_ = new QLabel(right_panel);
    recovery_label_->setObjectName("recoveryLabel");
    recovery_label_->setWordWrap(true);
    recovery_label_->setMinimumHeight(42);
    recovery_label_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    recovery_label_->setStyleSheet("color: #d89b3d;");
    recovery_row->addWidget(recovery_label_);
    auto *recovery_button_row = new QHBoxLayout();
    recovery_button_row->addStretch();
    recovery_ack_button_ = new QPushButton(text("RecoveryAcknowledge"), right_panel);
    recovery_ack_button_->setMinimumWidth(88);
    recovery_button_row->addWidget(recovery_ack_button_);
    recovery_row->addLayout(recovery_button_row);
    right_layout->addLayout(recovery_row);
    right_layout->addStretch();

    columns->addWidget(left_panel);
    columns->addWidget(right_panel);
    columns->setStretchFactor(0, 2);
    columns->setStretchFactor(1, 3);
    QTimer::singleShot(0, columns, [columns]() {
        const int width = columns->width();
        if (width > 0)
            columns->setSizes({width * 45 / 100, width * 55 / 100});
    });
    root_layout->addWidget(summary_card);
    root_layout->addWidget(columns, 1);

    auto *global_buttons = new QHBoxLayout();
    global_buttons->addStretch();
    global_buttons->addWidget(start_all_button_);
    root_layout->addLayout(global_buttons);

    setStyleSheet(R"(
QWidget#obs-multistream-rtmp-dock {
    background-color: #24212c;
    color: #d9ebe5;
    font-size: 12px;
}
QFrame#summaryCard, QFrame#destinationsCard, QFrame#editorPanel {
    background-color: transparent;
    border: none;
    border-radius: 0;
}
QFrame#summaryCard {
    border-bottom: 1px solid #3a3544;
}
QSplitter::handle:horizontal {
    background-color: #3a3544;
    width: 6px;
}
QSplitter::handle:horizontal:hover {
    background-color: #6d5c82;
}
QLabel#pageTitle {
    color: #c4e8dc;
    font-size: 22px;
    font-weight: 600;
}
QLabel#pageSubtitle, QLabel#summaryLabel {
    color: #9698a6;
    font-size: 11px;
}
QLabel#summaryValue {
    color: #c4e8dc;
    font-size: 16px;
    font-weight: 600;
}
QFrame#summaryMetric {
    background-color: transparent;
    border: none;
    border-left: 1px solid #3a3544;
    border-radius: 0;
}
QGroupBox#settingsCard {
    background-color: #292631;
    border: 1px solid #3b3646;
    border-radius: 8px;
    margin-top: 12px;
    padding: 18px 14px 14px 14px;
    font-weight: 500;
}
QGroupBox#settingsCard::title {
    subcontrol-origin: margin;
    left: 14px;
    padding: 0 6px;
    color: #c4e8dc;
}
QListWidget {
    background-color: #24212c;
    border: none;
    border-radius: 0;
    padding: 2px 0;
    outline: none;
}
QListWidget::item {
    background-color: transparent;
    border: none;
    border-bottom: 1px solid #3a3544;
    border-radius: 0;
    margin: 0;
    padding: 5px 2px;
}
QListWidget::item:hover {
    background-color: #2b2935;
    border-bottom-color: #54cfc2;
}
QListWidget::item:selected {
    background-color: #2c3940;
    border-bottom-color: #54cfc2;
}
QFrame#destinationRow {
    background-color: transparent;
}
QLabel#destinationName {
    color: #e2f0eb;
    font-size: 12px;
}
QLabel#destinationMeta {
    color: #9698a6;
    font-size: 10px;
}
QLineEdit, QComboBox, QTextEdit {
    background-color: #302d39;
    color: #e2f0eb;
    border: 1px solid #474252;
    border-radius: 4px;
    padding: 7px 9px;
    selection-background-color: #3fb9d8;
}
QLineEdit:focus, QComboBox:focus, QTextEdit:focus {
    border-color: #54cfc2;
}
QPushButton {
    background-color: transparent;
    color: #d9ebe5;
    border: 1px solid #514b5d;
    border-radius: 4px;
    padding: 7px 14px;
    min-height: 18px;
}
QPushButton:hover {
    background-color: #30333d;
    border-color: #62d8cb;
}
QPushButton:pressed {
    background-color: #1d3940;
}
QPushButton:disabled {
    color: #6d6d7b;
    background-color: transparent;
    border-color: #373342;
}
QPushButton#primaryButton {
    background-color: #59d6c7;
    color: #17262a;
    border-color: #59d6c7;
    font-weight: 600;
}
QPushButton#primaryButton:hover {
    background-color: #7be5d7;
    border-color: #7be5d7;
}
QPushButton#secondaryButton {
    background-color: #303b45;
    color: #e2f0eb;
    border-color: #3f4b56;
    font-weight: 600;
    min-height: 24px;
}
QPushButton#secondaryButton:hover {
    background-color: #394854;
    border-color: #54cfc2;
}
QFrame#platformRow {
    background-color: #2c3940;
    border: 1px solid #33434b;
    border-radius: 4px;
}
QLabel#platformIcon {
    background-color: transparent;
}
QLabel#platformName {
    color: #e2f0eb;
    font-size: 13px;
    font-weight: 600;
}
QPushButton#platformConnectButton {
    background-color: #303b45;
    color: #ffffff;
    border-color: #3f4b56;
    font-weight: 600;
    min-height: 20px;
}
QPushButton#platformConnectButton:hover {
    background-color: #3b4a55;
    border-color: #54cfc2;
}
QPushButton#platformConnectButton:pressed {
    background-color: #1d3940;
}
QLabel#emptyEditorLabel {
    color: #9698a6;
    font-size: 14px;
    padding: 24px;
}
QLabel#stateBadge {
    background-color: transparent;
    border: none;
    border-radius: 0;
    color: #a9a8b5;
    padding: 3px 5px;
    font-size: 10px;
    font-weight: 600;
}
QLabel#stateBadge[state="streaming"] {
    color: #61dfbd;
}
QLabel#stateBadge[state="active"] {
    color: #e1c36f;
}
QLabel#stateBadge[state="stopping"] {
    color: #c6a5ed;
}
QLabel#stateBadge[state="error"] {
    color: #f18a93;
}
QLabel#stateBadge[state="configuring"] {
    color: #7fd8ed;
}
QLabel#errorLabel {
    color: #f18a93;
    background-color: #39252d;
    border: 1px solid #633945;
    border-radius: 4px;
    padding: 7px;
}
QLabel#recoveryLabel {
    color: #e6ca7d;
    background-color: #393329;
    border: 1px solid #5c4d35;
    border-radius: 4px;
    padding: 8px;
}
QScrollBar:vertical {
    background: transparent;
    width: 8px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: #514b5d;
    border-radius: 4px;
    min-height: 24px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
}
)" );

    connect(add_button, &QPushButton::clicked, this, &MultistreamDock::add_destination);
    connect(destination_list_, &QListWidget::currentRowChanged, this, &MultistreamDock::selection_changed);
    connect(destination_list_, &QListWidget::customContextMenuRequested, this,
            &MultistreamDock::show_destination_context_menu);
    connect(destination_list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
                if (item)
                    destination_list_->setCurrentItem(item);
                delete_destination();
            });
    connect(connect_facebook_button_, &QPushButton::clicked, this, &MultistreamDock::connect_facebook);
    connect(disconnect_facebook_button_, &QPushButton::clicked, this, &MultistreamDock::disconnect_facebook);
    connect(connect_twitch_button_, &QPushButton::clicked, this, &MultistreamDock::connect_twitch);
    connect(disconnect_twitch_button_, &QPushButton::clicked, this, &MultistreamDock::disconnect_twitch);
    connect(connect_youtube_button_, &QPushButton::clicked, this, &MultistreamDock::connect_youtube);
    connect(disconnect_youtube_button_, &QPushButton::clicked, this, &MultistreamDock::disconnect_youtube);
    connect(recovery_ack_button_, &QPushButton::clicked, this, [this]() {
        const auto notices = manager_->recovery_notices();
        for (const auto &notice : notices)
            manager_->clear_recovery_pending(notice.destination_id);
        refresh_list(true);
    });
    connect(provider_edit_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { update_provider_visibility(); });
    connect(page_edit_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { update_provider_visibility(); });
    connect(start_all_button_, &QPushButton::clicked, this, &MultistreamDock::start_all);

    clear_editor();
}

void MultistreamDock::update_dashboard_summary()
{
    if (!summary_destinations_value_ || !summary_enabled_value_ || !summary_live_value_ ||
        !summary_status_value_)
        return;

    int total = 0;
    int enabled = 0;
    int live = 0;
    for (const DestinationConfig &config : manager_->destinations()) {
        ++total;
        if (config.enabled)
            ++enabled;
        if (manager_->is_active(config.id) || manager_->state(config.id) == DestinationState::Streaming)
            ++live;
    }

    summary_destinations_value_->setText(QString::number(total));
    summary_enabled_value_->setText(QString::number(enabled));
    summary_live_value_->setText(QString::number(live));
    summary_status_value_->setText(live > 0 ? text("Streaming") : text("Ready"));
}

void MultistreamDock::refresh_list(bool preserve_editor)
{
    const std::string previous_id = selected_id_;
    QSignalBlocker blocker(destination_list_);
    const bool recovery_pending = manager_->has_recovery_notice();
    start_all_button_->setEnabled(!recovery_pending && frontend_cleanup_ == FrontendCleanup::None &&
                                  !native_stream_redirect_pending_ && !native_start_requested_);
    const int destination_count = static_cast<int>(manager_->destinations().size());
    constexpr int max_visible_destination_rows = 4;
    const bool destinations_overflow = destination_count > max_visible_destination_rows;
    destination_list_->setVisible(destination_count > 0);
    destination_list_->setVerticalScrollBarPolicy(
        destinations_overflow ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    if (destination_count > 0) {
        const int visible_rows = std::min(destination_count, max_visible_destination_rows);
        const int row_spacing = destination_list_->spacing();
        // QListWidget applies the spacing as a margin around every item (top and
        // bottom), so each row occupies row_height + 2*spacing of vertical space.
        // Reserve that full amount for each visible row so the configured rows
        // (e.g. Facebook and Twitch) always fit without triggering the scroll bar.
        constexpr int row_height = 66;
        const int list_height = visible_rows * (row_height + 2 * row_spacing) +
                                 2 * destination_list_->frameWidth() + 4;
        destination_list_->setFixedHeight(list_height);
    }
    destination_list_->clear();

    const auto has_destination_provider = [this](const char *provider_id) {
        return std::any_of(manager_->destinations().begin(), manager_->destinations().end(),
                           [provider_id](const DestinationConfig &config) {
                               return config.provider_id == provider_id;
                           });
    };
    if (facebook_platform_row_)
        facebook_platform_row_->setVisible(!has_destination_provider("facebook"));
    if (twitch_platform_row_)
        twitch_platform_row_->setVisible(!has_destination_provider("twitch"));
    if (youtube_platform_row_)
        youtube_platform_row_->setVisible(!has_destination_provider("youtube"));

    for (const DestinationConfig &config : manager_->destinations()) {
        auto *item = new QListWidgetItem(destination_list_);
        item->setData(Qt::UserRole, QString::fromUtf8(config.id.c_str()));
        item->setSizeHint(QSize(0, 66));

        auto *row = new QWidget(destination_list_);
        row->setObjectName("destinationRow");
        auto *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(6, 4, 6, 4);
        auto *enabled = new ToggleSwitch(row);
        style_switch(enabled);
        enabled->setChecked(config.enabled);
        enabled->setEnabled(frontend_cleanup_ == FrontendCleanup::None && !native_session_active_ &&
                            !native_stream_redirect_pending_ && !native_start_requested_ &&
                            !obs_frontend_streaming_active());
        enabled->setToolTip(text("IncludeInMultistream"));
        enabled->setAccessibleName(text("IncludeInMultistream"));

        auto *provider_icon = new QLabel(row);
        provider_icon->setObjectName("providerIcon");
        provider_icon->setFixedSize(28, 28);
        provider_icon->setAlignment(Qt::AlignCenter);
        if (is_facebook_destination(config))
            provider_icon->setPixmap(resource_pixmap(":/icons/facebook_logo.png", QSize(24, 24)));
        else if (is_twitch_destination(config))
            provider_icon->setPixmap(resource_pixmap(":/icons/twitch_logo.svg", QSize(24, 24)));
        else if (is_youtube_destination(config))
            provider_icon->setPixmap(resource_pixmap(":/icons/youtube_logo.svg", QSize(24, 24)));
        row_layout->addWidget(provider_icon);

        auto *labels = new QVBoxLayout();
        auto *name = new QLabel(QString::fromUtf8(config.name.c_str()), row);
        name->setObjectName("destinationName");
        name->setStyleSheet("font-weight: 600;");
        name->setMinimumWidth(180);
        name->setToolTip(QString::fromUtf8(config.name.c_str()));
        QLabel *url = nullptr;
        if (is_facebook_destination(config)) {
            QString account;
            if (facebook_session_restore_pending_ || facebook_authentication_pending_) {
                account = text("FacebookConnecting");
            } else if (facebook_provider_.authenticated()) {
                account = QString::fromUtf8(facebook_provider_.account_name().c_str());
                if (account.isEmpty())
                    account = QString::fromUtf8(facebook_provider_.account_id().c_str());
                if (account.isEmpty())
                    account = text("FacebookAccountUnavailable");
            } else {
                account = text("FacebookNotConnected");
            }
            url = new QLabel(account, row);
        } else if (is_twitch_destination(config)) {
            QString account;
            if (twitch_session_restore_pending_ || twitch_authentication_pending_)
                account = text("TwitchConnecting");
            else if (twitch_provider_.authenticated())
                account = QString::fromUtf8(twitch_provider_.account_name().c_str());
            else
                account = text("TwitchNotConnected");
            url = new QLabel(account, row);
        } else if (is_youtube_destination(config)) {
            QString account;
            if (youtube_session_restore_pending_ || youtube_authentication_pending_)
                account = text("YouTubeConnecting");
            else if (youtube_provider_.authenticated())
                account = QString::fromUtf8(youtube_provider_.account_name().c_str());
            else
                account = text("YouTubeNotConnected");
            url = new QLabel(account, row);
        } else {
            url = new QLabel(display_url(config.rtmp_url), row);
        }
        url->setObjectName("destinationMeta");
        if (is_facebook_destination(config) || is_twitch_destination(config) || is_youtube_destination(config))
            url->setStyleSheet("color: #ffffff; font-size: 11px; font-weight: 600;");
        else
            url->setStyleSheet("color: palette(mid); font-size: 11px;");
        url->setTextInteractionFlags(Qt::TextSelectableByMouse);
        labels->addWidget(name);
        labels->addWidget(url);
        row_layout->addLayout(labels, 1);

        const DestinationState current_state = manager_->state(config.id);
        auto *status = new QLabel(state_text(current_state), row);
        status->setObjectName("stateBadge");
        status->setProperty("state", state_badge(current_state));
        status->setAlignment(Qt::AlignCenter);
        const int status_width = status->fontMetrics().horizontalAdvance(status->text()) + 18;
        status->setMinimumWidth(status_width);
        row_layout->addWidget(status);
        row_layout->addWidget(enabled);
        destination_list_->setItemWidget(item, row);

        const std::string id = config.id;
        connect(enabled, &QCheckBox::toggled, this, [this, id](bool checked) {
            if (!manager_->set_enabled(id, checked))
                QMessageBox::warning(this, text("DockTitle"), text("SaveError"));
            refresh_list(true);
        });
    }

    int selected_row = -1;
    for (int row = 0; row < destination_list_->count(); ++row) {
        if (destination_list_->item(row)->data(Qt::UserRole).toString().toStdString() == previous_id) {
            selected_row = row;
            break;
        }
    }
    if (selected_row < 0 && destination_list_->count() > 0)
        selected_row = 0;
    if (selected_row >= 0)
        destination_list_->setCurrentRow(selected_row);

    if (!preserve_editor) {
        if (selected_row >= 0)
            populate_editor(destination_list_->item(selected_row)->data(Qt::UserRole).toString().toStdString());
        else
            clear_editor();
    }
    update_dashboard_summary();
    update_recovery_notice();
}

void MultistreamDock::populate_editor(const std::string &id)
{
    const DestinationConfig *config = manager_->find(id);
    if (!config) {
        clear_editor();
        return;
    }

    selected_id_ = id;
    if (settings_group_)
        settings_group_->show();
    if (empty_editor_label_)
        empty_editor_label_->hide();
    name_edit_->setText(QString::fromUtf8(config->name.c_str()));
    const int provider_index = provider_edit_->findData(QString::fromStdString(config->provider_id));
    provider_edit_->setCurrentIndex(provider_index >= 0 ? provider_index : 0);
    url_edit_->setText(QString::fromUtf8(config->rtmp_url.c_str()));
    key_edit_->setText(QString::fromUtf8(config->stream_key.c_str()));
    facebook_title_edit_->setText(QString::fromUtf8(config->facebook_title.c_str()));
    facebook_description_edit_->setPlainText(QString::fromUtf8(config->facebook_description.c_str()));
    populate_facebook_pages();
    const int destination_index = config->facebook_target == "timeline"
                                       ? 0
                                       : page_edit_->findData(QString::fromStdString(config->facebook_page_id), Qt::UserRole);
    if (destination_index >= 0)
        page_edit_->setCurrentIndex(destination_index);
    const int privacy_index = facebook_privacy_edit_->findData(QString::fromStdString(config->facebook_privacy));
    facebook_privacy_edit_->setCurrentIndex(privacy_index >= 0 ? privacy_index : 0);
    twitch_title_edit_->setText(QString::fromUtf8(config->twitch_title.c_str()));
    youtube_title_edit_->setText(QString::fromUtf8(config->youtube_title.c_str()));
    youtube_description_edit_->setPlainText(QString::fromUtf8(config->youtube_description.c_str()));
    const int youtube_privacy_index = youtube_privacy_edit_->findData(QString::fromStdString(config->youtube_privacy));
    youtube_privacy_edit_->setCurrentIndex(youtube_privacy_index >= 0 ? youtube_privacy_index : 0);
    update_provider_visibility();
    update_editor_status();
}

void MultistreamDock::clear_editor()
{
    selected_id_.clear();
    if (settings_group_)
        settings_group_->hide();
    if (empty_editor_label_)
        empty_editor_label_->show();
    name_edit_->clear();
    provider_edit_->setCurrentIndex(0);
    page_edit_->clear();
    facebook_title_edit_->clear();
    facebook_description_edit_->clear();
    page_edit_->clear();
    facebook_privacy_edit_->setCurrentIndex(0);
    twitch_title_edit_->clear();
    youtube_title_edit_->clear();
    youtube_description_edit_->clear();
    youtube_privacy_edit_->setCurrentIndex(0);
    url_edit_->clear();
    key_edit_->clear();
    status_label_->setText(text("SelectDestination"));
    error_label_->clear();
    error_label_->hide();
}

void MultistreamDock::update_editor_status()
{
    if (selected_id_.empty()) {
        clear_editor();
        return;
    }

    const DestinationConfig *config = manager_->find(selected_id_);
    if (!config) {
        clear_editor();
        return;
    }

    const DestinationState current_state = manager_->state(selected_id_);
    status_label_->setText(state_text(current_state));
    status_label_->setProperty("state", state_badge(current_state));
    status_label_->style()->unpolish(status_label_);
    status_label_->style()->polish(status_label_);
    const std::string error = manager_->error(selected_id_);
    error_label_->setText(QString::fromUtf8(error.c_str()));
    const bool facebook_unavailable = is_facebook_destination(*config) && !facebook_provider_.authenticated();
    const bool twitch_unavailable = is_twitch_destination(*config) && !twitch_provider_.authenticated();
    const bool youtube_unavailable = is_youtube_destination(*config) && !youtube_provider_.authenticated();
    error_label_->setVisible(!error.empty() && !facebook_unavailable && !twitch_unavailable && !youtube_unavailable);
    const bool busy = manager_->is_busy(selected_id_) || frontend_cleanup_ != FrontendCleanup::None;
    name_edit_->setEnabled(!busy);
    provider_edit_->setEnabled(!busy);
    page_edit_->setEnabled(!busy);
    facebook_title_edit_->setEnabled(!busy);
    facebook_description_edit_->setEnabled(!busy);
    facebook_privacy_edit_->setEnabled(!busy);
    twitch_title_edit_->setEnabled(!busy);
    youtube_title_edit_->setEnabled(!busy);
    youtube_description_edit_->setEnabled(!busy);
    youtube_privacy_edit_->setEnabled(!busy);
    url_edit_->setEnabled(!busy);
    key_edit_->setEnabled(!busy);
}

void MultistreamDock::selection_changed(int row)
{
    if (row < 0 || row >= destination_list_->count()) {
        clear_editor();
        return;
    }
    populate_editor(destination_list_->item(row)->data(Qt::UserRole).toString().toStdString());
}

bool MultistreamDock::eventFilter(QObject *watched, QEvent *event)
{
    const bool mouse_event = event->type() == QEvent::MouseButtonPress ||
                             event->type() == QEvent::MouseButtonRelease;
    if (mouse_event && !native_session_active_ && !native_start_requested_ &&
        !native_stream_redirect_pending_ && !obs_frontend_streaming_active()) {
        auto *main_window = static_cast<QWidget *>(obs_frontend_get_main_window());
        auto *widget = qobject_cast<QWidget *>(watched);
        if (main_window && widget && main_window->isAncestorOf(widget) && !isAncestorOf(widget) &&
            is_native_stream_start_button(watched)) {
            if (event->type() == QEvent::MouseButtonPress) {
                blog(LOG_INFO, "Intercepted OBS native Start Streaming button; opening Multistream dock");
                // Do not reparent/show the dock while OBS is dispatching the
                // native button event. Mutating the main-window hierarchy from
                // inside this event can invalidate OBS's active mouse handler.
                QTimer::singleShot(0, this, [this]() {
                    if (!native_session_active_ && !native_start_requested_ &&
                        !native_stream_redirect_pending_ && !obs_frontend_streaming_active()) {
                        show_and_raise_dock();
                        update_recovery_notice();
                    }
                });
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MultistreamDock::show_destination_context_menu(const QPoint &position)
{
    QListWidgetItem *item = destination_list_->itemAt(position);
    if (!item)
        return;

    destination_list_->setCurrentItem(item);
    const std::string id = item->data(Qt::UserRole).toString().toStdString();
    const DestinationConfig *config = manager_->find(id);
    if (!config)
        return;

    QMenu menu(this);
    QAction *delete_action = menu.addAction(text("DeleteDestination"));
    QAction *disconnect_action = nullptr;
    if (is_facebook_destination(*config)) {
        menu.addSeparator();
        disconnect_action = menu.addAction(text("DisconnectFacebook"));
    } else if (is_youtube_destination(*config)) {
        menu.addSeparator();
        disconnect_action = menu.addAction(text("DisconnectYouTube"));
    }

    const bool active = manager_->is_busy(id) || facebook_operations_active() || twitch_operations_active() ||
                        youtube_operations_active() || frontend_cleanup_ != FrontendCleanup::None;
    delete_action->setEnabled(!active);
    if (disconnect_action)
        disconnect_action->setEnabled(!active);

    QAction *selected_action = menu.exec(destination_list_->viewport()->mapToGlobal(position));
    if (selected_action == delete_action)
        delete_destination();
    else if (selected_action == disconnect_action)
        disconnect_facebook();
}

void MultistreamDock::update_provider_visibility()
{
    const QString provider = provider_edit_ ? provider_edit_->currentData().toString() : QString{};
    const bool facebook = provider == "facebook";
    const bool twitch = provider == "twitch";
    const bool youtube = provider == "youtube";
    const bool timeline = facebook && page_edit_->currentData(kFacebookTargetRole).toString() == "timeline";
    const bool facebook_session_available = facebook_provider_.authenticated();
    const bool twitch_session_available = twitch_provider_.authenticated();
    const bool youtube_session_available = youtube_provider_.authenticated();
    const bool facebook_ready = facebook && facebook_session_available;
    const bool twitch_ready = twitch && twitch_session_available;
    const bool youtube_ready = youtube && youtube_session_available;
    const bool managed = facebook || twitch || youtube;
    name_label_->setVisible(!managed);
    name_edit_->setVisible(!managed);
    provider_label_->setVisible(!managed);
    provider_edit_->setVisible(!managed);
    facebook_auth_row_->setVisible(facebook && !facebook_session_available && !facebook_session_restore_pending_ &&
                                    !facebook_authentication_pending_);
    page_label_->setVisible(facebook_ready);
    page_edit_->setVisible(facebook_ready);
    facebook_title_label_->setVisible(facebook_ready);
    facebook_title_edit_->setVisible(facebook_ready);
    facebook_description_label_->setVisible(facebook_ready);
    facebook_description_edit_->setVisible(facebook_ready);
    facebook_privacy_label_->setVisible(facebook_ready && timeline);
    facebook_privacy_edit_->setVisible(facebook_ready && timeline);
    twitch_auth_row_->setVisible(twitch && !twitch_session_available && !twitch_session_restore_pending_ &&
                                  !twitch_authentication_pending_);
    twitch_title_label_->setVisible(twitch_ready);
    twitch_title_edit_->setVisible(twitch_ready);
    youtube_auth_row_->setVisible(youtube && !youtube_session_available && !youtube_session_restore_pending_ &&
                                   !youtube_authentication_pending_);
    youtube_title_label_->setVisible(youtube_ready);
    youtube_title_edit_->setVisible(youtube_ready);
    youtube_description_label_->setVisible(youtube_ready);
    youtube_description_edit_->setVisible(youtube_ready);
    youtube_privacy_label_->setVisible(youtube_ready);
    youtube_privacy_edit_->setVisible(youtube_ready);
    url_label_->setVisible(!managed);
    url_edit_->setVisible(!managed);
    key_label_->setVisible(!managed);
    key_edit_->setVisible(!managed);
    if ((facebook && !facebook_ready) || (twitch && !twitch_ready) || (youtube && !youtube_ready))
        error_label_->hide();
    update_facebook_auth_state();
    update_twitch_auth_state();
    update_youtube_auth_state();
}

bool MultistreamDock::facebook_operations_active() const
{
    if (native_stream_redirect_pending_ || native_start_requested_ || facebook_authentication_pending_ ||
        facebook_session_restore_pending_ || !facebook_operations_.empty())
        return true;
    for (const DestinationConfig &config : manager_->destinations())
        if (is_facebook_destination(config) && manager_->is_busy(config.id))
            return true;
    return false;
}

void MultistreamDock::update_facebook_auth_state()
{
    if (!connect_facebook_button_ || !facebook_auth_row_)
        return;
    const bool facebook_selected = provider_edit_ && provider_edit_->currentData().toString() == "facebook";
    const bool active = facebook_operations_active() || frontend_cleanup_ != FrontendCleanup::None;
    const bool session_pending = facebook_session_restore_pending_ || facebook_authentication_pending_;
    const bool session_available = facebook_provider_.authenticated();
    facebook_auth_row_->setVisible(facebook_selected && !session_pending && !session_available);
    connect_facebook_button_->setVisible(facebook_selected && !session_pending && !session_available);
    connect_facebook_button_->setEnabled(facebook_selected && !active && !session_available);
}

void MultistreamDock::update_twitch_auth_state()
{
    if (!connect_twitch_button_ || !twitch_auth_row_)
        return;
    const bool selected = provider_edit_ && provider_edit_->currentData().toString() == "twitch";
    const bool active = twitch_operations_active() || frontend_cleanup_ != FrontendCleanup::None;
    const bool pending = twitch_session_restore_pending_ || twitch_authentication_pending_;
    const bool available = twitch_provider_.authenticated();
    twitch_auth_row_->setVisible(selected && !pending && !available);
    connect_twitch_button_->setVisible(selected && !pending && !available);
    disconnect_twitch_button_->setVisible(selected && !pending && available);
    connect_twitch_button_->setEnabled(selected && !active && !available);
    disconnect_twitch_button_->setEnabled(selected && !active);
}

bool MultistreamDock::twitch_operations_active() const
{
    if (twitch_authentication_pending_ || twitch_session_restore_pending_ || !twitch_operations_.empty())
        return true;
    for (const DestinationConfig &config : manager_->destinations())
        if (is_twitch_destination(config) && manager_->is_busy(config.id))
            return true;
    return false;
}

void MultistreamDock::update_youtube_auth_state()
{
    if (!connect_youtube_button_ || !youtube_auth_row_)
        return;
    const bool selected = provider_edit_ && provider_edit_->currentData().toString() == "youtube";
    const bool active = youtube_operations_active() || frontend_cleanup_ != FrontendCleanup::None;
    const bool pending = youtube_session_restore_pending_ || youtube_authentication_pending_;
    const bool available = youtube_provider_.authenticated();
    youtube_auth_row_->setVisible(selected && !pending && !available);
    connect_youtube_button_->setVisible(selected && !pending && !available);
    disconnect_youtube_button_->setVisible(selected && !pending && available);
    connect_youtube_button_->setEnabled(selected && !active && !available);
    disconnect_youtube_button_->setEnabled(selected && !active);
}

bool MultistreamDock::youtube_operations_active() const
{
    if (youtube_authentication_pending_ || youtube_session_restore_pending_ || !youtube_operations_.empty())
        return true;
    for (const DestinationConfig &config : manager_->destinations())
        if (is_youtube_destination(config) && manager_->is_busy(config.id))
            return true;
    return false;
}

void MultistreamDock::populate_facebook_pages()
{
    const QString selected_page_id = page_edit_->currentData(Qt::UserRole).toString();
    const QString selected_target = page_edit_->currentData(kFacebookTargetRole).toString();
    QSignalBlocker blocker(page_edit_);
    page_edit_->clear();
    page_edit_->addItem(text("FacebookTimelineTarget"), QString{});
    page_edit_->setItemData(0, "timeline", kFacebookTargetRole);
    for (const FacebookPage &page : facebook_provider_.pages()) {
        page_edit_->addItem(QString::fromUtf8(page.name.c_str()), QString::fromUtf8(page.id.c_str()));
        page_edit_->setItemData(page_edit_->count() - 1, "page", kFacebookTargetRole);
    }

    int selected_index = -1;
    if (selected_target == "timeline")
        selected_index = 0;
    else if (!selected_page_id.isEmpty())
        selected_index = page_edit_->findData(selected_page_id, Qt::UserRole);
    if (selected_index >= 0)
        page_edit_->setCurrentIndex(selected_index);
}

void MultistreamDock::restore_facebook_session()
{
    facebook_session_restore_pending_ = true;
    update_facebook_auth_state();
    facebook_provider_.restore_session([this](bool success, const std::string &message) {
        facebook_session_restore_pending_ = false;
        if (!success && !message.empty() && frontend_cleanup_ == FrontendCleanup::None)
            blog(LOG_WARNING, "Facebook session restore failed: %s", message.c_str());
        populate_facebook_pages();
        update_provider_visibility();
        update_recovery_notice();
        refresh_list(true);
    });
}

void MultistreamDock::connect_facebook()
{
    if (facebook_operations_active() || frontend_cleanup_ != FrontendCleanup::None)
        return;
    facebook_authentication_pending_ = true;
    update_facebook_auth_state();
    facebook_provider_.authenticate(this, [this](bool success, const std::string &message) {
        facebook_authentication_pending_ = false;
        if (!success) {
            if (frontend_cleanup_ == FrontendCleanup::None &&
                message != text("FacebookLoginCancelled").toStdString())
                QMessageBox::warning(this, text("DockTitle"), QString::fromUtf8(message.c_str()));
            update_facebook_auth_state();
            maybe_finish_frontend_cleanup();
            return;
        }
        populate_facebook_pages();
        update_provider_visibility();
        update_recovery_notice();
        maybe_finish_frontend_cleanup();
    });
}

void MultistreamDock::disconnect_facebook()
{
    if (facebook_operations_active() || frontend_cleanup_ != FrontendCleanup::None) {
        QMessageBox::information(this, text("DockTitle"), text("StopFacebookBeforeDisconnect"));
        return;
    }
    if (!facebook_provider_.forget_session())
        QMessageBox::warning(this, text("DockTitle"), text("DisconnectFacebookError"));
    populate_facebook_pages();
    update_provider_visibility();
    refresh_list(true);
}

void MultistreamDock::restore_twitch_session()
{
    twitch_session_restore_pending_ = true;
    update_twitch_auth_state();
    twitch_provider_.restore_session([this](bool success, const std::string &message) {
        twitch_session_restore_pending_ = false;
        if (!success && !message.empty() && frontend_cleanup_ == FrontendCleanup::None)
            blog(LOG_WARNING, "Twitch session restore failed: %s", message.c_str());
        update_provider_visibility();
        update_recovery_notice();
        refresh_list(true);
    });
}

void MultistreamDock::connect_twitch()
{
    if (twitch_operations_active() || frontend_cleanup_ != FrontendCleanup::None)
        return;
    twitch_authentication_pending_ = true;
    update_twitch_auth_state();
    twitch_provider_.authenticate(this, [this](bool success, const std::string &message) {
        twitch_authentication_pending_ = false;
        if (!success) {
            if (frontend_cleanup_ == FrontendCleanup::None &&
                message != text("TwitchLoginCancelled").toStdString())
                QMessageBox::warning(this, text("DockTitle"), QString::fromUtf8(message.c_str()));
            update_twitch_auth_state();
            maybe_finish_frontend_cleanup();
            return;
        }
        update_provider_visibility();
        update_recovery_notice();
        refresh_list(true);
        maybe_finish_frontend_cleanup();
    });
}

void MultistreamDock::disconnect_twitch()
{
    if (twitch_operations_active() || frontend_cleanup_ != FrontendCleanup::None) {
        QMessageBox::information(this, text("DockTitle"), text("StopTwitchBeforeDisconnect"));
        return;
    }
    if (!twitch_provider_.forget_session())
        QMessageBox::warning(this, text("DockTitle"), text("DisconnectTwitchError"));
    update_provider_visibility();
    refresh_list(true);
}

void MultistreamDock::restore_youtube_session()
{
    youtube_session_restore_pending_ = true;
    update_youtube_auth_state();
    youtube_provider_.restore_session([this](bool success, const std::string &message) {
        youtube_session_restore_pending_ = false;
        if (!success && !message.empty() && frontend_cleanup_ == FrontendCleanup::None)
            blog(LOG_WARNING, "YouTube session restore failed: %s", message.c_str());
        update_provider_visibility();
        update_recovery_notice();
        refresh_list(true);
    });
}

void MultistreamDock::connect_youtube()
{
    if (youtube_operations_active() || frontend_cleanup_ != FrontendCleanup::None)
        return;
    youtube_authentication_pending_ = true;
    update_youtube_auth_state();
    youtube_provider_.authenticate(this, [this](bool success, const std::string &message) {
        youtube_authentication_pending_ = false;
        if (!success) {
            if (frontend_cleanup_ == FrontendCleanup::None &&
                message != text("YouTubeLoginCancelled").toStdString())
                QMessageBox::warning(this, text("DockTitle"), QString::fromUtf8(message.c_str()));
            update_youtube_auth_state();
            maybe_finish_frontend_cleanup();
            return;
        }
        update_provider_visibility();
        update_recovery_notice();
        refresh_list(true);
        maybe_finish_frontend_cleanup();
    });
}

void MultistreamDock::disconnect_youtube()
{
    if (youtube_operations_active() || frontend_cleanup_ != FrontendCleanup::None) {
        QMessageBox::information(this, text("DockTitle"), text("StopYouTubeBeforeDisconnect"));
        return;
    }
    if (!youtube_provider_.forget_session())
        QMessageBox::warning(this, text("DockTitle"), text("DisconnectYouTubeError"));
    update_provider_visibility();
    refresh_list(true);
}

void MultistreamDock::update_recovery_notice()
{
    if (!recovery_label_ || !recovery_ack_button_)
        return;
    const bool pending = manager_->has_recovery_notice();
    recovery_label_->setText(pending ? text("RecoveryWarning") : QString{});
    recovery_label_->setVisible(pending);
    recovery_ack_button_->setVisible(pending);
    recovery_ack_button_->setEnabled(frontend_cleanup_ == FrontendCleanup::None);
}

void MultistreamDock::save_facebook_session_error(const std::string &id, const std::string &error)
{
    manager_->set_error(id, error);
    if (selected_id_ == id)
        update_editor_status();
}

void MultistreamDock::facebook_start(const DestinationConfig &config)
{
    if (manager_->is_busy(config.id))
        return;
    if (!is_facebook_timeline(config) && !facebook_provider_.has_page(config.facebook_page_id)) {
        save_facebook_session_error(config.id, "Connect Facebook and select the Page before starting.");
        return;
    }
    if (is_facebook_timeline(config) && !facebook_provider_.authenticated()) {
        save_facebook_session_error(config.id, "Connect Facebook before starting a timeline stream.");
        return;
    }

    const DestinationManager::OperationId generation = manager_->prepare_destination(config);
    if (generation == 0)
        return;
    native_generations_[config.id] = generation;
    facebook_operations_[config.id] = {generation, 0, {}};
    manager_->mark_recovery_pending(config);
    update_recovery_notice();

    const FacebookProvider::RequestId request_id = facebook_provider_.create_live(
        config, [this, config, generation](bool success, const FacebookLive &live, const std::string &message) {
        const auto operation = facebook_operations_.find(config.id);
        if (!manager_->is_current(config.id, generation) || operation == facebook_operations_.end() ||
            operation->second.generation != generation) {
            if (success)
                cancel_facebook_operation(config.id, generation, live);
            else {
                const auto stop_generation = manager_->operation_generation(config.id);
                if (manager_->is_current(config.id, stop_generation) &&
                    manager_->state(config.id) == DestinationState::Stopping) {
                    facebook_operations_.erase(config.id);
                    manager_->clear_recovery_pending(config.id);
                    manager_->complete_operation(config.id, stop_generation, DestinationState::Stopped);
                    update_recovery_notice();
                    maybe_finish_frontend_cleanup();
                }
            }
            return;
        }

        if (!success) {
            facebook_operations_.erase(operation);
            manager_->clear_recovery_pending(config.id);
            manager_->complete_operation(config.id, generation, DestinationState::Error, message);
            native_preparation_failed_ = true;
            if (native_preparation_pending_ > 0)
                --native_preparation_pending_;
            update_recovery_notice();
            maybe_start_native_stream();
            return;
        }

        operation->second.request_id = 0;
        operation->second.live_id = live.id;
        DestinationConfig runtime_config = config;
        runtime_config.provider_id = "rtmp_custom";
        runtime_config.rtmp_url = live.server;
        runtime_config.stream_key = live.stream_key;
        if (!manager_->set_prepared_config(config.id, runtime_config, generation)) {
            native_preparation_failed_ = true;
            finalize_facebook_live(config.id, generation, live.id, DestinationState::Error,
                                   "The Facebook stream was created but could not be prepared for OBS.");
        } else {
            native_runtime_configs_[config.id] = runtime_config;
        }
        if (native_preparation_pending_ > 0)
            --native_preparation_pending_;
        maybe_start_native_stream();
    });

    const auto operation = facebook_operations_.find(config.id);
    if (operation != facebook_operations_.end() && operation->second.generation == generation)
        operation->second.request_id = request_id;
}

void MultistreamDock::facebook_stop(const std::string &id)
{
    const auto operation = facebook_operations_.find(id);
    const FacebookProvider::RequestId request_id =
        operation == facebook_operations_.end() ? 0 : operation->second.request_id;
    const std::string live_id = operation == facebook_operations_.end() ? std::string{} : operation->second.live_id;
    const DestinationManager::OperationId generation = manager_->stop(id);
    if (generation == 0)
        return;

    if (operation != facebook_operations_.end()) {
        operation->second.generation = generation;
        operation->second.request_id = 0;
    }
    if (request_id != 0)
        facebook_provider_.cancel_request(request_id);
    if (!live_id.empty())
        finalize_facebook_live(id, generation, live_id, DestinationState::Stopped, {});
}

void MultistreamDock::twitch_start(const DestinationConfig &config)
{
    if (manager_->is_busy(config.id))
        return;
    if (!twitch_provider_.authenticated()) {
        manager_->set_error(config.id, text("TwitchNotConnected").toStdString());
        return;
    }

    const DestinationManager::OperationId generation = manager_->prepare_destination(config);
    if (generation == 0)
        return;
    native_generations_[config.id] = generation;
    twitch_operations_[config.id] = {generation, 0};
    const TwitchProvider::RequestId request_id = twitch_provider_.create_live(
        config, [this, config, generation](bool success, const TwitchLive &live, const std::string &message) {
            const auto operation = twitch_operations_.find(config.id);
            if (!manager_->is_current(config.id, generation) || operation == twitch_operations_.end() ||
                operation->second.generation != generation)
                return;
            if (!success) {
                twitch_operations_.erase(operation);
                manager_->complete_operation(config.id, generation, DestinationState::Error, message);
                native_preparation_failed_ = true;
                if (native_preparation_pending_ > 0)
                    --native_preparation_pending_;
                maybe_start_native_stream();
                return;
            }

            operation->second.request_id = 0;
            DestinationConfig runtime_config = config;
            runtime_config.provider_id = "rtmp_custom";
            runtime_config.rtmp_url = live.server;
            runtime_config.stream_key = live.stream_key;
            if (!manager_->set_prepared_config(config.id, runtime_config, generation)) {
                twitch_operations_.erase(operation);
                manager_->complete_operation(config.id, generation, DestinationState::Error,
                                             text("TwitchPrepareError").toStdString());
                native_preparation_failed_ = true;
            } else {
                native_runtime_configs_[config.id] = runtime_config;
            }
            if (native_preparation_pending_ > 0)
                --native_preparation_pending_;
            maybe_start_native_stream();
        });
    const auto operation = twitch_operations_.find(config.id);
    if (operation != twitch_operations_.end() && operation->second.generation == generation)
        operation->second.request_id = request_id;
}

void MultistreamDock::twitch_stop(const std::string &id)
{
    const auto operation = twitch_operations_.find(id);
    const TwitchProvider::RequestId request_id = operation == twitch_operations_.end() ? 0 : operation->second.request_id;
    if (request_id == 0 && native_session_active_) {
        twitch_operations_.erase(id);
        manager_->clear_recovery_pending(id);
        update_recovery_notice();
        maybe_finish_frontend_cleanup();
        return;
    }

    const DestinationManager::OperationId generation = manager_->stop(id);
    if (generation == 0)
        return;
    if (request_id != 0)
        twitch_provider_.cancel_request(request_id);
    else
        manager_->complete_operation(id, generation, DestinationState::Stopped);
    twitch_operations_.erase(id);
    manager_->clear_recovery_pending(id);
    update_recovery_notice();
    maybe_finish_frontend_cleanup();
}

void MultistreamDock::stop_provider_operations()
{
    std::vector<std::string> facebook_ids;
    facebook_ids.reserve(facebook_operations_.size());
    for (const auto &entry : facebook_operations_)
        facebook_ids.push_back(entry.first);
    for (const std::string &id : facebook_ids)
        facebook_stop(id);

    std::vector<std::string> twitch_ids;
    twitch_ids.reserve(twitch_operations_.size());
    for (const auto &entry : twitch_operations_)
        twitch_ids.push_back(entry.first);
    for (const std::string &id : twitch_ids)
        twitch_stop(id);

    std::vector<std::string> youtube_ids;
    youtube_ids.reserve(youtube_operations_.size());
    for (const auto &entry : youtube_operations_)
        youtube_ids.push_back(entry.first);
    for (const std::string &id : youtube_ids)
        youtube_stop(id);
}

void MultistreamDock::youtube_start(const DestinationConfig &config)
{
    if (manager_->is_busy(config.id))
        return;
    if (!youtube_provider_.authenticated()) {
        manager_->set_error(config.id, text("YouTubeNotConnected").toStdString());
        return;
    }

    const DestinationManager::OperationId generation = manager_->prepare_destination(config);
    if (generation == 0)
        return;
    native_generations_[config.id] = generation;
    youtube_operations_[config.id] = {generation, 0, {}};
    const YouTubeProvider::RequestId request_id = youtube_provider_.create_live(
        config, [this, config, generation](bool success, const YouTubeLive &live, const std::string &message) {
            const auto operation = youtube_operations_.find(config.id);
            if (!manager_->is_current(config.id, generation) || operation == youtube_operations_.end() ||
                operation->second.generation != generation) {
                if (success)
                    youtube_provider_.stop_live(live.broadcast_id, [](bool, const std::string &) {});
                return;
            }
            if (!success) {
                youtube_operations_.erase(operation);
                manager_->complete_operation(config.id, generation, DestinationState::Error, message);
                native_preparation_failed_ = true;
                if (native_preparation_pending_ > 0)
                    --native_preparation_pending_;
                maybe_start_native_stream();
                return;
            }

            operation->second.request_id = 0;
            operation->second.broadcast_id = live.broadcast_id;
            DestinationConfig runtime_config = config;
            runtime_config.provider_id = "rtmp_custom";
            runtime_config.rtmp_url = live.server;
            runtime_config.stream_key = live.stream_key;
            if (!manager_->set_prepared_config(config.id, runtime_config, generation)) {
                native_preparation_failed_ = true;
                finalize_youtube_live(config.id, generation, live.broadcast_id, DestinationState::Error,
                                     text("YouTubePrepareError").toStdString());
            } else {
                native_runtime_configs_[config.id] = runtime_config;
            }
            if (native_preparation_pending_ > 0)
                --native_preparation_pending_;
            maybe_start_native_stream();
        });
    const auto operation = youtube_operations_.find(config.id);
    if (operation != youtube_operations_.end() && operation->second.generation == generation)
        operation->second.request_id = request_id;
}

void MultistreamDock::youtube_stop(const std::string &id)
{
    const auto operation = youtube_operations_.find(id);
    const YouTubeProvider::RequestId request_id =
        operation == youtube_operations_.end() ? 0 : operation->second.request_id;
    const std::string broadcast_id = operation == youtube_operations_.end() ? std::string{}
                                                                             : operation->second.broadcast_id;
    const DestinationManager::OperationId generation = manager_->stop(id);
    if (generation == 0)
        return;
    if (operation != youtube_operations_.end()) {
        operation->second.generation = generation;
        operation->second.request_id = 0;
    }
    if (request_id != 0)
        youtube_provider_.cancel_request(request_id);
    if (!broadcast_id.empty())
        finalize_youtube_live(id, generation, broadcast_id, DestinationState::Stopped, {});
    else {
        youtube_operations_.erase(id);
        manager_->clear_recovery_pending(id);
        manager_->complete_operation(id, generation, DestinationState::Stopped);
        update_recovery_notice();
        maybe_finish_frontend_cleanup();
    }
}

void MultistreamDock::finalize_youtube_live(const std::string &id, DestinationManager::OperationId generation,
                                            const std::string &broadcast_id, DestinationState final_state,
                                            const std::string &existing_error)
{
    if (broadcast_id.empty()) {
        youtube_operations_.erase(id);
        manager_->clear_recovery_pending(id);
        manager_->complete_operation(id, generation, final_state, existing_error);
        update_recovery_notice();
        maybe_finish_frontend_cleanup();
        return;
    }

    manager_->set_operation_state(id, generation, DestinationState::Stopping, existing_error);
    const auto finish = [this, id, generation, final_state, existing_error](bool success,
                                                                            const std::string &message) {
        if (!manager_->is_current(id, generation))
            return;
        std::string error = existing_error;
        if (!success)
            error = error.empty() ? "YouTube live broadcast could not be finalized: " + message
                                  : error + " YouTube live broadcast could not be finalized: " + message;
        youtube_operations_.erase(id);
        if (success) {
            manager_->clear_recovery_pending(id);
            update_recovery_notice();
        }
        manager_->complete_operation(id, generation, success ? final_state : DestinationState::Error, error);
        maybe_finish_frontend_cleanup();
    };

    if (final_state == DestinationState::Error) {
        youtube_provider_.delete_live(
            broadcast_id, [this, broadcast_id, finish](bool deleted, const std::string &delete_message) mutable {
                if (deleted) {
                    finish(true, {});
                    return;
                }
                // If the broadcast already reached live state, deletion may be
                // rejected; complete it as a safe fallback.
                youtube_provider_.stop_live(
                    broadcast_id, [finish, delete_message](bool completed, const std::string &stop_message) mutable {
                        if (completed) {
                            finish(true, {});
                            return;
                        }
                        finish(false, delete_message + " " + stop_message);
                    });
            });
    } else {
        youtube_provider_.stop_live(broadcast_id, finish);
    }
}

void MultistreamDock::cancel_facebook_operation(const std::string &id,
                                                DestinationManager::OperationId generation,
                                                const FacebookLive &live)
{
    (void)generation;
    const DestinationManager::OperationId stop_generation = manager_->operation_generation(id);
    facebook_provider_.stop_live(live.id, [this, id, stop_generation](bool success, const std::string &message) {
        if (!manager_->is_current(id, stop_generation))
            return;
        manager_->complete_operation(id, stop_generation, success ? DestinationState::Stopped : DestinationState::Error,
                                     success ? std::string{} : "Facebook live video could not be finalized: " + message);
        if (success) {
            manager_->clear_recovery_pending(id);
            update_recovery_notice();
        }
        facebook_operations_.erase(id);
        maybe_finish_frontend_cleanup();
    });
}

void MultistreamDock::finalize_facebook_live(const std::string &id, DestinationManager::OperationId generation,
                                             const std::string &live_id, DestinationState final_state,
                                             const std::string &existing_error)
{
    if (live_id.empty()) {
        facebook_operations_.erase(id);
        manager_->clear_recovery_pending(id);
        manager_->complete_operation(id, generation, final_state, existing_error);
        update_recovery_notice();
        maybe_finish_frontend_cleanup();
        return;
    }

    manager_->set_operation_state(id, generation, DestinationState::Stopping, existing_error);
    facebook_provider_.stop_live(live_id, [this, id, generation, final_state, existing_error](bool success,
                                                                                               const std::string &message) {
        if (!manager_->is_current(id, generation))
            return;
        std::string error = existing_error;
        if (!success)
            error = error.empty() ? "Facebook live video could not be finalized: " + message
                                  : error + " Facebook live video could not be finalized: " + message;
        facebook_operations_.erase(id);
        if (success) {
            manager_->clear_recovery_pending(id);
            update_recovery_notice();
        }
        manager_->complete_operation(id, generation, success ? final_state : DestinationState::Error, error);
        maybe_finish_frontend_cleanup();
    });
}

void MultistreamDock::start_facebook_destinations()
{
    for (const DestinationConfig &config : manager_->destinations())
        if (config.enabled && is_facebook_destination(config))
            facebook_start(config);
}
void MultistreamDock::add_destination()
{
    QMenu menu(this);
    const auto has_destination_provider = [this](const char *provider_id) {
        return std::any_of(manager_->destinations().begin(), manager_->destinations().end(),
                           [provider_id](const DestinationConfig &config) {
                               return config.provider_id == provider_id;
                           });
    };
    QAction *facebook_action = has_destination_provider("facebook") ? nullptr : menu.addAction(text("Facebook"));
    QAction *twitch_action = has_destination_provider("twitch") ? nullptr : menu.addAction(text("Twitch"));
    QAction *youtube_action = has_destination_provider("youtube") ? nullptr : menu.addAction(text("YouTube"));
    QAction *custom_action = menu.addAction(text("GenericRtmp"));
    QAction *selected_action = menu.exec(QCursor::pos());
    if (selected_action == facebook_action)
        add_destination_kind("facebook");
    else if (selected_action == twitch_action)
        add_destination_kind("twitch");
    else if (selected_action == youtube_action)
        add_destination_kind("youtube");
    else if (selected_action == custom_action)
        add_destination_kind("rtmp_custom");
}

void MultistreamDock::add_destination_kind(const char *provider_id)
{
    const std::string requested_provider = provider_id;
    if (requested_provider != "rtmp_custom" &&
        std::any_of(manager_->destinations().begin(), manager_->destinations().end(),
                    [&requested_provider](const DestinationConfig &config) {
                        return config.provider_id == requested_provider;
                    })) {
        return;
    }

    DestinationConfig config;
    config.id = generate_destination_id();
    config.provider_id = provider_id;
    config.name = provider_id == std::string("facebook")
                      ? text("FacebookDestinationName").toStdString()
                      : provider_id == std::string("twitch")
                            ? text("TwitchDestinationName").toStdString()
                            : provider_id == std::string("youtube") ? text("YouTubeDestinationName").toStdString()
                                                                       : text("DefaultDestinationName").toStdString();
    config.facebook_title = provider_id == std::string("facebook") ? text("FacebookDefaultTitle").toStdString()
                                                                     : std::string{};
    config.twitch_title = provider_id == std::string("twitch") ? text("TwitchDefaultTitle").toStdString()
                                                                : std::string{};
    config.youtube_title = provider_id == std::string("youtube") ? text("YouTubeDefaultTitle").toStdString()
                                                                  : std::string{};
    config.rtmp_url = (provider_id == std::string("facebook") || provider_id == std::string("twitch") ||
                       provider_id == std::string("youtube"))
                          ? std::string{}
                          : "rtmp://localhost/live";
    config.enabled = true;

    if (!manager_->add(config))
        return;

    selected_id_ = config.id;
    refresh_list(false);
    for (int row = 0; row < destination_list_->count(); ++row) {
        if (destination_list_->item(row)->data(Qt::UserRole).toString().toStdString() == selected_id_) {
            destination_list_->setCurrentRow(row);
            break;
        }
    }

    if (provider_id == std::string("facebook")) {
        update_provider_visibility();
        if (!facebook_provider_.authenticated())
            connect_facebook();
        else
            populate_facebook_pages();
    } else if (provider_id == std::string("twitch")) {
        update_provider_visibility();
        if (!twitch_provider_.authenticated())
            connect_twitch();
    } else if (provider_id == std::string("youtube")) {
        update_provider_visibility();
        if (!youtube_provider_.authenticated())
            connect_youtube();
    } else {
        key_edit_->setFocus();
    }
}

bool MultistreamDock::save_destination()
{
    const DestinationConfig *existing = manager_->find(selected_id_);
    if (!existing)
        return true;

    DestinationConfig config = *existing;
    const bool facebook = is_facebook_destination(config);
    const bool twitch = is_twitch_destination(config);
    const bool youtube = is_youtube_destination(config);
    const bool managed = facebook || twitch || youtube;
    if (!managed) {
        config.name = name_edit_->text().trimmed().toUtf8().constData();
        config.provider_id = provider_edit_->currentData().toString().toUtf8().constData();
    }
    config.rtmp_url = url_edit_->text().trimmed().toUtf8().constData();
    config.stream_key = key_edit_->text().toUtf8().constData();
    config.facebook_page_id = page_edit_->currentData(Qt::UserRole).toString().toUtf8().constData();
    config.facebook_page_name = page_edit_->currentText().toUtf8().constData();
    config.facebook_target = page_edit_->currentData(kFacebookTargetRole).toString().toUtf8().constData();
    if (config.facebook_target == "timeline") {
        config.facebook_page_id.clear();
        config.facebook_page_name.clear();
    } else {
        config.facebook_target = "page";
    }
    config.facebook_title = facebook_title_edit_->text().trimmed().toUtf8().constData();
    config.facebook_description = facebook_description_edit_->toPlainText().trimmed().toUtf8().constData();
    config.facebook_privacy = facebook_privacy_edit_->currentData().toString().toUtf8().constData();
    config.twitch_title = twitch_title_edit_->text().trimmed().toUtf8().constData();
    config.youtube_title = youtube_title_edit_->text().trimmed().toUtf8().constData();
    config.youtube_description = youtube_description_edit_->toPlainText().trimmed().toUtf8().constData();
    config.youtube_privacy = youtube_privacy_edit_->currentData().toString().toUtf8().constData();

    const ValidationResult validation = validate_destination(config);
    if (!validation.valid) {
        QMessageBox::warning(this, text("DockTitle"), text(validation.message_key.c_str()));
        return false;
    }
    if (!manager_->update(config)) {
        QMessageBox::warning(this, text("DockTitle"), text("SaveError"));
        return false;
    }

    refresh_list(true);
    update_editor_status();
    return true;
}

void MultistreamDock::delete_destination()
{
    if (selected_id_.empty())
        return;

    if (QMessageBox::question(this, text("DockTitle"), text("DeleteQuestion")) != QMessageBox::Yes)
        return;

    if (!manager_->remove(selected_id_)) {
        QMessageBox::warning(this, text("DockTitle"), text("SaveError"));
        return;
    }
    selected_id_.clear();
    refresh_list(false);
}

void MultistreamDock::start_all()
{
    if (manager_->has_recovery_notice()) {
        show_and_raise_dock();
        update_recovery_notice();
        return;
    }
    if (native_session_active_ || native_start_requested_ || native_stream_redirect_pending_)
        return;
    if (!save_destination())
        return;

    native_start_requested_ = true;
    native_preparation_failed_ = false;
    native_preparation_pending_ = 0;
    native_primary_id_.clear();
    native_generations_.clear();
    native_runtime_configs_.clear();

    for (const DestinationConfig &config : manager_->destinations()) {
        if (!config.enabled)
            continue;
        if (native_primary_id_.empty())
            native_primary_id_ = config.id;
        if (is_facebook_destination(config)) {
            ++native_preparation_pending_;
            facebook_start(config);
            if (facebook_operations_.find(config.id) == facebook_operations_.end()) {
                if (native_preparation_pending_ > 0)
                    --native_preparation_pending_;
                native_preparation_failed_ = true;
            }
            continue;
        }
        if (is_twitch_destination(config)) {
            ++native_preparation_pending_;
            twitch_start(config);
            if (twitch_operations_.find(config.id) == twitch_operations_.end()) {
                if (native_preparation_pending_ > 0)
                    --native_preparation_pending_;
                native_preparation_failed_ = true;
            }
            continue;
        }
        if (is_youtube_destination(config)) {
            ++native_preparation_pending_;
            youtube_start(config);
            if (youtube_operations_.find(config.id) == youtube_operations_.end()) {
                if (native_preparation_pending_ > 0)
                    --native_preparation_pending_;
                native_preparation_failed_ = true;
            }
            continue;
        }

        const DestinationManager::OperationId generation = manager_->prepare_destination(config);
        if (generation == 0) {
            native_preparation_failed_ = true;
            continue;
        }
        native_generations_[config.id] = generation;
        native_runtime_configs_[config.id] = config;
    }

    if (native_primary_id_.empty()) {
        native_start_requested_ = false;
        return;
    }
    maybe_start_native_stream();
    refresh_list(true);
    update_editor_status();
}

void MultistreamDock::stop_all()
{
    if (native_session_active_ || obs_frontend_streaming_active()) {
        obs_frontend_streaming_stop();
        return;
    }

    if (native_start_requested_) {
        stop_provider_operations();
        manager_->stop_native_secondaries({});
        native_start_requested_ = false;
        native_preparation_pending_ = 0;
        native_preparation_failed_ = false;
        manager_->clear_native_session();
    }
    refresh_list(true);
    update_editor_status();
}

void MultistreamDock::reorder_destinations()
{
    std::vector<std::string> ordered_ids;
    ordered_ids.reserve(static_cast<std::size_t>(destination_list_->count()));
    for (int row = 0; row < destination_list_->count(); ++row)
        ordered_ids.push_back(destination_list_->item(row)->data(Qt::UserRole).toString().toStdString());

    if (!manager_->reorder(ordered_ids))
        refresh_list(true);
}

void MultistreamDock::handle_state_change(const std::string &id, DestinationState state,
                                           const std::string &error,
                                           DestinationManager::OperationId generation)
{
    QMetaObject::invokeMethod(this, [this, id, state, error, generation]() {
        if (!manager_->find(id) || !manager_->is_current(id, generation))
            return;

        const auto operation = facebook_operations_.find(id);
        if (operation != facebook_operations_.end() && operation->second.generation == generation &&
            (state == DestinationState::Stopped || state == DestinationState::Error)) {
            const std::string live_id = operation->second.live_id;
            if (!live_id.empty()) {
                finalize_facebook_live(id, generation, live_id, state, error);
                return;
            }
            facebook_operations_.erase(operation);
        }

        const auto twitch_operation = twitch_operations_.find(id);
        if (twitch_operation != twitch_operations_.end() && twitch_operation->second.generation == generation &&
            (state == DestinationState::Stopped || state == DestinationState::Error))
            twitch_operations_.erase(twitch_operation);

        const auto youtube_operation = youtube_operations_.find(id);
        if (youtube_operation != youtube_operations_.end() && youtube_operation->second.generation == generation &&
            (state == DestinationState::Stopped || state == DestinationState::Error)) {
            const std::string broadcast_id = youtube_operation->second.broadcast_id;
            if (!broadcast_id.empty()) {
                finalize_youtube_live(id, generation, broadcast_id, state, error);
                return;
            }
            youtube_operations_.erase(youtube_operation);
        }

        refresh_list(true);
        if (selected_id_ == id) {
            status_label_->setText(state_text(state));
            error_label_->setText(QString::fromUtf8(error.c_str()));
            error_label_->setVisible(!error.empty());
            update_editor_status();
        }
        update_facebook_auth_state();
        update_twitch_auth_state();
        update_youtube_auth_state();
        update_recovery_notice();
        maybe_finish_frontend_cleanup();
        finish_native_session_if_ready();
    }, Qt::QueuedConnection);
}

bool MultistreamDock::cleanup_barrier_empty() const
{
    if (facebook_authentication_pending_ || facebook_session_restore_pending_ || !facebook_operations_.empty() ||
        twitch_authentication_pending_ || twitch_session_restore_pending_ || !twitch_operations_.empty() ||
        youtube_authentication_pending_ || youtube_session_restore_pending_ || !youtube_operations_.empty())
        return false;
    for (const DestinationConfig &config : manager_->destinations())
        if (manager_->is_busy(config.id))
            return false;
    return true;
}

void MultistreamDock::request_frontend_cleanup(FrontendCleanup cleanup)
{
    if (cleanup == FrontendCleanup::Exit || frontend_cleanup_ == FrontendCleanup::None)
        frontend_cleanup_ = cleanup;
    if (frontend_cleanup_ == FrontendCleanup::None)
        return;

    QTimer::singleShot(20000, this, [this]() { cleanup_timeout(); });

    if (native_session_active_ || obs_frontend_streaming_active())
        obs_frontend_streaming_stop();

    std::vector<std::string> facebook_ids;
    facebook_ids.reserve(facebook_operations_.size());
    for (const auto &entry : facebook_operations_)
        facebook_ids.push_back(entry.first);
    for (const std::string &id : facebook_ids)
        facebook_stop(id);
    for (const DestinationConfig &config : manager_->destinations())
        if (is_facebook_destination(config) && manager_->is_busy(config.id))
            facebook_stop(config.id);

    std::vector<std::string> twitch_ids;
    twitch_ids.reserve(twitch_operations_.size());
    for (const auto &entry : twitch_operations_)
        twitch_ids.push_back(entry.first);
    for (const std::string &id : twitch_ids)
        twitch_stop(id);
    for (const DestinationConfig &config : manager_->destinations())
        if (is_twitch_destination(config) && manager_->is_busy(config.id))
            twitch_stop(config.id);

    std::vector<std::string> youtube_ids;
    youtube_ids.reserve(youtube_operations_.size());
    for (const auto &entry : youtube_operations_)
        youtube_ids.push_back(entry.first);
    for (const std::string &id : youtube_ids)
        youtube_stop(id);
    for (const DestinationConfig &config : manager_->destinations())
        if (is_youtube_destination(config) && manager_->is_busy(config.id))
            youtube_stop(config.id);

    manager_->stop_all();
    if (facebook_authentication_pending_ || facebook_session_restore_pending_)
        facebook_provider_.cancel_pending_requests();
    if (twitch_authentication_pending_ || twitch_session_restore_pending_)
        twitch_provider_.cancel_pending_requests();
    if (youtube_authentication_pending_ || youtube_session_restore_pending_)
        youtube_provider_.cancel_pending_requests();
    update_facebook_auth_state();
    update_twitch_auth_state();
    update_youtube_auth_state();
    update_recovery_notice();
    maybe_finish_frontend_cleanup();
}

void MultistreamDock::cleanup_timeout()
{
    if (frontend_cleanup_ == FrontendCleanup::None || cleanup_barrier_empty())
        return;

    blog(LOG_WARNING, "Multistream cleanup timed out; Facebook/Twitch live recovery may require manual verification");
    facebook_provider_.cancel_pending_requests();
    twitch_provider_.cancel_pending_requests();
    youtube_provider_.cancel_pending_requests();
    facebook_operations_.clear();
    twitch_operations_.clear();
    youtube_operations_.clear();
    facebook_authentication_pending_ = false;
    facebook_session_restore_pending_ = false;
    twitch_authentication_pending_ = false;
    twitch_session_restore_pending_ = false;
    youtube_authentication_pending_ = false;
    youtube_session_restore_pending_ = false;
    const FrontendCleanup cleanup = frontend_cleanup_;
    frontend_cleanup_ = FrontendCleanup::None;
    if (cleanup == FrontendCleanup::ProfileChanged) {
        manager_->shutdown();
        selected_id_.clear();
        manager_->load();
        refresh_list(false);
    } else {
        manager_->shutdown();
    }
    update_facebook_auth_state();
    update_twitch_auth_state();
    update_youtube_auth_state();
    update_recovery_notice();
}

void MultistreamDock::maybe_finish_frontend_cleanup()
{
    if (frontend_cleanup_ == FrontendCleanup::None || !cleanup_barrier_empty())
        return;

    const FrontendCleanup cleanup = frontend_cleanup_;
    frontend_cleanup_ = FrontendCleanup::None;
    facebook_provider_.clear_session();
    twitch_provider_.clear_session();
    youtube_provider_.clear_session();
    facebook_authentication_pending_ = false;
    facebook_session_restore_pending_ = false;
    twitch_authentication_pending_ = false;
    twitch_session_restore_pending_ = false;
    youtube_authentication_pending_ = false;
    youtube_session_restore_pending_ = false;

    if (cleanup == FrontendCleanup::ProfileChanged) {
        selected_id_.clear();
        manager_->load();
        refresh_list(false);
    } else if (cleanup == FrontendCleanup::Exit) {
        manager_->shutdown();
    }
    update_facebook_auth_state();
    update_twitch_auth_state();
    update_youtube_auth_state();
    update_recovery_notice();
}

void MultistreamDock::maybe_start_native_stream()
{
    if (!native_start_requested_ || native_preparation_pending_ > 0)
        return;
    if (native_primary_id_.empty())
        return;

    if (native_preparation_failed_) {
        native_start_requested_ = false;
        stop_provider_operations();
        manager_->stop_native_secondaries({});
        manager_->clear_native_session();
        refresh_list(true);
        update_editor_status();
        return;
    }

    const auto primary = native_runtime_configs_.find(native_primary_id_);
    if (primary == native_runtime_configs_.end() || !configure_native_service(primary->second)) {
        native_preparation_failed_ = true;
        maybe_start_native_stream();
        return;
    }

    native_start_authorized_ = true;
    native_start_requested_ = false;
    obs_frontend_streaming_start();
}

bool MultistreamDock::configure_native_service(const DestinationConfig &config)
{
    if (!previous_streaming_service_)
        previous_streaming_service_ = obs_frontend_get_streaming_service();

    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "server", config.rtmp_url.c_str());
    obs_data_set_string(settings, "key", config.stream_key.c_str());
    obs_service_t *service = obs_service_create("rtmp_custom", "Multistream", settings, nullptr);
    obs_data_release(settings);
    if (!service)
        return false;

    obs_frontend_set_streaming_service(service);
    obs_service_release(service);
    return true;
}

void MultistreamDock::restore_native_service()
{
    if (!previous_streaming_service_)
        return;
    obs_frontend_set_streaming_service(previous_streaming_service_);
    obs_service_release(previous_streaming_service_);
    previous_streaming_service_ = nullptr;
}

void MultistreamDock::finish_native_session_if_ready()
{
    if (native_primary_id_.empty() || !facebook_operations_.empty() || !twitch_operations_.empty() ||
        !youtube_operations_.empty())
        return;
    for (const DestinationConfig &config : manager_->destinations())
        if (manager_->is_busy(config.id))
            return;

    native_session_active_ = false;
    native_start_requested_ = false;
    native_start_authorized_ = false;
    native_preparation_pending_ = 0;
    native_preparation_failed_ = false;
    native_primary_id_.clear();
    native_generations_.clear();
    native_runtime_configs_.clear();
    manager_->clear_native_session();
    restore_native_service();
    refresh_list(true);
    update_editor_status();
}

void MultistreamDock::show_and_raise_dock()
{
    constexpr int kDockWidth = 1200;
    constexpr int kDockHeight = 800;
    const QSize fixed_size(kDockWidth, kDockHeight);

    if (!floating_window_) {
        QDockWidget *dock_widget = nullptr;
        for (QWidget *parent = parentWidget(); parent; parent = parent->parentWidget()) {
            dock_widget = qobject_cast<QDockWidget *>(parent);
            if (dock_widget)
                break;
        }
        if (dock_widget)
            dock_widget->setWidget(nullptr);

        floating_window_ = new QDialog(nullptr, Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                                                  Qt::WindowCloseButtonHint);
        floating_window_->setObjectName("obs-multistream-rtmp-window");
        floating_window_->setWindowTitle(text("DockTitle"));
        floating_window_->setModal(false);
        floating_window_->setAttribute(Qt::WA_DeleteOnClose, false);
        floating_window_->setFixedSize(fixed_size);

        auto *layout = new QVBoxLayout(floating_window_);
        layout->setContentsMargins(0, 0, 0, 0);
        setParent(floating_window_);
        setFixedSize(fixed_size);
        layout->addWidget(this);
        show();
        if (dock_widget)
            dock_widget->hide();
    }

    floating_window_->setFixedSize(fixed_size);
    setFixedSize(fixed_size);

    // The dock is intentionally shown as a separate top-level window. Select
    // the screen occupied by OBS, rather than using the window manager's last
    // screen/primary-screen choice. This matters in multi-monitor setups.
    QScreen *obs_screen = nullptr;
    if (auto *main_window = static_cast<QWidget *>(obs_frontend_get_main_window()))
        obs_screen = main_window->screen();
    if (!obs_screen)
        obs_screen = QApplication::primaryScreen();

    // Create the native window handle before selecting its screen. This avoids
    // the window briefly appearing on the primary monitor before being moved.
    floating_window_->createWinId();
    if (obs_screen && floating_window_->windowHandle() &&
        floating_window_->windowHandle()->screen() != obs_screen)
        floating_window_->windowHandle()->setScreen(obs_screen);

    const QRect available = obs_screen ? obs_screen->availableGeometry() : QRect(0, 0, 1280, 800);
    floating_window_->move(available.center() - QPoint(fixed_size.width() / 2, fixed_size.height() / 2));
    floating_window_->show();
    floating_window_->raise();
    floating_window_->activateWindow();
    setFocus(Qt::OtherFocusReason);
}

void MultistreamDock::hide_dock()
{
    hide();
    if (floating_window_) {
        floating_window_->hide();
        return;
    }
    for (QWidget *parent = parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto *dock_widget = qobject_cast<QDockWidget *>(parent)) {
            dock_widget->hide();
            return;
        }
    }
}

void MultistreamDock::on_frontend_event(enum obs_frontend_event event)
{
    switch (event) {
    case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
        request_frontend_cleanup(FrontendCleanup::ProfileChanged);
        break;
    case OBS_FRONTEND_EVENT_STREAMING_STARTING:
        if (!native_start_authorized_) {
            show_and_raise_dock();
            native_stream_redirect_pending_ = true;
            if (!native_stream_stop_requested_) {
                native_stream_stop_requested_ = true;
                // OBS does not expose a public pre-start veto. Cancel the
                // initial native start and let the plugin prepare the session.
                obs_frontend_streaming_stop();
            }
        } else {
            native_start_authorized_ = false;
        }
        break;
    case OBS_FRONTEND_EVENT_STREAMING_STARTED:
        if (native_stream_redirect_pending_) {
            show_and_raise_dock();
            if (obs_frontend_streaming_active())
                obs_frontend_streaming_stop();
            break;
        }
        if (!native_session_active_ && !native_primary_id_.empty()) {
            native_session_active_ = true;
            obs_output_t *native_output = obs_frontend_get_streaming_output();
            const auto generation = native_generations_.find(native_primary_id_);
            if (generation != native_generations_.end()) {
                manager_->set_native_primary_state(native_primary_id_, generation->second,
                                                   DestinationState::Streaming);
                const bool secondary_success =
                    manager_->start_native_secondaries(native_output, native_primary_id_);
                if (!secondary_success)
                    obs_frontend_streaming_stop();
            }
            if (native_output)
                obs_output_release(native_output);
            hide_dock();
            refresh_list(true);
            update_editor_status();
        }
        break;
    case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
        if (native_session_active_) {
            stop_provider_operations();
            manager_->stop_native_secondaries(native_primary_id_);
            const auto generation = native_generations_.find(native_primary_id_);
            if (generation != native_generations_.end() &&
                facebook_operations_.find(native_primary_id_) == facebook_operations_.end() &&
                twitch_operations_.find(native_primary_id_) == twitch_operations_.end() &&
                youtube_operations_.find(native_primary_id_) == youtube_operations_.end())
                manager_->set_native_primary_state(native_primary_id_, generation->second,
                                                   DestinationState::Stopping);
        }
        break;
    case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
        if (native_stream_redirect_pending_) {
            native_stream_redirect_pending_ = false;
            native_stream_stop_requested_ = false;
            show_and_raise_dock();
            refresh_list(true);
            update_editor_status();
        } else if (native_session_active_) {
            const auto generation = native_generations_.find(native_primary_id_);
            if (generation != native_generations_.end() &&
                facebook_operations_.find(native_primary_id_) == facebook_operations_.end() &&
                twitch_operations_.find(native_primary_id_) == twitch_operations_.end() &&
                youtube_operations_.find(native_primary_id_) == youtube_operations_.end())
                manager_->set_native_primary_state(native_primary_id_, generation->second,
                                                   DestinationState::Stopped);
            finish_native_session_if_ready();
        } else if (!native_primary_id_.empty()) {
            native_start_requested_ = false;
            native_start_authorized_ = false;
            native_preparation_pending_ = 0;
            stop_provider_operations();
            manager_->stop_native_secondaries(native_primary_id_);
            const auto generation = native_generations_.find(native_primary_id_);
            if (generation != native_generations_.end() &&
                facebook_operations_.find(native_primary_id_) == facebook_operations_.end() &&
                twitch_operations_.find(native_primary_id_) == twitch_operations_.end() &&
                youtube_operations_.find(native_primary_id_) == youtube_operations_.end())
                manager_->set_native_primary_state(native_primary_id_, generation->second,
                                                   DestinationState::Stopped);
            show_and_raise_dock();
            finish_native_session_if_ready();
            refresh_list(true);
            update_editor_status();
        }
        break;
    case OBS_FRONTEND_EVENT_EXIT:
        request_frontend_cleanup(FrontendCleanup::Exit);
        break;
    default:
        break;
    }
}

void MultistreamDock::frontend_event_callback(enum obs_frontend_event event, void *private_data)
{
    auto *dock = static_cast<MultistreamDock *>(private_data);
    if (dock)
        dock->on_frontend_event(event);
}

} // namespace multistream
