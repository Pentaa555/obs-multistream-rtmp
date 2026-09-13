// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace multistream {

class OperationGeneration {
public:
    using Id = std::uint64_t;

    Id begin()
    {
        if (current_ == UINT64_MAX)
            current_ = 1;
        else
            ++current_;
        return current_;
    }

    void invalidate() { begin(); }
    bool is_current(Id generation) const { return generation != 0 && generation == current_; }
    Id current() const { return current_; }

private:
    Id current_ = 0;
};

enum class DestinationState {
    Configuring,
    Starting,
    Connecting,
    Streaming,
    Reconnecting,
    Stopping,
    Stopped,
    Error,
};

struct DestinationConfig {
    std::string id;
    std::string name;
    std::string rtmp_url;
    std::string stream_key;
    bool enabled = true;
    std::size_t order = 0;
    std::string provider_id = "rtmp_custom";
    std::string facebook_page_id;
    std::string facebook_page_name;
    std::string facebook_title;
    std::string facebook_description;
    std::string facebook_privacy = "PUBLIC";
    std::string facebook_target = "page";
    std::string twitch_title;
    std::string youtube_title;
    std::string youtube_description;
    std::string youtube_privacy = "public";
};

bool is_facebook_destination(const DestinationConfig &config);
bool is_facebook_timeline(const DestinationConfig &config);
bool is_twitch_destination(const DestinationConfig &config);
bool is_youtube_destination(const DestinationConfig &config);
bool is_managed_destination(const DestinationConfig &config);

struct ValidationResult {
    bool valid = false;
    std::string message_key;
};

ValidationResult validate_destination(const DestinationConfig &config);
const char *state_text_key(DestinationState state);
std::string generate_destination_id();

} // namespace multistream
