// SPDX-License-Identifier: GPL-2.0-or-later

#include "twitch-provider.hpp"
#include "secure-token-store.hpp"
#include "twitch-utils.hpp"

#include <obs-module.h>

#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <utility>

namespace multistream {
namespace {

QString text(const char *key)
{
    return QString::fromUtf8(obs_module_text(key));
}

} // namespace

TwitchProvider::TwitchProvider() : TwitchProvider(Options{}) {}

TwitchProvider::TwitchProvider(Options options)
    : network_(std::make_unique<QNetworkAccessManager>()), options_(std::move(options))
{
#ifdef TWITCH_CLIENT_ID
    if (options_.client_id.isEmpty())
        options_.client_id = QStringLiteral(TWITCH_CLIENT_ID);
#endif
}

TwitchProvider::~TwitchProvider()
{
    cancel_pending_requests();
}

TwitchProvider::RequestId TwitchProvider::track_reply(QNetworkReply *reply)
{
    if (!reply)
        return 0;
    if (next_request_id_ == 0)
        next_request_id_ = 1;
    const RequestId id = next_request_id_++;
    pending_replies_[id] = reply;
    return id;
}

void TwitchProvider::untrack_reply(QNetworkReply *reply)
{
    for (auto iterator = pending_replies_.begin(); iterator != pending_replies_.end();) {
        if (iterator->second == reply)
            iterator = pending_replies_.erase(iterator);
        else
            ++iterator;
    }
}

void TwitchProvider::cancel_request(RequestId request_id)
{
    const auto chained = chained_requests_.find(request_id);
    if (chained != chained_requests_.end()) {
        const auto reply = pending_replies_.find(chained->second);
        if (reply != pending_replies_.end() && reply->second)
            reply->second->abort();
        return;
    }
    const auto iterator = pending_replies_.find(request_id);
    if (iterator != pending_replies_.end() && iterator->second)
        iterator->second->abort();
}

void TwitchProvider::cancel_pending_requests()
{
    if (oauth_server_) {
        oauth_server_->stop();
        oauth_server_.reset();
    }
    ++session_generation_;
    const auto replies = pending_replies_;
    for (const auto &entry : replies)
        if (entry.second)
            entry.second->abort();
    chained_requests_.clear();
}

void TwitchProvider::clear_session(bool cancel_requests)
{
    if (cancel_requests)
        cancel_pending_requests();
    user_token_.clear();
    oauth_state_.clear();
    account_id_.clear();
    account_name_.clear();
}

bool TwitchProvider::restore_session(ResultCallback callback)
{
    std::string token;
    std::string error;
    if (!SecureTokenStore::load_twitch_token(token, error)) {
        callback(false, error);
        return false;
    }
    if (token.empty()) {
        callback(false, {});
        return false;
    }
    user_token_ = std::move(token);
    finish_session(std::move(callback), false);
    return true;
}

bool TwitchProvider::forget_session(bool cancel_requests)
{
    clear_session(cancel_requests);
    std::string error;
    return SecureTokenStore::remove_twitch_token(error);
}

bool TwitchProvider::persist_token(std::string &error) const
{
    return SecureTokenStore::store_twitch_token(user_token_, error);
}

void TwitchProvider::invalidate_persisted_token()
{
    user_token_.clear();
    account_id_.clear();
    account_name_.clear();
    std::string ignored;
    SecureTokenStore::remove_twitch_token(ignored);
}

void TwitchProvider::authenticate(QWidget *parent, ResultCallback callback)
{
    (void)parent;
    cancel_pending_requests();
    if (options_.client_id.trimmed().isEmpty()) {
        callback(false, text("TwitchClientIdMissing").toStdString());
        return;
    }

    oauth_state_ = generate_oauth_state();

    oauth_server_ = std::make_unique<LoopbackOAuthServer>();
    // Twitch uses the implicit grant; the token arrives in the URL fragment, so
    // the loopback server serves a bridge page that reposts it to the server.
    if (!oauth_server_->start(LoopbackOAuthServer::CaptureMode::Fragment)) {
        oauth_server_.reset();
        callback(false, text("TwitchLoginServerError").toStdString());
        return;
    }

    QUrl authorization(options_.auth_base_url);
    QUrlQuery query;
    query.addQueryItem("client_id", options_.client_id);
    query.addQueryItem("redirect_uri", oauth_server_->redirect_uri());
    query.addQueryItem("response_type", "token");
    query.addQueryItem("scope", "channel:read:stream_key channel:manage:broadcast");
    query.addQueryItem("state", QString::fromStdString(oauth_state_));
    authorization.setQuery(query);

    const std::uint64_t auth_generation = session_generation_;
    oauth_server_->set_result_callback([this, auth_generation, callback](const LoopbackOAuthServer::Result &result) {
        if (auth_generation != session_generation_)
            return;
        if (oauth_server_)
            oauth_server_->stop();
        std::string token;
        std::string error;
        if (!parse_twitch_oauth_values(result.access_token, result.state, result.error, result.error_detail,
                                       oauth_state_, token, error)) {
            callback(false, error.empty() ? text("TwitchLoginCancelled").toStdString() : error);
            return;
        }
        user_token_ = std::move(token);
        finish_session(callback, true);
    });

    if (!QDesktopServices::openUrl(authorization)) {
        oauth_server_->stop();
        oauth_server_.reset();
        callback(false, text("TwitchLoginBrowserError").toStdString());
    }
}

QNetworkRequest TwitchProvider::make_request(const QUrl &url) const
{
    QNetworkRequest request(url);
    request.setRawHeader("Client-Id", options_.client_id.toUtf8());
    request.setRawHeader("Authorization", "Bearer " + QByteArray::fromStdString(user_token_));
    request.setTransferTimeout(options_.transfer_timeout_ms);
    return request;
}

void TwitchProvider::fetch_account(ResultCallback callback)
{
    const std::uint64_t request_generation = session_generation_;
    const RequestId unused = 0;
    (void)unused;
    QNetworkReply *reply = network_->get(make_request(QUrl(options_.api_base_url + "/users")));
    track_reply(reply);
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, request_generation, callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport = reply->error() != QNetworkReply::NoError ? reply->errorString().toStdString() : std::string{};
        untrack_reply(reply);
        if (request_generation != session_generation_) {
            reply->deleteLater();
            callback(false, text("TwitchRequestCancelled").toStdString());
            return;
        }
        if (reply->error() != QNetworkReply::NoError || !document.isObject()) {
            if (status == 401)
                invalidate_persisted_token();
            const std::string error = format_twitch_error(document, status, transport);
            reply->deleteLater();
            callback(false, error);
            return;
        }
        const QJsonArray data = document.object().value("data").toArray();
        if (data.isEmpty()) {
            reply->deleteLater();
            callback(false, text("TwitchIncompleteProfile").toStdString());
            return;
        }
        const QJsonObject account = data.first().toObject();
        account_id_ = account.value("id").toString().toStdString();
        account_name_ = account.value("display_name").toString(account.value("login").toString()).toStdString();
        reply->deleteLater();
        if (account_id_.empty()) {
            callback(false, text("TwitchIncompleteProfile").toStdString());
            return;
        }
        callback(true, {});
    });
}

void TwitchProvider::finish_session(ResultCallback callback, bool persist)
{
    fetch_account([this, callback = std::move(callback), persist](bool success, const std::string &message) mutable {
        if (!success) {
            clear_session(false);
            callback(false, message);
            return;
        }
        if (persist) {
            std::string storage_error;
            if (!persist_token(storage_error)) {
                clear_session(false);
                callback(false, text("TwitchTokenStorageError").toStdString() + storage_error);
                return;
            }
        }
        callback(true, {});
    });
}

void TwitchProvider::update_channel(const DestinationConfig &config, RequestId operation_id, ResultCallback callback)
{
    const std::uint64_t request_generation = session_generation_;
    QUrl url(options_.api_base_url + "/channels");
    QUrlQuery query;
    query.addQueryItem("broadcaster_id", QString::fromStdString(account_id_));
    url.setQuery(query);
    QNetworkRequest request = make_request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QJsonObject body;
    body.insert("title", QString::fromStdString(config.twitch_title));
    QNetworkReply *reply = network_->sendCustomRequest(request, "PATCH", QJsonDocument(body).toJson(QJsonDocument::Compact));
    const RequestId reply_id = track_reply(reply);
    chained_requests_[operation_id] = reply_id;
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, operation_id, request_generation, callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport = reply->error() != QNetworkReply::NoError ? reply->errorString().toStdString() : std::string{};
        untrack_reply(reply);
        if (request_generation != session_generation_) {
            chained_requests_.erase(operation_id);
            reply->deleteLater();
            callback(false, text("TwitchRequestCancelled").toStdString());
            return;
        }
        const bool success = reply->error() == QNetworkReply::NoError && (status == 0 || (status >= 200 && status < 300));
        if (!success && status == 401)
            invalidate_persisted_token();
        const std::string error = success ? std::string{} : format_twitch_error(document, status, transport);
        reply->deleteLater();
        if (!success)
            chained_requests_.erase(operation_id);
        callback(success, error);
    });
}

TwitchProvider::RequestId TwitchProvider::fetch_stream_key(RequestId operation_id, LiveCallback callback)
{
    const std::uint64_t request_generation = session_generation_;
    QUrl url(options_.api_base_url + "/streams/key");
    QUrlQuery query;
    query.addQueryItem("broadcaster_id", QString::fromStdString(account_id_));
    url.setQuery(query);
    QNetworkReply *reply = network_->get(make_request(url));
    const RequestId request_id = track_reply(reply);
    chained_requests_[operation_id] = request_id;
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, operation_id, request_generation, callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport = reply->error() != QNetworkReply::NoError ? reply->errorString().toStdString() : std::string{};
        untrack_reply(reply);
        chained_requests_.erase(operation_id);
        if (request_generation != session_generation_) {
            reply->deleteLater();
            callback(false, {}, text("TwitchRequestCancelled").toStdString());
            return;
        }
        if (reply->error() != QNetworkReply::NoError || !document.isObject()) {
            if (status == 401)
                invalidate_persisted_token();
            const std::string error = format_twitch_error(document, status, transport);
            reply->deleteLater();
            callback(false, {}, error);
            return;
        }
        const QJsonArray data = document.object().value("data").toArray();
        const QString key = data.isEmpty() ? QString{} : data.first().toObject().value("stream_key").toString();
        reply->deleteLater();
        if (key.isEmpty()) {
            callback(false, {}, text("TwitchStreamKeyUnavailable").toStdString());
            return;
        }
        TwitchLive live{options_.ingest_url.toStdString(), key.toStdString()};
        callback(true, live, {});
    });
    return request_id;
}

TwitchProvider::RequestId TwitchProvider::create_live(const DestinationConfig &config, LiveCallback callback)
{
    if (!authenticated() || account_id_.empty()) {
        callback(false, {}, text("TwitchNotConnected").toStdString());
        return 0;
    }

    const RequestId operation_id = next_request_id_++;
    if (config.twitch_title.empty()) {
        fetch_stream_key(operation_id, std::move(callback));
        return operation_id;
    }

    update_channel(config, operation_id,
                   [this, operation_id, callback = std::move(callback)](bool success,
                                                                         const std::string &message) mutable {
        if (!success) {
            callback(false, {}, message);
            return;
        }
        fetch_stream_key(operation_id, std::move(callback));
    });
    return operation_id;
}

} // namespace multistream
