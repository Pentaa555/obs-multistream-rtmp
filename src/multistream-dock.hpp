// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "destination-manager.hpp"
#include "facebook-provider.hpp"
#include "twitch-provider.hpp"
#include "youtube-provider.hpp"

#include <obs-frontend-api.h>

#include <QWidget>

#include <memory>
#include <string>
#include <unordered_map>

class QCheckBox;
class QComboBox;
class QDialog;
class QEvent;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPoint;
class QPushButton;
class QTextEdit;

namespace multistream {

class MultistreamDock final : public QWidget {
public:
    explicit MultistreamDock(QWidget *parent = nullptr);
    ~MultistreamDock() override;

    void open_dock();

    MultistreamDock(const MultistreamDock &) = delete;
    MultistreamDock &operator=(const MultistreamDock &) = delete;

private:
    enum class FrontendCleanup { None, ProfileChanged, Exit };

    struct FacebookOperation {
        DestinationManager::OperationId generation = 0;
        FacebookProvider::RequestId request_id = 0;
        std::string live_id;
    };

    struct TwitchOperation {
        DestinationManager::OperationId generation = 0;
        TwitchProvider::RequestId request_id = 0;
    };

    struct YouTubeOperation {
        DestinationManager::OperationId generation = 0;
        YouTubeProvider::RequestId request_id = 0;
        std::string broadcast_id;
    };

    void build_ui();
    void refresh_list(bool preserve_editor);
    void update_dashboard_summary();
    void show_and_raise_dock();
    void hide_dock();
    void maybe_start_native_stream();
    bool configure_native_service(const DestinationConfig &config);
    void restore_native_service();
    void finish_native_session_if_ready();
    void populate_editor(const std::string &id);
    void clear_editor();
    void update_editor_status();
    void update_facebook_auth_state();
    void update_twitch_auth_state();
    void update_youtube_auth_state();
    bool facebook_operations_active() const;
    bool twitch_operations_active() const;
    bool youtube_operations_active() const;
    void handle_state_change(const std::string &id, DestinationState state, const std::string &error,
                             DestinationManager::OperationId generation);
    void finalize_facebook_live(const std::string &id, DestinationManager::OperationId generation,
                                const std::string &live_id, DestinationState final_state,
                                const std::string &existing_error = {});
    void cancel_facebook_operation(const std::string &id, DestinationManager::OperationId generation,
                                   const FacebookLive &live);
    void request_frontend_cleanup(FrontendCleanup cleanup);
    void maybe_finish_frontend_cleanup();
    void cleanup_timeout();
    bool cleanup_barrier_empty() const;
    void update_recovery_notice();

    void connect_facebook();
    void disconnect_facebook();
    void restore_facebook_session();
    void connect_twitch();
    void disconnect_twitch();
    void restore_twitch_session();
    void connect_youtube();
    void disconnect_youtube();
    void restore_youtube_session();
    void show_destination_context_menu(const QPoint &position);
    void populate_facebook_pages();
    void facebook_start(const DestinationConfig &config);
    void facebook_stop(const std::string &id);
    void twitch_start(const DestinationConfig &config);
    void twitch_stop(const std::string &id);
    void youtube_start(const DestinationConfig &config);
    void youtube_stop(const std::string &id);
    void stop_provider_operations();
    void finalize_youtube_live(const std::string &id, DestinationManager::OperationId generation,
                               const std::string &broadcast_id, DestinationState final_state,
                               const std::string &existing_error = {});
    void update_provider_visibility();
    void start_facebook_destinations();
    void save_facebook_session_error(const std::string &id, const std::string &error);
    void add_destination();
    void add_destination_kind(const char *provider_id);
    bool save_destination();
    void delete_destination();
    void start_all();
    void stop_all();
    void reorder_destinations();
    void selection_changed(int row);
    bool eventFilter(QObject *watched, QEvent *event) override;
    void on_frontend_event(enum obs_frontend_event event);

    static void frontend_event_callback(enum obs_frontend_event event, void *private_data);

    std::unique_ptr<DestinationManager> manager_;
    QListWidget *destination_list_ = nullptr;
    QLineEdit *name_edit_ = nullptr;
    QLabel *name_label_ = nullptr;
    QComboBox *provider_edit_ = nullptr;
    QLabel *provider_label_ = nullptr;
    QLabel *page_label_ = nullptr;
    QComboBox *page_edit_ = nullptr;
    QLabel *facebook_title_label_ = nullptr;
    QLineEdit *facebook_title_edit_ = nullptr;
    QLabel *facebook_description_label_ = nullptr;
    QTextEdit *facebook_description_edit_ = nullptr;
    QLabel *facebook_privacy_label_ = nullptr;
    QComboBox *facebook_privacy_edit_ = nullptr;
    QLabel *twitch_title_label_ = nullptr;
    QLineEdit *twitch_title_edit_ = nullptr;
    QLabel *youtube_title_label_ = nullptr;
    QLineEdit *youtube_title_edit_ = nullptr;
    QLabel *youtube_description_label_ = nullptr;
    QTextEdit *youtube_description_edit_ = nullptr;
    QLabel *youtube_privacy_label_ = nullptr;
    QComboBox *youtube_privacy_edit_ = nullptr;
    QPushButton *connect_facebook_button_ = nullptr;
    QPushButton *disconnect_facebook_button_ = nullptr;
    QPushButton *connect_twitch_button_ = nullptr;
    QPushButton *disconnect_twitch_button_ = nullptr;
    QPushButton *connect_youtube_button_ = nullptr;
    QPushButton *disconnect_youtube_button_ = nullptr;
    QWidget *facebook_auth_row_ = nullptr;
    QWidget *twitch_auth_row_ = nullptr;
    QWidget *youtube_auth_row_ = nullptr;
    QLabel *url_label_ = nullptr;
    QLineEdit *url_edit_ = nullptr;
    QLabel *key_label_ = nullptr;
    QLineEdit *key_edit_ = nullptr;
    QGroupBox *settings_group_ = nullptr;
    QLabel *empty_editor_label_ = nullptr;
    QLabel *status_label_ = nullptr;
    QLabel *error_label_ = nullptr;
    QLabel *recovery_label_ = nullptr;
    QPushButton *recovery_ack_button_ = nullptr;
    QPushButton *start_all_button_ = nullptr;
    QWidget *facebook_platform_row_ = nullptr;
    QWidget *twitch_platform_row_ = nullptr;
    QWidget *youtube_platform_row_ = nullptr;
    QLabel *summary_destinations_value_ = nullptr;
    QLabel *summary_enabled_value_ = nullptr;
    QLabel *summary_live_value_ = nullptr;
    QLabel *summary_status_value_ = nullptr;
    std::string selected_id_;
    FacebookProvider facebook_provider_;
    TwitchProvider twitch_provider_;
    YouTubeProvider youtube_provider_;
    std::unordered_map<std::string, FacebookOperation> facebook_operations_;
    std::unordered_map<std::string, TwitchOperation> twitch_operations_;
    std::unordered_map<std::string, YouTubeOperation> youtube_operations_;
    FrontendCleanup frontend_cleanup_ = FrontendCleanup::None;
    bool facebook_authentication_pending_ = false;
    bool facebook_session_restore_pending_ = false;
    bool twitch_authentication_pending_ = false;
    bool twitch_session_restore_pending_ = false;
    bool youtube_authentication_pending_ = false;
    bool youtube_session_restore_pending_ = false;
    bool native_stream_redirect_pending_ = false;
    bool native_stream_stop_requested_ = false;
    bool native_start_requested_ = false;
    bool native_start_authorized_ = false;
    bool native_session_active_ = false;
    bool native_preparation_failed_ = false;
    int native_preparation_pending_ = 0;
    std::string native_primary_id_;
    std::unordered_map<std::string, DestinationManager::OperationId> native_generations_;
    std::unordered_map<std::string, DestinationConfig> native_runtime_configs_;
    obs_service_t *previous_streaming_service_ = nullptr;
    QDialog *floating_window_ = nullptr;
};

} // namespace multistream
