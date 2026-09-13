// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "destination.hpp"
#include "destination-provider.hpp"

#include <obs.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace multistream {

class DestinationManager {
public:
    using OperationId = std::uint64_t;
    using StateCallback =
        std::function<void(const std::string &, DestinationState, const std::string &, OperationId)>;
    using ProfileDirectoryCallback = std::function<std::string()>;
    using LoadCallback = std::function<obs_data_t *(const std::string &)>;
    using SaveCallback = std::function<bool(obs_data_t *, const std::string &)>;
    using RemoveCallback = std::function<bool(const std::string &)>;

    enum class LoadStatus {
        Loaded,
        NoProfile,
        NoFile,
        InvalidFile,
        UnsupportedVersion,
    };

    struct PersistenceHooks {
        ProfileDirectoryCallback profile_directory;
        LoadCallback load;
        SaveCallback save;
        RemoveCallback remove;
    };

    struct RecoveryNotice {
        std::string destination_id;
        std::string destination_name;
        std::int64_t timestamp = 0;
    };

    DestinationManager();
    explicit DestinationManager(PersistenceHooks persistence);
    ~DestinationManager();

    DestinationManager(const DestinationManager &) = delete;
    DestinationManager &operator=(const DestinationManager &) = delete;

    void set_state_callback(StateCallback callback);

    LoadStatus load();
    bool save() const;

    const std::vector<DestinationConfig> &destinations() const { return destinations_; }
    const DestinationConfig *find(const std::string &id) const;

    bool add(DestinationConfig config);
    bool update(const DestinationConfig &config);
    bool remove(const std::string &id);
    bool set_enabled(const std::string &id, bool enabled);
    bool reorder(const std::vector<std::string> &ordered_ids);

    bool start(const std::string &id);
    bool start_with_config(const std::string &id, const DestinationConfig &runtime_config,
                           OperationId generation = 0);
    OperationId prepare_destination(const DestinationConfig &runtime_config);
    bool set_prepared_config(const std::string &id, const DestinationConfig &runtime_config,
                             OperationId generation);
    bool start_native_secondaries(obs_output_t *native_output, const std::string &primary_id);
    void stop_native_secondaries(const std::string &primary_id);
    bool set_native_primary_state(const std::string &id, OperationId generation, DestinationState state,
                                  std::string error = {});
    void clear_native_session();
    OperationId begin_start(const std::string &id);
    OperationId begin_stop(const std::string &id);
    void set_configuring(const std::string &id);
    void set_error(const std::string &id, std::string error);
    void start_all();
    OperationId stop(const std::string &id);
    void stop_all();
    void shutdown();

    bool is_active(const std::string &id) const;
    bool is_busy(const std::string &id) const;
    bool is_current(const std::string &id, OperationId generation) const;
    OperationId operation_generation(const std::string &id) const;
    bool set_operation_state(const std::string &id, OperationId generation, DestinationState state,
                             std::string error = {});
    bool complete_operation(const std::string &id, OperationId generation, DestinationState state,
                            std::string error = {});
    DestinationState state(const std::string &id) const;
    std::string error(const std::string &id) const;

    const std::vector<RecoveryNotice> &recovery_notices() const { return recovery_notices_; }
    bool has_recovery_notice() const { return !recovery_notices_.empty(); }
    bool mark_recovery_pending(const DestinationConfig &config);
    bool clear_recovery_pending(const std::string &destination_id);

private:
    struct Runtime;

    bool create_runtime(const DestinationConfig &config, OperationId generation,
                         obs_output_t *encoder_source = nullptr);
    bool start_prepared(const std::string &id, obs_output_t *native_output);
    bool release_runtime(Runtime &runtime, bool force);
    bool create_fallback_encoders(std::string &error);
    void release_fallback_encoders();
    void set_runtime_state(const std::string &id, DestinationState state, std::string error = {},
                           OperationId generation = 0);
    void set_runtime_state(Runtime &runtime, DestinationState state, std::string error = {});
    void emit_state(const Runtime &runtime) const;
    bool native_streaming_active() const;
    OperationId allocate_generation();
    bool is_current_runtime(const Runtime &runtime) const;
    std::string profile_file() const;
    std::string recovery_file() const;
    void load_recovery_notices();
    bool save_recovery_notices() const;

    static void on_starting(void *data, calldata_t *params);
    static void on_started(void *data, calldata_t *params);
    static void on_reconnect(void *data, calldata_t *params);
    static void on_reconnect_success(void *data, calldata_t *params);
    static void on_stopping(void *data, calldata_t *params);
    static void on_stopped(void *data, calldata_t *params);

    std::vector<DestinationConfig> destinations_;
    std::unordered_map<std::string, std::unique_ptr<Runtime>> runtimes_;
    std::unordered_map<std::string, DestinationConfig> prepared_configs_;
    std::vector<RecoveryNotice> recovery_notices_;
    GenericRtmpProvider provider_;
    PersistenceHooks persistence_;
    StateCallback state_callback_;
    bool shutting_down_ = false;
    OperationGeneration generation_tracker_;
    obs_encoder_t *fallback_video_encoder_ = nullptr;
    obs_encoder_t *fallback_audio_encoder_ = nullptr;
};

} // namespace multistream
