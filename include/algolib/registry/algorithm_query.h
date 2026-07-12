#pragma once

#include <optional>
#include <string>
#include <vector>

#include "algolib/core/algorithm_entry.h"
#include "algolib/core/algorithm_status.h"
#include "algolib/runtime/algorithm_runtime_manager.h"

namespace algolib {

// AlgorithmQuery describes filter conditions for the model query API: business fields
// (task_family/capability/status), resource-profile constraints (max memory/bandwidth,
// preferred_compute) and runtime states (from AlgorithmRuntimeManager). All optional;
// leaving a field empty means that dimension is not filtered.
struct AlgorithmQuery {
    std::optional<std::string> algorithm_id;
    std::optional<std::string> task_family;
    std::optional<std::string> capability;
    std::optional<AlgorithmStatus> status;
    std::optional<int> max_memory_footprint_mb;
    std::optional<int> max_bandwidth_requirement_mbps;
    std::optional<std::string> preferred_compute;
    std::vector<RuntimeState> runtime_states;
};

// Serializes one query result (algorithm entry + runtime state snapshot) to JSON,
// shared by the CLI and HTTP query endpoints.
nlohmann::json ToQueryResultJson(const std::pair<AlgorithmEntry, RuntimeStateInfo>& item);

}  // namespace algolib
