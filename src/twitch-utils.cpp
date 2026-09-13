// SPDX-License-Identifier: GPL-2.0-or-later

#include "twitch-utils.hpp"

#include <QJsonObject>
#include <QUrlQuery>

namespace multistream {
namespace {

QString api_message(const QJsonDocument &document)
{
    const QString message = document.object().value("message").toString().trimmed();
    return message;
}

} // namespace

bool validate_twitch_redirect(const QUrl &redirected)
{
    // The OAuth flow now uses a loopback redirect served by a local HTTP server
    // on an ephemeral port (RFC 8252). Accept the loopback host on any port.
    if (!redirected.isValid() || redirected.scheme() != "http")
        return false;
    const QString host = redirected.host();
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

bool parse_twitch_oauth_redirect(const QUrl &redirected, const std::string &expected_state,
                                 std::string &token, std::string &error)
{
    token.clear();
    error.clear();
    if (!validate_twitch_redirect(redirected)) {
        error = "The URL must be the loopback Twitch callback.";
        return false;
    }

    // Twitch implicit responses place values in the fragment; loopback captures
    // may forward them as the query string. Accept either location.
    const QString source = !redirected.fragment().isEmpty() ? redirected.fragment() : redirected.query();
    const QUrlQuery params(source);
    const QString error_description = params.queryItemValue("error_description");
    if (!error_description.isEmpty()) {
        error = error_description.toStdString();
        return false;
    }

    const QString state = params.queryItemValue("state");
    const QString access_token = params.queryItemValue("access_token");
    if (access_token.isEmpty() || state.toStdString() != expected_state) {
        error = "Twitch authorization was incomplete or the state did not match.";
        return false;
    }

    token = access_token.toStdString();
    return true;
}

bool parse_twitch_oauth_values(const std::string &access_token, const std::string &state,
                               const std::string &error_code, const std::string &error_detail,
                               const std::string &expected_state, std::string &token, std::string &error)
{
    token.clear();
    error.clear();
    if (!error_code.empty() || !error_detail.empty()) {
        error = !error_detail.empty() ? error_detail : error_code;
        return false;
    }
    if (access_token.empty() || state != expected_state) {
        error = "Twitch authorization was incomplete or the state did not match.";
        return false;
    }
    token = access_token;
    return true;
}

TwitchErrorClassification classify_twitch_error(const QJsonDocument &document, int http_status,
                                                const std::string &transport_error)
{
    if (http_status == 401)
        return {TwitchErrorCategory::Authentication, false};
    if (http_status == 403)
        return {TwitchErrorCategory::Permission, false};
    if (http_status == 429)
        return {TwitchErrorCategory::RateLimited, true};
    if (http_status >= 500)
        return {TwitchErrorCategory::Temporary, true};
    if (!transport_error.empty())
        return {TwitchErrorCategory::Network, true};
    if (!document.isObject() || api_message(document).isEmpty())
        return {TwitchErrorCategory::InvalidResponse, false};
    return {TwitchErrorCategory::Unknown, false};
}

std::string format_twitch_error(const QJsonDocument &document, int http_status,
                                const std::string &transport_error)
{
    const TwitchErrorClassification classification = classify_twitch_error(document, http_status, transport_error);
    const std::string details = !api_message(document).isEmpty() ? api_message(document).toStdString() :
                                (transport_error.empty() ? "Twitch returned an invalid response" : transport_error);
    switch (classification.category) {
    case TwitchErrorCategory::Authentication:
        return "Twitch session expired or was revoked. Reconnect Twitch. " + details;
    case TwitchErrorCategory::Permission:
        return "Twitch denied permission for this operation. Check the requested scopes. " + details;
    case TwitchErrorCategory::RateLimited:
        return "Twitch rate-limited the request. Try again later. " + details;
    case TwitchErrorCategory::Temporary:
        return "Twitch is temporarily unavailable. Try again later. " + details;
    case TwitchErrorCategory::Network:
        return "A network error occurred while contacting Twitch: " + details;
    case TwitchErrorCategory::InvalidResponse:
        return "Twitch returned an invalid response.";
    case TwitchErrorCategory::Unknown:
        return details;
    }
    return details;
}

bool split_twitch_stream_url(const QString &value, std::string &server, std::string &key)
{
    const QUrl url(value);
    if (!url.isValid() || url.scheme() != "rtmps" || url.host().isEmpty())
        return false;
    const int slash = value.lastIndexOf('/');
    if (slash < 0 || slash == value.size() - 1)
        return false;
    server = value.left(slash + 1).toStdString();
    key = value.mid(slash + 1).toStdString();
    return !server.empty() && !key.empty();
}

} // namespace multistream
