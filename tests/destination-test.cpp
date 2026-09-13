// SPDX-License-Identifier: GPL-2.0-or-later

#include "destination.hpp"

#include <cassert>
#include <iostream>

using multistream::DestinationConfig;
using multistream::DestinationState;

int main()
{
    DestinationConfig config;
    config.id = "one";
    config.name = "YouTube";
    config.rtmp_url = "rtmps://example.test/live";
    config.stream_key = "secret";

    assert(multistream::validate_destination(config).valid);

    DestinationConfig twitch;
    twitch.id = "twitch";
    twitch.name = "Twitch";
    twitch.provider_id = "twitch";
    twitch.twitch_title = "Live title";
    assert(multistream::validate_destination(twitch).valid);
    twitch.twitch_title.clear();
    assert(!multistream::validate_destination(twitch).valid);
    assert(!multistream::validate_destination({"", "", "rtmp://example.test", "key", true, 0}).valid);
    assert(!multistream::validate_destination({"id", "Name", "https://example.test/live", "key", true, 0}).valid);
    assert(!multistream::validate_destination({"id", "Name", "rtmp://example.test/live", "", true, 0}).valid);
    assert(std::string(multistream::state_text_key(DestinationState::Starting)) == "Starting");
    assert(std::string(multistream::state_text_key(DestinationState::Stopping)) == "Stopping");

    multistream::OperationGeneration operations;
    const auto first_generation = operations.begin();
    assert(operations.is_current(first_generation));
    const auto second_generation = operations.begin();
    assert(second_generation != first_generation);
    assert(!operations.is_current(first_generation));
    assert(operations.is_current(second_generation));
    operations.invalidate();
    assert(!operations.is_current(second_generation));

    assert(!multistream::generate_destination_id().empty());

    std::cout << "destination model tests passed\n";
    return 0;
}
