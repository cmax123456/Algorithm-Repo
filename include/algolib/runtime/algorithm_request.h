#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "algolib/core/backend_type.h"
#include "algolib/core/status.h"

namespace algolib {

// 中文注释：AlgorithmRequest 对齐 SPEC 的统一执行请求结构，CLI run 和后续 HTTP 接口共用它。
struct AlgorithmRequest {
    std::string request_id;
    std::string trace_id;
    std::string algorithm_id;
    std::string version;
    BackendType backend_type = BackendType::kOnnx;
    nlohmann::json inputs = nlohmann::json::object();
    nlohmann::json params = nlohmann::json::object();
};

// 中文注释：AgentCondition 把上层 Agent 的路由条件结构化，供框架在 active 服务里做确定性筛选。
struct AgentHardwareProfile {
    bool has_gpu = false;
    int available_gpu_memory_mb = 0;
    int available_system_memory_mb = 0;
    int available_cpu_cores = 0;
    std::string preferred_device;
};

struct AgentCondition {
    std::string task_family;
    std::vector<std::string> required_capabilities;
    std::vector<std::string> preferred_capabilities;
    std::vector<std::string> input_modalities;
    std::vector<std::string> intent_keywords;
    int input_chars = 0;
    int request_bytes = 0;
    int max_latency_ms = 0;
    bool allow_human_review = false;
    std::string max_risk_level;
    AgentHardwareProfile hardware_profile;
};

// 中文注释：AgentRoutingRequest 只描述“要找什么算法”和“要传什么 inputs”，不直接指定算法 key。
struct AgentRoutingRequest {
    std::string request_id;
    std::string trace_id;
    AgentCondition condition;
    nlohmann::json inputs = nlohmann::json::object();
    nlohmann::json params = nlohmann::json::object();
};

nlohmann::json ToJson(const AlgorithmRequest& request);
nlohmann::json ToJson(const AgentCondition& condition);
nlohmann::json ToJson(const AgentRoutingRequest& request);
Result<AlgorithmRequest> AlgorithmRequestFromJson(const nlohmann::json& json_value);
Result<AgentRoutingRequest> AgentRoutingRequestFromJson(const nlohmann::json& json_value);

}  // namespace algolib