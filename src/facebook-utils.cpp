// SPDX-License-Identifier: GPL-2.0-or-later

#include "facebook-utils.hpp"

#include <QJsonObject>
#include <QUrlQuery>

#include <cstring>

namespace multistream {
namespace {

int graph_error_code(const QJsonDocument &document)
{
    const QJsonObject error = document.object().value("error").toObject();
    return error.value("code").toInt(-1);
}

std::string graph_error_details(const QJsonDocument &document)
{
    const QJsonObject root = document.object();
    const QJsonObject error = root.value("error").toObject();
    const QString message = error.value("message").toString();
    if (message.isEmpty())
        return {};

    const QString code = error.value("code").toVariant().toString();
    const QString subcode = error.value("error_subcode").toVariant().toString();
    QString details = message;
    if (!code.isEmpty())
        details += " (code " + code;
    if (!subcode.isEmpty())
        details += ", subcode " + subcode;
    if (!code.isEmpty())
        details += ")";
    return details.toStdString();
}

} // namespace

bool validate_facebook_redirect(const QUrl &redirected)
{
    // The OAuth flow now uses a loopback redirect served by a local HTTP server
    // (RFC 8252). Accept the loopback host on any port. The legacy desktop
    // callback is still accepted for backward compatibility.
    if (!redirected.isValid())
        return false;
    if (redirected.scheme() == "http") {
        const QString host = redirected.host();
        return host == "127.0.0.1" || host == "localhost" || host == "::1";
    }
    if (redirected.scheme() == "https")
        return redirected.host() == "www.facebook.com" && redirected.port() == -1 &&
               redirected.path() == "/connect/login_success.html";
    return false;
}

bool parse_facebook_oauth_redirect(const QUrl &redirected, const std::string &expected_state,
                                   std::string &token, std::string &error)
{
    token.clear();
    error.clear();
    if (!validate_facebook_redirect(redirected)) {
        error = "The URL must be the registered Facebook callback.";
        return false;
    }

    // Implicit responses place values in the fragment; loopback captures may
    // forward them as the query string. Accept either location.
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
        error = "Facebook authorization was incomplete or the state did not match.";
        return false;
    }

    token = access_token.toStdString();
    return true;
}

bool parse_facebook_oauth_values(const std::string &access_token, const std::string &state,
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
        error = "Facebook authorization was incomplete or the state did not match.";
        return false;
    }
    token = access_token;
    return true;
}

bool parse_facebook_pasted_input(const std::string &pasted, const std::string &expected_state,
                                 std::string &token, std::string &error)
{
    token.clear();
    error.clear();

    QString value = QString::fromStdString(pasted).trimmed();
    if (value.isEmpty()) {
        error = "Paste the connection code or the Facebook URL first.";
        return false;
    }

    // Shape 1: bridge-page code "mstoken:v1:<access_token>:<state>".
    if (value.startsWith(QStringLiteral("mstoken:v1:"))) {
        const QString rest = value.mid(static_cast<int>(std::strlen("mstoken:v1:")));
        const int separator = rest.lastIndexOf(QLatin1Char(':'));
        const QString access_token = separator >= 0 ? rest.left(separator) : rest;
        const QString state = separator >= 0 ? rest.mid(separator + 1) : QString();
        return parse_facebook_oauth_values(access_token.toStdString(), state.toStdString(), {}, {},
                                           expected_state, token, error);
    }

    // Shape 2: a full redirect URL (login_success.html or the bridge page).
    if (value.startsWith(QStringLiteral("http://")) || value.startsWith(QStringLiteral("https://"))) {
        const QUrl url(value, QUrl::TolerantMode);
        const QString source = !url.fragment().isEmpty() ? url.fragment() : url.query();
        const QUrlQuery params(source);
        const QString error_description = params.queryItemValue("error_description");
        if (!error_description.isEmpty()) {
            error = error_description.toStdString();
            return false;
        }
        const QString access_token = params.queryItemValue("access_token");
        const QString state = params.queryItemValue("state");
        if (access_token.isEmpty()) {
            error = "The pasted URL does not contain a Facebook access token.";
            return false;
        }
        return parse_facebook_oauth_values(access_token.toStdString(), state.toStdString(), {}, {},
                                           expected_state, token, error);
    }

    // Shape 3: a bare access token. State cannot be verified in this case.
    token = value.toStdString();
    return true;
}

FacebookErrorClassification classify_facebook_error(const QJsonDocument &document, int http_status,
                                                    const std::string &transport_error)
{
    const int code = graph_error_code(document);
    if (http_status == 401 || code == 190)
        return {FacebookErrorCategory::Authentication, false};
    if (http_status == 403 || code == 10 || code == 200)
        return {FacebookErrorCategory::Permission, false};
    if (http_status == 429 || code == 4)
        return {FacebookErrorCategory::RateLimited, true};
    if (http_status >= 500 || code == 2)
        return {FacebookErrorCategory::Temporary, true};
    if (!transport_error.empty())
        return {FacebookErrorCategory::Network, true};
    if (!document.isObject() || graph_error_details(document).empty())
        return {FacebookErrorCategory::InvalidResponse, false};
    return {FacebookErrorCategory::Unknown, false};
}

std::string format_facebook_json_error(const QJsonDocument &document, const std::string &transport_error)
{
    const std::string details = graph_error_details(document);
    if (!details.empty())
        return details;
    if (!transport_error.empty())
        return transport_error;
    return "Facebook returned an invalid response";
}

std::string format_facebook_error(const QJsonDocument &document, int http_status,
                                  const std::string &transport_error)
{
    const FacebookErrorClassification classification = classify_facebook_error(document, http_status, transport_error);
    const std::string details = format_facebook_json_error(document, transport_error);
    std::string prefix;
    switch (classification.category) {
    case FacebookErrorCategory::Authentication:
        prefix = "Facebook session expired or was revoked. Reconnect Facebook. ";
        break;
    case FacebookErrorCategory::Permission:
        prefix = "Facebook denied permission for this operation. Check the app and Page permissions. ";
        break;
    case FacebookErrorCategory::RateLimited:
        prefix = "Facebook rate-limited the request. Try again later. ";
        break;
    case FacebookErrorCategory::Temporary:
        prefix = "Facebook is temporarily unavailable. Try again later. ";
        break;
    case FacebookErrorCategory::Network:
        prefix = "A network error occurred while contacting Facebook: ";
        break;
    case FacebookErrorCategory::InvalidResponse:
        prefix = "Facebook returned an invalid response. ";
        break;
    case FacebookErrorCategory::Unknown:
        break;
    }
    if (classification.category == FacebookErrorCategory::InvalidResponse)
        return "Facebook returned an invalid response.";
    return prefix + details;
}

bool split_facebook_stream_url(const QString &value, std::string &server, std::string &key)
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

std::string normalize_facebook_privacy(std::string value)
{
    if (value == "UNLISTED")
        return "ALL_FRIENDS";
    return value;
}

} // namespace multistream
