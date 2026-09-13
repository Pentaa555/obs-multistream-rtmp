// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QJsonDocument>
#include <QUrl>

#include <string>

namespace multistream {

enum class TwitchErrorCategory {
    Authentication,
    Permission,
    RateLimited,
    Temporary,
    Network,
    InvalidResponse,
    Unknown,
};

struct TwitchErrorClassification {
    TwitchErrorCategory category = TwitchErrorCategory::Unknown;
    bool retryable = false;
};

bool validate_twitch_redirect(const QUrl &redirected);
bool parse_twitch_oauth_redirect(const QUrl &redirected, const std::string &expected_state,
                                 std::string &token, std::string &error);
bool parse_twitch_oauth_values(const std::string &access_token, const std::string &state,
                               const std::string &error_code, const std::string &error_detail,
                               const std::string &expected_state, std::string &token, std::string &error);
TwitchErrorClassification classify_twitch_error(const QJsonDocument &document, int http_status,
                                                const std::string &transport_error = {});
std::string format_twitch_error(const QJsonDocument &document, int http_status,
                                const std::string &transport_error = {});
bool split_twitch_stream_url(const QString &value, std::string &server, std::string &key);

} // namespace multistream
