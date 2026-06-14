#include "algolib/runtime/algorithm_request.h"

#include <string>
#include <vector>

namespace algolib {
namespace {

Status RequireStringField(const nlohmann::json& json_value,
                        const std::string& field_name) {
    if (!json_value.contains(field_name) || !json_value.at(field_name).is_string() ||
        json_value.at(field_name).get<std::string>().empty()) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            "AlgorithmRequest must contain non-empty string field " + field_name + ".");
    }
    return Status::Ok();
}

Status RequireObjectField(const nlohmann::json& json_value,
                          const std::string& field_name,
                          const std::string& context) {
    if (!json_value.contains(field_name) || !json_value.at(field_name).is_object()) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            context + " must contain object field " + field_name + ".");
    }
    return Status::Ok();
}

Result<std::vector<std::string>> ReadOptionalStringArray(
    const nlohmann::json& json_value,
    const std::string& field_name,
    const std::string& context) {
    if (!json_value.contains(field_name)) {
        return std::vector<std::string>{};
    }
    if (!json_value.at(field_name).is_array()) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            context + " field " + field_name + " must be an array of strings.");
    }
    std::vector<std::string> values;
    for (const auto& item : json_value.at(field_name)) {
        if (!item.is_string() || item.get<std::string>().empty()) {
            return Status::Error(
                ErrorCode::kInvalidArgument,
                context + " field " + field_name + " must only contain non-empty strings.");
        }
        values.push_back(item.get<std::string>());
    }
    return values;
}

Result<int> ReadOptionalNonNegativeInt(const nlohmann::json& json_value,
                                       const std::string& field_name,
                                       const std::string& context) {
    if (!json_value.contains(field_name)) {
        return 0;
    }
    if (!json_value.at(field_name).is_number_integer() ||
        json_value.at(field_name).get<int>() < 0) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            context + " field " + field_name + " must be a non-negative integer.");
    }
    return json_value.at(field_name).get<int>();
}

Result<std::string> ReadOptionalString(const nlohmann::json& json_value,
                                       const std::string& field_name,
                                       const std::string& context) {
    if (!json_value.contains(field_name)) {
        return std::string();
    }
    if (!json_value.at(field_name).is_string()) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            context + " field " + field_name + " must be a string.");
    }
    return json_value.at(field_name).get<std::string>();
}

Result<bool> ReadOptionalBool(const nlohmann::json& json_value,
                              const std::string& field_name,
                              const std::string& context) {
    if (!json_value.contains(field_name)) {
        return false;
    }
    if (!json_value.at(field_name).is_boolean()) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            context + " field " + field_name + " must be a boolean.");
    }
    return json_value.at(field_name).get<bool>();
}

}  // namespace

nlohmann::json ToJson(const AlgorithmRequest& request) {
    return nlohmann::json{
        {"request_id", request.request_id},
        {"trace_id", request.trace_id},
        {"algorithm_id", request.algorithm_id},
        {"version", request.version},
        {"backend_type", ToString(request.backend_type)},
        {"inputs", request.inputs},
        {"params", request.params},
    };
}

nlohmann::json ToJson(const AgentCondition& condition) {
    return nlohmann::json{
        {"task_family", condition.task_family},
        {"required_capabilities", condition.required_capabilities},
        {"preferred_capabilities", condition.preferred_capabilities},
        {"input_modalities", condition.input_modalities},
        {"intent_keywords", condition.intent_keywords},
        {"input_chars", condition.input_chars},
        {"request_bytes", condition.request_bytes},
        {"max_latency_ms", condition.max_latency_ms},
        {"allow_human_review", condition.allow_human_review},
        {"max_risk_level", condition.max_risk_level},
        {"hardware_profile",
         {
             {"has_gpu", condition.hardware_profile.has_gpu},
             {"available_gpu_memory_mb", condition.hardware_profile.available_gpu_memory_mb},
             {"available_system_memory_mb", condition.hardware_profile.available_system_memory_mb},
             {"available_cpu_cores", condition.hardware_profile.available_cpu_cores},
             {"preferred_device", condition.hardware_profile.preferred_device},
         }},
    };
}

nlohmann::json ToJson(const AgentRoutingRequest& request) {
    return nlohmann::json{
        {"request_id", request.request_id},
        {"trace_id", request.trace_id},
        {"condition", ToJson(request.condition)},
        {"inputs", request.inputs},
        {"params", request.params},
    };
}


Result<AlgorithmRequest> AlgorithmRequestFromJson(const nlohmann::json& json_value) {
    if (!json_value.is_object()) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            "AlgorithmRequest JSON must be an object.");
    }

    for (const std::string& field_name : {"algorithm_id", "version", "backend_type"}) {
        auto field_status = RequireStringField(json_value, field_name);
        if (!field_status.ok()) {
            return field_status;
        }
    }

    if (!json_value.contains("inputs")) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            "AlgorithmRequest must contain inputs.");
    }

    auto backend_result = ParseBackendType(json_value.at("backend_type").get<std::string>());
    if (!backend_result.ok()) {
        return backend_result.status();
    }

    if (json_value.contains("request_id") && !json_value.at("request_id").is_string()) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            "AlgorithmRequest request_id must be a string when provided.");
    }
    if (json_value.contains("trace_id") && !json_value.at("trace_id").is_string()) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            "AlgorithmRequest trace_id must be a string when provided.");
    }
    if (json_value.contains("params") && !json_value.at("params").is_object()) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            "AlgorithmRequest params must be a JSON object when provided.");
    }

    AlgorithmRequest request;
    request.request_id = json_value.value("request_id", std::string());
    request.trace_id = json_value.value("trace_id", std::string());
    request.algorithm_id = json_value.at("algorithm_id").get<std::string>();
    request.version = json_value.at("version").get<std::string>();
    request.backend_type = backend_result.value();
    request.inputs = json_value.at("inputs");
    request.params = json_value.value("params", nlohmann::json::object());
    return request;
}

Result<AgentRoutingRequest> AgentRoutingRequestFromJson(const nlohmann::json& json_value) {
    if (!json_value.is_object()) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            "AgentRoutingRequest JSON must be an object.");
    }

    auto condition_status = RequireObjectField(json_value, "condition", "AgentRoutingRequest");
    if (!condition_status.ok()) {
        return condition_status;
    }
    if (!json_value.contains("inputs")) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            "AgentRoutingRequest must contain inputs.");
    }
    if (json_value.contains("request_id") && !json_value.at("request_id").is_string()) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            "AgentRoutingRequest request_id must be a string when provided.");
    }
    if (json_value.contains("trace_id") && !json_value.at("trace_id").is_string()) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            "AgentRoutingRequest trace_id must be a string when provided.");
    }
    if (json_value.contains("params") && !json_value.at("params").is_object()) {
        return Status::Error(
            ErrorCode::kInvalidArgument,
            "AgentRoutingRequest params must be a JSON object when provided.");
    }

    const auto& condition_json = json_value.at("condition");

    auto task_family_result =
        ReadOptionalString(condition_json, "task_family", "AgentCondition");
    if (!task_family_result.ok()) {
        return task_family_result.status();
    }
    auto required_capabilities_result =
        ReadOptionalStringArray(condition_json, "required_capabilities", "AgentCondition");
    if (!required_capabilities_result.ok()) {
        return required_capabilities_result.status();
    }
    auto preferred_capabilities_result =
        ReadOptionalStringArray(condition_json, "preferred_capabilities", "AgentCondition");
    if (!preferred_capabilities_result.ok()) {
        return preferred_capabilities_result.status();
    }
    auto input_modalities_result =
        ReadOptionalStringArray(condition_json, "input_modalities", "AgentCondition");
    if (!input_modalities_result.ok()) {
        return input_modalities_result.status();
    }
    auto intent_keywords_result =
        ReadOptionalStringArray(condition_json, "intent_keywords", "AgentCondition");
    if (!intent_keywords_result.ok()) {
        return intent_keywords_result.status();
    }
    auto input_chars_result =
        ReadOptionalNonNegativeInt(condition_json, "input_chars", "AgentCondition");
    if (!input_chars_result.ok()) {
        return input_chars_result.status();
    }
    auto request_bytes_result =
        ReadOptionalNonNegativeInt(condition_json, "request_bytes", "AgentCondition");
    if (!request_bytes_result.ok()) {
        return request_bytes_result.status();
    }
    auto max_latency_result =
        ReadOptionalNonNegativeInt(condition_json, "max_latency_ms", "AgentCondition");
    if (!max_latency_result.ok()) {
        return max_latency_result.status();
    }
    auto allow_human_review_result =
        ReadOptionalBool(condition_json, "allow_human_review", "AgentCondition");
    if (!allow_human_review_result.ok()) {
        return allow_human_review_result.status();
    }
    auto max_risk_level_result =
        ReadOptionalString(condition_json, "max_risk_level", "AgentCondition");
    if (!max_risk_level_result.ok()) {
        return max_risk_level_result.status();
    }

    AgentHardwareProfile hardware_profile;
    if (condition_json.contains("hardware_profile")) {
        auto hardware_status =
            RequireObjectField(condition_json, "hardware_profile", "AgentCondition");
        if (!hardware_status.ok()) {
            return hardware_status;
        }
        const auto& hardware_json = condition_json.at("hardware_profile");

        auto has_gpu_result =
            ReadOptionalBool(hardware_json, "has_gpu", "AgentHardwareProfile");
        if (!has_gpu_result.ok()) {
            return has_gpu_result.status();
        }
        auto gpu_memory_result = ReadOptionalNonNegativeInt(
            hardware_json, "available_gpu_memory_mb", "AgentHardwareProfile");
        if (!gpu_memory_result.ok()) {
            return gpu_memory_result.status();
        }
        auto system_memory_result = ReadOptionalNonNegativeInt(
            hardware_json, "available_system_memory_mb", "AgentHardwareProfile");
        if (!system_memory_result.ok()) {
            return system_memory_result.status();
        }
        auto cpu_cores_result = ReadOptionalNonNegativeInt(
            hardware_json, "available_cpu_cores", "AgentHardwareProfile");
        if (!cpu_cores_result.ok()) {
            return cpu_cores_result.status();
        }
        auto preferred_device_result =
            ReadOptionalString(hardware_json, "preferred_device", "AgentHardwareProfile");
        if (!preferred_device_result.ok()) {
            return preferred_device_result.status();
        }

        hardware_profile.has_gpu = has_gpu_result.value();
        hardware_profile.available_gpu_memory_mb = gpu_memory_result.value();
        hardware_profile.available_system_memory_mb = system_memory_result.value();
        hardware_profile.available_cpu_cores = cpu_cores_result.value();
        hardware_profile.preferred_device = preferred_device_result.value();
    }

    AgentRoutingRequest request;
    request.request_id = json_value.value("request_id", std::string());
    request.trace_id = json_value.value("trace_id", std::string());
    request.inputs = json_value.at("inputs");
    request.params = json_value.value("params", nlohmann::json::object());
    request.condition.task_family = task_family_result.value();
    request.condition.required_capabilities = required_capabilities_result.value();
    request.condition.preferred_capabilities = preferred_capabilities_result.value();
    request.condition.input_modalities = input_modalities_result.value();
    request.condition.intent_keywords = intent_keywords_result.value();
    request.condition.input_chars = input_chars_result.value();
    request.condition.request_bytes = request_bytes_result.value();
    request.condition.max_latency_ms = max_latency_result.value();
    request.condition.allow_human_review = allow_human_review_result.value();
    request.condition.max_risk_level = max_risk_level_result.value();
    request.condition.hardware_profile = std::move(hardware_profile);
    return request;
}

}  // namespace algolib
