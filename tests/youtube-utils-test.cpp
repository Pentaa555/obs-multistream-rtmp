// SPDX-License-Identifier: GPL-2.0-or-later

#include "youtube-utils.hpp"

#include <QJsonDocument>
#include <QUrl>

#include <cassert>
#include <iostream>

int main()
{
    using namespace multistream;

    // Loopback redirect validation.
    assert(validate_youtube_redirect(QUrl("http://127.0.0.1:54321/?code=abc&state=st")));
    assert(validate_youtube_redirect(QUrl("http://localhost:8080/?code=abc&state=st")));
    assert(!validate_youtube_redirect(QUrl("https://accounts.google.com/o/oauth2/callback")));

    // OAuth value parsing.
    std::string code;
    std::string error;
    assert(parse_youtube_oauth_values("the-code", "st", "", "", "st", code, error));
    assert(code == "the-code" && error.empty());
    assert(!parse_youtube_oauth_values("the-code", "st", "", "", "other", code, error));
    assert(code.empty());
    assert(!parse_youtube_oauth_values("", "st", "access_denied", "User denied", "st", code, error));
    assert(error == "User denied");

    // Token response parsing.
    const QJsonDocument token = QJsonDocument::fromJson(
        R"({"access_token":"at","refresh_token":"rt","expires_in":3600,"token_type":"Bearer"})");
    std::string access;
    std::string refresh;
    int expires = 0;
    std::string terror;
    assert(parse_youtube_token_response(token, access, refresh, expires, terror));
    assert(access == "at" && refresh == "rt" && expires == 3600);
    const QJsonDocument token_error = QJsonDocument::fromJson(
        R"({"error":"invalid_grant","error_description":"Bad code"})");
    assert(!parse_youtube_token_response(token_error, access, refresh, expires, terror));
    assert(terror == "Bad code");

    // Resource id parsing.
    std::string id;
    std::string rerror;
    assert(parse_youtube_resource_id(QJsonDocument::fromJson(R"({"id":"broadcast-1"})"), id, rerror));
    assert(id == "broadcast-1");
    assert(!parse_youtube_resource_id(QJsonDocument::fromJson(R"({"error":{"message":"nope"}})"), id, rerror));
    assert(rerror == "nope");

    // Stream info parsing.
    const QJsonDocument stream = QJsonDocument::fromJson(
        R"({"id":"stream-1","cdn":{"ingestionInfo":{"ingestionAddress":"rtmp://a.rtmp.youtube.com/live2","streamName":"key-123"}}})");
    YouTubeStreamInfo info;
    std::string serror;
    assert(parse_youtube_stream_info(stream, info, serror));
    assert(info.stream_id == "stream-1");
    assert(info.ingestion_address == "rtmp://a.rtmp.youtube.com/live2");
    assert(info.stream_name == "key-123");
    assert(!parse_youtube_stream_info(QJsonDocument::fromJson(R"({"id":"stream-1"})"), info, serror));

    // Channel parsing.
    const QJsonDocument channel = QJsonDocument::fromJson(
        R"({"items":[{"id":"chan-1","snippet":{"title":"My Channel"}}]})");
    std::string channel_id;
    std::string channel_title;
    std::string cerror;
    assert(parse_youtube_channel(channel, channel_id, channel_title, cerror));
    assert(channel_id == "chan-1" && channel_title == "My Channel");
    assert(!parse_youtube_channel(QJsonDocument::fromJson(R"({"items":[]})"), channel_id, channel_title, cerror));

    // Error classification.
    assert(classify_youtube_error(QJsonDocument::fromJson(R"({"error":{"message":"x"}})"), 401).category ==
           YouTubeErrorCategory::Authentication);
    assert(classify_youtube_error(QJsonDocument::fromJson(R"({"error":{"message":"x"}})"), 429).retryable);
    assert(format_youtube_error(QJsonDocument::fromJson(R"({"error":{"message":"Denied"}})"), 403).find("permission") !=
           std::string::npos);

    std::cout << "youtube utils tests passed\n";
    return 0;
}
