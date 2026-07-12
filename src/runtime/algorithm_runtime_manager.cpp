#include "algolib/runtime/algorithm_runtime_manager.h"

#include "algolib/core/error_code.h"

namespace algolib {

std::string ToString(RuntimeState state) {
    switch (state) {
        case RuntimeState::kUnloaded:
            return "unloaded";
        case RuntimeState::kLoading:
            return "loading";
        case RuntimeState::kLoaded:
            return "loaded";
        case RuntimeState::kFailed:
            return "failed";
    }
    return "unloaded";
}

Result<RuntimeState> ParseRuntimeState(std::string_view raw_value) {
    if (raw_value == "unloaded") return RuntimeState::kUnloaded;
    if (raw_value == "loading") return RuntimeState::kLoading;
    if (raw_value == "loaded") return RuntimeState::kLoaded;
    if (raw_value == "failed") return RuntimeState::kFailed;
    return Status::Error(ErrorCode::kInvalidArgument,
                         std::string("Unknown runtime_state: ") + std::string(raw_value));
}

nlohmann::json ToJson(const RuntimeStateInfo& info) {
    const auto since_epoch = info.updated_at.time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
    return nlohmann::json{
        {"state", ToString(info.state)},
        {"last_error", info.last_error},
        {"updated_at_epoch_ms", millis},
        {"node_id", info.node_id},
    };
}

RuntimeStateInfo AlgorithmRuntimeManager::GetState(const AlgorithmKey& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(key);
    if (it == states_.end()) {
        return RuntimeStateInfo{};
    }
    return it->second;
}

bool AlgorithmRuntimeManager::TryBeginLoading(const AlgorithmKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(key);
    if (it != states_.end() && (it->second.state == RuntimeState::kLoading ||
                                 it->second.state == RuntimeState::kLoaded)) {
        return false;
    }
    RuntimeStateInfo info;
    info.state = RuntimeState::kLoading;
    info.updated_at = std::chrono::system_clock::now();
    states_[key] = info;
    return true;
}

void AlgorithmRuntimeManager::MarkLoaded(const AlgorithmKey& key, std::string node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeStateInfo& info = states_[key];
    info.state = RuntimeState::kLoaded;
    info.last_error.clear();
    info.node_id = std::move(node_id);
    info.updated_at = std::chrono::system_clock::now();
}

void AlgorithmRuntimeManager::MarkFailed(const AlgorithmKey& key, std::string error_message) {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeStateInfo& info = states_[key];
    info.state = RuntimeState::kFailed;
    info.last_error = std::move(error_message);
    info.updated_at = std::chrono::system_clock::now();
}

void AlgorithmRuntimeManager::MarkUnloaded(const AlgorithmKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeStateInfo& info = states_[key];
    info.state = RuntimeState::kUnloaded;
    info.last_error.clear();
    info.node_id.clear();
    info.updated_at = std::chrono::system_clock::now();
}

std::vector<std::pair<AlgorithmKey, RuntimeStateInfo>> AlgorithmRuntimeManager::ListStates() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<AlgorithmKey, RuntimeStateInfo>> result;
    result.reserve(states_.size());
    for (const auto& [key, info] : states_) {
        result.emplace_back(key, info);
    }
    return result;
}

}  // namespace algolib
