// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "destination.hpp"
#include "loopback-oauth.hpp"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QWidget;

namespace multistream {

struct YouTubeLive {
    std::string broadcast_id;
    std::string stream_id;
    std::string server;
    std::string stream_key;
};

// YouTube Live integration. OAuth uses the authorization-code + PKCE flow via the
// system browser (RFC 8252) and a loopback redirect. The refresh token is the
// persistent credential stored in the OS secure store; the short-lived access
// token is refreshed on demand and kept only in memory.
class YouTubeProvider final {
public:
    using RequestId = std::uint64_t;
    using ResultCallback = std::function<void(bool success, const std::string &message)>;
    using LiveCallback = std::function<void(bool success, const YouTubeLive &, const std::string &message)>;

    struct Options {
        QString client_id;
        QString client_secret;
        QString auth_base_url = QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth");
        QString token_url = QStringLiteral("https://oauth2.googleapis.com/token");
        QString api_base_url = QStringLiteral("https://www.googleapis.com/youtube/v3");
        QString scope = QStringLiteral("https://www.googleapis.com/auth/youtube");
        int transfer_timeout_ms = 15000;
    };

    YouTubeProvider();
    explicit YouTubeProvider(Options options);
    ~YouTubeProvider();

    YouTubeProvider(const YouTubeProvider &) = delete;
    YouTubeProvider &operator=(const YouTubeProvider &) = delete;

    void authenticate(QWidget *parent, ResultCallback callback);
    bool restore_session(ResultCallback callback);
    bool forget_session(bool cancel_requests = true);
    RequestId create_live(const DestinationConfig &config, LiveCallback callback);
    RequestId stop_live(const std::string &broadcast_id, ResultCallback callback);
    RequestId delete_live(const std::string &broadcast_id, ResultCallback callback);
    void cancel_request(RequestId request_id);
    void cancel_pending_requests();
    void clear_session(bool cancel_requests = true);

    bool authenticated() const { return !refresh_token_.empty() || !access_token_.empty(); }
    const std::string &account_id() const { return channel_id_; }
    const std::string &account_name() const { return channel_title_; }
    const QString &client_id() const { return options_.client_id; }

private:
    friend class YouTubeProviderTestAccess;

    void exchange_code(const std::string &code, const std::string &verifier, const std::string &redirect_uri,
                       ResultCallback callback);
    void refresh_access_token(ResultCallback callback);
    void fetch_channel(ResultCallback callback);
    void finish_session(ResultCallback callback, bool persist_refresh);
    bool persist_refresh_token(std::string &error) const;
    void invalidate_persisted_token();

    // create_live sub-steps.
    void insert_broadcast(const DestinationConfig &config, RequestId operation_id, LiveCallback callback);
    void insert_stream(const DestinationConfig &config, RequestId operation_id, std::string broadcast_id,
                       LiveCallback callback);
    void bind_stream(RequestId operation_id, std::string broadcast_id, YouTubeLive live, LiveCallback callback);

    QNetworkRequest make_request(const QUrl &url, bool json_body) const;
    RequestId track_reply(QNetworkReply *reply);
    void untrack_reply(QNetworkReply *reply);

    std::unique_ptr<QNetworkAccessManager> network_;
    Options options_;
    std::unique_ptr<LoopbackOAuthServer> oauth_server_;
    std::string access_token_;
    std::string refresh_token_;
    std::string oauth_state_;
    std::string channel_id_;
    std::string channel_title_;
    std::unordered_map<RequestId, QNetworkReply *> pending_replies_;
    RequestId next_request_id_ = 1;
    std::uint64_t session_generation_ = 1;
};

} // namespace multistream
