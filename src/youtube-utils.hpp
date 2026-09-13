// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QJsonDocument>
#include <QUrl>

#include <string>

namespace multistream {

enum class YouTubeErrorCategory {
    Authentication,
    Permission,
    RateLimited,
    Temporary,
    Network,
    InvalidResponse,
    Unknown,
};

struct YouTubeErrorClassification {
    YouTubeErrorCategory category = YouTubeErrorCategory::Unknown;
    bool retryable = false;
};

// Result of a YouTube liveStream creation: the RTMP ingestion address and the
// stream name (stream key) OBS needs to publish.
struct YouTubeStreamInfo {
    std::string stream_id;
    std::string ingestion_address; // e.g. rtmp://a.rtmp.youtube.com/live2
    std::string stream_name;       // stream key appended to the ingestion address
};

// Accept the loopback redirect served by the local HTTP server on any port.
bool validate_youtube_redirect(const QUrl &redirected);

// Validate the authorization-code response captured from the loopback query
// string against the expected CSRF state.
bool parse_youtube_oauth_values(const std::string &code, const std::string &state,
                                const std::string &error_code, const std::string &error_detail,
                                const std::string &expected_state, std::string &out_code, std::string &error);

// Extract access_token / refresh_token / expires_in from the token endpoint JSON.
bool parse_youtube_token_response(const QJsonDocument &document, std::string &access_token,
                                  std::string &refresh_token, int &expires_in_seconds, std::string &error);

// Extract a resource id (used for liveBroadcasts.insert and liveStreams.insert).
bool parse_youtube_resource_id(const QJsonDocument &document, std::string &id, std::string &error);

// Extract the ingestion address and stream name from a liveStreams response.
bool parse_youtube_stream_info(const QJsonDocument &document, YouTubeStreamInfo &info, std::string &error);

// Extract the channel title from a channels response.
bool parse_youtube_channel(const QJsonDocument &document, std::string &channel_id, std::string &channel_title,
                           std::string &error);

YouTubeErrorClassification classify_youtube_error(const QJsonDocument &document, int http_status,
                                                  const std::string &transport_error = {});
std::string format_youtube_error(const QJsonDocument &document, int http_status,
                                 const std::string &transport_error = {});

} // namespace multistream
