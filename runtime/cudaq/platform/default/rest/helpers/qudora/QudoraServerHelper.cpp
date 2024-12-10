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

  /// @brief Given a completed job response, map back to the sample_result
  cudaq::sample_result processResults(ServerMessage &postJobResponse,
                                      std::string &jobID) override;
};

ServerJobPayload
QudoraServerHelper::createJob(std::vector<KernelExecution> &circuitCodes) {
  std::cout << "Create job!\n";

  std::vector<ServerMessage> messages;
  for (auto &circuitCode : circuitCodes) {
    // Construct the job itself
    ServerMessage j;
    j["name"] = circuitCode.name;
    j["language"] = "QIR_BITCODE";
    j["shots"] = {shots,};
    j["target"] = machine;
    j["input_data"] = {circuitCode.code,};
    j["backend_settings"] = nullptr;
    
    messages.push_back(j);

    std::cout << "Circuit: " << circuitCode.code << "\n";
  }

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

std::string QudoraServerHelper::constructGetJobPath(std::string &jobId) {
  return baseUrl + "?job_id=" + jobId + "&include_results=True";
}

bool QudoraServerHelper::jobIsDone(ServerMessage &getJobResponse) {
  std::cout << "Is job done? " << getJobResponse << "\n";

  auto status = getJobResponse[0]["status"].get<std::string>();

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
  std::string results_str = postJobResponse[0]["result"][0].get<std::string>();
  std::cout << "Processing results! " << results_str << "\n";

  nlohmann::json results = nlohmann::json::parse(results_str);

  std::cout << "Processing results! " << results << "\n";

  std::vector<ExecutionResult> srs;

  CountsDictionary reg_counts;
  for (auto &[bitstring, count] : results.items()) {
    std::cout << "Counts: " << bitstring << ", " << count << "\n";
    //auto bitResults = result.get<std::vector<std::string>>();
    
    reg_counts[bitstring] = count;
  }
  srs.emplace_back(reg_counts, "r00000");
  //srs.back().sequentialData = bitResults;

  // // Construct idx[] such that output_names[idx[:]] is sorted by QIR qubit
  // // number. There may initially be duplicate qubit numbers if that qubit was
  // // measured multiple times. If that's true, make the lower-numbered result
  // // occur first. (Dups will be removed in the next step below.)
  // std::vector<std::size_t> idx;
  
  // idx.resize(output_names.size());
  // std::iota(idx.begin(), idx.end(), 0);
  // std::sort(idx.begin(), idx.end(), [&](std::size_t i1, std::size_t i2) {
  //   if (output_names[i1].qubitNum == output_names[i2].qubitNum)
  //     return i1 < i2; // choose lower result number
  //   return output_names[i1].qubitNum < output_names[i2].qubitNum;
  // });

  // // The global register only contains the *final* measurement of each
  // // requested qubit, so eliminate lower-numbered results from idx array.
  // for (auto it = idx.begin(); it != idx.end();) {
  //   if (std::next(it) != idx.end()) {
  //     if (output_names[*it].qubitNum ==
  //         output_names[*std::next(it)].qubitNum) {
  //       it = idx.erase(it);
  //       continue;
  //     }
  //   }
  //   ++it;
  // }


  // // For each shot, we concatenate the measurements results of all qubits.
  // auto begin = results.begin();
  // auto nShots = begin.value().get<std::vector<std::string>>().size();
  // std::vector<std::string> bitstrings(nShots);
  // for (auto r : idx) {
  //   // If allNamesPresent == false, that means we are running local mock server
  //   // tests which don't support the full QIR output recording functions. Just
  //   // use the first key in that case.
  //   auto bitResults =
  //       mockServer ? results.at(begin.key()).get<std::vector<std::string>>()
  //                  : results.at(output_names[r].registerName)
  //                        .get<std::vector<std::string>>();
  //   for (size_t i = 0; auto &bit : bitResults)
  //     bitstrings[i++] += bit;
  // }

  // cudaq::CountsDictionary counts;
  // for (auto &b : bitstrings)
  //   counts[b]++;

  // // Store the combined results into the global register
  // srs.emplace_back(counts, GlobalRegisterName);
  // srs.back().sequentialData = bitstrings;
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
