// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "destination.hpp"

#include <obs.h>

#include <string>

namespace multistream {

// Provider abstraction keeps destination credentials separate from output lifecycle.
// OAuth providers can later resolve credentials and service settings without changing
// DestinationManager or the Qt dock.
class DestinationProvider {
public:
    virtual ~DestinationProvider() = default;

    virtual const char *service_id() const = 0;
    virtual bool configure_service(obs_data_t *settings, const DestinationConfig &config,
                                   std::string &error) const = 0;
};

class GenericRtmpProvider final : public DestinationProvider {
public:
    const char *service_id() const override;
    bool configure_service(obs_data_t *settings, const DestinationConfig &config,
                           std::string &error) const override;
};

} // namespace multistream
