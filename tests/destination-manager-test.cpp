// SPDX-License-Identifier: GPL-2.0-or-later

#include "destination-manager.hpp"

#include <cassert>
#include <iostream>
#include <string>

namespace {

obs_data_t *make_destination_root(long long schema_version, bool include_schema_version = true)
{
    obs_data_t *root = obs_data_create();
    if (include_schema_version)
        obs_data_set_int(root, "schema_version", schema_version);

    obs_data_t *item = obs_data_create();
    obs_data_set_string(item, "id", "legacy-destination");
    obs_data_set_string(item, "name", "Legacy destination");
    obs_data_set_string(item, "rtmp_url", "rtmp://example.test/live");
    obs_data_set_string(item, "stream_key", "secret");
    obs_data_array_t *destinations = obs_data_array_create();
    obs_data_array_push_back(destinations, item);
    obs_data_release(item);

    obs_data_t *invalid_item = obs_data_create();
    obs_data_set_string(invalid_item, "id", "invalid-destination");
    obs_data_set_string(invalid_item, "name", "Invalid destination");
    obs_data_set_string(invalid_item, "rtmp_url", "https://example.test/live");
    obs_data_set_string(invalid_item, "stream_key", "secret");
    obs_data_array_push_back(destinations, invalid_item);
    obs_data_release(invalid_item);

    obs_data_set_array(root, "destinations", destinations);
    obs_data_array_release(destinations);
    return root;
}

multistream::DestinationConfig generic_config(const std::string &id)
{
    multistream::DestinationConfig config;
    config.id = id;
    config.name = "Destination";
    config.rtmp_url = "rtmp://example.test/live";
    config.stream_key = "secret";
    return config;
}

} // namespace

int main()
{
    bool save_success = true;
    long long saved_schema_version = 0;
    int load_mode = 0;
    std::string loaded_filename;
    bool removed_recovery = false;

    multistream::DestinationManager::PersistenceHooks hooks;
    hooks.profile_directory = []() { return std::string("/virtual/multistream-profile"); };
    hooks.load = [&](const std::string &filename) {
        loaded_filename = filename;
        if (filename.find("recovery") != std::string::npos)
            return static_cast<obs_data_t *>(nullptr);
        if (load_mode == 1)
            return make_destination_root(99);
        if (load_mode == 2)
            return static_cast<obs_data_t *>(nullptr);
        return make_destination_root(0, false);
    };
    hooks.save = [&](obs_data_t *root, const std::string &filename) {
        if (filename.find("recovery") != std::string::npos)
            return save_success;
        saved_schema_version = obs_data_get_int(root, "schema_version");
        return save_success;
    };
    hooks.remove = [&](const std::string &) {
        removed_recovery = true;
        return save_success;
    };

    multistream::DestinationManager manager(hooks);
    assert(manager.load() == multistream::DestinationManager::LoadStatus::Loaded);
    assert(loaded_filename.find("obs-multistream-rtmp.json") != std::string::npos);
    assert(manager.destinations().size() == 1);
    const auto &legacy = manager.destinations().front();
    assert(legacy.provider_id == "rtmp_custom");
    assert(legacy.facebook_privacy == "PUBLIC");
    assert(legacy.facebook_target == "page");
    assert(legacy.enabled);

    save_success = false;
    assert(!manager.add(generic_config("save-fails")));
    assert(manager.find("save-fails") == nullptr);

    save_success = true;
    assert(manager.add(generic_config("saved")));
    assert(saved_schema_version == 1);

    save_success = false;
    auto updated = *manager.find("saved");
    updated.name = "Must not be committed";
    assert(!manager.update(updated));
    assert(manager.find("saved")->name == "Destination");

    assert(!manager.reorder({"saved", "legacy-destination"}));
    assert(manager.destinations().front().id == "legacy-destination");

    const auto legacy_count = manager.destinations().size();
    load_mode = 1;
    assert(manager.load() == multistream::DestinationManager::LoadStatus::UnsupportedVersion);
    assert(manager.destinations().size() == legacy_count);

    load_mode = 2;
    assert(manager.load() == multistream::DestinationManager::LoadStatus::NoFile);
    assert(manager.destinations().empty());

    save_success = false;
    const auto recovery_config = generic_config("recovery");
    assert(!manager.mark_recovery_pending(recovery_config));
    assert(!manager.has_recovery_notice());
    save_success = true;
    assert(manager.mark_recovery_pending(recovery_config));
    assert(manager.has_recovery_notice());
    save_success = false;
    assert(!manager.clear_recovery_pending("recovery"));
    assert(manager.has_recovery_notice());
    save_success = true;
    assert(manager.clear_recovery_pending("recovery"));
    assert(!manager.has_recovery_notice());
    assert(removed_recovery);

    std::cout << "destination manager persistence tests passed\n";
    return 0;
}
