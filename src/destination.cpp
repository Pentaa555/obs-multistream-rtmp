// SPDX-License-Identifier: GPL-2.0-or-later

#include "destination.hpp"

#include <atomic>
#include <chrono>
#include <cctype>
#include <sstream>

namespace multistream {
namespace {

bool empty_or_whitespace(const std::string &value)
{
    if (value.empty())
        return true;

    for (const unsigned char character : value) {
        if (!std::isspace(character))
            return false;
    }
    return true;
}

bool valid_rtmp_url(const std::string &url)
{
    if (url.find_first_of(" \t\r\n") != std::string::npos)
        return false;

    const bool has_scheme = url.rfind("rtmp://", 0) == 0 || url.rfind("rtmps://", 0) == 0;
    if (!has_scheme)
        return false;

    const std::size_t scheme_end = url.find("://");
    return scheme_end != std::string::npos && url.size() > scheme_end + 3;
}

} // namespace

bool is_facebook_destination(const DestinationConfig &config)
{
    return config.provider_id == "facebook";
}

bool is_facebook_timeline(const DestinationConfig &config)
{
    return is_facebook_destination(config) && config.facebook_target == "timeline";
}

bool is_twitch_destination(const DestinationConfig &config)
{
    return config.provider_id == "twitch";
}

bool is_youtube_destination(const DestinationConfig &config)
{
    return config.provider_id == "youtube";
}

bool is_managed_destination(const DestinationConfig &config)
{
    return is_facebook_destination(config) || is_twitch_destination(config) ||
           is_youtube_destination(config);
}

ValidationResult validate_destination(const DestinationConfig &config)
{
    if (empty_or_whitespace(config.id))
        return {false, "InvalidId"};
    if (empty_or_whitespace(config.name))
        return {false, "InvalidName"};

    if (is_facebook_destination(config)) {
        if (!is_facebook_timeline(config) && empty_or_whitespace(config.facebook_page_id))
            return {false, "InvalidFacebookPage"};
        if (empty_or_whitespace(config.facebook_title))
            return {false, "InvalidFacebookTitle"};
        return {true, {}};
    }

    if (is_twitch_destination(config)) {
        if (empty_or_whitespace(config.twitch_title))
            return {false, "InvalidTwitchTitle"};
        return {true, {}};
    }

    if (is_youtube_destination(config)) {
        if (empty_or_whitespace(config.youtube_title))
            return {false, "InvalidYouTubeTitle"};
        return {true, {}};
    }

    if (!valid_rtmp_url(config.rtmp_url))
        return {false, "InvalidUrl"};
    if (empty_or_whitespace(config.stream_key))
        return {false, "InvalidKey"};
    return {true, {}};
}

const char *state_text_key(DestinationState state)
{
    switch (state) {
    case DestinationState::Configuring:
        return "Configuring";
    case DestinationState::Starting:
        return "Starting";
    case DestinationState::Connecting:
        return "Connecting";
    case DestinationState::Streaming:
        return "Streaming";
    case DestinationState::Reconnecting:
        return "Reconnecting";
    case DestinationState::Stopping:
        return "Stopping";
    case DestinationState::Stopped:
        return "Stopped";
    case DestinationState::Error:
        return "Error";
    }
    return "Error";
}

std::string generate_destination_id()
{
    static std::atomic<unsigned long long> sequence{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream id;
    id << "destination-" << now << "-" << sequence.fetch_add(1);
    return id.str();
}

} // namespace multistream
