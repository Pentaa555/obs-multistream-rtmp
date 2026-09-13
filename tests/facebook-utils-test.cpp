// SPDX-License-Identifier: GPL-2.0-or-later

#include "facebook-utils.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <cassert>
#include <iostream>
#include <string>

using multistream::FacebookErrorCategory;
using multistream::FacebookErrorClassification;
using multistream::classify_facebook_error;
using multistream::format_facebook_json_error;
using multistream::normalize_facebook_privacy;
using multistream::parse_facebook_oauth_redirect;
using multistream::parse_facebook_oauth_values;
using multistream::split_facebook_stream_url;
using multistream::validate_facebook_redirect;

int main()
{
    const QUrl valid_redirect(
        "http://127.0.0.1:54321/fragment?access_token=token-123&state=state-123");
    assert(validate_facebook_redirect(valid_redirect));
    const QUrl embedded_redirect(
        "https://www.facebook.com/connect/login_success.html#access_token=token-456&state=state-456");
    assert(validate_facebook_redirect(embedded_redirect));

    std::string token;
    std::string error;
    assert(parse_facebook_oauth_redirect(embedded_redirect, "state-456", token, error));
    assert(token == "token-456");
    assert(error.empty());

    assert(parse_facebook_oauth_redirect(valid_redirect, "state-123", token, error));
    assert(token == "token-123");
    assert(error.empty());

    // Loopback host on any port is accepted.
    assert(validate_facebook_redirect(
        QUrl("http://localhost:8766/fragment?access_token=token&state=state")));
    // Non-loopback http hosts are rejected.
    assert(!validate_facebook_redirect(
        QUrl("http://example.test:8765/oauth/facebook/callback#access_token=token&state=state")));
    // The old https loopback callback is no longer valid.
    assert(!validate_facebook_redirect(
        QUrl("https://localhost:8765/oauth/facebook/callback#access_token=token&state=state")));
    assert(!validate_facebook_redirect(
        QUrl("https://www.facebook.com:443/connect/login_success.html#access_token=token&state=state")));

    assert(!parse_facebook_oauth_redirect(valid_redirect, "wrong-state", token, error));
    assert(token.empty());
    assert(error == "Facebook authorization was incomplete or the state did not match.");

    const QUrl missing_token("http://127.0.0.1:54321/fragment?state=state-123");
    assert(!parse_facebook_oauth_redirect(missing_token, "state-123", token, error));
    assert(token.empty());

    const QUrl oauth_error(
        "http://127.0.0.1:54321/fragment?error_description=The%20user%20cancelled");
    assert(!parse_facebook_oauth_redirect(oauth_error, "state-123", token, error));
    assert(error == "The user cancelled");

    // Value-based helper used by the loopback capture path.
    std::string vtoken;
    std::string verror;
    assert(parse_facebook_oauth_values("tok", "st", "", "", "st", vtoken, verror));
    assert(vtoken == "tok" && verror.empty());
    assert(!parse_facebook_oauth_values("tok", "st", "", "", "other", vtoken, verror));
    assert(vtoken.empty());
    assert(!parse_facebook_oauth_values("", "st", "access_denied", "Denied", "st", vtoken, verror));
    assert(verror == "Denied");

    const QJsonDocument graph_error = QJsonDocument::fromJson(
        R"({"error":{"message":"Permission denied","code":200,"error_subcode":458}})");
    assert(format_facebook_json_error(graph_error) == "Permission denied (code 200, subcode 458)");
    assert(format_facebook_json_error(QJsonDocument(), "Connection timed out") == "Connection timed out");
    assert(format_facebook_json_error(QJsonDocument()) == "Facebook returned an invalid response");

    const FacebookErrorClassification classification = classify_facebook_error(graph_error, 403);
    assert(classification.category == FacebookErrorCategory::Permission);
    assert(!classification.retryable);
    const FacebookErrorClassification rate_limited = classify_facebook_error(
        QJsonDocument::fromJson(R"({"error":{"message":"Rate limited","code":4}})"), 429);
    assert(rate_limited.category == FacebookErrorCategory::RateLimited);
    assert(rate_limited.retryable);
    const FacebookErrorClassification temporary = classify_facebook_error(
        QJsonDocument::fromJson(R"({"error":{"message":"Unavailable","code":2}})"), 500);
    assert(temporary.category == FacebookErrorCategory::Temporary);
    assert(temporary.retryable);
    const FacebookErrorClassification network = classify_facebook_error(QJsonDocument(), 0, "Timeout");
    assert(network.category == FacebookErrorCategory::Network);
    const FacebookErrorClassification invalid = classify_facebook_error(QJsonDocument(), 200);
    assert(invalid.category == FacebookErrorCategory::InvalidResponse);

    std::string server;
    std::string stream_key;
    assert(split_facebook_stream_url("rtmps://live.example.test/app/stream-key", server, stream_key));
    assert(server == "rtmps://live.example.test/app/");
    assert(stream_key == "stream-key");
    assert(!split_facebook_stream_url("rtmp://live.example.test/app/stream-key", server, stream_key));
    assert(!split_facebook_stream_url("rtmps:///stream-key", server, stream_key));
    assert(!split_facebook_stream_url("rtmps://live.example.test/app/", server, stream_key));

    assert(normalize_facebook_privacy("UNLISTED") == "ALL_FRIENDS");
    assert(normalize_facebook_privacy("PUBLIC") == "PUBLIC");

    std::cout << "facebook utils tests passed\n";
    return 0;
}
