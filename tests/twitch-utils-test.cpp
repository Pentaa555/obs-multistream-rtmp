// SPDX-License-Identifier: GPL-2.0-or-later

#include "twitch-utils.hpp"

#include <QJsonDocument>
#include <QUrl>

#include <cassert>
#include <iostream>

int main()
{
    const QUrl redirect("http://127.0.0.1:54321/fragment?access_token=token-123&state=state-123");
    assert(multistream::validate_twitch_redirect(redirect));
    std::string token;
    std::string error;
    assert(multistream::parse_twitch_oauth_redirect(redirect, "state-123", token, error));
    assert(token == "token-123");
    assert(!multistream::parse_twitch_oauth_redirect(redirect, "wrong", token, error));
    assert(token.empty());
    // https scheme is no longer a valid loopback redirect.
    assert(!multistream::validate_twitch_redirect(
        QUrl("https://localhost:8765/oauth/twitch/callback#access_token=token&state=state")));
    // Loopback host on any port is accepted.
    assert(multistream::validate_twitch_redirect(
        QUrl("http://127.0.0.1:8766/fragment?access_token=token&state=state")));

    // Value-based helper used by the loopback capture path.
    std::string vtoken;
    std::string verror;
    assert(multistream::parse_twitch_oauth_values("tok", "st", "", "", "st", vtoken, verror));
    assert(vtoken == "tok" && verror.empty());
    assert(!multistream::parse_twitch_oauth_values("tok", "st", "", "", "other", vtoken, verror));
    assert(vtoken.empty());
    assert(!multistream::parse_twitch_oauth_values("", "st", "access_denied", "The user denied you access", "st",
                                                   vtoken, verror));
    assert(verror == "The user denied you access");

    const auto auth = multistream::classify_twitch_error(QJsonDocument::fromJson(R"({"message":"bad token"})"), 401);
    assert(auth.category == multistream::TwitchErrorCategory::Authentication);
    const auto retry = multistream::classify_twitch_error(QJsonDocument::fromJson(R"({"message":"busy"})"), 429);
    assert(retry.retryable);
    assert(multistream::format_twitch_error(QJsonDocument::fromJson(R"({"message":"Denied"})"), 403).find("permission") != std::string::npos);

    std::string server;
    std::string key;
    assert(multistream::split_twitch_stream_url("rtmps://live.twitch.tv/app/stream-key", server, key));
    assert(server == "rtmps://live.twitch.tv/app/");
    assert(key == "stream-key");
    assert(!multistream::split_twitch_stream_url("rtmp://live.twitch.tv/app/key", server, key));

    std::cout << "twitch utils tests passed\n";
    return 0;
}
