/*******************************************************************************
 * Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/
#include "common/Logger.h"
#include "common/RestClient.h"
#include "common/ServerHelper.h"
#include "cudaq/utils/cudaq_utils.h"
#include <fstream>
#include <iostream>
#include <thread>

namespace cudaq {

/// @brief Find and set the API and refresh tokens, and the time string.
void findApiKeyInFileQudora(std::string &apiKey, const std::string &path,
                      std::string &refreshKey, std::string &timeStr);

/// Search for the API key, invokes findApiKeyInFileQudora
std::string searchAPIKeyQudora(std::string &key, std::string &refreshKey,
                         std::string &timeStr,
                         std::string userSpecifiedConfig = "");

/// @brief The QudoraServerHelper implements the ServerHelper interface
/// to map Job requests and Job result retrievals actions from the calling
/// Executor to the specific schema required by the remote Qudora REST
/// server.
class QudoraServerHelper : public ServerHelper {
protected:
  /// @brief The base URL
  std::string baseUrl = "https://api.qudora.com/jobs/";
  /// @brief The machine we are targeting
  std::string machine = "QVLS-Q1";
  /// @brief Time string, when the last tokens were retrieved
  std::string timeStr = "";
  /// @brief The refresh token
  std::string refreshKey = "";
  /// @brief The API token for the remote server
  std::string apiKey = "";

  std::string userSpecifiedCredentials = "";
  std::string credentialsPath = "";

  /// @brief Return the headers required for the REST calls
  RestHeaders generateRequestHeader() const;

public:
  /// @brief Return the name of this server helper, must be the
  /// same as the qpu config file.
  const std::string name() const override { return "qudora"; }
  RestHeaders getHeaders() override;

  void initialize(BackendConfig config) override {
    std::cout << "Initialize!\n";
    backendConfig = config;

    // Set the machine
    auto iter = backendConfig.find("machine");
    if (iter != backendConfig.end())
      machine = iter->second;

    // Set an alternate base URL if provided
    iter = backendConfig.find("url");
    if (iter != backendConfig.end()) {
      baseUrl = iter->second;
      if (!baseUrl.ends_with("/"))
        baseUrl += "/";
    }

    iter = backendConfig.find("credentials");
    if (iter != backendConfig.end())
      userSpecifiedCredentials = iter->second;

    parseConfigForCommonParams(config);
    std::cout << "Initialize done!\n";
  }

  /// @brief Create a job payload for the provided quantum codes
  ServerJobPayload
  createJob(std::vector<KernelExecution> &circuitCodes) override;

  /// @brief Return the job id from the previous job post
  std::string extractJobId(ServerMessage &postResponse) override;

  /// @brief Return the URL for retrieving job results
  std::string constructGetJobPath(ServerMessage &postResponse) override;
  std::string constructGetJobPath(std::string &jobId) override;

  /// @brief Return true if the job is done
  bool jobIsDone(ServerMessage &getJobResponse) override;

  /// @brief Get the jobs results polling interval.
  /// @return
  std::chrono::microseconds nextResultPollingInterval(ServerMessage &postResponse) override;

  /// @brief Given a completed job response, map back to the sample_result
  cudaq::sample_result processResults(ServerMessage &postJobResponse,
                                      std::string &jobID) override;
};

ServerJobPayload
QudoraServerHelper::createJob(std::vector<KernelExecution> &circuitCodes) {
  std::cout << "Create job with " << circuitCodes.size() << " circuits!\n";

  // Construct the job itself
  ServerMessage j;
  j["name"] = "CUDA-Q " + circuitCodes[0].name;
  j["language"] = "QIR_BITCODE";
  j["shots"] = {};
  j["target"] = machine;
  j["input_data"] = {};
  j["backend_settings"] = nullptr;

  std::vector<ServerMessage> messages;
  for (auto &circuitCode : circuitCodes) {
    
    j["shots"].push_back(shots);
    j["input_data"].push_back(circuitCode.code);
  }
  messages.push_back(j);

  // Get the tokens we need
  credentialsPath =
      searchAPIKeyQudora(apiKey, refreshKey, timeStr, userSpecifiedCredentials);

  // Get the headers
  RestHeaders headers = generateRequestHeader();

  cudaq::info(
      "Created job payload for Qudora, language is QIR 1.0, targeting {}",
      machine);

  // return the payload
  return std::make_tuple(baseUrl, headers, messages);
}

std::string QudoraServerHelper::extractJobId(ServerMessage &postResponse) {
  std::cout << "extractJobId: " << postResponse << "\n";
  std::string id = to_string(postResponse);
  std::cout << "Id:" << id << "\n";
  return id;
}

std::string
QudoraServerHelper::constructGetJobPath(ServerMessage &postResponse) {
  std::string job_id = extractJobId(postResponse);
  return constructGetJobPath(job_id);
}

std::chrono::microseconds
QudoraServerHelper::nextResultPollingInterval(ServerMessage &postResponse) {
  return std::chrono::seconds(1);
}

std::string QudoraServerHelper::constructGetJobPath(std::string &jobId) {
  return baseUrl + "?job_id=" + jobId + "&include_results=True";
}

bool QudoraServerHelper::jobIsDone(ServerMessage &getJobResponse) {
  std::cout << "Is job done?\n";

  auto status = getJobResponse[0]["status"].get<std::string>(); //TODO: error handling in case lookup fails

  if (status == "Failed") {
    throw std::runtime_error("Job failed to execute. See Qudora Cloud for more details.");
  }
  else if (status == "Canceled" || status == "Deleted" || status == "Cancelling") {
    throw std::runtime_error("Job was cancelled.");
  }

  return status == "Completed";
}

cudaq::sample_result
QudoraServerHelper::processResults(ServerMessage &postJobResponse,
                                       std::string &jobId) {

  std::cout << "Processing results! " << postJobResponse << "\n";
  auto& resultList = postJobResponse[0]["result"]; //TODO: error handling in case lookup fails

  std::vector<ExecutionResult> srs;

  for (auto& circuitCodeResult: resultList){
    nlohmann::json circuitCodeResultDict = nlohmann::json::parse(circuitCodeResult.get<std::string>());
    CountsDictionary reg_counts;
    for (auto &[bitstring, count] : circuitCodeResultDict.items()) {
      std::cout << "Counts: " << bitstring << ", " << count << "\n";
      
      reg_counts[bitstring] = count;
    }
    srs.emplace_back(reg_counts, "__global__");
  }
  return sample_result(srs);
}

std::map<std::string, std::string>
QudoraServerHelper::generateRequestHeader() const {
  std::cout << "generateRequestHeader!\n";
  std::string apiKey, refreshKey, timeStr;
  searchAPIKeyQudora(apiKey, refreshKey, timeStr, userSpecifiedCredentials);
  std::map<std::string, std::string> headers{
      {"Authorization", "Bearer " + apiKey},
      {"Content-Type", "application/json"},
      {"Connection", "keep-alive"},
      {"Accept", "*/*"}
  };
  return headers;
}

RestHeaders QudoraServerHelper::getHeaders() {
  return generateRequestHeader();
}


void findApiKeyInFileQudora(std::string &apiKey, const std::string &path,
                      std::string &refreshKey, std::string &timeStr) {
  std::ifstream stream(path);
  std::string contents((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());

  std::vector<std::string> lines;
  lines = cudaq::split(contents, '\n');
  for (const std::string &l : lines) {
    std::vector<std::string> keyAndValue = cudaq::split(l, ':');
    if (keyAndValue.size() != 2)
      throw std::runtime_error("Ill-formed configuration file (" + path +
                               "). Key-value pairs must be in `<key> : "
                               "<value>` format. (One per line)");
    cudaq::trim(keyAndValue[0]);
    cudaq::trim(keyAndValue[1]);
    if (keyAndValue[0] == "key")
      apiKey = keyAndValue[1];
    else if (keyAndValue[0] == "refresh")
      refreshKey = keyAndValue[1];
    else if (keyAndValue[0] == "time")
      timeStr = keyAndValue[1];
    else
      throw std::runtime_error(
          "Unknown key in configuration file: " + keyAndValue[0] + ".");
  }
  if (apiKey.empty())
    throw std::runtime_error("Empty API key in configuration file (" + path +
                             ").");
  if (refreshKey.empty())
    throw std::runtime_error("Empty refresh key in configuration file (" +
                             path + ").");
  // The `time` key is not required.
}

/// Search for the API key
std::string searchAPIKeyQudora(std::string &key, std::string &refreshKey,
                         std::string &timeStr,
                         std::string userSpecifiedConfig) {
  std::string hwConfig;
  // Allow someone to tweak this with an environment variable
  if (auto creds = std::getenv("CUDAQ_QUDORA_CREDENTIALS")){
    hwConfig = std::string(creds);
    key = hwConfig;
  }
  else if (!userSpecifiedConfig.empty())
    hwConfig = userSpecifiedConfig;
  else {
    hwConfig = std::string(getenv("HOME")) + std::string("/.qudora_config");
    if (cudaq::fileExists(hwConfig)) {
      findApiKeyInFileQudora(key, hwConfig, refreshKey, timeStr);
    } else {
      throw std::runtime_error(
          "Cannot find Qudora Config file with credentials "
          "(~/.qudora_config).");
    }
  }

  return hwConfig;
}

} // namespace cudaq

CUDAQ_REGISTER_TYPE(cudaq::ServerHelper, cudaq::QudoraServerHelper,
                    qudora)
