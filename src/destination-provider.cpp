// SPDX-License-Identifier: GPL-2.0-or-later

#include "destination-provider.hpp"

namespace multistream {

const char *GenericRtmpProvider::service_id() const
{
    return "rtmp_custom";
}

bool GenericRtmpProvider::configure_service(obs_data_t *settings, const DestinationConfig &config,
                                             std::string &error) const
{
    if (!settings) {
        error = "RTMP service settings are unavailable";
        return false;
    }

    const ValidationResult validation = validate_destination(config);
    if (!validation.valid) {
        error = "Destination configuration is incomplete";
        return false;
    }

    obs_data_set_string(settings, "server", config.rtmp_url.c_str());
    obs_data_set_string(settings, "key", config.stream_key.c_str());
    return true;
}

} // namespace multistream
