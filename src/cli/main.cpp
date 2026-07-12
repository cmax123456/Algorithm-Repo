#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "algolib/io/file_utils.h"
#include "algolib/io/json_utils.h"
#include "algolib/registry/algorithm_query.h"
#include "algolib/registry/algorithm_registry.h"
#include "algolib/runtime/algorithm_request.h"
#include "algolib/runtime/execution_coordinator.h"

namespace {

using algolib::AlgorithmEntry;
using algolib::AlgorithmKey;
using algolib::AlgorithmRegistry;
using algolib::AlgorithmRequestFromJson;
using algolib::ExecutionCoordinator;
using algolib::FileUtils;
using algolib::JsonUtils;
using algolib::ParseBackendType;
using algolib::Status;

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
              << "algolib show-card <algorithm_id> <version> <backend_type>\n"
              << "algolib model-info <algorithm_id> <version> <backend_type>\n"
              << "algolib query [--algorithm_id=x] [--task_family=x] [--capability=x] [--status=x] [--max_memory_mb=x] [--max_bandwidth_mbps=x] [--preferred_compute=cpu|gpu] [--runtime_state=x]\n"
              << "algolib run <request_json_path>\n";
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

algolib::AlgorithmQuery ParseQueryArgs(const std::vector<std::string>& args) {
    algolib::AlgorithmQuery query;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const auto eq_pos = arg.find('=');
        if (arg.rfind("--", 0) != 0 || eq_pos == std::string::npos) {
            throw std::runtime_error("Invalid query argument: " + arg);
        }
        const std::string option_name = arg.substr(2, eq_pos - 2);
        const std::string option_value = arg.substr(eq_pos + 1);
        if (option_name == "algorithm_id") {
            query.algorithm_id = option_value;
        } else if (option_name == "task_family") {
            query.task_family = option_value;
        } else if (option_name == "capability") {
            query.capability = option_value;
        } else if (option_name == "status") {
            auto status_result = algolib::ParseAlgorithmStatus(option_value);
            if (!status_result.ok()) {
                throw std::runtime_error(status_result.status().ToString());
            }
            query.status = status_result.value();
        } else if (option_name == "max_memory_mb") {
            query.max_memory_footprint_mb = std::stoi(option_value);
        } else if (option_name == "max_bandwidth_mbps") {
            query.max_bandwidth_requirement_mbps = std::stoi(option_value);
        } else if (option_name == "preferred_compute") {
            query.preferred_compute = option_value;
        } else if (option_name == "runtime_state") {
            auto state_result = algolib::ParseRuntimeState(option_value);
            if (!state_result.ok()) {
                throw std::runtime_error(state_result.status().ToString());
            }
            query.runtime_states.push_back(state_result.value());
        } else {
            throw std::runtime_error("Unknown query argument: " + option_name);
        }
    }
    return query;
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

        if (command == "model-info") {
            AlgorithmKey key = ParseKeyOrThrow(args, 1);
            auto result = registry.Get(key);
            if (!result.ok()) {
                return PrintStatusError(result.status());
            }
            PrintJson(algolib::ToModelInfoJson(result.value()));
            return 0;
        }

        if (command == "query") {
            algolib::AlgorithmQuery query = ParseQueryArgs(args);
            nlohmann::json results_json = nlohmann::json::array();
            for (const auto& item : registry.Query(query)) {
                results_json.push_back(algolib::ToQueryResultJson(item));
            }
            PrintJson({
                {"ok", true},
                {"count", results_json.size()},
                {"results", results_json},
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
