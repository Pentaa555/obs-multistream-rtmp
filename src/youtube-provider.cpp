// SPDX-License-Identifier: GPL-2.0-or-later

#include "youtube-provider.hpp"
#include "secure-token-store.hpp"
#include "youtube-utils.hpp"

#include <obs-module.h>

#include <QDesktopServices>
#include <QDateTime>
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

std::string normalize_privacy(const std::string &value)
{
    if (value == "public" || value == "unlisted" || value == "private")
        return value;
    return "public";
}

} // namespace

YouTubeProvider::YouTubeProvider() : YouTubeProvider(Options{}) {}

YouTubeProvider::YouTubeProvider(Options options)
    : network_(std::make_unique<QNetworkAccessManager>()), options_(std::move(options))
{
#ifdef YOUTUBE_CLIENT_ID
    if (options_.client_id.isEmpty())
        options_.client_id = QStringLiteral(YOUTUBE_CLIENT_ID);
#endif
#ifdef YOUTUBE_CLIENT_SECRET
    if (options_.client_secret.isEmpty())
        options_.client_secret = QStringLiteral(YOUTUBE_CLIENT_SECRET);
#endif
    if (options_.client_secret.isEmpty()) {
        const QString environment_secret = qEnvironmentVariable("YOUTUBE_CLIENT_SECRET");
        if (!environment_secret.isEmpty()) {
            // An explicit environment value is a one-time bootstrap/repair path
            // and must override a stale value already stored in libsecret.
            options_.client_secret = environment_secret;
            std::string ignored_error;
            SecureTokenStore::store_youtube_client_secret(environment_secret.toStdString(), ignored_error);
        } else {
            std::string stored_secret;
            std::string storage_error;
            if (SecureTokenStore::load_youtube_client_secret(stored_secret, storage_error) && !stored_secret.empty())
                options_.client_secret = QString::fromStdString(stored_secret);
        }
    }
}

YouTubeProvider::~YouTubeProvider()
{
    cancel_pending_requests();
}

YouTubeProvider::RequestId YouTubeProvider::track_reply(QNetworkReply *reply)
{
    if (!reply)
        return 0;
    if (next_request_id_ == 0)
        next_request_id_ = 1;
    const RequestId id = next_request_id_++;
    pending_replies_[id] = reply;
    return id;
}

void YouTubeProvider::untrack_reply(QNetworkReply *reply)
{
    for (auto iterator = pending_replies_.begin(); iterator != pending_replies_.end();) {
        if (iterator->second == reply)
            iterator = pending_replies_.erase(iterator);
        else
            ++iterator;
    }
}

void YouTubeProvider::cancel_request(RequestId request_id)
{
    const auto iterator = pending_replies_.find(request_id);
    if (iterator != pending_replies_.end() && iterator->second)
        iterator->second->abort();
}

void YouTubeProvider::cancel_pending_requests()
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
}

void YouTubeProvider::clear_session(bool cancel_requests)
{
    if (cancel_requests)
        cancel_pending_requests();
    access_token_.clear();
    refresh_token_.clear();
    oauth_state_.clear();
    channel_id_.clear();
    channel_title_.clear();
}

bool YouTubeProvider::persist_refresh_token(std::string &error) const
{
    return SecureTokenStore::store_youtube_token(refresh_token_, error);
}

void YouTubeProvider::invalidate_persisted_token()
{
    access_token_.clear();
    refresh_token_.clear();
    channel_id_.clear();
    channel_title_.clear();
    std::string ignored;
    SecureTokenStore::remove_youtube_token(ignored);
}

bool YouTubeProvider::forget_session(bool cancel_requests)
{
    clear_session(cancel_requests);
    std::string error;
    return SecureTokenStore::remove_youtube_token(error);
}

QNetworkRequest YouTubeProvider::make_request(const QUrl &url, bool json_body) const
{
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + QByteArray::fromStdString(access_token_));
    if (json_body)
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(options_.transfer_timeout_ms);
    return request;
}

void YouTubeProvider::authenticate(QWidget *parent, ResultCallback callback)
{
    (void)parent;
    cancel_pending_requests();
    if (options_.client_id.trimmed().isEmpty()) {
        callback(false, text("YouTubeClientIdMissing").toStdString());
        return;
    }

    oauth_state_ = generate_oauth_state();
    const PkcePair pkce = generate_pkce_pair();

    oauth_server_ = std::make_unique<LoopbackOAuthServer>();
    // Google uses the authorization-code flow; the code is returned in the query
    // string, so capture in Query mode.
    if (!oauth_server_->start(LoopbackOAuthServer::CaptureMode::Query)) {
        oauth_server_.reset();
        callback(false, text("YouTubeLoginServerError").toStdString());
        return;
    }
    const std::string redirect_uri = oauth_server_->redirect_uri().toStdString();

    QUrl authorization(options_.auth_base_url);
    QUrlQuery query;
    query.addQueryItem("client_id", options_.client_id);
    query.addQueryItem("redirect_uri", QString::fromStdString(redirect_uri));
    query.addQueryItem("response_type", "code");
    query.addQueryItem("scope", options_.scope);
    query.addQueryItem("code_challenge", QString::fromStdString(pkce.challenge));
    query.addQueryItem("code_challenge_method", "S256");
    query.addQueryItem("access_type", "offline");
    query.addQueryItem("prompt", "consent");
    query.addQueryItem("state", QString::fromStdString(oauth_state_));
    authorization.setQuery(query);

    const std::uint64_t auth_generation = session_generation_;
    const std::string verifier = pkce.verifier;
    oauth_server_->set_result_callback(
        [this, auth_generation, verifier, redirect_uri, callback](const LoopbackOAuthServer::Result &result) {
        if (auth_generation != session_generation_)
            return;
        if (oauth_server_)
            oauth_server_->stop();
        std::string code;
        std::string error;
        if (!parse_youtube_oauth_values(result.code, result.state, result.error, result.error_detail,
                                        oauth_state_, code, error)) {
            callback(false, error.empty() ? text("YouTubeLoginCancelled").toStdString() : error);
            return;
        }
        exchange_code(code, verifier, redirect_uri, callback);
    });

    if (!QDesktopServices::openUrl(authorization)) {
        oauth_server_->stop();
        oauth_server_.reset();
        callback(false, text("YouTubeLoginBrowserError").toStdString());
    }
}

void YouTubeProvider::exchange_code(const std::string &code, const std::string &verifier,
                                    const std::string &redirect_uri, ResultCallback callback)
{
    const std::uint64_t request_generation = session_generation_;
    QNetworkRequest request(QUrl(options_.token_url));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setTransferTimeout(options_.transfer_timeout_ms);
    QUrlQuery body;
    body.addQueryItem("client_id", options_.client_id);
    if (!options_.client_secret.isEmpty())
        body.addQueryItem("client_secret", options_.client_secret);
    body.addQueryItem("code", QString::fromStdString(code));
    body.addQueryItem("code_verifier", QString::fromStdString(verifier));
    body.addQueryItem("grant_type", "authorization_code");
    body.addQueryItem("redirect_uri", QString::fromStdString(redirect_uri));
    QNetworkReply *reply = network_->post(request, body.toString(QUrl::FullyEncoded).toUtf8());
    track_reply(reply);
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, request_generation, callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport =
            reply->error() != QNetworkReply::NoError ? reply->errorString().toStdString() : std::string{};
        untrack_reply(reply);
        reply->deleteLater();
        if (request_generation != session_generation_) {
            callback(false, text("YouTubeRequestCancelled").toStdString());
            return;
        }
        std::string access;
        std::string refresh;
        int expires = 0;
        std::string error;
        if (!parse_youtube_token_response(document, access, refresh, expires, error)) {
            callback(false, error.empty() ? format_youtube_error(document, status, transport) : error);
            return;
        }
        access_token_ = std::move(access);
        if (!refresh.empty())
            refresh_token_ = std::move(refresh);
        finish_session(std::move(callback), true);
    });
}

void YouTubeProvider::refresh_access_token(ResultCallback callback)
{
    if (refresh_token_.empty()) {
        callback(false, text("YouTubeNotConnected").toStdString());
        return;
    }
    const std::uint64_t request_generation = session_generation_;
    QNetworkRequest request(QUrl(options_.token_url));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setTransferTimeout(options_.transfer_timeout_ms);
    QUrlQuery body;
    body.addQueryItem("client_id", options_.client_id);
    if (!options_.client_secret.isEmpty())
        body.addQueryItem("client_secret", options_.client_secret);
    body.addQueryItem("refresh_token", QString::fromStdString(refresh_token_));
    body.addQueryItem("grant_type", "refresh_token");
    QNetworkReply *reply = network_->post(request, body.toString(QUrl::FullyEncoded).toUtf8());
    track_reply(reply);
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, request_generation, callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport =
            reply->error() != QNetworkReply::NoError ? reply->errorString().toStdString() : std::string{};
        untrack_reply(reply);
        reply->deleteLater();
        if (request_generation != session_generation_) {
            callback(false, text("YouTubeRequestCancelled").toStdString());
            return;
        }
        std::string access;
        std::string refresh;
        int expires = 0;
        std::string error;
        if (!parse_youtube_token_response(document, access, refresh, expires, error)) {
            if (status == 400 || status == 401)
                invalidate_persisted_token();
            callback(false, error.empty() ? format_youtube_error(document, status, transport) : error);
            return;
        }
        access_token_ = std::move(access);
        if (!refresh.empty())
            refresh_token_ = std::move(refresh);
        callback(true, {});
    });
}

void YouTubeProvider::fetch_channel(ResultCallback callback)
{
    const std::uint64_t request_generation = session_generation_;
    QUrl url(options_.api_base_url + "/channels");
    QUrlQuery query;
    query.addQueryItem("part", "snippet");
    query.addQueryItem("mine", "true");
    url.setQuery(query);
    QNetworkReply *reply = network_->get(make_request(url, false));
    track_reply(reply);
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, request_generation, callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport =
            reply->error() != QNetworkReply::NoError ? reply->errorString().toStdString() : std::string{};
        untrack_reply(reply);
        reply->deleteLater();
        if (request_generation != session_generation_) {
            callback(false, text("YouTubeRequestCancelled").toStdString());
            return;
        }
        if (reply->error() != QNetworkReply::NoError || !document.isObject()) {
            if (status == 401)
                invalidate_persisted_token();
            callback(false, format_youtube_error(document, status, transport));
            return;
        }
        std::string channel_id;
        std::string channel_title;
        std::string error;
        if (!parse_youtube_channel(document, channel_id, channel_title, error)) {
            callback(false, error);
            return;
        }
        channel_id_ = std::move(channel_id);
        channel_title_ = std::move(channel_title);
        callback(true, {});
    });
}

void YouTubeProvider::finish_session(ResultCallback callback, bool persist_refresh)
{
    fetch_channel([this, callback = std::move(callback), persist_refresh](bool success,
                                                                          const std::string &message) mutable {
        if (!success) {
            clear_session(false);
            callback(false, message);
            return;
        }
        if (persist_refresh && !refresh_token_.empty()) {
            std::string storage_error;
            if (!persist_refresh_token(storage_error)) {
                clear_session(false);
                callback(false, text("YouTubeTokenStorageError").toStdString() + storage_error);
                return;
            }
        }
        callback(true, {});
    });
}

bool YouTubeProvider::restore_session(ResultCallback callback)
{
    std::string token;
    std::string error;
    if (!SecureTokenStore::load_youtube_token(token, error)) {
        callback(false, error);
        return false;
    }
    if (token.empty()) {
        callback(false, {});
        return false;
    }
    refresh_token_ = std::move(token);
    refresh_access_token([this, callback = std::move(callback)](bool success, const std::string &message) mutable {
        if (!success) {
            clear_session(false);
            callback(false, message);
            return;
        }
        finish_session(std::move(callback), false);
    });
    return true;
}

void YouTubeProvider::insert_broadcast(const DestinationConfig &config, RequestId operation_id, LiveCallback callback)
{
    const std::uint64_t request_generation = session_generation_;
    QUrl url(options_.api_base_url + "/liveBroadcasts");
    QUrlQuery query;
    query.addQueryItem("part", "snippet,status,contentDetails");
    url.setQuery(query);

    QJsonObject snippet;
    snippet.insert("title", QString::fromStdString(config.youtube_title));
    if (!config.youtube_description.empty())
        snippet.insert("description", QString::fromStdString(config.youtube_description));
    // YouTube requires a scheduled start time; use now.
    snippet.insert("scheduledStartTime",
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    QJsonObject status;
    status.insert("privacyStatus", QString::fromStdString(normalize_privacy(config.youtube_privacy)));
    status.insert("selfDeclaredMadeForKids", false);
    QJsonObject content_details;
    content_details.insert("enableAutoStart", true);
    content_details.insert("enableAutoStop", true);
    QJsonObject body;
    body.insert("snippet", snippet);
    body.insert("status", status);
    body.insert("contentDetails", content_details);

    QNetworkReply *reply = network_->post(make_request(url, true), QJsonDocument(body).toJson(QJsonDocument::Compact));
    track_reply(reply);
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, config, operation_id, request_generation,
                      callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport =
            reply->error() != QNetworkReply::NoError ? reply->errorString().toStdString() : std::string{};
        untrack_reply(reply);
        reply->deleteLater();
        if (request_generation != session_generation_) {
            callback(false, {}, text("YouTubeRequestCancelled").toStdString());
            return;
        }
        std::string broadcast_id;
        std::string error;
        if (reply->error() != QNetworkReply::NoError || !parse_youtube_resource_id(document, broadcast_id, error)) {
            if (status == 401)
                invalidate_persisted_token();
            callback(false, {}, error.empty() ? format_youtube_error(document, status, transport) : error);
            return;
        }
        insert_stream(config, operation_id, std::move(broadcast_id), std::move(callback));
    });
}

void YouTubeProvider::insert_stream(const DestinationConfig &config, RequestId operation_id,
                                    std::string broadcast_id, LiveCallback callback)
{
    const std::uint64_t request_generation = session_generation_;
    QUrl url(options_.api_base_url + "/liveStreams");
    QUrlQuery query;
    query.addQueryItem("part", "snippet,cdn,contentDetails");
    url.setQuery(query);

    QJsonObject snippet;
    snippet.insert("title", QString::fromStdString(config.youtube_title));
    QJsonObject cdn;
    cdn.insert("ingestionType", "rtmp");
    cdn.insert("frameRate", "variable");
    cdn.insert("resolution", "variable");
    QJsonObject body;
    body.insert("snippet", snippet);
    body.insert("cdn", cdn);

    QNetworkReply *reply = network_->post(make_request(url, true), QJsonDocument(body).toJson(QJsonDocument::Compact));
    track_reply(reply);
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, operation_id, broadcast_id, request_generation,
                      callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport =
            reply->error() != QNetworkReply::NoError ? reply->errorString().toStdString() : std::string{};
        untrack_reply(reply);
        reply->deleteLater();
        if (request_generation != session_generation_) {
            callback(false, {}, text("YouTubeRequestCancelled").toStdString());
            return;
        }
        YouTubeStreamInfo info;
        std::string error;
        if (reply->error() != QNetworkReply::NoError || !parse_youtube_stream_info(document, info, error)) {
            if (status == 401)
                invalidate_persisted_token();
            callback(false, {}, error.empty() ? format_youtube_error(document, status, transport) : error);
            return;
        }
        YouTubeLive live;
        live.broadcast_id = broadcast_id;
        live.stream_id = info.stream_id;
        live.server = info.ingestion_address;
        live.stream_key = info.stream_name;
        bind_stream(operation_id, std::move(broadcast_id), std::move(live), std::move(callback));
    });
}

void YouTubeProvider::bind_stream(RequestId operation_id, std::string broadcast_id, YouTubeLive live,
                                  LiveCallback callback)
{
    (void)operation_id;
    const std::uint64_t request_generation = session_generation_;
    QUrl url(options_.api_base_url + "/liveBroadcasts/bind");
    QUrlQuery query;
    query.addQueryItem("id", QString::fromStdString(broadcast_id));
    query.addQueryItem("streamId", QString::fromStdString(live.stream_id));
    query.addQueryItem("part", "id,contentDetails");
    url.setQuery(query);

    QNetworkReply *reply = network_->post(make_request(url, true), QByteArray());
    track_reply(reply);
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, live, request_generation, callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport =
            reply->error() != QNetworkReply::NoError ? reply->errorString().toStdString() : std::string{};
        untrack_reply(reply);
        reply->deleteLater();
        if (request_generation != session_generation_) {
            callback(false, {}, text("YouTubeRequestCancelled").toStdString());
            return;
        }
        const bool ok = reply->error() == QNetworkReply::NoError && (status >= 200 && status < 300);
        if (!ok) {
            if (status == 401)
                invalidate_persisted_token();
            callback(false, {}, format_youtube_error(document, status, transport));
            return;
        }
        callback(true, live, {});
    });
}

YouTubeProvider::RequestId YouTubeProvider::create_live(const DestinationConfig &config, LiveCallback callback)
{
    if (!authenticated() || channel_id_.empty()) {
        callback(false, {}, text("YouTubeNotConnected").toStdString());
        return 0;
    }
    const RequestId operation_id = next_request_id_++;
    insert_broadcast(config, operation_id, std::move(callback));
    return operation_id;
}

YouTubeProvider::RequestId YouTubeProvider::stop_live(const std::string &broadcast_id, ResultCallback callback)
{
    if (broadcast_id.empty()) {
        callback(true, {});
        return 0;
    }
    const std::uint64_t request_generation = session_generation_;
    QUrl url(options_.api_base_url + "/liveBroadcasts/transition");
    QUrlQuery query;
    query.addQueryItem("broadcastStatus", "complete");
    query.addQueryItem("id", QString::fromStdString(broadcast_id));
    query.addQueryItem("part", "status");
    url.setQuery(query);
    QNetworkReply *reply = network_->post(make_request(url, true), QByteArray());
    const RequestId request_id = track_reply(reply);
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, request_generation, callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport =
            reply->error() != QNetworkReply::NoError ? reply->errorString().toStdString() : std::string{};
        untrack_reply(reply);
        reply->deleteLater();
        if (request_generation != session_generation_) {
            callback(false, text("YouTubeRequestCancelled").toStdString());
            return;
        }
        // A transition may legitimately fail if the broadcast already completed;
        // treat 2xx and "redundant transition" style 4xx as success is risky, so
        // only accept 2xx as a clean stop.
        const bool ok = reply->error() == QNetworkReply::NoError && (status >= 200 && status < 300);
        callback(ok, ok ? std::string{} : format_youtube_error(document, status, transport));
    });
    return request_id;
}

YouTubeProvider::RequestId YouTubeProvider::delete_live(const std::string &broadcast_id, ResultCallback callback)
{
    if (broadcast_id.empty()) {
        callback(true, {});
        return 0;
    }
    const std::uint64_t request_generation = session_generation_;
    QUrl url(options_.api_base_url + "/liveBroadcasts");
    QUrlQuery query;
    query.addQueryItem("id", QString::fromStdString(broadcast_id));
    url.setQuery(query);
    QNetworkReply *reply = network_->deleteResource(make_request(url, false));
    const RequestId request_id = track_reply(reply);
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, request_generation, callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport =
            reply->error() != QNetworkReply::NoError ? reply->errorString().toStdString() : std::string{};
        untrack_reply(reply);
        reply->deleteLater();
        if (request_generation != session_generation_) {
            callback(false, text("YouTubeRequestCancelled").toStdString());
            return;
        }
        const bool ok = reply->error() == QNetworkReply::NoError && (status >= 200 && status < 300);
        callback(ok, ok ? std::string{} : format_youtube_error(document, status, transport));
    });
    return request_id;
}

} // namespace multistream
