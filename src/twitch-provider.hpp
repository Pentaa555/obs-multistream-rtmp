// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "destination.hpp"
#include "loopback-oauth.hpp"

#include <QPointer>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class QDialog;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QWidget;

namespace multistream {

struct TwitchLive {
    std::string server;
    std::string stream_key;
};

class TwitchProvider final {
public:
    using RequestId = std::uint64_t;
    using ResultCallback = std::function<void(bool success, const std::string &message)>;
    using LiveCallback = std::function<void(bool success, const TwitchLive &, const std::string &message)>;

    struct Options {
        QString client_id;
        QString api_base_url = QStringLiteral("https://api.twitch.tv/helix");
        QString auth_base_url = QStringLiteral("https://id.twitch.tv/oauth2/authorize");
        QString ingest_url = QStringLiteral("rtmps://live.twitch.tv/app/");
        int transfer_timeout_ms = 15000;
    };

    TwitchProvider();
    explicit TwitchProvider(Options options);
    ~TwitchProvider();

    TwitchProvider(const TwitchProvider &) = delete;
    TwitchProvider &operator=(const TwitchProvider &) = delete;

    void authenticate(QWidget *parent, ResultCallback callback);
    bool restore_session(ResultCallback callback);
    bool forget_session(bool cancel_requests = true);
    RequestId create_live(const DestinationConfig &config, LiveCallback callback);
    void cancel_request(RequestId request_id);
    void cancel_pending_requests();
    void clear_session(bool cancel_requests = true);

    bool authenticated() const { return !user_token_.empty(); }
    const std::string &account_id() const { return account_id_; }
    const std::string &account_name() const { return account_name_; }
    const QString &client_id() const { return options_.client_id; }

private:
    friend class TwitchProviderTestAccess;

    void fetch_account(ResultCallback callback);
    void finish_session(ResultCallback callback, bool persist_token);
    bool persist_token(std::string &error) const;
    void update_channel(const DestinationConfig &config, RequestId operation_id, ResultCallback callback);
    RequestId fetch_stream_key(RequestId operation_id, LiveCallback callback);
    void invalidate_persisted_token();
    RequestId track_reply(QNetworkReply *reply);
    void untrack_reply(QNetworkReply *reply);
    QNetworkRequest make_request(const QUrl &url) const;

    std::unique_ptr<QNetworkAccessManager> network_;
    Options options_;
    std::unique_ptr<LoopbackOAuthServer> oauth_server_;
    std::string user_token_;
    std::string oauth_state_;
    std::string account_id_;
    std::string account_name_;
    std::unordered_map<RequestId, QNetworkReply *> pending_replies_;
    std::unordered_map<RequestId, RequestId> chained_requests_;
    RequestId next_request_id_ = 1;
    std::uint64_t session_generation_ = 1;
};

} // namespace multistream
