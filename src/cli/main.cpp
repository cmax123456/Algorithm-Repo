#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "algolib/core/algorithm_card.h"
#include "algolib/io/file_utils.h"
#include "algolib/io/json_utils.h"
#include "algolib/io/yaml_utils.h"
#include "algolib/registry/algorithm_registry.h"
#include "algolib/runtime/algorithm_request.h"
#include "algolib/runtime/execution_coordinator.h"

namespace {

using algolib::AgentRoutingRequestFromJson;
using algolib::AlgorithmCard;
using algolib::AlgorithmCardFromJson;
using algolib::AlgorithmEntry;
using algolib::AlgorithmKey;
using algolib::AlgorithmRegistry;
using algolib::AlgorithmRequestFromJson;
using algolib::ExecutionCoordinator;
using algolib::FileUtils;
using algolib::JsonUtils;
using algolib::ParseBackendType;
using algolib::Result;
using algolib::Status;
using algolib::YamlUtils;

struct LoadedCard {
    std::filesystem::path card_path;
    AlgorithmCard card;
};

std::filesystem::path ResolveRegistryPath() {
    if (const char* env_value = std::getenv("ALGOLIB_REGISTRY_PATH"); env_value != nullptr) {
        return std::filesystem::path(env_value);
    }
    return std::filesystem::current_path() / ".algolib" / "registry.json";
}

void PrintUsage() {
    std::cout << "algolib register <package_or_card_path>\n"
              << "algolib validate <algorithm_id> <version> <backend_type>\n"
              << "algolib activate <algorithm_id> <version> <backend_type>\n"
              << "algolib disable <algorithm_id> <version> <backend_type>\n"
              << "algolib delete <algorithm_id> <version> <backend_type>\n"
              << "algolib list\n"
              << "algolib list-agent\n"
              << "algolib show-card <algorithm_id> <version> <backend_type>\n"
              << "algolib run <request_json_path>\n"
              << "algolib agent-run <routing_request_json_path>\n"
              << "algolib agent-template <algorithm_id> <version> <backend_type>\n"
              << "algolib configure-service <package_or_card_path> <base_url>\n"
              << "algolib register-activate <package_or_card_path>\n"
              << "algolib bootstrap-service <package_or_card_path> <base_url>\n";
}

nlohmann::json BuildEntrySummary(const AlgorithmEntry& entry) {
    return {
        {"algorithm_id", entry.key.algorithm_id},
        {"version", entry.key.version},
        {"backend_type", algolib::ToString(entry.key.backend_type)},
        {"status", algolib::ToString(entry.status)},
        {"display_name", entry.card.display_name},
        {"task_family", entry.card.task_family},
    };
}

AlgorithmKey ParseKeyOrThrow(const std::vector<std::string>& args, std::size_t start_index) {
    if (args.size() <= start_index + 2) {
        throw std::runtime_error("Expected <algorithm_id> <version> <backend_type>.");
    }

    auto backend_result = ParseBackendType(args[start_index + 2]);
    if (!backend_result.ok()) {
        throw std::runtime_error(backend_result.status().ToString());
    }

    return AlgorithmKey{
        args[start_index],
        args[start_index + 1],
        backend_result.value(),
    };
}

int PrintStatusError(const Status& status) {
    nlohmann::json error_json{
        {"ok", false},
        {"error_code", algolib::ToString(status.code())},
        {"message", status.message()},
    };
    std::cerr << JsonUtils::Pretty(error_json) << std::endl;
    return 1;
}

void PrintJson(const nlohmann::json& payload) {
    std::cout << JsonUtils::Pretty(payload) << std::endl;
}

std::string TrimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

Result<LoadedCard> LoadCardForEditing(const std::filesystem::path& package_or_card_path) {
    auto card_path_result = FileUtils::ResolveCardPath(package_or_card_path);
    if (!card_path_result.ok()) {
        return card_path_result.status();
    }

    auto yaml_result = YamlUtils::LoadYamlFile(card_path_result.value());
    if (!yaml_result.ok()) {
        return yaml_result.status();
    }

    auto card_result = AlgorithmCardFromJson(YamlUtils::YamlNodeToJson(yaml_result.value()));
    if (!card_result.ok()) {
        return card_result.status();
    }

    return LoadedCard{card_path_result.value(), card_result.value()};
}

Status SaveCard(const LoadedCard& loaded_card) {
    return FileUtils::WriteTextFile(loaded_card.card_path,
                                    algolib::ToYamlString(loaded_card.card));
}

Status ConfigurePythonServiceBaseUrl(LoadedCard* loaded_card, const std::string& base_url) {
    if (loaded_card->card.backend_type != algolib::BackendType::kPythonHttpService) {
        return Status::Error(algolib::ErrorCode::kBackendTypeMismatch,
                             "configure-service only supports python_http_service cards.");
    }

    const std::string normalized_base_url = TrimTrailingSlash(base_url);
    if (normalized_base_url.empty() || normalized_base_url.rfind("http://", 0) != 0) {
        return Status::Error(algolib::ErrorCode::kInvalidArgument,
                             "base_url must start with http:// and include host:port.");
    }

    loaded_card->card.machine_spec.runtime.endpoint = normalized_base_url + "/predict";
    loaded_card->card.machine_spec.runtime.health_endpoint = normalized_base_url + "/health";
    loaded_card->card.machine_spec.runtime.metadata_endpoint = normalized_base_url + "/metadata";
    return Status::Ok();
}

std::optional<int> TryExtractPort(const std::string& base_url) {
    const std::string normalized = TrimTrailingSlash(base_url);
    if (normalized.rfind("http://", 0) != 0) {
        return std::nullopt;
    }
    const std::string host_port = normalized.substr(std::string("http://").size());
    const std::size_t colon_pos = host_port.rfind(':');
    if (colon_pos == std::string::npos || colon_pos + 1 >= host_port.size()) {
        return std::nullopt;
    }
    try {
        return std::stoi(host_port.substr(colon_pos + 1));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> BuildLocalStartCommand(const LoadedCard& loaded_card,
                                                  const std::string& base_url) {
    const auto service_script = loaded_card.card_path.parent_path() / "service.py";
    if (!std::filesystem::exists(service_script)) {
        return std::nullopt;
    }

    const auto port = TryExtractPort(base_url);
    if (!port.has_value()) {
        return std::string("python3 ") + service_script.generic_string();
    }

    return std::string("python3 ") + service_script.generic_string() +
           " --host 127.0.0.1 --port " + std::to_string(port.value());
}

int EstimateStringChars(const nlohmann::json& value) {
    if (value.is_string()) {
        return static_cast<int>(value.get_ref<const std::string&>().size());
    }
    if (value.is_array()) {
        int total = 0;
        for (const auto& item : value) {
            total += EstimateStringChars(item);
        }
        return total;
    }
    if (value.is_object()) {
        int total = 0;
        for (auto it = value.begin(); it != value.end(); ++it) {
            total += EstimateStringChars(it.value());
        }
        return total;
    }
    return 0;
}

nlohmann::json BuildAgentTemplate(const AlgorithmEntry& entry) {
    nlohmann::json inputs = nlohmann::json::object();
    if (!entry.card.agent_card.examples.empty()) {
        inputs = entry.card.agent_card.examples.front().input;
    }

    nlohmann::json required_capabilities = nlohmann::json::array();
    nlohmann::json preferred_capabilities = nlohmann::json::array();
    if (!entry.card.capabilities.empty()) {
        required_capabilities.push_back(entry.card.capabilities.front());
        for (std::size_t index = 1; index < entry.card.capabilities.size(); ++index) {
            preferred_capabilities.push_back(entry.card.capabilities[index]);
        }
    }

    const int max_latency_ms = entry.card.performance.has_value()
                                   ? entry.card.performance->latency_ms_p95.value_or(
                                         entry.card.performance->latency_ms_p50.value_or(0))
                                   : 0;
    const bool allow_human_review =
        entry.card.safety.has_value() &&
        entry.card.safety->requires_human_review.value_or(false);
    const std::string max_risk_level =
        entry.card.safety.has_value() && !entry.card.safety->risk_level.empty()
            ? entry.card.safety->risk_level
            : "medium";

    nlohmann::json intent_keywords = nlohmann::json::array();
    intent_keywords.push_back(entry.card.task_family);
    for (const auto& capability : entry.card.capabilities) {
        intent_keywords.push_back(capability);
    }

    return {
        {"request_id", "req_agent_template_001"},
        {"trace_id", "trace_agent_template_001"},
        {"condition",
         {
             {"task_family", entry.card.task_family},
             {"required_capabilities", required_capabilities},
             {"preferred_capabilities", preferred_capabilities},
             {"input_modalities", entry.card.modalities.input},
             {"intent_keywords", intent_keywords},
             {"input_chars", EstimateStringChars(inputs)},
             {"request_bytes", static_cast<int>(JsonUtils::Dump(inputs).size())},
             {"max_latency_ms", max_latency_ms},
             {"allow_human_review", allow_human_review},
             {"max_risk_level", max_risk_level},
         }},
        {"inputs", inputs},
        {"params", nlohmann::json::object()},
    };
}

Result<AlgorithmEntry> EnsureRegisteredAndActive(AlgorithmRegistry* registry,
                                                 const std::filesystem::path& package_or_card_path) {
    auto loaded_card_result = LoadCardForEditing(package_or_card_path);
    if (!loaded_card_result.ok()) {
        return loaded_card_result.status();
    }

    const AlgorithmKey key{
        loaded_card_result.value().card.algorithm_id,
        loaded_card_result.value().card.version,
        loaded_card_result.value().card.backend_type,
    };

    auto existing_result = registry->Get(key);
    if (existing_result.ok()) {
        auto validate_result = registry->Validate(key);
        if (!validate_result.ok()) {
            return validate_result.status();
        }
        auto activate_result = registry->Activate(key);
        if (!activate_result.ok()) {
            return activate_result.status();
        }
        return activate_result.value();
    }

    auto register_result = registry->Register(package_or_card_path);
    if (!register_result.ok()) {
        return register_result.status();
    }

    auto activate_result = registry->Activate(register_result.value().key);
    if (!activate_result.ok()) {
        return activate_result.status();
    }
    return activate_result.value();
}

nlohmann::json BuildServiceSummary(const LoadedCard& loaded_card,
                                   const std::optional<std::string>& start_command) {
    nlohmann::json payload{
        {"ok", true},
        {"algorithm_id", loaded_card.card.algorithm_id},
        {"version", loaded_card.card.version},
        {"backend_type", algolib::ToString(loaded_card.card.backend_type)},
        {"card_path", loaded_card.card_path.generic_string()},
        {"runtime",
         {
             {"endpoint", loaded_card.card.machine_spec.runtime.endpoint},
             {"health_endpoint", loaded_card.card.machine_spec.runtime.health_endpoint},
             {"metadata_endpoint", loaded_card.card.machine_spec.runtime.metadata_endpoint},
         }},
    };
    if (start_command.has_value()) {
        payload["local_start_command"] = start_command.value();
    }
    return payload;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        std::vector<std::string> args(argv + 1, argv + argc);
        if (args.empty()) {
            PrintUsage();
            return 1;
        }

        AlgorithmRegistry registry(ResolveRegistryPath());
        auto reload_status = registry.Reload();
        if (!reload_status.ok()) {
            return PrintStatusError(reload_status);
        }

        const std::string& command = args[0];

        if (command == "register") {
            if (args.size() != 2) {
                PrintUsage();
                return 1;
            }
            auto result = registry.Register(args[1]);
            if (!result.ok()) {
                return PrintStatusError(result.status());
            }
            PrintJson({
                {"ok", true},
                {"algorithm_id", result.value().key.algorithm_id},
                {"version", result.value().key.version},
                {"backend_type", algolib::ToString(result.value().key.backend_type)},
                {"status", algolib::ToString(result.value().status)},
            });
            return 0;
        }

        if (command == "validate") {
            AlgorithmKey key = ParseKeyOrThrow(args, 1);
            auto result = registry.Validate(key);
            if (!result.ok()) {
                return PrintStatusError(result.status());
            }
            PrintJson({
                {"ok", true},
                {"algorithm_id", result.value().key.algorithm_id},
                {"version", result.value().key.version},
                {"backend_type", algolib::ToString(result.value().key.backend_type)},
                {"status", algolib::ToString(result.value().status)},
            });
            return 0;
        }

        if (command == "activate") {
            AlgorithmKey key = ParseKeyOrThrow(args, 1);
            auto result = registry.Activate(key);
            if (!result.ok()) {
                return PrintStatusError(result.status());
            }
            PrintJson({
                {"ok", true},
                {"algorithm_id", result.value().key.algorithm_id},
                {"version", result.value().key.version},
                {"backend_type", algolib::ToString(result.value().key.backend_type)},
                {"status", algolib::ToString(result.value().status)},
            });
            return 0;
        }

        if (command == "disable") {
            AlgorithmKey key = ParseKeyOrThrow(args, 1);
            auto result = registry.Disable(key);
            if (!result.ok()) {
                return PrintStatusError(result.status());
            }
            PrintJson({
                {"ok", true},
                {"algorithm_id", result.value().key.algorithm_id},
                {"version", result.value().key.version},
                {"backend_type", algolib::ToString(result.value().key.backend_type)},
                {"status", algolib::ToString(result.value().status)},
            });
            return 0;
        }

        if (command == "delete") {
            AlgorithmKey key = ParseKeyOrThrow(args, 1);
            auto result = registry.Delete(key);
            if (!result.ok()) {
                return PrintStatusError(result.status());
            }
            PrintJson({
                {"ok", true},
                {"algorithm_id", result.value().key.algorithm_id},
                {"version", result.value().key.version},
                {"backend_type", algolib::ToString(result.value().key.backend_type)},
                {"status", algolib::ToString(result.value().status)},
            });
            return 0;
        }

        if (command == "list") {
            if (args.size() != 1) {
                PrintUsage();
                return 1;
            }
            nlohmann::json list_json = nlohmann::json::array();
            for (const auto& entry : registry.List(false)) {
                list_json.push_back(BuildEntrySummary(entry));
            }
            PrintJson(list_json);
            return 0;
        }

        if (command == "list-agent") {
            if (args.size() != 1) {
                PrintUsage();
                return 1;
            }
            PrintJson(nlohmann::json(registry.ListAgentViews(true)));
            return 0;
        }

        if (command == "show-card") {
            AlgorithmKey key = ParseKeyOrThrow(args, 1);
            auto result = registry.Get(key);
            if (!result.ok()) {
                return PrintStatusError(result.status());
            }
            PrintJson({
                {"ok", true},
                {"entry", algolib::ToJson(result.value())},
                {"agent_view", algolib::ToAgentViewJson(result.value())},
            });
            return 0;
        }

        if (command == "run") {
            if (args.size() != 2) {
                PrintUsage();
                return 1;
            }

            auto request_path_result = FileUtils::NormalizeInputPath(args[1]);
            if (!request_path_result.ok()) {
                return PrintStatusError(request_path_result.status());
            }

            auto request_json = JsonUtils::ReadJsonFile(request_path_result.value());
            if (!request_json.ok()) {
                return PrintStatusError(request_json.status());
            }

            auto request_result = AlgorithmRequestFromJson(request_json.value());
            if (!request_result.ok()) {
                return PrintStatusError(request_result.status());
            }

            ExecutionCoordinator coordinator(registry);
            const auto run_result = coordinator.Run(request_result.value());
            PrintJson(algolib::ToJson(run_result));
            return run_result.ok ? 0 : 1;
        }

        if (command == "agent-run") {
            if (args.size() != 2) {
                PrintUsage();
                return 1;
            }

            auto request_path_result = FileUtils::NormalizeInputPath(args[1]);
            if (!request_path_result.ok()) {
                return PrintStatusError(request_path_result.status());
            }

            auto request_json = JsonUtils::ReadJsonFile(request_path_result.value());
            if (!request_json.ok()) {
                return PrintStatusError(request_json.status());
            }

            auto request_result = AgentRoutingRequestFromJson(request_json.value());
            if (!request_result.ok()) {
                return PrintStatusError(request_result.status());
            }

            ExecutionCoordinator coordinator(registry);
            const auto run_result = coordinator.RouteForAgent(request_result.value());
            PrintJson(algolib::ToJson(run_result));
            return run_result.ok ? 0 : 1;
        }

        if (command == "agent-template") {
            AlgorithmKey key = ParseKeyOrThrow(args, 1);
            auto result = registry.Get(key);
            if (!result.ok()) {
                return PrintStatusError(result.status());
            }
            PrintJson(BuildAgentTemplate(result.value()));
            return 0;
        }

        if (command == "configure-service") {
            if (args.size() != 3) {
                PrintUsage();
                return 1;
            }

            auto loaded_card_result = LoadCardForEditing(args[1]);
            if (!loaded_card_result.ok()) {
                return PrintStatusError(loaded_card_result.status());
            }

            LoadedCard loaded_card = loaded_card_result.value();
            auto configure_status = ConfigurePythonServiceBaseUrl(&loaded_card, args[2]);
            if (!configure_status.ok()) {
                return PrintStatusError(configure_status);
            }
            auto save_status = SaveCard(loaded_card);
            if (!save_status.ok()) {
                return PrintStatusError(save_status);
            }

            PrintJson(BuildServiceSummary(loaded_card, BuildLocalStartCommand(loaded_card, args[2])));
            return 0;
        }

        if (command == "register-activate") {
            if (args.size() != 2) {
                PrintUsage();
                return 1;
            }

            auto result = EnsureRegisteredAndActive(&registry, args[1]);
            if (!result.ok()) {
                return PrintStatusError(result.status());
            }
            PrintJson({
                {"ok", true},
                {"algorithm_id", result.value().key.algorithm_id},
                {"version", result.value().key.version},
                {"backend_type", algolib::ToString(result.value().key.backend_type)},
                {"status", algolib::ToString(result.value().status)},
                {"agent_template", BuildAgentTemplate(result.value())},
            });
            return 0;
        }

        if (command == "bootstrap-service") {
            if (args.size() != 3) {
                PrintUsage();
                return 1;
            }

            auto loaded_card_result = LoadCardForEditing(args[1]);
            if (!loaded_card_result.ok()) {
                return PrintStatusError(loaded_card_result.status());
            }

            LoadedCard loaded_card = loaded_card_result.value();
            auto configure_status = ConfigurePythonServiceBaseUrl(&loaded_card, args[2]);
            if (!configure_status.ok()) {
                return PrintStatusError(configure_status);
            }
            auto save_status = SaveCard(loaded_card);
            if (!save_status.ok()) {
                return PrintStatusError(save_status);
            }

            auto activate_result = EnsureRegisteredAndActive(&registry, loaded_card.card_path);
            if (!activate_result.ok()) {
                return PrintStatusError(activate_result.status());
            }

            auto response = BuildServiceSummary(loaded_card, BuildLocalStartCommand(loaded_card, args[2]));
            response["status"] = algolib::ToString(activate_result.value().status);
            response["agent_template"] = BuildAgentTemplate(activate_result.value());
            PrintJson(response);
            return 0;
        }

        PrintUsage();
        return 1;
    } catch (const std::exception& ex) {
        nlohmann::json error_json{
            {"ok", false},
            {"error_code", "INVALID_ARGUMENT"},
            {"message", ex.what()},
        };
        std::cerr << error_json.dump(2) << std::endl;
        return 1;
    }
}
