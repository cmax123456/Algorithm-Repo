#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "algolib/core/algorithm_key.h"
#include "algolib/core/status.h"

namespace algolib {

enum class RuntimeState {
    kUnloaded = 0,
    kLoading,
    kLoaded,
    kFailed,
};

std::string ToString(RuntimeState state);
Result<RuntimeState> ParseRuntimeState(std::string_view raw_value);

struct RuntimeStateInfo {
    RuntimeState state = RuntimeState::kUnloaded;
    std::string last_error;
    std::chrono::system_clock::time_point updated_at = std::chrono::system_clock::now();
    std::string node_id;
};

nlohmann::json ToJson(const RuntimeStateInfo& info);

class AlgorithmRuntimeManager {
public:
    AlgorithmRuntimeManager() = default;
    RuntimeStateInfo GetState(const AlgorithmKey& key) const;
    bool TryBeginLoading(const AlgorithmKey& key);
    void MarkLoaded(const AlgorithmKey& key, std::string node_id = std::string());
    void MarkFailed(const AlgorithmKey& key, std::string error_message);
    void MarkUnloaded(const AlgorithmKey& key);
    std::vector<std::pair<AlgorithmKey, RuntimeStateInfo>> ListStates() const;
private:
    mutable std::mutex mutex_;
    std::map<AlgorithmKey, RuntimeStateInfo> states_;
};

}  // namespace algolib
