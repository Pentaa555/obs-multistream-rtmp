// SPDX-License-Identifier: GPL-2.0-or-later

#include "destination-manager.hpp"

#include <obs-frontend-api.h>
#include <util/bmem.h>
#include <util/config-file.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_set>
#include <utility>

namespace multistream {

struct DestinationManager::Runtime {
    DestinationManager *owner = nullptr;
    std::string id;
    obs_output_t *output = nullptr;
    obs_service_t *service = nullptr;
    DestinationState state = DestinationState::Stopped;
    std::string error;
    OperationId generation = 0;
    bool stop_requested = false;
};

namespace {

constexpr const char *kOutputId = "rtmp_output";
constexpr const char *kConfigFile = "obs-multistream-rtmp.json";
constexpr long long kCurrentSchemaVersion = 1;

std::string current_profile_directory()
{
    char *profile_path = obs_frontend_get_current_profile_path();
    if (!profile_path)
        return {};
    std::string path(profile_path);
    bfree(profile_path);
    return path;
}

bool is_draft_or_valid(const DestinationConfig &config)
{
    const ValidationResult result = validate_destination(config);
    return result.valid || result.message_key == "InvalidKey" || result.message_key == "InvalidFacebookPage" ||
           result.message_key == "InvalidFacebookTitle";
}

} // namespace

DestinationManager::DestinationManager() : DestinationManager(PersistenceHooks{}) {}

DestinationManager::DestinationManager(PersistenceHooks persistence) : persistence_(std::move(persistence))
{
    if (!persistence_.profile_directory)
        persistence_.profile_directory = []() { return current_profile_directory(); };
    if (!persistence_.load)
        persistence_.load = [](const std::string &filename) {
            return obs_data_create_from_json_file_safe(filename.c_str(), "bak");
        };
    if (!persistence_.save)
        persistence_.save = [](obs_data_t *root, const std::string &filename) {
            return obs_data_save_json_safe(root, filename.c_str(), "tmp", "bak");
        };
    if (!persistence_.remove)
        persistence_.remove = [](const std::string &filename) {
            return std::remove(filename.c_str()) == 0 || errno == ENOENT;
        };
}

DestinationManager::~DestinationManager()
{
    shutdown();
}

void DestinationManager::set_state_callback(StateCallback callback)
{
    state_callback_ = std::move(callback);
}

const DestinationConfig *DestinationManager::find(const std::string &id) const
{
    const auto iterator = std::find_if(destinations_.begin(), destinations_.end(),
                                       [&id](const DestinationConfig &config) { return config.id == id; });
    return iterator == destinations_.end() ? nullptr : &*iterator;
}

std::string DestinationManager::profile_file() const
{
    std::string path = persistence_.profile_directory ? persistence_.profile_directory() : std::string{};
    if (path.empty())
        return {};
    if (path.back() != '/' && path.back() != '\\')
        path += '/';
    return path + kConfigFile;
}

std::string DestinationManager::recovery_file() const
{
    const std::string config = profile_file();
    if (config.empty())
        return {};
    const std::size_t slash = config.find_last_of("/\\");
    return slash == std::string::npos ? "obs-multistream-rtmp-recovery.json"
                                      : config.substr(0, slash + 1) + "obs-multistream-rtmp-recovery.json";
}

DestinationManager::LoadStatus DestinationManager::load()
{
    shutdown();
    load_recovery_notices();

    const std::string filename = profile_file();
    if (filename.empty())
        return LoadStatus::NoProfile;

    obs_data_t *root = persistence_.load(filename);
    if (!root) {
        std::ifstream existing_file(filename);
        if (existing_file.good())
            return LoadStatus::InvalidFile;
        destinations_.clear();
        return LoadStatus::NoFile;
    }

    const long long schema_version = obs_data_has_user_value(root, "schema_version")
                                         ? obs_data_get_int(root, "schema_version")
                                         : 0;
    if (schema_version > kCurrentSchemaVersion) {
        blog(LOG_WARNING, "Unsupported multistream destination schema version: %lld", schema_version);
        obs_data_release(root);
        return LoadStatus::UnsupportedVersion;
    }

    std::vector<DestinationConfig> loaded_destinations;
    std::unordered_set<std::string> loaded_ids;
    obs_data_array_t *array = obs_data_get_array(root, "destinations");
    if (array) {
        const std::size_t count = obs_data_array_count(array);
        loaded_destinations.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            obs_data_t *item = obs_data_array_item(array, index);
            if (!item)
                continue;

            DestinationConfig config;
            config.id = obs_data_get_string(item, "id");
            config.name = obs_data_get_string(item, "name");
            config.provider_id = obs_data_has_user_value(item, "provider_id")
                                     ? obs_data_get_string(item, "provider_id")
                                     : "rtmp_custom";
            config.rtmp_url = obs_data_get_string(item, "rtmp_url");
            config.stream_key = obs_data_get_string(item, "stream_key");
            config.facebook_page_id = obs_data_get_string(item, "facebook_page_id");
            config.facebook_page_name = obs_data_get_string(item, "facebook_page_name");
            config.facebook_title = obs_data_get_string(item, "facebook_title");
            config.facebook_description = obs_data_get_string(item, "facebook_description");
            config.facebook_privacy = obs_data_has_user_value(item, "facebook_privacy")
                                          ? obs_data_get_string(item, "facebook_privacy")
                                          : "PUBLIC";
            config.facebook_target = obs_data_has_user_value(item, "facebook_target")
                                         ? obs_data_get_string(item, "facebook_target")
                                         : "page";
            config.twitch_title = obs_data_get_string(item, "twitch_title");
            config.youtube_title = obs_data_get_string(item, "youtube_title");
            config.youtube_description = obs_data_get_string(item, "youtube_description");
            config.youtube_privacy = obs_data_has_user_value(item, "youtube_privacy")
                                         ? obs_data_get_string(item, "youtube_privacy")
                                         : "public";
            config.enabled = !obs_data_has_user_value(item, "enabled") || obs_data_get_bool(item, "enabled");
            config.order = static_cast<std::size_t>(std::max<long long>(0, obs_data_get_int(item, "order")));

            if (config.id.empty())
                config.id = generate_destination_id();
            if (config.name.empty())
                config.name = "Destination";
            if (is_draft_or_valid(config) && loaded_ids.insert(config.id).second)
                loaded_destinations.push_back(std::move(config));
            else
                blog(LOG_WARNING, "Skipping invalid or duplicate multistream destination configuration");

            obs_data_release(item);
        }
        obs_data_array_release(array);
    }

    obs_data_release(root);
    std::stable_sort(loaded_destinations.begin(), loaded_destinations.end(),
                     [](const DestinationConfig &left, const DestinationConfig &right) {
                         return left.order < right.order;
                     });
    destinations_ = std::move(loaded_destinations);

    for (const DestinationConfig &config : destinations_) {
        if (state_callback_)
            state_callback_(config.id, DestinationState::Stopped, {}, 0);
    }
    return LoadStatus::Loaded;
}

bool DestinationManager::save() const
{
    const std::string filename = profile_file();
    if (filename.empty())
        return false;

    obs_data_t *root = obs_data_create();
    obs_data_array_t *array = obs_data_array_create();

    for (std::size_t index = 0; index < destinations_.size(); ++index) {
        const DestinationConfig &config = destinations_[index];
        obs_data_t *item = obs_data_create();
        obs_data_set_string(item, "id", config.id.c_str());
        obs_data_set_string(item, "name", config.name.c_str());
        obs_data_set_string(item, "provider_id", config.provider_id.c_str());
        obs_data_set_string(item, "rtmp_url", config.rtmp_url.c_str());
        obs_data_set_string(item, "stream_key", config.stream_key.c_str());
        obs_data_set_string(item, "facebook_page_id", config.facebook_page_id.c_str());
        obs_data_set_string(item, "facebook_page_name", config.facebook_page_name.c_str());
        obs_data_set_string(item, "facebook_title", config.facebook_title.c_str());
        obs_data_set_string(item, "facebook_description", config.facebook_description.c_str());
        obs_data_set_string(item, "facebook_privacy", config.facebook_privacy.c_str());
        obs_data_set_string(item, "facebook_target", config.facebook_target.c_str());
        obs_data_set_string(item, "twitch_title", config.twitch_title.c_str());
        obs_data_set_string(item, "youtube_title", config.youtube_title.c_str());
        obs_data_set_string(item, "youtube_description", config.youtube_description.c_str());
        obs_data_set_string(item, "youtube_privacy", config.youtube_privacy.c_str());
        obs_data_set_bool(item, "enabled", config.enabled);
        obs_data_set_int(item, "order", static_cast<long long>(index));
        obs_data_array_push_back(array, item);
        obs_data_release(item);
    }

    obs_data_set_int(root, "schema_version", kCurrentSchemaVersion);
    obs_data_set_array(root, "destinations", array);
    obs_data_array_release(array);
    const bool saved = persistence_.save(root, filename);
    obs_data_release(root);
    return saved;
}

void DestinationManager::load_recovery_notices()
{
    recovery_notices_.clear();
    const std::string filename = recovery_file();
    if (filename.empty())
        return;

    obs_data_t *root = persistence_.load(filename);
    if (!root)
        return;
    const long long schema_version = obs_data_has_user_value(root, "schema_version")
                                         ? obs_data_get_int(root, "schema_version")
                                         : 0;
    if (schema_version > kCurrentSchemaVersion) {
        obs_data_release(root);
        return;
    }
    obs_data_array_t *array = obs_data_get_array(root, "pending");
    if (array) {
        const std::size_t count = obs_data_array_count(array);
        recovery_notices_.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            obs_data_t *item = obs_data_array_item(array, index);
            if (!item)
                continue;
            RecoveryNotice notice;
            notice.destination_id = obs_data_get_string(item, "destination_id");
            notice.destination_name = obs_data_get_string(item, "destination_name");
            notice.timestamp = obs_data_get_int(item, "timestamp");
            if (!notice.destination_id.empty())
                recovery_notices_.push_back(std::move(notice));
            obs_data_release(item);
        }
        obs_data_array_release(array);
    }
    obs_data_release(root);
}

bool DestinationManager::save_recovery_notices() const
{
    const std::string filename = recovery_file();
    if (filename.empty())
        return false;
    if (recovery_notices_.empty())
        return persistence_.remove(filename);

    obs_data_t *root = obs_data_create();
    obs_data_array_t *array = obs_data_array_create();
    for (const RecoveryNotice &notice : recovery_notices_) {
        obs_data_t *item = obs_data_create();
        obs_data_set_string(item, "destination_id", notice.destination_id.c_str());
        obs_data_set_string(item, "destination_name", notice.destination_name.c_str());
        obs_data_set_int(item, "timestamp", notice.timestamp);
        obs_data_array_push_back(array, item);
        obs_data_release(item);
    }
    obs_data_set_int(root, "schema_version", kCurrentSchemaVersion);
    obs_data_set_array(root, "pending", array);
    obs_data_array_release(array);
    const bool saved = persistence_.save(root, filename);
    obs_data_release(root);
    return saved;
}

bool DestinationManager::mark_recovery_pending(const DestinationConfig &config)
{
    const auto previous = recovery_notices_;
    const auto iterator = std::find_if(recovery_notices_.begin(), recovery_notices_.end(),
                                       [&config](const RecoveryNotice &notice) {
                                           return notice.destination_id == config.id;
                                       });
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    if (iterator == recovery_notices_.end())
        recovery_notices_.push_back({config.id, config.name, timestamp});
    else {
        iterator->destination_name = config.name;
        iterator->timestamp = timestamp;
    }

    if (save_recovery_notices())
        return true;
    recovery_notices_ = previous;
    return false;
}

bool DestinationManager::clear_recovery_pending(const std::string &destination_id)
{
    const auto previous = recovery_notices_;
    const auto old_size = recovery_notices_.size();
    recovery_notices_.erase(std::remove_if(recovery_notices_.begin(), recovery_notices_.end(),
                                           [&destination_id](const RecoveryNotice &notice) {
                                               return notice.destination_id == destination_id;
                                           }),
                            recovery_notices_.end());
    if (recovery_notices_.size() == old_size || save_recovery_notices())
        return true;
    recovery_notices_ = previous;
    return false;
}

bool DestinationManager::add(DestinationConfig config)
{
    if (config.id.empty())
        config.id = generate_destination_id();
    if (config.name.empty())
        config.name = "Custom destination";
    config.order = destinations_.size();

    if (!is_draft_or_valid(config) || find(config.id))
        return false;

    destinations_.push_back(std::move(config));
    if (!save()) {
        destinations_.pop_back();
        return false;
    }
    return true;
}

bool DestinationManager::update(const DestinationConfig &config)
{
    if (!validate_destination(config).valid)
        return false;
    if (is_busy(config.id))
        return false;

    const auto iterator = std::find_if(destinations_.begin(), destinations_.end(),
                                       [&config](const DestinationConfig &item) { return item.id == config.id; });
    if (iterator == destinations_.end())
        return false;

    const DestinationConfig previous = *iterator;
    *iterator = config;
    if (!save()) {
        *iterator = previous;
        return false;
    }
    if (const auto runtime = runtimes_.find(config.id); runtime != runtimes_.end())
        release_runtime(*runtime->second, false);
    return true;
}

bool DestinationManager::remove(const std::string &id)
{
    if (is_busy(id))
        return false;

    const auto iterator = std::find_if(destinations_.begin(), destinations_.end(),
                                       [&id](const DestinationConfig &config) { return config.id == id; });
    if (iterator == destinations_.end())
        return false;

    const auto previous = destinations_;
    destinations_.erase(iterator);
    if (!save()) {
        destinations_ = previous;
        return false;
    }
    prepared_configs_.erase(id);
    clear_recovery_pending(id);
    if (const auto runtime = runtimes_.find(id); runtime != runtimes_.end()) {
        release_runtime(*runtime->second, false);
        runtimes_.erase(runtime);
    }
    return true;
}

bool DestinationManager::set_enabled(const std::string &id, bool enabled)
{
    auto iterator = std::find_if(destinations_.begin(), destinations_.end(),
                                 [&id](DestinationConfig &config) { return config.id == id; });
    if (iterator == destinations_.end())
        return false;

    const bool previous = iterator->enabled;
    iterator->enabled = enabled;
    if (!save()) {
        iterator->enabled = previous;
        return false;
    }
    if (!enabled && is_busy(id))
        stop(id);
    return true;
}

bool DestinationManager::reorder(const std::vector<std::string> &ordered_ids)
{
    if (ordered_ids.size() != destinations_.size())
        return false;

    std::vector<DestinationConfig> reordered;
    reordered.reserve(destinations_.size());
    for (const std::string &id : ordered_ids) {
        const auto iterator = std::find_if(destinations_.begin(), destinations_.end(),
                                           [&id](const DestinationConfig &config) { return config.id == id; });
        if (iterator == destinations_.end())
            return false;
        reordered.push_back(*iterator);
    }

    const auto previous = destinations_;
    destinations_ = std::move(reordered);
    if (!save()) {
        destinations_ = previous;
        return false;
    }
    return true;
}

bool DestinationManager::native_streaming_active() const
{
    obs_output_t *streaming_output = obs_frontend_get_streaming_output();
    if (!streaming_output)
        return false;
    const bool active = obs_output_active(streaming_output);
    obs_output_release(streaming_output);
    return active;
}

bool DestinationManager::create_fallback_encoders(std::string &error)
{
    if (fallback_video_encoder_ && fallback_audio_encoder_)
        return true;

    release_fallback_encoders();

    config_t *profile = obs_frontend_get_profile_config();
    const char *configured_video = profile ? config_get_string(profile, "SimpleOutput", "StreamEncoder") : nullptr;
    const char *video_id = "obs_x264";
    if (configured_video && strcmp(configured_video, "qsv") == 0)
        video_id = "obs_qsv11_v2";
    else if (configured_video && strcmp(configured_video, "nvenc") == 0)
        video_id = "obs_nvenc_h264_tex";
    else if (configured_video && strcmp(configured_video, "amd") == 0)
        video_id = "h264_texture_amf";

    obs_data_t *video_settings = obs_data_create();
    const int video_bitrate = profile ? (int)config_get_uint(profile, "SimpleOutput", "VBitrate") : 6000;
    obs_data_set_string(video_settings, "rate_control", "CBR");
    obs_data_set_int(video_settings, "bitrate", video_bitrate > 0 ? video_bitrate : 6000);
    if (profile) {
        const char *preset = config_get_string(profile, "SimpleOutput", "Preset");
        const char *x264_options = config_get_string(profile, "SimpleOutput", "x264Settings");
        if (preset && *preset)
            obs_data_set_string(video_settings, "preset", preset);
        if (x264_options && *x264_options)
            obs_data_set_string(video_settings, "x264opts", x264_options);
    }

    fallback_video_encoder_ = obs_video_encoder_create(video_id, "multistream_video", video_settings, nullptr);
    if (!fallback_video_encoder_ && strcmp(video_id, "obs_x264") != 0) {
        video_id = "obs_x264";
        fallback_video_encoder_ = obs_video_encoder_create(video_id, "multistream_video", video_settings, nullptr);
    }
    obs_data_release(video_settings);
    if (!fallback_video_encoder_) {
        error = std::string("Could not create video encoder '") + video_id + "'";
        return false;
    }

    audio_t *audio = obs_get_audio();
    video_t *video = obs_get_video();
    if (!video || !audio) {
        release_fallback_encoders();
        error = "OBS media outputs are not initialized";
        return false;
    }
    obs_encoder_set_video(fallback_video_encoder_, video);

    config_t *audio_profile = profile;
    const char *configured_audio =
        audio_profile ? config_get_string(audio_profile, "SimpleOutput", "StreamAudioEncoder") : nullptr;
    const char *audio_id = configured_audio && strcmp(configured_audio, "opus") == 0 ? "ffmpeg_opus" : "ffmpeg_aac";
    obs_data_t *audio_settings = obs_data_create();
    const int audio_bitrate = audio_profile ? (int)config_get_uint(audio_profile, "SimpleOutput", "ABitrate") : 160;
    obs_data_set_int(audio_settings, "bitrate", audio_bitrate > 0 ? audio_bitrate : 160);
    if (audio_profile) {
        const unsigned sample_rate = config_get_uint(audio_profile, "Audio", "SampleRate");
        if (sample_rate > 0)
            obs_data_set_int(audio_settings, "samplerate", sample_rate);
    }
    fallback_audio_encoder_ = obs_audio_encoder_create(audio_id, "multistream_audio", audio_settings, 0, nullptr);
    obs_data_release(audio_settings);
    if (!fallback_audio_encoder_) {
        release_fallback_encoders();
        error = std::string("Could not create audio encoder '") + audio_id + "'";
        return false;
    }
    obs_encoder_set_audio(fallback_audio_encoder_, audio);
    blog(LOG_INFO, "Using fallback streaming encoders: video=%s audio=%s", video_id, audio_id);
    return true;
}

void DestinationManager::release_fallback_encoders()
{
    if (fallback_video_encoder_) {
        obs_encoder_release(fallback_video_encoder_);
        fallback_video_encoder_ = nullptr;
    }
    if (fallback_audio_encoder_) {
        obs_encoder_release(fallback_audio_encoder_);
        fallback_audio_encoder_ = nullptr;
    }
}

bool DestinationManager::create_runtime(const DestinationConfig &config, OperationId generation,
                                         obs_output_t *encoder_source)
{
    auto report_creation_error = [this, &config, generation](std::string message) {
        auto failed_runtime = std::make_unique<Runtime>();
        failed_runtime->owner = this;
        failed_runtime->id = config.id;
        failed_runtime->generation = generation;
        failed_runtime->state = DestinationState::Error;
        failed_runtime->error = message;
        runtimes_[config.id] = std::move(failed_runtime);
        blog(LOG_ERROR, "Could not start destination '%s': %s", config.name.c_str(), message.c_str());
        emit_state(*runtimes_[config.id]);
        return false;
    };

    if (!validate_destination(config).valid)
        return report_creation_error("Destination configuration is incomplete");

    obs_output_t *main_output = nullptr;
    obs_output_t *encoder_output = encoder_source;
    if (!encoder_output) {
        if (native_streaming_active())
            return report_creation_error("Native OBS streaming is already active");
        main_output = obs_frontend_get_streaming_output();
        encoder_output = main_output;
    }

    auto release_main_output = [&]() {
        if (main_output) {
            obs_output_release(main_output);
            main_output = nullptr;
        }
    };

    obs_encoder_t *video_encoder = encoder_output ? obs_output_get_video_encoder(encoder_output) : nullptr;
    obs_encoder_t *audio_encoder = encoder_output ? obs_output_get_audio_encoder(encoder_output, 0) : nullptr;

    if (!video_encoder || !audio_encoder) {
        if (main_output) {
            release_main_output();
            main_output = nullptr;
        }

        std::string fallback_error;
        if (!create_fallback_encoders(fallback_error))
            return report_creation_error(std::move(fallback_error));

        video_encoder = fallback_video_encoder_;
        audio_encoder = fallback_audio_encoder_;
    }

    auto runtime = std::make_unique<Runtime>();
    runtime->owner = this;
    runtime->id = config.id;
    runtime->generation = generation;

    obs_data_t *output_settings = obs_data_create();
    runtime->output = obs_output_create(kOutputId, ("multistream-" + config.id).c_str(), output_settings, nullptr);
    obs_data_release(output_settings);
    if (!runtime->output) {
        release_main_output();
        return report_creation_error("Could not create RTMP output");
    }

    obs_data_t *service_settings = obs_data_create();
    std::string provider_error;
    if (!provider_.configure_service(service_settings, config, provider_error)) {
        obs_data_release(service_settings);
        obs_output_release(runtime->output);
        runtime->output = nullptr;
        release_main_output();
        return report_creation_error(std::move(provider_error));
    }
    runtime->service = obs_service_create(provider_.service_id(), ("multistream-service-" + config.id).c_str(),
                                          service_settings, nullptr);
    obs_data_release(service_settings);
    if (!runtime->service) {
        obs_output_release(runtime->output);
        runtime->output = nullptr;
        release_main_output();
        return report_creation_error("Could not create RTMP service");
    }

    obs_output_set_service(runtime->output, runtime->service);
    obs_output_set_video_encoder(runtime->output, video_encoder);
    obs_output_set_audio_encoder(runtime->output, audio_encoder, 0);
    obs_output_set_reconnect_settings(runtime->output, 10, 2);

    runtimes_[config.id] = std::move(runtime);
    Runtime &stored = *runtimes_[config.id];
    signal_handler_t *signals = obs_output_get_signal_handler(stored.output);
    signal_handler_connect(signals, "starting", &DestinationManager::on_starting, &stored);
    signal_handler_connect(signals, "start", &DestinationManager::on_started, &stored);
    signal_handler_connect(signals, "reconnect", &DestinationManager::on_reconnect, &stored);
    signal_handler_connect(signals, "reconnect_success", &DestinationManager::on_reconnect_success, &stored);
    signal_handler_connect(signals, "stopping", &DestinationManager::on_stopping, &stored);
    signal_handler_connect(signals, "stop", &DestinationManager::on_stopped, &stored);

    release_main_output();
    return true;
}

bool DestinationManager::release_runtime(Runtime &runtime, bool force)
{
    if (!runtime.output)
        return true;

    if (obs_output_active(runtime.output)) {
        if (!force)
            return false;
        obs_output_force_stop(runtime.output);
    }

    signal_handler_t *signals = obs_output_get_signal_handler(runtime.output);
    signal_handler_disconnect(signals, "starting", &DestinationManager::on_starting, &runtime);
    signal_handler_disconnect(signals, "start", &DestinationManager::on_started, &runtime);
    signal_handler_disconnect(signals, "reconnect", &DestinationManager::on_reconnect, &runtime);
    signal_handler_disconnect(signals, "reconnect_success", &DestinationManager::on_reconnect_success, &runtime);
    signal_handler_disconnect(signals, "stopping", &DestinationManager::on_stopping, &runtime);
    signal_handler_disconnect(signals, "stop", &DestinationManager::on_stopped, &runtime);

    obs_encoder_t *video_encoder = obs_output_get_video_encoder(runtime.output);
    if (video_encoder) {
        obs_output_set_video_encoder(runtime.output, nullptr);
        obs_encoder_release(video_encoder);
    }
    obs_encoder_t *audio_encoder = obs_output_get_audio_encoder(runtime.output, 0);
    if (audio_encoder) {
        obs_output_set_audio_encoder(runtime.output, nullptr, 0);
        obs_encoder_release(audio_encoder);
    }
    obs_output_set_service(runtime.output, nullptr);
    if (runtime.service) {
        obs_service_release(runtime.service);
        runtime.service = nullptr;
    }
    obs_output_release(runtime.output);
    runtime.output = nullptr;
    return true;
}

DestinationManager::OperationId DestinationManager::prepare_destination(const DestinationConfig &runtime_config)
{
    const DestinationConfig *stored_config = find(runtime_config.id);
    if (!stored_config || !stored_config->enabled || !validate_destination(runtime_config).valid)
        return 0;

    const OperationId generation = begin_start(runtime_config.id);
    if (generation == 0)
        return 0;
    prepared_configs_[runtime_config.id] = runtime_config;
    return generation;
}

bool DestinationManager::set_prepared_config(const std::string &id, const DestinationConfig &runtime_config,
                                             OperationId generation)
{
    if (!is_current(id, generation) || state(id) != DestinationState::Starting ||
        !validate_destination(runtime_config).valid)
        return false;
    prepared_configs_[id] = runtime_config;
    return true;
}

bool DestinationManager::start_prepared(const std::string &id, obs_output_t *native_output)
{
    if (!native_output)
        return false;

    const DestinationConfig *stored_config = find(id);
    if (!stored_config || !stored_config->enabled)
        return false;

    const auto prepared = prepared_configs_.find(id);
    const DestinationConfig runtime_config = prepared != prepared_configs_.end() ? prepared->second : *stored_config;
    OperationId generation = operation_generation(id);
    if (generation == 0 || state(id) != DestinationState::Starting) {
        generation = begin_start(id);
        if (generation == 0)
            return false;
    }

    auto existing = runtimes_.find(id);
    if (existing != runtimes_.end() && existing->second->output) {
        if (obs_output_active(existing->second->output))
            return false;
        if (!release_runtime(*existing->second, false))
            return false;
    }

    if (!create_runtime(runtime_config, generation, native_output))
        return false;

    Runtime &runtime = *runtimes_[id];
    set_runtime_state(runtime, DestinationState::Connecting);
    if (!obs_output_start(runtime.output)) {
        set_runtime_state(runtime, DestinationState::Error, "Could not start RTMP output");
        release_runtime(runtime, false);
        return false;
    }
    return true;
}

bool DestinationManager::start_native_secondaries(obs_output_t *native_output, const std::string &primary_id)
{
    if (!native_output)
        return false;

    bool success = true;
    for (const DestinationConfig &config : destinations_) {
        if (!config.enabled || config.id == primary_id || is_facebook_destination(config))
            continue;
        if (!start_prepared(config.id, native_output))
            success = false;
    }
    return success;
}

void DestinationManager::stop_native_secondaries(const std::string &primary_id)
{
    std::vector<std::string> ids;
    ids.reserve(destinations_.size());
    for (const DestinationConfig &config : destinations_)
        if (config.id != primary_id && !is_facebook_destination(config) && is_busy(config.id))
            ids.push_back(config.id);
    for (const std::string &id : ids)
        stop(id);
}

bool DestinationManager::set_native_primary_state(const std::string &id, OperationId generation,
                                                  DestinationState state, std::string error)
{
    if (!is_current(id, generation))
        return false;
    set_runtime_state(id, state, std::move(error), generation);
    return true;
}

void DestinationManager::clear_native_session()
{
    prepared_configs_.clear();
}

bool DestinationManager::start(const std::string &id)
{
    const DestinationConfig *config = find(id);
    if (!config || !config->enabled || is_managed_destination(*config))
        return false;
    return start_with_config(id, *config);
}

bool DestinationManager::start_with_config(const std::string &id, const DestinationConfig &runtime_config,
                                           OperationId generation)
{
    const DestinationConfig *stored_config = find(id);
    if (!stored_config || !stored_config->enabled || !validate_destination(runtime_config).valid)
        return false;

    if (generation == 0)
        generation = begin_start(id);
    else if (!is_current(id, generation) || state(id) != DestinationState::Starting)
        return false;

    if (generation == 0)
        return false;

    auto existing = runtimes_.find(id);
    if (existing != runtimes_.end() && existing->second->output) {
        if (obs_output_active(existing->second->output)) {
            set_runtime_state(id, DestinationState::Error, "Destination is already streaming", generation);
            return false;
        }
        if (!release_runtime(*existing->second, false)) {
            set_runtime_state(id, DestinationState::Error, "Destination is still stopping", generation);
            return false;
        }
    }

    if (!create_runtime(runtime_config, generation))
        return false;

    Runtime &runtime = *runtimes_[id];
    set_runtime_state(runtime, DestinationState::Connecting);
    if (!obs_output_start(runtime.output)) {
        set_runtime_state(runtime, DestinationState::Error, "Could not start RTMP output");
        release_runtime(runtime, false);
        return false;
    }
    return true;
}

DestinationManager::OperationId DestinationManager::allocate_generation()
{
    return generation_tracker_.begin();
}

DestinationManager::OperationId DestinationManager::begin_start(const std::string &id)
{
    const DestinationConfig *config = find(id);
    if (!config || !config->enabled || is_busy(id))
        return 0;

    auto iterator = runtimes_.find(id);
    if (iterator == runtimes_.end()) {
        auto runtime = std::make_unique<Runtime>();
        runtime->owner = this;
        runtime->id = id;
        iterator = runtimes_.emplace(id, std::move(runtime)).first;
    } else if (iterator->second->output && !release_runtime(*iterator->second, false)) {
        return 0;
    }

    Runtime &runtime = *iterator->second;
    runtime.generation = allocate_generation();
    runtime.stop_requested = false;
    runtime.state = DestinationState::Starting;
    runtime.error.clear();
    emit_state(runtime);
    return runtime.generation;
}

DestinationManager::OperationId DestinationManager::begin_stop(const std::string &id)
{
    const auto iterator = runtimes_.find(id);
    if (iterator == runtimes_.end())
        return 0;

    Runtime &runtime = *iterator->second;
    if (runtime.state == DestinationState::Stopping)
        return runtime.generation;
    if (runtime.state == DestinationState::Stopped && !runtime.output)
        return 0;
    if (runtime.state == DestinationState::Error && !runtime.output)
        return 0;

    runtime.generation = allocate_generation();
    runtime.stop_requested = true;
    runtime.state = DestinationState::Stopping;
    runtime.error.clear();
    emit_state(runtime);
    return runtime.generation;
}

void DestinationManager::set_configuring(const std::string &id)
{
    if (!find(id) || is_busy(id))
        return;
    auto iterator = runtimes_.find(id);
    if (iterator == runtimes_.end()) {
        auto runtime = std::make_unique<Runtime>();
        runtime->owner = this;
        runtime->id = id;
        iterator = runtimes_.emplace(id, std::move(runtime)).first;
    }
    Runtime &runtime = *iterator->second;
    runtime.generation = allocate_generation();
    runtime.stop_requested = false;
    set_runtime_state(runtime, DestinationState::Configuring);
}

void DestinationManager::set_error(const std::string &id, std::string error)
{
    if (!find(id))
        return;
    auto iterator = runtimes_.find(id);
    if (iterator == runtimes_.end()) {
        auto runtime = std::make_unique<Runtime>();
        runtime->owner = this;
        runtime->id = id;
        runtime->generation = allocate_generation();
        iterator = runtimes_.emplace(id, std::move(runtime)).first;
    }
    Runtime &runtime = *iterator->second;
    runtime.stop_requested = false;
    set_runtime_state(runtime, DestinationState::Error, std::move(error));
}

void DestinationManager::start_all()
{
    if (native_streaming_active()) {
        for (const DestinationConfig &config : destinations_)
            if (config.enabled)
                set_runtime_state(config.id, DestinationState::Error, "Native OBS streaming is already active");
        return;
    }

    for (const DestinationConfig &config : destinations_)
        if (config.enabled)
            start(config.id);
}

DestinationManager::OperationId DestinationManager::stop(const std::string &id)
{
    const OperationId generation = begin_stop(id);
    if (generation == 0)
        return 0;

    const auto iterator = runtimes_.find(id);
    if (iterator == runtimes_.end())
        return generation;
    if (!iterator->second->output) {
        complete_operation(id, generation, DestinationState::Stopped);
        return generation;
    }

    if (obs_output_active(iterator->second->output))
        obs_output_stop(iterator->second->output);
    else
        complete_operation(id, generation, DestinationState::Stopped);
    return generation;
}

void DestinationManager::stop_all()
{
    std::vector<std::string> ids;
    ids.reserve(runtimes_.size());
    for (const auto &runtime : runtimes_)
        ids.push_back(runtime.first);
    for (const std::string &id : ids)
        stop(id);
}

void DestinationManager::shutdown()
{
    if (shutting_down_)
        return;
    shutting_down_ = true;

    for (auto &runtime : runtimes_) {
        runtime.second->generation = allocate_generation();
        runtime.second->stop_requested = true;
        release_runtime(*runtime.second, true);
    }
    runtimes_.clear();
    prepared_configs_.clear();
    release_fallback_encoders();
    shutting_down_ = false;
}

bool DestinationManager::is_active(const std::string &id) const
{
    const auto iterator = runtimes_.find(id);
    return iterator != runtimes_.end() && iterator->second->output && obs_output_active(iterator->second->output);
}

bool DestinationManager::is_busy(const std::string &id) const
{
    const DestinationState current = state(id);
    return current == DestinationState::Configuring || current == DestinationState::Starting ||
           current == DestinationState::Connecting || current == DestinationState::Streaming ||
           current == DestinationState::Reconnecting || current == DestinationState::Stopping;
}

bool DestinationManager::is_current(const std::string &id, OperationId generation) const
{
    if (generation == 0)
        return false;
    const auto iterator = runtimes_.find(id);
    return iterator != runtimes_.end() && iterator->second->generation == generation;
}

DestinationManager::OperationId DestinationManager::operation_generation(const std::string &id) const
{
    const auto iterator = runtimes_.find(id);
    return iterator == runtimes_.end() ? 0 : iterator->second->generation;
}

bool DestinationManager::set_operation_state(const std::string &id, OperationId generation,
                                               DestinationState state, std::string error)
{
    if (!is_current(id, generation))
        return false;
    set_runtime_state(id, state, std::move(error), generation);
    return true;
}

bool DestinationManager::complete_operation(const std::string &id, OperationId generation,
                                             DestinationState state, std::string error)
{
    if (!set_operation_state(id, generation, state, std::move(error)))
        return false;
    const auto iterator = runtimes_.find(id);
    if (iterator != runtimes_.end())
        iterator->second->stop_requested = state == DestinationState::Stopping;
    return true;
}

DestinationState DestinationManager::state(const std::string &id) const
{
    const auto iterator = runtimes_.find(id);
    return iterator == runtimes_.end() ? DestinationState::Stopped : iterator->second->state;
}

std::string DestinationManager::error(const std::string &id) const
{
    const auto iterator = runtimes_.find(id);
    return iterator == runtimes_.end() ? std::string{} : iterator->second->error;
}

void DestinationManager::set_runtime_state(const std::string &id, DestinationState state, std::string error,
                                             OperationId generation)
{
    const auto iterator = runtimes_.find(id);
    if (iterator == runtimes_.end())
        return;
    if (generation != 0 && iterator->second->generation != generation)
        return;
    set_runtime_state(*iterator->second, state, std::move(error));
}

void DestinationManager::set_runtime_state(Runtime &runtime, DestinationState state, std::string error)
{
    if (!is_current_runtime(runtime))
        return;
    runtime.state = state;
    runtime.error = std::move(error);
    if (state != DestinationState::Stopping)
        runtime.stop_requested = false;
    emit_state(runtime);
}

bool DestinationManager::is_current_runtime(const Runtime &runtime) const
{
    const auto iterator = runtimes_.find(runtime.id);
    return iterator != runtimes_.end() && iterator->second.get() == &runtime;
}

void DestinationManager::emit_state(const Runtime &runtime) const
{
    if (state_callback_ && !shutting_down_)
        state_callback_(runtime.id, runtime.state, runtime.error, runtime.generation);
}

void DestinationManager::on_starting(void *data, calldata_t *)
{
    auto *runtime = static_cast<Runtime *>(data);
    if (!runtime || !runtime->owner || runtime->stop_requested || runtime->state == DestinationState::Stopping)
        return;
    runtime->owner->set_runtime_state(*runtime, DestinationState::Connecting);
}

void DestinationManager::on_started(void *data, calldata_t *)
{
    auto *runtime = static_cast<Runtime *>(data);
    if (!runtime || !runtime->owner || runtime->stop_requested || runtime->state == DestinationState::Stopping)
        return;
    runtime->owner->set_runtime_state(*runtime, DestinationState::Streaming);
}

void DestinationManager::on_reconnect(void *data, calldata_t *)
{
    auto *runtime = static_cast<Runtime *>(data);
    if (!runtime || !runtime->owner || runtime->stop_requested)
        return;
    runtime->owner->set_runtime_state(*runtime, DestinationState::Reconnecting);
}

void DestinationManager::on_reconnect_success(void *data, calldata_t *)
{
    auto *runtime = static_cast<Runtime *>(data);
    if (!runtime || !runtime->owner || runtime->stop_requested)
        return;
    runtime->owner->set_runtime_state(*runtime, DestinationState::Streaming);
}

void DestinationManager::on_stopping(void *data, calldata_t *)
{
    auto *runtime = static_cast<Runtime *>(data);
    if (!runtime || !runtime->owner)
        return;
    runtime->stop_requested = true;
    runtime->owner->set_runtime_state(*runtime, DestinationState::Stopping);
}

void DestinationManager::on_stopped(void *data, calldata_t *params)
{
    auto *runtime = static_cast<Runtime *>(data);
    if (!runtime || !runtime->owner)
        return;
    const int code = calldata_int(params, "code");
    if (code == OBS_OUTPUT_SUCCESS) {
        runtime->owner->set_runtime_state(*runtime, DestinationState::Stopped);
        runtime->owner->release_runtime(*runtime, false);
        return;
    }

    std::string message = "RTMP output stopped with code " + std::to_string(code);
    if (runtime->output) {
        const char *last_error = obs_output_get_last_error(runtime->output);
        if (last_error && *last_error)
            message = last_error;
    }
    runtime->owner->set_runtime_state(*runtime, DestinationState::Error, std::move(message));
}

} // namespace multistream
