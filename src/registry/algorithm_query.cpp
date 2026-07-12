#include "algolib/registry/algorithm_query.h"

namespace algolib {

nlohmann::json ToQueryResultJson(const std::pair<AlgorithmEntry, RuntimeStateInfo>& item) {
    const AlgorithmEntry& entry = item.first;
    const RuntimeStateInfo& runtime_state = item.second;
    nlohmann::json result = ToModelInfoJson(entry);
    result["runtime_state"] = ToJson(runtime_state);
    return result;
}

}  // namespace algolib
