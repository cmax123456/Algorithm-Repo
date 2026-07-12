#include "algolib/registry/algorithm_registry.h"

#include <algorithm>
#include <utility>

namespace algolib {
namespace {

Status EnsureActivatable(AlgorithmStatus status) {
    if (status == AlgorithmStatus::kValidated || status == AlgorithmStatus::kDisabled ||
        status == AlgorithmStatus::kActive) {
        return Status::Ok();
    }
    return Status::Error(
        ErrorCode::kStatusTransitionInvalid,
        "Only validated or disabled algorithms can be activated.");
}

Status EnsureDisableable(AlgorithmStatus status) {
    if (status == AlgorithmStatus::kValidated || status == AlgorithmStatus::kActive ||
        status == AlgorithmStatus::kDisabled) {
        return Status::Ok();
    }
    return Status::Error(
        ErrorCode::kStatusTransitionInvalid,
        "Only validated or active algorithms can be disabled.");
}

Status EnsureValidatable(AlgorithmStatus status) {
    if (status == AlgorithmStatus::kDeleted) {
        return Status::Error(
            ErrorCode::kStatusTransitionInvalid,
            "Deleted algorithms cannot be re-validated.");
    }
    return Status::Ok();
}

Status EnsureDeletable(AlgorithmStatus status) {
    if (status == AlgorithmStatus::kDeleted) {
        return Status::Ok();
    }
    return Status::Ok();
}

}  // namespace

AlgorithmRegistry::AlgorithmRegistry(std::filesystem::path registry_path)
    : store_(std::move(registry_path)) {}

Status AlgorithmRegistry::Reload() {
    return store_.Load(&entries_);
}

Result<AlgorithmEntry> AlgorithmRegistry::Register(
    const std::filesystem::path& package_or_card_path) {
    auto validated_result = validator_.ValidateFromPath(package_or_card_path);
    if (!validated_result.ok()) {
        return validated_result.status();
    }

    AlgorithmEntry new_entry =
        BuildEntry(validated_result.value(), AlgorithmStatus::kValidated);
    if (entries_.find(new_entry.key) != entries_.end()) {
        return Status::Error(
            ErrorCode::kRegistryConflict,
            "An algorithm with the same algorithm_id, version and backend_type already exists: " +
                new_entry.key.ToUniqueString());
    }

    entries_.insert_or_assign(new_entry.key, new_entry);
    auto persist_status = Persist();
    if (!persist_status.ok()) {
        return persist_status;
    }

    return new_entry;
}

Result<AlgorithmEntry> AlgorithmRegistry::Validate(const AlgorithmKey& key) {
    auto find_result = FindMutable(key);
    if (!find_result.ok()) {
        return find_result.status();
    }

    AlgorithmEntry* entry = find_result.value();
    auto validate_status = EnsureValidatable(entry->status);
    if (!validate_status.ok()) {
        return validate_status;
    }

    auto validated_result = validator_.ValidateFromPath(entry->card_path);
    if (!validated_result.ok()) {
        return validated_result.status();
    }

    const AlgorithmStatus effective_status =
        entry->status == AlgorithmStatus::kDraft ? AlgorithmStatus::kValidated : entry->status;
    *entry = BuildEntry(validated_result.value(), effective_status);

    auto persist_status = Persist();
    if (!persist_status.ok()) {
        return persist_status;
    }

    return *entry;
}

Result<AlgorithmEntry> AlgorithmRegistry::Activate(const AlgorithmKey& key) {
    auto find_result = FindMutable(key);
    if (!find_result.ok()) {
        return find_result.status();
    }

    AlgorithmEntry* entry = find_result.value();
    auto activatable_status = EnsureActivatable(entry->status);
    if (!activatable_status.ok()) {
        return activatable_status;
    }

    entry->status = AlgorithmStatus::kActive;
    auto persist_status = Persist();
    if (!persist_status.ok()) {
        return persist_status;
    }

    return *entry;
}

Result<AlgorithmEntry> AlgorithmRegistry::Disable(const AlgorithmKey& key) {
    auto find_result = FindMutable(key);
    if (!find_result.ok()) {
        return find_result.status();
    }

    AlgorithmEntry* entry = find_result.value();
    auto disableable_status = EnsureDisableable(entry->status);
    if (!disableable_status.ok()) {
        return disableable_status;
    }

    entry->status = AlgorithmStatus::kDisabled;
    auto persist_status = Persist();
    if (!persist_status.ok()) {
        return persist_status;
    }

    return *entry;
}

Result<AlgorithmEntry> AlgorithmRegistry::Delete(const AlgorithmKey& key) {
    auto find_result = FindMutable(key);
    if (!find_result.ok()) {
        return find_result.status();
    }

    AlgorithmEntry* entry = find_result.value();
    auto deletable_status = EnsureDeletable(entry->status);
    if (!deletable_status.ok()) {
        return deletable_status;
    }

    entry->status = AlgorithmStatus::kDeleted;
    auto persist_status = Persist();
    if (!persist_status.ok()) {
        return persist_status;
    }

    return *entry;
}

Result<AlgorithmEntry> AlgorithmRegistry::Get(const AlgorithmKey& key) const {
    auto find_result = Find(key);
    if (!find_result.ok()) {
        return find_result.status();
    }
    return *find_result.value();
}

Status AlgorithmRegistry::ValidateInputPayload(const AlgorithmKey& key,
                                               const nlohmann::json& input_json) const {
    auto find_result = Find(key);
    if (!find_result.ok()) {
        return find_result.status();
    }
    return schema_validator_.ValidateInputForEntry(*find_result.value(), input_json);
}

Status AlgorithmRegistry::ValidateOutputPayload(const AlgorithmKey& key,
                                                const nlohmann::json& output_json) const {
    auto find_result = Find(key);
    if (!find_result.ok()) {
        return find_result.status();
    }
    return schema_validator_.ValidateOutputForEntry(*find_result.value(), output_json);
}

std::vector<AlgorithmEntry> AlgorithmRegistry::List(bool include_deleted) const {
    std::vector<AlgorithmEntry> result;
    for (const auto& [key, entry] : entries_) {
        (void)key;
        if (!include_deleted && entry.status == AlgorithmStatus::kDeleted) {
            continue;
        }
        result.push_back(entry);
    }
    return result;
}

std::vector<nlohmann::json> AlgorithmRegistry::ListAgentViews(bool active_only) const {
    std::vector<nlohmann::json> result;
    for (const auto& [key, entry] : entries_) {
        (void)key;
        if (entry.status == AlgorithmStatus::kDeleted) {
            continue;
        }
        if (active_only && entry.status != AlgorithmStatus::kActive) {
            continue;
        }
        result.push_back(ToAgentViewJson(entry));
    }
    return result;
}

std::vector<std::pair<AlgorithmEntry, RuntimeStateInfo>> AlgorithmRegistry::Query(
    const AlgorithmQuery& query) const {
    std::vector<std::pair<AlgorithmEntry, RuntimeStateInfo>> result;
    for (const auto& [key, entry] : entries_) {
        if (entry.status == AlgorithmStatus::kDeleted) {
            continue;
        }
        if (query.algorithm_id.has_value() && entry.key.algorithm_id != *query.algorithm_id) {
            continue;
        }
        if (query.status.has_value() && entry.status != *query.status) {
            continue;
        }
        if (query.task_family.has_value() && entry.card.task_family != *query.task_family) {
            continue;
        }
        if (query.capability.has_value()) {
            const auto& capabilities = entry.card.capabilities;
            bool matched = std::find(capabilities.begin(), capabilities.end(),
                                     *query.capability) != capabilities.end();
            if (!matched) {
                continue;
            }
        }
        if (query.preferred_compute.has_value()) {
            if (!entry.card.resource_requirements.has_value()) {
                continue;
            }
            const auto& resource_requirements = *entry.card.resource_requirements;
            const bool wants_gpu = *query.preferred_compute != "cpu";
            const bool has_gpu = resource_requirements.min_gpu_count.has_value() &&
                                 *resource_requirements.min_gpu_count > 0;
            if (wants_gpu != has_gpu) {
                continue;
            }
        }
        if (query.max_memory_footprint_mb.has_value()) {
            if (!entry.card.resource_requirements.has_value()) {
                continue;
            }
            const auto& resource_requirements = *entry.card.resource_requirements;
            const std::optional<int> memory_mb = resource_requirements.recommended_memory_mb.has_value()
                                                      ? resource_requirements.recommended_memory_mb
                                                      : resource_requirements.min_memory_mb;
            if (!memory_mb.has_value() || *memory_mb > *query.max_memory_footprint_mb) {
                continue;
            }
        }
        if (query.max_bandwidth_requirement_mbps.has_value()) {
            if (!entry.card.resource_requirements.has_value() ||
                !entry.card.resource_requirements->bandwidth_requirement_mbps.has_value() ||
                *entry.card.resource_requirements->bandwidth_requirement_mbps >
                    *query.max_bandwidth_requirement_mbps) {
                continue;
            }
        }
        const RuntimeStateInfo runtime_state = runtime_manager_.GetState(key);
        if (!query.runtime_states.empty()) {
            bool matched = std::find(query.runtime_states.begin(), query.runtime_states.end(),
                                     runtime_state.state) != query.runtime_states.end();
            if (!matched) {
                continue;
            }
        }
        result.emplace_back(entry, runtime_state);
    }
    return result;
}

AlgorithmRuntimeManager& AlgorithmRegistry::runtime_manager() {
    return runtime_manager_;
}

const AlgorithmRuntimeManager& AlgorithmRegistry::runtime_manager() const {
    return runtime_manager_;
}

const std::filesystem::path& AlgorithmRegistry::registry_path() const {
    return store_.registry_path();
}

Status AlgorithmRegistry::Persist() const {
    return store_.Save(entries_);
}

Result<AlgorithmEntry*> AlgorithmRegistry::FindMutable(const AlgorithmKey& key) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return Status::Error(ErrorCode::kAlgorithmNotFound,
                             "Algorithm not found: " + key.ToUniqueString());
    }
    return &it->second;
}

Result<const AlgorithmEntry*> AlgorithmRegistry::Find(const AlgorithmKey& key) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return Status::Error(ErrorCode::kAlgorithmNotFound,
                             "Algorithm not found: " + key.ToUniqueString());
    }
    return &it->second;
}

AlgorithmEntry AlgorithmRegistry::BuildEntry(
    const ValidatedAlgorithmPackage& validated_package,
    AlgorithmStatus effective_status) const {
    AlgorithmEntry entry;
    entry.key.algorithm_id = validated_package.card.algorithm_id;
    entry.key.version = validated_package.card.version;
    entry.key.backend_type = validated_package.card.backend_type;
    entry.status = effective_status;
    entry.package_root = validated_package.package_root;
    entry.card_path = validated_package.card_path;
    entry.card = validated_package.card;
    entry.input_schema_summary = validated_package.input_schema_summary;
    entry.output_schema_summary = validated_package.output_schema_summary;
    return entry;
}

}  // namespace algolib
