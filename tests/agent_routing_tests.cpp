#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "algolib/io/json_utils.h"
#include "algolib/registry/algorithm_registry.h"
#include "algolib/runtime/algorithm_request.h"
#include "algolib/runtime/execution_coordinator.h"
#include "python_service_test_support.h"

namespace {

namespace fs = std::filesystem;
using algolib::AlgorithmKey;
using algolib::AlgorithmRegistry;
using algolib::BackendType;
using algolib::ExecutionCoordinator;
using algolib::JsonUtils;
using algolib::testsupport::MockPythonService;
using algolib::testsupport::MockPythonServiceConfig;
using algolib::testsupport::PointServiceFixtureAtBaseUrl;
using algolib::testsupport::SourceRoot;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

fs::path MakeTempDir(const std::string& name) {
    fs::path temp_dir = fs::temp_directory_path() / ("algolib_agent_routing_" + name);
    std::error_code ec;
    fs::remove_all(temp_dir, ec);
    fs::create_directories(temp_dir);
    return temp_dir;
}

AlgorithmKey ServiceKey(const std::string& algorithm_id, const std::string& version = "1.0.0") {
    return AlgorithmKey{algorithm_id, version, BackendType::kPythonHttpService};
}

fs::path CopyExampleFixture(const fs::path& temp_dir, const std::string& example_name) {
    const fs::path source_dir = SourceRoot() / "examples" / example_name / "1.0.0";
    const fs::path target_dir = temp_dir / example_name / "1.0.0";
    fs::create_directories(target_dir.parent_path());
    fs::copy(source_dir, target_dir,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    return target_dir;
}

nlohmann::json LoadGoldenResponse(const std::string& example_name) {
    const fs::path response_path = SourceRoot() / "examples" / example_name / "1.0.0" /
                                   "golden_cases" / "case_001_response.json";
    auto json_result = JsonUtils::ReadJsonFile(response_path);
    if (!json_result.ok()) {
        throw std::runtime_error("Failed to read golden response: " +
                                 response_path.generic_string());
    }
    return json_result.value();
}

MockPythonService CreateMockServiceForExample(const std::string& algorithm_id,
                                              const std::string& example_name) {
    MockPythonServiceConfig config;
    config.algorithm_id = algorithm_id;
    config.version = "1.0.0";
    config.predict_body = LoadGoldenResponse(example_name);
    return MockPythonService(config);
}

fs::path PrepareExampleFixture(const fs::path& temp_dir,
                               const std::string& example_name,
                               const std::string& base_url) {
    const fs::path fixture_dir = CopyExampleFixture(temp_dir, example_name);
    PointServiceFixtureAtBaseUrl(fixture_dir, base_url);
    return fixture_dir;
}

void RegisterAndActivate(AlgorithmRegistry* registry,
                         const fs::path& fixture_dir,
                         const std::string& algorithm_id) {
    Expect(registry->Register(fixture_dir).ok(), algorithm_id + " should register.");
    Expect(registry->Activate(ServiceKey(algorithm_id)).ok(),
           algorithm_id + " should activate.");
}

algolib::AgentHardwareProfile CpuOnlyHardware() {
    algolib::AgentHardwareProfile hardware;
    hardware.has_gpu = false;
    hardware.available_system_memory_mb = 4096;
    hardware.available_cpu_cores = 4;
    hardware.preferred_device = "cpu";
    return hardware;
}

algolib::AgentHardwareProfile GpuHardware() {
    algolib::AgentHardwareProfile hardware;
    hardware.has_gpu = true;
    hardware.available_gpu_memory_mb = 16384;
    hardware.available_system_memory_mb = 32768;
    hardware.available_cpu_cores = 8;
    hardware.preferred_device = "gpu";
    return hardware;
}

void TestAgentRoutingSelectsPolicySummarizerExample() {
    fs::path temp_dir = MakeTempDir("policy_summarizer");
    fs::path registry_path = temp_dir / "registry.json";

    auto policy_service = CreateMockServiceForExample(
        "llm_policy_summarizer", "python_http_service_llm_policy_summarizer");
    auto risk_service = CreateMockServiceForExample(
        "llm_risk_review_advisor", "python_http_service_llm_risk_review_advisor");

    const fs::path policy_fixture = PrepareExampleFixture(
        temp_dir / "fixtures", "python_http_service_llm_policy_summarizer",
        policy_service.base_url());
    const fs::path risk_fixture = PrepareExampleFixture(
        temp_dir / "fixtures", "python_http_service_llm_risk_review_advisor",
        risk_service.base_url());

    AlgorithmRegistry registry(registry_path);
    Expect(registry.Reload().ok(), "Reload should succeed.");
    RegisterAndActivate(&registry, policy_fixture, "llm_policy_summarizer");
    RegisterAndActivate(&registry, risk_fixture, "llm_risk_review_advisor");

    ExecutionCoordinator coordinator(registry);
    algolib::AgentRoutingRequest request;
    request.request_id = "req_policy_route_001";
    request.trace_id = "trace_policy_route_001";
    request.inputs = {
        {"task_text", "Summarize the policy update for reviewers"},
        {"entities", nlohmann::json::array()},
    };
    request.condition.task_family = "generation";
    request.condition.required_capabilities = {"policy_summary"};
    request.condition.preferred_capabilities = {"structured_summary"};
    request.condition.input_modalities = {"text", "structured_json"};
    request.condition.intent_keywords = {"summary", "policy"};
    request.condition.input_chars = 64;
    request.condition.request_bytes = 256;
    request.condition.max_latency_ms = 1500;
    request.condition.allow_human_review = false;
    request.condition.max_risk_level = "low";
    request.condition.hardware_profile = CpuOnlyHardware();

    const auto result = coordinator.RouteForAgent(request);
    Expect(result.ok, "Policy summarizer should be routable.");
    Expect(result.algorithm_id == "llm_policy_summarizer",
           "Policy request should select llm_policy_summarizer.");
    Expect(result.outputs.value("summary", std::string()) ==
               "Policy update summary for reviewers.",
           "Policy summarizer should return the golden summary.");
}

void TestAgentRoutingSelectsRiskReviewAdvisorExample() {
    fs::path temp_dir = MakeTempDir("risk_review");
    fs::path registry_path = temp_dir / "registry.json";

    auto policy_service = CreateMockServiceForExample(
        "llm_policy_summarizer", "python_http_service_llm_policy_summarizer");
    auto risk_service = CreateMockServiceForExample(
        "llm_risk_review_advisor", "python_http_service_llm_risk_review_advisor");

    const fs::path policy_fixture = PrepareExampleFixture(
        temp_dir / "fixtures", "python_http_service_llm_policy_summarizer",
        policy_service.base_url());
    const fs::path risk_fixture = PrepareExampleFixture(
        temp_dir / "fixtures", "python_http_service_llm_risk_review_advisor",
        risk_service.base_url());

    AlgorithmRegistry registry(registry_path);
    Expect(registry.Reload().ok(), "Reload should succeed.");
    RegisterAndActivate(&registry, policy_fixture, "llm_policy_summarizer");
    RegisterAndActivate(&registry, risk_fixture, "llm_risk_review_advisor");

    ExecutionCoordinator coordinator(registry);
    algolib::AgentRoutingRequest request;
    request.request_id = "req_risk_route_001";
    request.trace_id = "trace_risk_route_001";
    request.inputs = {
        {"task_text", "Review the task for high-risk escalation"},
        {"entities", nlohmann::json::array()},
    };
    request.condition.task_family = "generation";
    request.condition.required_capabilities = {"risk_assessment"};
    request.condition.preferred_capabilities = {"review_recommendation"};
    request.condition.input_modalities = {"text", "structured_json"};
    request.condition.intent_keywords = {"risk", "review"};
    request.condition.input_chars = 64;
    request.condition.request_bytes = 256;
    request.condition.max_latency_ms = 2000;
    request.condition.allow_human_review = true;
    request.condition.max_risk_level = "medium";
    request.condition.hardware_profile = GpuHardware();

    const auto result = coordinator.RouteForAgent(request);
    Expect(result.ok, "Risk advisor should be routable.");
    Expect(result.algorithm_id == "llm_risk_review_advisor",
           "Risk request should select llm_risk_review_advisor.");
    Expect(result.outputs.value("risk_level", std::string()) == "high",
           "Risk advisor should return the golden risk level.");
}

void TestAgentRoutingSelectsEntityNormalizerExample() {
    fs::path temp_dir = MakeTempDir("entity_normalizer");
    fs::path registry_path = temp_dir / "registry.json";

    auto normalizer_service = CreateMockServiceForExample(
        "llm_entity_normalizer", "python_http_service_llm_entity_normalizer");
    auto policy_service = CreateMockServiceForExample(
        "llm_policy_summarizer", "python_http_service_llm_policy_summarizer");

    const fs::path normalizer_fixture = PrepareExampleFixture(
        temp_dir / "fixtures", "python_http_service_llm_entity_normalizer",
        normalizer_service.base_url());
    const fs::path policy_fixture = PrepareExampleFixture(
        temp_dir / "fixtures", "python_http_service_llm_policy_summarizer",
        policy_service.base_url());

    AlgorithmRegistry registry(registry_path);
    Expect(registry.Reload().ok(), "Reload should succeed.");
    RegisterAndActivate(&registry, normalizer_fixture, "llm_entity_normalizer");
    RegisterAndActivate(&registry, policy_fixture, "llm_policy_summarizer");

    ExecutionCoordinator coordinator(registry);
    algolib::AgentRoutingRequest request;
    request.request_id = "req_entity_route_001";
    request.trace_id = "trace_entity_route_001";
    request.inputs = {
        {"task_text", "Normalize the extracted entities for storage"},
        {"entities", nlohmann::json::array({
            {{"name", "Alice"}, {"type", "person"}},
        })},
    };
    request.condition.task_family = "generation";
    request.condition.required_capabilities = {"entity_normalization"};
    request.condition.preferred_capabilities = {"structured_summary"};
    request.condition.input_modalities = {"text", "structured_json"};
    request.condition.intent_keywords = {"normalize", "entity"};
    request.condition.input_chars = 64;
    request.condition.request_bytes = 512;
    request.condition.max_latency_ms = 1200;
    request.condition.allow_human_review = false;
    request.condition.max_risk_level = "low";
    request.condition.hardware_profile = CpuOnlyHardware();

    const auto result = coordinator.RouteForAgent(request);
    Expect(result.ok, "Entity normalizer should be routable.");
    Expect(result.algorithm_id == "llm_entity_normalizer",
           "Entity request should select llm_entity_normalizer.");
    Expect(result.outputs.at("normalized_entities").at(0).value("canonical_name", std::string()) ==
               "alice",
           "Entity normalizer should return the golden canonical name.");
}

void TestAgentRoutingRejectsGpuAlgorithmWithoutGpu() {
    fs::path temp_dir = MakeTempDir("reject_no_gpu");
    fs::path registry_path = temp_dir / "registry.json";

    auto risk_service = CreateMockServiceForExample(
        "llm_risk_review_advisor", "python_http_service_llm_risk_review_advisor");
    const fs::path risk_fixture = PrepareExampleFixture(
        temp_dir / "fixtures", "python_http_service_llm_risk_review_advisor",
        risk_service.base_url());

    AlgorithmRegistry registry(registry_path);
    Expect(registry.Reload().ok(), "Reload should succeed.");
    RegisterAndActivate(&registry, risk_fixture, "llm_risk_review_advisor");

    ExecutionCoordinator coordinator(registry);
    algolib::AgentRoutingRequest request;
    request.request_id = "req_risk_reject_gpu_001";
    request.trace_id = "trace_risk_reject_gpu_001";
    request.inputs = {
        {"task_text", "Review the task for high-risk escalation"},
        {"entities", nlohmann::json::array()},
    };
    request.condition.task_family = "generation";
    request.condition.required_capabilities = {"risk_assessment"};
    request.condition.input_modalities = {"text", "structured_json"};
    request.condition.allow_human_review = true;
    request.condition.max_risk_level = "medium";
    request.condition.hardware_profile = CpuOnlyHardware();

    const auto result = coordinator.RouteForAgent(request);
    Expect(!result.ok, "Routing should fail when GPU-only service meets CPU-only agent.");
    Expect(result.error.has_value(), "Failure should expose an error payload.");
    Expect(result.error->code == "ALGORITHM_NOT_FOUND",
           "No eligible hardware-compatible service should map to ALGORITHM_NOT_FOUND.");
}

void TestAgentRoutingRejectsHumanReviewWhenNotAllowed() {
    fs::path temp_dir = MakeTempDir("reject_human_review");
    fs::path registry_path = temp_dir / "registry.json";

    auto risk_service = CreateMockServiceForExample(
        "llm_risk_review_advisor", "python_http_service_llm_risk_review_advisor");
    const fs::path risk_fixture = PrepareExampleFixture(
        temp_dir / "fixtures", "python_http_service_llm_risk_review_advisor",
        risk_service.base_url());

    AlgorithmRegistry registry(registry_path);
    Expect(registry.Reload().ok(), "Reload should succeed.");
    RegisterAndActivate(&registry, risk_fixture, "llm_risk_review_advisor");

    ExecutionCoordinator coordinator(registry);
    algolib::AgentRoutingRequest request;
    request.request_id = "req_risk_reject_001";
    request.trace_id = "trace_risk_reject_001";
    request.inputs = {
        {"task_text", "Review the task for high-risk escalation"},
        {"entities", nlohmann::json::array()},
    };
    request.condition.task_family = "generation";
    request.condition.required_capabilities = {"risk_assessment"};
    request.condition.input_modalities = {"text", "structured_json"};
    request.condition.allow_human_review = false;
    request.condition.max_risk_level = "medium";
    request.condition.hardware_profile = GpuHardware();

    const auto result = coordinator.RouteForAgent(request);
    Expect(!result.ok, "Routing should fail when only human-review services match.");
    Expect(result.error.has_value(), "Failure should expose an error payload.");
    Expect(result.error->code == "ALGORITHM_NOT_FOUND",
           "No eligible service should map to ALGORITHM_NOT_FOUND.");
    Expect(result.usage.contains("routing"), "Failure should still expose routing diagnostics.");
    Expect(result.usage.at("routing").at("rejected_candidates").is_array(),
           "Rejected candidates should be listed.");
}

void TestAgentViewExposesExamplesPerformanceAndHardware() {
    fs::path temp_dir = MakeTempDir("agent_view_fields");
    fs::path registry_path = temp_dir / "registry.json";

    auto policy_service = CreateMockServiceForExample(
        "llm_policy_summarizer", "python_http_service_llm_policy_summarizer");
    const fs::path policy_fixture = PrepareExampleFixture(
        temp_dir / "fixtures", "python_http_service_llm_policy_summarizer",
        policy_service.base_url());

    AlgorithmRegistry registry(registry_path);
    Expect(registry.Reload().ok(), "Reload should succeed.");
    RegisterAndActivate(&registry, policy_fixture, "llm_policy_summarizer");

    const auto views = registry.ListAgentViews(true);
    Expect(views.size() == 1, "One active agent view should be returned.");
    const auto& view = views.front();
    Expect(view.contains("performance"), "Agent view should include performance.");
    Expect(view.contains("hardware_requirements"),
           "Agent view should include hardware_requirements.");
    Expect(view.at("agent_card").contains("examples"),
           "Agent view should include agent_card.examples.");
    Expect(view.at("agent_card").at("examples").is_array() &&
               !view.at("agent_card").at("examples").empty(),
           "Agent examples should not be empty for the new service example.");
}

}  // namespace

int RunAgentRoutingTests() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"TestAgentRoutingSelectsPolicySummarizerExample",
         TestAgentRoutingSelectsPolicySummarizerExample},
        {"TestAgentRoutingSelectsRiskReviewAdvisorExample",
         TestAgentRoutingSelectsRiskReviewAdvisorExample},
        {"TestAgentRoutingSelectsEntityNormalizerExample",
         TestAgentRoutingSelectsEntityNormalizerExample},
        {"TestAgentRoutingRejectsGpuAlgorithmWithoutGpu",
         TestAgentRoutingRejectsGpuAlgorithmWithoutGpu},
        {"TestAgentRoutingRejectsHumanReviewWhenNotAllowed",
         TestAgentRoutingRejectsHumanReviewWhenNotAllowed},
        {"TestAgentViewExposesExamplesPerformanceAndHardware",
         TestAgentViewExposesExamplesPerformanceAndHardware},
    };

    int failed = 0;
    for (const auto& [name, test_fn] : tests) {
        try {
            test_fn();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& ex) {
            ++failed;
            std::cerr << "[FAIL] " << name << ": " << ex.what() << '\n';
        }
    }

    return failed == 0 ? 0 : 1;
}
