// SPDX-License-Identifier: GPL-2.0-or-later

#include "youtube-utils.hpp"

#include <QJsonArray>
#include <QJsonObject>

namespace multistream {
namespace {

std::string api_message(const QJsonDocument &document)
{
    if (!document.isObject())
        return {};
    const QJsonObject error = document.object().value("error").toObject();
    // Google APIs return {"error":{"message":"..."}} or the OAuth token endpoint
    // returns {"error":"invalid_grant","error_description":"..."}.
    const QString message = error.value("message").toString();
    if (!message.isEmpty())
        return message.toStdString();
    const QString description = document.object().value("error_description").toString();
    if (!description.isEmpty())
        return description.toStdString();
    const QString code = document.object().value("error").toString();
    return code.toStdString();
}

} // namespace

bool validate_youtube_redirect(const QUrl &redirected)
{
    if (!redirected.isValid() || redirected.scheme() != "http")
        return false;
    const QString host = redirected.host();
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

bool parse_youtube_oauth_values(const std::string &code, const std::string &state,
                                const std::string &error_code, const std::string &error_detail,
                                const std::string &expected_state, std::string &out_code, std::string &error)
{
    out_code.clear();
    error.clear();
    if (!error_code.empty() || !error_detail.empty()) {
        error = !error_detail.empty() ? error_detail : error_code;
        return false;
    }
    if (code.empty() || state != expected_state) {
        error = "YouTube authorization was incomplete or the state did not match.";
        return false;
    }
    out_code = code;
    return true;
}

bool parse_youtube_token_response(const QJsonDocument &document, std::string &access_token,
                                  std::string &refresh_token, int &expires_in_seconds, std::string &error)
{
    access_token.clear();
    refresh_token.clear();
    expires_in_seconds = 0;
    error.clear();
    if (!document.isObject()) {
        error = "YouTube returned an invalid token response.";
        return false;
    }
    const QJsonObject root = document.object();
    const QString token = root.value("access_token").toString();
    if (token.isEmpty()) {
        error = api_message(document);
        if (error.empty())
            error = "YouTube did not return an access token.";
        return false;
    }
    access_token = token.toStdString();
    // A refresh token is only returned on the first consent (access_type=offline
    // with prompt=consent). It may be absent on subsequent exchanges.
    refresh_token = root.value("refresh_token").toString().toStdString();
    expires_in_seconds = root.value("expires_in").toInt(0);
    return true;
}

bool parse_youtube_resource_id(const QJsonDocument &document, std::string &id, std::string &error)
{
    id.clear();
    error.clear();
    if (!document.isObject()) {
        error = "YouTube returned an invalid response.";
        return false;
    }
    const QString resource_id = document.object().value("id").toString();
    if (resource_id.isEmpty()) {
        error = api_message(document);
        if (error.empty())
            error = "YouTube did not return a resource id.";
        return false;
    }
    id = resource_id.toStdString();
    return true;
}

bool parse_youtube_stream_info(const QJsonDocument &document, YouTubeStreamInfo &info, std::string &error)
{
    info = {};
    error.clear();
    if (!document.isObject()) {
        error = "YouTube returned an invalid stream response.";
        return false;
    }
    const QJsonObject root = document.object();
    info.stream_id = root.value("id").toString().toStdString();
    const QJsonObject cdn = root.value("cdn").toObject();
    const QJsonObject ingestion = cdn.value("ingestionInfo").toObject();
    info.ingestion_address = ingestion.value("ingestionAddress").toString().toStdString();
    info.stream_name = ingestion.value("streamName").toString().toStdString();
    if (info.stream_id.empty() || info.ingestion_address.empty() || info.stream_name.empty()) {
        error = api_message(document);
        if (error.empty())
            error = "YouTube did not return a usable ingestion address or stream key.";
        return false;
    }
    return true;
}

bool parse_youtube_channel(const QJsonDocument &document, std::string &channel_id, std::string &channel_title,
                           std::string &error)
{
    channel_id.clear();
    channel_title.clear();
    error.clear();
    if (!document.isObject()) {
        error = "YouTube returned an invalid channel response.";
        return false;
    }
    const QJsonArray items = document.object().value("items").toArray();
    if (items.isEmpty()) {
        error = api_message(document);
        if (error.empty())
            error = "YouTube returned an incomplete channel profile.";
        return false;
    }
    const QJsonObject channel = items.first().toObject();
    channel_id = channel.value("id").toString().toStdString();
    channel_title = channel.value("snippet").toObject().value("title").toString().toStdString();
    if (channel_id.empty()) {
        error = "YouTube returned an incomplete channel profile.";
        return false;
    }
    return true;
}

YouTubeErrorClassification classify_youtube_error(const QJsonDocument &document, int http_status,
                                                  const std::string &transport_error)
{
    if (http_status == 401)
        return {YouTubeErrorCategory::Authentication, false};
    if (http_status == 403)
        return {YouTubeErrorCategory::Permission, false};
    if (http_status == 429)
        return {YouTubeErrorCategory::RateLimited, true};
    if (http_status >= 500)
        return {YouTubeErrorCategory::Temporary, true};
    if (!transport_error.empty())
        return {YouTubeErrorCategory::Network, true};
    if (api_message(document).empty())
        return {YouTubeErrorCategory::InvalidResponse, false};
    return {YouTubeErrorCategory::Unknown, false};
}

std::string format_youtube_error(const QJsonDocument &document, int http_status,
                                 const std::string &transport_error)
{
    const YouTubeErrorClassification classification = classify_youtube_error(document, http_status, transport_error);
    const std::string details = !api_message(document).empty()
                                    ? api_message(document)
                                    : (transport_error.empty() ? "YouTube returned an invalid response"
                                                               : transport_error);
    switch (classification.category) {
    case YouTubeErrorCategory::Authentication:
        return "YouTube session expired or was revoked. Reconnect YouTube. " + details;
    case YouTubeErrorCategory::Permission:
        return "YouTube denied permission for this operation. Check the requested scopes. " + details;
    case YouTubeErrorCategory::RateLimited:
        return "YouTube rate-limited the request or the daily quota was exceeded. Try again later. " + details;
    case YouTubeErrorCategory::Temporary:
        return "YouTube is temporarily unavailable. Try again later. " + details;
    case YouTubeErrorCategory::Network:
        return "A network error occurred while contacting YouTube: " + details;
    case YouTubeErrorCategory::InvalidResponse:
        return "YouTube returned an invalid response.";
    case YouTubeErrorCategory::Unknown:
        return details;
    }
    return details;
}

} // namespace multistream
