// SPDX-License-Identifier: GPL-2.0-or-later

#include "facebook-provider.hpp"
#include "facebook-utils.hpp"
#include "secure-token-store.hpp"

#include <QDialog>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>

namespace multistream {

FacebookProvider::FacebookProvider() : FacebookProvider(Options{}) {}

FacebookProvider::FacebookProvider(Options options)
    : network_(std::make_unique<QNetworkAccessManager>()), options_(std::move(options)) {}

FacebookProvider::~FacebookProvider()
{
    cancel_pending_requests();
}

FacebookProvider::RequestId FacebookProvider::track_reply(QNetworkReply *reply)
{
    if (!reply)
        return 0;
    if (next_request_id_ == 0)
        next_request_id_ = 1;
    const RequestId request_id = next_request_id_++;
    pending_replies_[request_id] = reply;
    return request_id;
}

void FacebookProvider::untrack_reply(QNetworkReply *reply)
{
    for (auto iterator = pending_replies_.begin(); iterator != pending_replies_.end();) {
        if (iterator->second == reply)
            iterator = pending_replies_.erase(iterator);
        else
            ++iterator;
    }
}

void FacebookProvider::cancel_request(RequestId request_id)
{
    const auto iterator = pending_replies_.find(request_id);
    if (iterator != pending_replies_.end() && iterator->second)
        iterator->second->abort();
}

void FacebookProvider::cancel_pending_requests()
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

void FacebookProvider::clear_session(bool cancel_requests)
{
    if (cancel_requests)
        cancel_pending_requests();
    user_token_.clear();
    oauth_state_.clear();
    account_id_.clear();
    account_name_.clear();
    pages_.clear();
    page_tokens_.clear();
    live_tokens_.clear();
}

bool FacebookProvider::restore_session(ResultCallback callback)
{
    std::string token;
    std::string error;
    if (!SecureTokenStore::load_facebook_token(token, error)) {
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

bool FacebookProvider::forget_session(bool cancel_requests)
{
    clear_session(cancel_requests);
    std::string error;
    return SecureTokenStore::remove_facebook_token(error);
}

bool FacebookProvider::persist_token(std::string &error) const
{
    return SecureTokenStore::store_facebook_token(user_token_, error);
}

void FacebookProvider::finish_session(ResultCallback callback, bool persist)
{
    fetch_account([this, callback = std::move(callback), persist](bool success, const std::string &message) mutable {
        if (!success) {
            clear_session(false);
            callback(false, message);
            return;
        }

        fetch_pages([this, callback = std::move(callback), persist](bool pages_success,
                                                                      const std::string &pages_message) mutable {
            if (!pages_success) {
                clear_session(false);
                callback(false, pages_message);
                return;
            }
            if (persist) {
                std::string storage_error;
                if (!persist_token(storage_error)) {
                    clear_session(false);
                    callback(false, "Facebook login succeeded but the token could not be stored securely: " +
                                         storage_error);
                    return;
                }
            }
            callback(true, {});
        });
    });
}

void FacebookProvider::fetch_account(ResultCallback callback)
{
    QUrl url(options_.graph_base_url + "/me");
    QUrlQuery query;
    query.addQueryItem("fields", "id,name,first_name,last_name");
    url.setQuery(query);
    const std::uint64_t request_generation = session_generation_;

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + QByteArray::fromStdString(user_token_));
    request.setTransferTimeout(options_.transfer_timeout_ms);
    QNetworkReply *reply = network_->get(request);
    track_reply(reply);
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, request_generation, callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport_error = reply->error() != QNetworkReply::NoError
                                                 ? reply->errorString().toStdString()
                                                 : std::string{};
        untrack_reply(reply);
        if (request_generation != session_generation_) {
            reply->deleteLater();
            callback(false, "Facebook request was cancelled.");
            return;
        }
        if (reply->error() != QNetworkReply::NoError || !document.isObject()) {
            const std::string message = format_facebook_error(document, http_status, transport_error);
            reply->deleteLater();
            callback(false, message);
            return;
        }

        const QJsonObject object = document.object();
        account_id_ = object.value("id").toString().toStdString();
        QString account_name = object.value("name").toString().trimmed();
        if (account_name.isEmpty()) {
            const QString first_name = object.value("first_name").toString().trimmed();
            const QString last_name = object.value("last_name").toString().trimmed();
            account_name = (first_name + " " + last_name).simplified();
        }
        account_name_ = account_name.toStdString();
        reply->deleteLater();
        if (account_id_.empty()) {
            callback(false, "Facebook returned an incomplete account profile.");
            return;
        }
        callback(true, {});
    });
}

bool FacebookProvider::has_page(const std::string &page_id) const
{
    return page_tokens_.find(page_id) != page_tokens_.end();
}

bool FacebookProvider::page_token(const std::string &page_id, std::string &token) const
{
    const auto iterator = page_tokens_.find(page_id);
    if (iterator == page_tokens_.end())
        return false;
    token = iterator->second;
    return !token.empty();
}

void FacebookProvider::fetch_pages(ResultCallback callback)
{
    QUrl url(options_.graph_base_url + "/me/accounts");
    QUrlQuery query;
    query.addQueryItem("fields", "id,name,access_token");
    url.setQuery(query);
    fetch_pages_page(url, session_generation_, {}, {}, 0, std::move(callback));
}

bool FacebookProvider::is_safe_pagination_url(const QUrl &url) const
{
    const QUrl base(options_.graph_base_url);
    if (!url.isValid() || url.isRelative() || base.isRelative() || url.scheme() != base.scheme() ||
        url.host() != base.host())
        return false;

    const auto normalized_port = [](const QUrl &value) {
        if (value.port() != -1)
            return value.port();
        if (value.scheme() == "https")
            return 443;
        if (value.scheme() == "http")
            return 80;
        return -1;
    };
    return normalized_port(url) == normalized_port(base);
}

void FacebookProvider::fetch_pages_page(QUrl url, std::uint64_t request_generation,
                                        std::vector<FacebookPage> fetched_pages,
                                        std::unordered_map<std::string, std::string> fetched_page_tokens,
                                        int retry_count, ResultCallback callback)
{
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + QByteArray::fromStdString(user_token_));
    request.setTransferTimeout(options_.transfer_timeout_ms);
    QNetworkReply *reply = network_->get(request);
    track_reply(reply);
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, url, request_generation, fetched_pages = std::move(fetched_pages),
                      fetched_page_tokens = std::move(fetched_page_tokens), retry_count,
                      callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport_error = reply->error() != QNetworkReply::NoError
                                                 ? reply->errorString().toStdString()
                                                 : std::string{};
        untrack_reply(reply);
        if (request_generation != session_generation_) {
            reply->deleteLater();
            callback(false, "Facebook request was cancelled.");
            return;
        }
        if (reply->error() != QNetworkReply::NoError || !document.isObject()) {
            const FacebookErrorClassification classification =
                classify_facebook_error(document, http_status, transport_error);
            if (classification.retryable && retry_count < options_.max_retries) {
                const int multiplier = retry_count >= 10 ? 1024 : (1 << retry_count);
                const int delay_ms = std::max(0, options_.retry_backoff_ms) * multiplier;
                reply->deleteLater();
                QTimer::singleShot(
                    delay_ms, network_.get(),
                    [this, url, request_generation, fetched_pages = std::move(fetched_pages),
                     fetched_page_tokens = std::move(fetched_page_tokens), retry_count,
                     callback = std::move(callback)]() mutable {
                        if (request_generation != session_generation_) {
                            callback(false, "Facebook request was cancelled.");
                            return;
                        }
                        fetch_pages_page(url, request_generation, std::move(fetched_pages),
                                         std::move(fetched_page_tokens), retry_count + 1,
                                         std::move(callback));
                    });
                return;
            }

            const std::string message = format_facebook_error(document, http_status, transport_error);
            reply->deleteLater();
            callback(false, message);
            return;
        }

        auto pages = std::move(fetched_pages);
        auto page_tokens = std::move(fetched_page_tokens);
        const QJsonArray data = document.object().value("data").toArray();
        for (const QJsonValue &value : data) {
            const QJsonObject page = value.toObject();
            const QString id = page.value("id").toString();
            const QString name = page.value("name").toString();
            const QString token = page.value("access_token").toString();
            if (id.isEmpty() || token.isEmpty())
                continue;
            pages.push_back({id.toStdString(), name.toStdString()});
            page_tokens[id.toStdString()] = token.toStdString();
        }

        const QString next = document.object().value("paging").toObject().value("next").toString();
        reply->deleteLater();
        if (!next.isEmpty()) {
            const QUrl next_url(next);
            if (!is_safe_pagination_url(next_url)) {
                callback(false, "Facebook returned an unsafe pagination URL.");
                return;
            }
            fetch_pages_page(next_url, request_generation, std::move(pages), std::move(page_tokens), 0,
                             std::move(callback));
            return;
        }

        pages_ = std::move(pages);
        page_tokens_ = std::move(page_tokens);
        callback(true, {});
    });
}

FacebookProvider::RequestId FacebookProvider::create_live(const DestinationConfig &config, LiveCallback callback)
{
    const std::uint64_t request_generation = session_generation_;
    std::string token;
    QString endpoint;
    if (is_facebook_timeline(config)) {
        token = user_token_;
        endpoint = options_.graph_base_url + "/me/live_videos";
    } else {
        if (!page_token(config.facebook_page_id, token)) {
            callback(false, {}, "Connect Facebook and select an available Page first.");
            return 0;
        }
        endpoint = options_.graph_base_url + "/" + QString::fromStdString(config.facebook_page_id) +
                   "/live_videos";
    }
    if (token.empty()) {
        callback(false, {}, "Connect Facebook before starting a timeline stream.");
        return 0;
    }

    QUrl url(endpoint);
    QUrlQuery query;
    query.addQueryItem("status", "LIVE_NOW");
    query.addQueryItem("title", QString::fromStdString(config.facebook_title));
    if (!config.facebook_description.empty())
        query.addQueryItem("description", QString::fromStdString(config.facebook_description));
    if (!config.facebook_privacy.empty() && is_facebook_timeline(config)) {
        const std::string privacy_value = normalize_facebook_privacy(config.facebook_privacy);
        QJsonObject privacy;
        privacy.insert("value", QString::fromStdString(privacy_value));
        query.addQueryItem("privacy", QString::fromUtf8(
                                             QJsonDocument(privacy).toJson(QJsonDocument::Compact)));
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("Authorization", "Bearer " + QByteArray::fromStdString(token));
    request.setTransferTimeout(options_.transfer_timeout_ms);
    QNetworkReply *reply = network_->post(request, query.query(QUrl::FullyEncoded).toUtf8());
    const RequestId request_id = track_reply(reply);
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, token, request_generation, callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport_error = reply->error() != QNetworkReply::NoError
                                                 ? reply->errorString().toStdString()
                                                 : std::string{};
        untrack_reply(reply);
        if (request_generation != session_generation_) {
            reply->deleteLater();
            callback(false, {}, "Facebook request was cancelled.");
            return;
        }
        if (reply->error() != QNetworkReply::NoError || !document.isObject()) {
            const std::string message = format_facebook_error(document, http_status, transport_error);
            reply->deleteLater();
            callback(false, {}, message);
            return;
        }

        const QJsonObject object = document.object();
        const QString endpoint = object.value("secure_stream_url").toString().isEmpty()
                                     ? object.value("stream_url").toString()
                                     : object.value("secure_stream_url").toString();
        FacebookLive live;
        live.id = object.value("id").toString().toStdString();
        if (live.id.empty() || !split_facebook_stream_url(endpoint, live.server, live.stream_key)) {
            reply->deleteLater();
            callback(false, {}, "Facebook did not return a usable secure RTMPS stream URL.");
            return;
        }
        reply->deleteLater();
        live_tokens_[live.id] = token;
        callback(true, live, {});
    });
    return request_id;
}

FacebookProvider::RequestId FacebookProvider::stop_live(const std::string &live_id, ResultCallback callback)
{
    if (live_id.empty()) {
        callback(true, {});
        return 0;
    }

    const std::uint64_t request_generation = session_generation_;
    std::string token = user_token_;
    const auto token_iterator = live_tokens_.find(live_id);
    if (token_iterator != live_tokens_.end())
        token = token_iterator->second;
    if (token.empty()) {
        callback(false, "The Facebook session has expired.");
        return 0;
    }

    QUrl url(options_.graph_base_url + "/" + QString::fromStdString(live_id));
    QUrlQuery query;
    query.addQueryItem("end_live_video", "true");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("Authorization", "Bearer " + QByteArray::fromStdString(token));
    request.setTransferTimeout(options_.transfer_timeout_ms);
    QNetworkReply *reply = network_->post(request, query.query(QUrl::FullyEncoded).toUtf8());
    const RequestId request_id = track_reply(reply);
    QObject::connect(reply, &QNetworkReply::finished, network_.get(),
                     [this, reply, live_id, request_generation, callback = std::move(callback)]() mutable {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string transport_error = reply->error() != QNetworkReply::NoError
                                                 ? reply->errorString().toStdString()
                                                 : std::string{};
        untrack_reply(reply);
        if (request_generation != session_generation_) {
            reply->deleteLater();
            callback(false, "Facebook request was cancelled.");
            return;
        }
        const bool ok = reply->error() == QNetworkReply::NoError;
        const std::string message = ok
                                         ? std::string{}
                                         : format_facebook_error(document, http_status, transport_error);
        reply->deleteLater();
        if (ok)
            live_tokens_.erase(live_id);
        callback(ok, message);
    });
    return request_id;
}

} // namespace multistream
