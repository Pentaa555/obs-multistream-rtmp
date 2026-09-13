// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QJsonDocument>
#include <QUrl>

#include <string>

namespace multistream {

enum class FacebookErrorCategory {
    Authentication,
    Permission,
    RateLimited,
    Temporary,
    Network,
    InvalidResponse,
    Unknown,
};

struct FacebookErrorClassification {
    FacebookErrorCategory category = FacebookErrorCategory::Unknown;
    bool retryable = false;
};

bool validate_facebook_redirect(const QUrl &redirected);
bool parse_facebook_oauth_redirect(const QUrl &redirected, const std::string &expected_state,
                                   std::string &token, std::string &error);
bool parse_facebook_oauth_values(const std::string &access_token, const std::string &state,
                                 const std::string &error_code, const std::string &error_detail,
                                 const std::string &expected_state, std::string &token, std::string &error);
FacebookErrorClassification classify_facebook_error(const QJsonDocument &document, int http_status,
                                                    const std::string &transport_error = {});
std::string format_facebook_json_error(const QJsonDocument &document, const std::string &transport_error = {});
std::string format_facebook_error(const QJsonDocument &document, int http_status,
                                  const std::string &transport_error = {});
bool split_facebook_stream_url(const QString &value, std::string &server, std::string &key);
std::string normalize_facebook_privacy(std::string value);

} // namespace multistream
