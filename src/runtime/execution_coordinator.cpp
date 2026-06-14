#include "algolib/runtime/execution_coordinator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace algolib {
namespace {

using nlohmann::json;

struct CandidateEvaluation {
    const AlgorithmEntry* entry = nullptr;
    double score = 0.0;
    std::vector<std::string> reasons;
};

std::filesystem::path ResolveExecutionLogPath(const AlgorithmRegistry& registry,
                                              const std::filesystem::path& requested_path) {
    if (!requested_path.empty()) {
        return requested_path;
    }
    if (const char* env_value = std::getenv("ALGOLIB_EXECUTION_LOG_PATH");
        env_value != nullptr && *env_value != '\0') {
        return std::filesystem::path(env_value);
    }

    std::filesystem::path parent = registry.registry_path().parent_path();
    if (parent.empty()) {
        parent = std::filesystem::current_path();
    }
    return parent / "execution_audit.jsonl";
}

std::string GenerateId(const std::string& prefix) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    return prefix + "_" + std::to_string(now_ms) + "_" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

std::string ToLowerAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

void EnsureRequestEnvelope(AlgorithmRequest* request) {
    if (request->request_id.empty()) {
        request->request_id = GenerateId("req");
    }
    if (request->trace_id.empty()) {
        request->trace_id = GenerateId("trace");
    }
}

AlgorithmResult BuildFailureResult(const AlgorithmRequest& request,
                                   const std::string& code,
                                   const std::string& message,
                                   json usage = json::object()) {
    AlgorithmResult result;
    result.ok = false;
    result.request_id = request.request_id;
    result.trace_id = request.trace_id;
    result.algorithm_id = request.algorithm_id;
    result.version = request.version;
    result.backend_type = request.backend_type;
    result.outputs = json::object();
    result.usage = usage.is_object() ? std::move(usage) : json::object();
    result.error = AlgorithmError{code, message};
    return result;
}

AlgorithmResult BuildFailureResult(const AlgorithmRequest& request,
                                   const Status& status,
                                   json usage = json::object()) {
    return BuildFailureResult(request, ToString(status.code()), status.message(), std::move(usage));
}

void NormalizeResultEnvelope(const AlgorithmRequest& request, AlgorithmResult* result) {
    result->request_id = result->request_id.empty() ? request.request_id : result->request_id;
    result->trace_id = result->trace_id.empty() ? request.trace_id : result->trace_id;
    result->algorithm_id =
        result->algorithm_id.empty() ? request.algorithm_id : result->algorithm_id;
    result->version = result->version.empty() ? request.version : result->version;
    result->backend_type = request.backend_type;
    if (!result->usage.is_object()) {
        result->usage = json::object();
    }
}

void EnsureLatency(AlgorithmResult* result, std::int64_t latency_ms) {
    if (!result->usage.is_object()) {
        result->usage = json::object();
    }
    if (!result->usage.contains("latency_ms")) {
        result->usage["latency_ms"] = latency_ms;
    }
}

bool ContainsValueIgnoreCase(const std::vector<std::string>& values, const std::string& needle) {
    const std::string lowered_needle = ToLowerAscii(needle);
    for (const auto& value : values) {
        if (ToLowerAscii(value) == lowered_needle) {
            return true;
        }
    }
    return false;
}

bool ContainsKeywordIgnoreCase(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return false;
    }
    return ToLowerAscii(haystack).find(ToLowerAscii(needle)) != std::string::npos;
}

std::string JoinStrings(const std::vector<std::string>& values) {
    std::string joined;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            joined += ", ";
        }
        joined += values[index];
    }
    return joined;
}

std::vector<std::string> FindMissingValues(const std::vector<std::string>& provided,
                                           const std::vector<std::string>& required) {
    std::vector<std::string> missing;
    for (const auto& required_value : required) {
        if (!ContainsValueIgnoreCase(provided, required_value)) {
            missing.push_back(required_value);
        }
    }
    return missing;
}

std::optional<int> EffectiveLatencyMs(const AlgorithmEntry& entry) {
    if (!entry.card.performance.has_value()) {
        return std::nullopt;
    }
    if (entry.card.performance->latency_ms_p95.has_value()) {
        return entry.card.performance->latency_ms_p95.value();
    }
    if (entry.card.performance->latency_ms_p50.has_value()) {
        return entry.card.performance->latency_ms_p50.value();
    }
    return std::nullopt;
}

bool RequiresGpu(const AlgorithmEntry& entry) {
    return entry.card.hardware_requirements.has_value() &&
           entry.card.hardware_requirements->requires_gpu.value_or(false);
}

std::optional<int> MinGpuMemoryMb(const AlgorithmEntry& entry) {
    if (!entry.card.hardware_requirements.has_value()) {
        return std::nullopt;
    }
    return entry.card.hardware_requirements->min_gpu_memory_mb;
}

std::optional<int> MinSystemMemoryMb(const AlgorithmEntry& entry) {
    if (!entry.card.hardware_requirements.has_value()) {
        return std::nullopt;
    }
    return entry.card.hardware_requirements->min_system_memory_mb;
}

std::optional<int> MinCpuCores(const AlgorithmEntry& entry) {
    if (!entry.card.hardware_requirements.has_value()) {
        return std::nullopt;
    }
    return entry.card.hardware_requirements->min_cpu_cores;
}

std::string PreferredDevice(const AlgorithmEntry& entry) {
    if (!entry.card.hardware_requirements.has_value()) {
        return "";
    }
    return ToLowerAscii(entry.card.hardware_requirements->preferred_device);
}

bool RequiresHumanReview(const AlgorithmEntry& entry) {
    return entry.card.safety.has_value() &&
           entry.card.safety->requires_human_review.value_or(false);
}

std::string RiskLevel(const AlgorithmEntry& entry) {
    if (!entry.card.safety.has_value()) {
        return "";
    }
    return ToLowerAscii(entry.card.safety->risk_level);
}

int RiskRank(const std::string& risk_level) {
    const std::string lowered = ToLowerAscii(risk_level);
    if (lowered == "low") {
        return 0;
    }
    if (lowered == "medium") {
        return 1;
    }
    if (lowered == "high") {
        return 2;
    }
    if (lowered == "critical") {
        return 3;
    }
    return 4;
}

int CountKeywordHits(const std::vector<std::string>& texts,
                     const std::vector<std::string>& keywords) {
    int hits = 0;
    for (const auto& keyword : keywords) {
        if (keyword.empty()) {
            continue;
        }
        bool matched = false;
        for (const auto& text : texts) {
            if (ContainsKeywordIgnoreCase(text, keyword)) {
                matched = true;
                break;
            }
        }
        if (matched) {
            ++hits;
        }
    }
    return hits;
}

json BuildRoutingSelectionJson(const CandidateEvaluation& candidate) {
    json selection{
        {"algorithm_id", candidate.entry->key.algorithm_id},
        {"version", candidate.entry->key.version},
        {"backend_type", ToString(candidate.entry->key.backend_type)},
        {"score", candidate.score},
        {"reasons", candidate.reasons},
    };
    if (const auto latency_ms = EffectiveLatencyMs(*candidate.entry); latency_ms.has_value()) {
        selection["latency_ms"] = latency_ms.value();
    }
    if (candidate.entry->card.hardware_requirements.has_value()) {
        selection["hardware_requirements"] = {
            {"requires_gpu", candidate.entry->card.hardware_requirements->requires_gpu.value_or(false)},
            {"min_gpu_memory_mb", candidate.entry->card.hardware_requirements->min_gpu_memory_mb.value_or(0)},
            {"min_system_memory_mb", candidate.entry->card.hardware_requirements->min_system_memory_mb.value_or(0)},
            {"min_cpu_cores", candidate.entry->card.hardware_requirements->min_cpu_cores.value_or(0)},
            {"preferred_device", candidate.entry->card.hardware_requirements->preferred_device},
        };
    }
    if (candidate.entry->card.safety.has_value()) {
        selection["risk_level"] = candidate.entry->card.safety->risk_level;
        selection["requires_human_review"] =
            candidate.entry->card.safety->requires_human_review.value_or(false);
    }
    return selection;
}

bool IsBetterCandidate(const CandidateEvaluation& lhs, const CandidateEvaluation& rhs) {
    if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
    }

    const int lhs_latency = EffectiveLatencyMs(*lhs.entry).value_or(std::numeric_limits<int>::max());
    const int rhs_latency = EffectiveLatencyMs(*rhs.entry).value_or(std::numeric_limits<int>::max());
    if (lhs_latency != rhs_latency) {
        return lhs_latency < rhs_latency;
    }

    const int lhs_risk = RiskRank(RiskLevel(*lhs.entry));
    const int rhs_risk = RiskRank(RiskLevel(*rhs.entry));
    if (lhs_risk != rhs_risk) {
        return lhs_risk < rhs_risk;
    }

    return lhs.entry->key.ToUniqueString() < rhs.entry->key.ToUniqueString();
}

bool EvaluatePythonServiceCandidate(const AlgorithmEntry& entry,
                                    const AgentCondition& condition,
                                    CandidateEvaluation* evaluation,
                                    std::string* rejection_reason) {
    if (entry.key.backend_type != BackendType::kPythonHttpService) {
        *rejection_reason = "backend_type is not python_http_service";
        return false;
    }
    if (entry.status != AlgorithmStatus::kActive) {
        *rejection_reason = "algorithm is not active";
        return false;
    }

    if (!condition.task_family.empty() &&
        ToLowerAscii(entry.card.task_family) != ToLowerAscii(condition.task_family)) {
        *rejection_reason = "task_family does not match";
        return false;
    }

    const auto missing_capabilities =
        FindMissingValues(entry.card.capabilities, condition.required_capabilities);
    if (!missing_capabilities.empty()) {
        *rejection_reason =
            "missing required capabilities: " + JoinStrings(missing_capabilities);
        return false;
    }

    const auto missing_modalities =
        FindMissingValues(entry.card.modalities.input, condition.input_modalities);
    if (!missing_modalities.empty()) {
        *rejection_reason = "input modalities are incompatible: " + JoinStrings(missing_modalities);
        return false;
    }

    if (condition.input_chars > 0 && entry.card.constraints.has_value() &&
        entry.card.constraints->max_input_chars.has_value() &&
        condition.input_chars > entry.card.constraints->max_input_chars.value()) {
        *rejection_reason = "input_chars exceeds constraints.max_input_chars";
        return false;
    }

    if (condition.request_bytes > 0 && entry.card.constraints.has_value() &&
        entry.card.constraints->max_request_bytes.has_value() &&
        condition.request_bytes > entry.card.constraints->max_request_bytes.value()) {
        *rejection_reason = "request_bytes exceeds constraints.max_request_bytes";
        return false;
    }

    if (!condition.allow_human_review && RequiresHumanReview(entry)) {
        *rejection_reason = "algorithm requires human review";
        return false;
    }

    if (!condition.max_risk_level.empty() &&
        RiskRank(RiskLevel(entry)) > RiskRank(condition.max_risk_level)) {
        *rejection_reason = "risk_level exceeds the allowed threshold";
        return false;
    }

    if (RequiresGpu(entry) && !condition.hardware_profile.has_gpu) {
        *rejection_reason = "algorithm requires GPU but hardware_profile.has_gpu=false";
        return false;
    }
    if (const auto min_gpu_memory_mb = MinGpuMemoryMb(entry); min_gpu_memory_mb.has_value() &&
        condition.hardware_profile.available_gpu_memory_mb > 0 &&
        condition.hardware_profile.available_gpu_memory_mb < min_gpu_memory_mb.value()) {
        *rejection_reason = "available_gpu_memory_mb is smaller than min_gpu_memory_mb";
        return false;
    }
    if (const auto min_system_memory_mb = MinSystemMemoryMb(entry);
        min_system_memory_mb.has_value() &&
        condition.hardware_profile.available_system_memory_mb > 0 &&
        condition.hardware_profile.available_system_memory_mb < min_system_memory_mb.value()) {
        *rejection_reason = "available_system_memory_mb is smaller than min_system_memory_mb";
        return false;
    }
    if (const auto min_cpu_cores = MinCpuCores(entry); min_cpu_cores.has_value() &&
        condition.hardware_profile.available_cpu_cores > 0 &&
        condition.hardware_profile.available_cpu_cores < min_cpu_cores.value()) {
        *rejection_reason = "available_cpu_cores is smaller than min_cpu_cores";
        return false;
    }

    const auto latency_ms = EffectiveLatencyMs(entry);
    if (condition.max_latency_ms > 0 && latency_ms.has_value() &&
        latency_ms.value() > condition.max_latency_ms) {
        *rejection_reason = "latency exceeds max_latency_ms";
        return false;
    }

    evaluation->entry = &entry;
    evaluation->score = 0.0;
    evaluation->reasons.clear();

    if (!condition.task_family.empty()) {
        evaluation->score += 25.0;
        evaluation->reasons.push_back("task_family matched");
    }

    if (!condition.required_capabilities.empty()) {
        evaluation->score += 12.0 * static_cast<double>(condition.required_capabilities.size());
        evaluation->reasons.push_back(
            "required capabilities matched: " + JoinStrings(condition.required_capabilities));
    }

    std::vector<std::string> matched_preferred_capabilities;
    for (const auto& capability : condition.preferred_capabilities) {
        if (ContainsValueIgnoreCase(entry.card.capabilities, capability)) {
            matched_preferred_capabilities.push_back(capability);
        }
    }
    if (!matched_preferred_capabilities.empty()) {
        evaluation->score +=
            8.0 * static_cast<double>(matched_preferred_capabilities.size());
        evaluation->reasons.push_back(
            "preferred capabilities matched: " + JoinStrings(matched_preferred_capabilities));
    }

    if (!condition.input_modalities.empty()) {
        evaluation->score += 3.0 * static_cast<double>(condition.input_modalities.size());
    }

    const std::vector<std::string> positive_texts = {
        entry.card.agent_card.summary,
        entry.card.agent_card.input_description,
        entry.card.agent_card.output_description,
    };
    const int positive_keyword_hits =
        CountKeywordHits(positive_texts, condition.intent_keywords) +
        CountKeywordHits(entry.card.agent_card.when_to_use, condition.intent_keywords);
    if (positive_keyword_hits > 0) {
        evaluation->score += 5.0 * static_cast<double>(positive_keyword_hits);
        evaluation->reasons.push_back("when_to_use and descriptions matched intent keywords");
    }

    const int negative_keyword_hits =
        CountKeywordHits(entry.card.agent_card.when_not_to_use, condition.intent_keywords);
    if (negative_keyword_hits > 0) {
        evaluation->score -= 6.0 * static_cast<double>(negative_keyword_hits);
        evaluation->reasons.push_back("when_not_to_use partially overlaps with intent keywords");
    }

    if (latency_ms.has_value()) {
        evaluation->score += std::max(0.0, 15.0 - latency_ms.value() / 100.0);
        evaluation->reasons.push_back("declared latency budget available");
    }

    if (entry.card.performance.has_value() && entry.card.performance->primary_score.has_value()) {
        evaluation->score += std::clamp(entry.card.performance->primary_score.value(), 0.0, 1.0) *
                             5.0;
    }

    const std::string risk_level = RiskLevel(entry);
    if (!risk_level.empty()) {
        if (risk_level == "low") {
            evaluation->score += 4.0;
        } else if (risk_level == "medium") {
            evaluation->score += 2.0;
        }
        evaluation->reasons.push_back("risk_level=" + risk_level);
    }

    if (entry.card.hardware_requirements.has_value()) {
        if (RequiresGpu(entry) && condition.hardware_profile.has_gpu) {
            evaluation->score += 6.0;
            evaluation->reasons.push_back("GPU requirement satisfied");
        } else if (!RequiresGpu(entry) && !condition.hardware_profile.has_gpu) {
            evaluation->score += 2.0;
            evaluation->reasons.push_back("CPU-only path fits current hardware");
        }

        if (const auto preferred_device = PreferredDevice(entry);
            !preferred_device.empty() &&
            !condition.hardware_profile.preferred_device.empty() &&
            preferred_device == ToLowerAscii(condition.hardware_profile.preferred_device)) {
            evaluation->score += 3.0;
            evaluation->reasons.push_back("preferred_device matched hardware profile");
        }
    }

    if (!RequiresHumanReview(entry)) {
        evaluation->score += 3.0;
        evaluation->reasons.push_back("does not require human review");
    }

    return true;
}

}  // namespace

ExecutionCoordinator::ExecutionCoordinator(const AlgorithmRegistry& registry,
                                           std::filesystem::path execution_log_path)
    : registry_(registry),
      execution_logger_(ResolveExecutionLogPath(registry, execution_log_path)) {}

AlgorithmResult ExecutionCoordinator::Run(const AlgorithmRequest& request) {
    AlgorithmRequest effective_request = request;
    EnsureRequestEnvelope(&effective_request);

    const auto started_at = std::chrono::steady_clock::now();
    const auto finalize = [this, &effective_request, started_at](AlgorithmResult result) {
        NormalizeResultEnvelope(effective_request, &result);
        const auto latency_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at)
                .count();
        EnsureLatency(&result, latency_ms);
        const Status log_status =
            execution_logger_.Append(effective_request, result, latency_ms);
        (void)log_status;
        return result;
    };

    const AlgorithmKey key{
        effective_request.algorithm_id,
        effective_request.version,
        effective_request.backend_type,
    };

    auto entry_result = registry_.Get(key);
    if (!entry_result.ok()) {
        return finalize(BuildFailureResult(effective_request, entry_result.status()));
    }

    const AlgorithmEntry& entry = entry_result.value();
    if (entry.status != AlgorithmStatus::kActive) {
        return finalize(BuildFailureResult(
            effective_request,
            Status::Error(ErrorCode::kAlgorithmNotActive,
                          "Algorithm is not active: " + key.ToUniqueString())));
    }

    auto input_status = registry_.ValidateInputPayload(key, effective_request.inputs);
    if (!input_status.ok()) {
        return finalize(BuildFailureResult(effective_request, input_status));
    }

    auto runner = runtime_factory_.Create(effective_request.backend_type);
    if (!runner) {
        return finalize(BuildFailureResult(
            effective_request,
            Status::Error(
                ErrorCode::kUnsupportedBackendType,
                "No runtime runner is registered for backend_type=" +
                    ToString(effective_request.backend_type) + ".")));
    }

    auto load_status = runner->Load(entry);
    if (!load_status.ok()) {
        return finalize(BuildFailureResult(effective_request, load_status));
    }

    AlgorithmResult result = runner->Run(effective_request);
    NormalizeResultEnvelope(effective_request, &result);
    if (!result.ok) {
        if (!result.error.has_value()) {
            result.error = AlgorithmError{
                ToString(ErrorCode::kInvalidArgument),
                "Runner returned ok=false without an error payload.",
            };
        }
        return finalize(std::move(result));
    }

    auto output_status = registry_.ValidateOutputPayload(key, result.outputs);
    if (!output_status.ok()) {
        return finalize(BuildFailureResult(effective_request, output_status, result.usage));
    }

    return finalize(std::move(result));
}

AlgorithmResult ExecutionCoordinator::RouteForAgent(const AgentRoutingRequest& request) {
    AlgorithmRequest routing_envelope;
    routing_envelope.request_id = request.request_id;
    routing_envelope.trace_id = request.trace_id;
    routing_envelope.backend_type = BackendType::kPythonHttpService;
    routing_envelope.inputs = request.inputs;
    routing_envelope.params = request.params;
    EnsureRequestEnvelope(&routing_envelope);

    const auto entries = registry_.List(false);

    std::optional<CandidateEvaluation> best_candidate;
    json rejected_candidates = json::array();
    int considered_candidates = 0;

    for (const auto& entry : entries) {
        if (entry.status != AlgorithmStatus::kActive ||
            entry.key.backend_type != BackendType::kPythonHttpService) {
            continue;
        }
        ++considered_candidates;

        CandidateEvaluation evaluation;
        std::string rejection_reason;
        if (!EvaluatePythonServiceCandidate(entry, request.condition, &evaluation,
                                            &rejection_reason)) {
            rejected_candidates.push_back({
                {"algorithm_id", entry.key.algorithm_id},
                {"version", entry.key.version},
                {"reason", rejection_reason},
            });
            continue;
        }

        if (!best_candidate.has_value() || IsBetterCandidate(evaluation, best_candidate.value())) {
            best_candidate = std::move(evaluation);
        }
    }

    if (!best_candidate.has_value()) {
        json usage{
            {"routing",
             {
                 {"condition", ToJson(request.condition)},
                 {"considered_candidates", considered_candidates},
                 {"rejected_candidates", rejected_candidates},
             }},
        };
        return BuildFailureResult(
            routing_envelope,
            Status::Error(ErrorCode::kAlgorithmNotFound,
                          "No active python_http_service algorithm matched the agent conditions."),
            std::move(usage));
    }

    AlgorithmRequest selected_request;
    selected_request.request_id = routing_envelope.request_id;
    selected_request.trace_id = routing_envelope.trace_id;
    selected_request.algorithm_id = best_candidate->entry->key.algorithm_id;
    selected_request.version = best_candidate->entry->key.version;
    selected_request.backend_type = best_candidate->entry->key.backend_type;
    selected_request.inputs = request.inputs;
    selected_request.params = request.params;

    AlgorithmResult result = Run(selected_request);
    if (!result.usage.is_object()) {
        result.usage = json::object();
    }
    result.usage["routing"] = {
        {"condition", ToJson(request.condition)},
        {"considered_candidates", considered_candidates},
        {"rejected_candidates", rejected_candidates},
        {"selected_candidate", BuildRoutingSelectionJson(best_candidate.value())},
    };
    return result;
}

const std::filesystem::path& ExecutionCoordinator::execution_log_path() const {
    return execution_logger_.log_path();
}

}  // namespace algolib


