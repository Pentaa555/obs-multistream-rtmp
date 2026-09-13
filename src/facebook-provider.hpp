// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "destination.hpp"
#include "loopback-oauth.hpp"

#include <QPointer>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class QDialog;
class QNetworkAccessManager;
class QNetworkReply;
class QWidget;

namespace multistream {

struct FacebookPage {
    std::string id;
    std::string name;
};

struct FacebookLive {
    std::string id;
    std::string server;
    std::string stream_key;
};

// Facebook desktop integration uses the implicit OAuth flow. The token is kept in
// memory while active and persisted only through the operating system's secure
// credential store; it is never written to OBS profiles or plugin files.
class FacebookProvider final {
public:
    using RequestId = std::uint64_t;
    using ResultCallback = std::function<void(bool success, const std::string &message)>;
    using LiveCallback = std::function<void(bool success, const FacebookLive &, const std::string &message)>;

    struct Options {
        QString graph_base_url = QStringLiteral("https://graph.facebook.com/v26.0");
        int transfer_timeout_ms = 15000;
        int max_retries = 2;
        int retry_backoff_ms = 250;
    };

    FacebookProvider();
    explicit FacebookProvider(Options options);
    ~FacebookProvider();

    FacebookProvider(const FacebookProvider &) = delete;
    FacebookProvider &operator=(const FacebookProvider &) = delete;

    void authenticate(QWidget *parent, ResultCallback callback);
    bool restore_session(ResultCallback callback);
    bool forget_session(bool cancel_requests = true);
    RequestId create_live(const DestinationConfig &config, LiveCallback callback);
    RequestId stop_live(const std::string &live_id, ResultCallback callback);
    void cancel_request(RequestId request_id);
    void cancel_pending_requests();

    const std::vector<FacebookPage> &pages() const { return pages_; }
    bool authenticated() const { return !user_token_.empty(); }
    const std::string &account_id() const { return account_id_; }
    const std::string &account_name() const { return account_name_; }
    bool has_page(const std::string &page_id) const;
    void clear_session(bool cancel_requests = true);

private:
    friend class FacebookProviderTestAccess;

    void fetch_account(ResultCallback callback);
    void finish_session(ResultCallback callback, bool persist_token);
    bool persist_token(std::string &error) const;
    void fetch_pages(ResultCallback callback);
    void fetch_pages_page(QUrl url, std::uint64_t request_generation,
                          std::vector<FacebookPage> fetched_pages,
                          std::unordered_map<std::string, std::string> fetched_page_tokens,
                          int retry_count, ResultCallback callback);
    bool is_safe_pagination_url(const QUrl &url) const;
    bool page_token(const std::string &page_id, std::string &token) const;
    RequestId track_reply(QNetworkReply *reply);
    void untrack_reply(QNetworkReply *reply);

    std::unique_ptr<QNetworkAccessManager> network_;
    Options options_;
    std::unique_ptr<LoopbackOAuthServer> oauth_server_;
    std::string user_token_;
    std::string oauth_state_;
    std::string account_id_;
    std::string account_name_;
    std::vector<FacebookPage> pages_;
    std::unordered_map<std::string, std::string> page_tokens_;
    std::unordered_map<std::string, std::string> live_tokens_;
    std::unordered_map<RequestId, QNetworkReply *> pending_replies_;
    RequestId next_request_id_ = 1;
    std::uint64_t session_generation_ = 1;
};

} // namespace multistream
