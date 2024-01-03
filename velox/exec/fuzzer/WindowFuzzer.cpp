/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/exec/fuzzer/WindowFuzzer.h"

#include <boost/random/uniform_int_distribution.hpp>
#include "velox/exec/tests/utils/PlanBuilder.h"

DEFINE_bool(
    enable_window_reference_verification,
    false,
    "When true, the results of the window aggregation are compared to reference DB results");

namespace facebook::velox::exec::test {

namespace {

void logVectors(const std::vector<RowVectorPtr>& vectors) {
  for (auto i = 0; i < vectors.size(); ++i) {
    VLOG(1) << "Input batch " << i << ":";
    for (auto j = 0; j < vectors[i]->size(); ++j) {
      VLOG(1) << "\tRow " << j << ": " << vectors[i]->toString(j);
    }
  }
}

} // namespace

void WindowFuzzer::addWindowFunctionSignatures(
    const WindowFunctionMap& signatureMap) {
  for (const auto& [name, entry] : signatureMap) {
    ++functionsStats.numFunctions;
    bool hasSupportedSignature = false;
    for (auto& signature : entry.signatures) {
      hasSupportedSignature |= addSignature(name, signature);
    }
    if (hasSupportedSignature) {
      ++functionsStats.numSupportedFunctions;
    }
  }
}

void WindowFuzzer::go() {
  VELOX_CHECK(
      FLAGS_steps > 0 || FLAGS_duration_sec > 0,
      "Either --steps or --duration_sec needs to be greater than zero.")

  auto startTime = std::chrono::system_clock::now();
  size_t iteration = 0;

  while (!isDone(iteration, startTime)) {
    LOG(INFO) << "==============================> Started iteration "
              << iteration << " (seed: " << currentSeed_ << ")";

    auto signatureWithStats = pickSignature();
    signatureWithStats.second.numRuns++;

    auto signature = signatureWithStats.first;
    stats_.functionNames.insert(signature.name);

    const bool customVerification =
        customVerificationFunctions_.count(signature.name) != 0;
    const bool requireSortedInput =
        orderDependentFunctions_.count(signature.name) != 0;

    std::vector<TypePtr> argTypes = signature.args;
    std::vector<std::string> argNames = makeNames(argTypes.size());

    auto call = makeFunctionCall(signature.name, argNames, false);

    std::vector<std::string> sortingKeys;
    // 50% chance without order-by clause.
    if (vectorFuzzer_.coinToss(0.5)) {
      sortingKeys = generateSortingKeys("s", argNames, argTypes);
    }
    auto partitionKeys = generateKeys("p", argNames, argTypes);
    auto frameClause = generateFrameClause();
    auto input = generateInputDataWithRowNumber(argNames, argTypes, signature);
    // If the function is order-dependent, sort all input rows by row_number
    // additionally.
    if (requireSortedInput) {
      sortingKeys.push_back("row_number");
      ++stats_.numSortedInputs;
    }

    logVectors(input);

    bool failed = verifyWindow(
        partitionKeys,
        sortingKeys,
        frameClause,
        {call},
        input,
        customVerification,
        FLAGS_enable_window_reference_verification);
    if (failed) {
      signatureWithStats.second.numFailed++;
    }

    LOG(INFO) << "==============================> Done with iteration "
              << iteration;

    if (persistAndRunOnce_) {
      LOG(WARNING)
          << "Iteration succeeded with --persist_and_run_once flag enabled "
             "(expecting crash failure)";
      exit(0);
    }

    reSeed();
    ++iteration;
  }

  stats_.print(iteration);
  printSignatureStats();
}

void WindowFuzzer::go(const std::string& /*planPath*/) {
  // TODO: allow running window fuzzer with saved plans and splits.
  VELOX_NYI();
}

void WindowFuzzer::updateReferenceQueryStats(
    AggregationFuzzerBase::ReferenceQueryErrorCode ec) {
  if (ec == ReferenceQueryErrorCode::kReferenceQueryFail) {
    ++stats_.numReferenceQueryFailed;
  } else if (ec == ReferenceQueryErrorCode::kReferenceQueryUnsupported) {
    ++stats_.numVerificationNotSupported;
  }
}

const std::string WindowFuzzer::generateFrameClause() {
  auto frameType = [](int value) -> const std::string {
    switch (value) {
      case 0:
        return "ROWS";
      case 1:
        return "RANGE";
      default:
        VELOX_UNREACHABLE("Unknown value for frame bound generation");
    }
  };
  auto frameTypeValue =
      boost::random::uniform_int_distribution<uint32_t>(0, 1)(rng_);
  auto frameTypeString = frameType(frameTypeValue);

  auto frameBound = [&](int value, bool start) -> const std::string {
    // Generating only constant bounded k PRECEDING/FOLLOWING frames for now.
    auto kValue =
        boost::random::uniform_int_distribution<uint32_t>(1, 10)(rng_);
    switch (value) {
      case 0:
        return "CURRENT ROW";
      case 1:
        return start ? "UNBOUNDED PRECEDING" : "UNBOUNDED FOLLOWING";
      case 2:
        return fmt::format("{} FOLLOWING", kValue);
      case 3:
        return fmt::format("{} PRECEDING", kValue);
      default:
        VELOX_UNREACHABLE("Unknown option for frame clause generation");
    }
  };

  // Generating k PRECEDING and k FOLLOWING frames only for ROWS type.
  // k RANGE frames require more work as we have to generate columns with the
  // frame bound values.
  auto frameLimit = frameTypeValue == 0 ? 3 : 1;
  auto frameStartValue =
      boost::random::uniform_int_distribution<uint32_t>(0, frameLimit)(rng_);
  auto frameStartBound = frameBound(frameStartValue, true);
  // Frame end has additional limitation that if the frameStart is k FOLLOWING
  // then the frameEnd cannot be k PRECEDING.
  frameLimit = frameTypeValue == 0 ? (frameStartValue == 2 ? 2 : 3) : 1;
  auto frameEndBound = frameBound(
      boost::random::uniform_int_distribution<uint32_t>(0, frameLimit)(rng_),
      false);

  return frameTypeString + " BETWEEN " + frameStartBound + " AND " +
      frameEndBound;
}

bool WindowFuzzer::verifyWindow(
    const std::vector<std::string>& partitionKeys,
    const std::vector<std::string>& sortingKeys,
    const std::string& frameClause,
    const std::vector<std::string>& functionCalls,
    const std::vector<RowVectorPtr>& input,
    bool customVerification,
    bool enableWindowVerification) {
  std::stringstream frame;
  VELOX_CHECK(!partitionKeys.empty());
  frame << "partition by " << folly::join(", ", partitionKeys);
  if (!sortingKeys.empty()) {
    frame << " order by " << folly::join(", ", sortingKeys);
  }
  frame << " " << frameClause;
  auto plan =
      PlanBuilder()
          .values(input)
          .window({fmt::format("{} over ({})", functionCalls[0], frame.str())})
          .planNode();
  if (persistAndRunOnce_) {
    persistReproInfo({{plan, {}}}, reproPersistPath_);
  }
  try {
    auto resultOrError = execute(plan);
    if (resultOrError.exceptionPtr) {
      ++stats_.numFailed;
    }

    if (!customVerification && enableWindowVerification) {
      if (resultOrError.result) {
        auto referenceResult = computeReferenceResults(plan, input);
        updateReferenceQueryStats(referenceResult.second);
        if (auto expectedResult = referenceResult.first) {
          ++stats_.numVerified;
          VELOX_CHECK(
              assertEqualResults(
                  expectedResult.value(),
                  plan->outputType(),
                  {resultOrError.result}),
              "Velox and reference DB results don't match");
          LOG(INFO) << "Verified results against reference DB";
        }
      }
    } else {
      // TODO: support custom verification.
      LOG(INFO) << "Verification skipped";
      ++stats_.numVerificationSkipped;
    }

    return resultOrError.exceptionPtr != nullptr;
  } catch (...) {
    if (!reproPersistPath_.empty()) {
      persistReproInfo({{plan, {}}}, reproPersistPath_);
    }
    throw;
  }
}

void windowFuzzer(
    AggregateFunctionSignatureMap aggregationSignatureMap,
    WindowFunctionMap windowSignatureMap,
    size_t seed,
    const std::unordered_map<std::string, std::shared_ptr<ResultVerifier>>&
        customVerificationFunctions,
    const std::unordered_map<std::string, std::shared_ptr<InputGenerator>>&
        customInputGenerators,
    const std::unordered_set<std::string>& orderDependentFunctions,
    VectorFuzzer::Options::TimestampPrecision timestampPrecision,
    const std::unordered_map<std::string, std::string>& queryConfigs,
    const std::optional<std::string>& planPath,
    std::unique_ptr<ReferenceQueryRunner> referenceQueryRunner) {
  auto windowFuzzer = WindowFuzzer(
      std::move(aggregationSignatureMap),
      std::move(windowSignatureMap),
      seed,
      customVerificationFunctions,
      customInputGenerators,
      orderDependentFunctions,
      timestampPrecision,
      queryConfigs,
      std::move(referenceQueryRunner));
  planPath.has_value() ? windowFuzzer.go(planPath.value()) : windowFuzzer.go();
}

void WindowFuzzer::Stats::print(size_t numIterations) const {
  LOG(INFO) << "Total functions tested: " << functionNames.size();
  LOG(INFO) << "Total functions requiring sorted inputs: "
            << printPercentageStat(numSortedInputs, numIterations);
  LOG(INFO) << "Total functions verified against reference DB: "
            << printPercentageStat(numVerified, numIterations);
  LOG(INFO)
      << "Total functions not verified (verification skipped / not supported by reference DB / reference DB failed): "
      << printPercentageStat(numVerificationSkipped, numIterations) << " / "
      << printPercentageStat(numVerificationNotSupported, numIterations)
      << " / " << printPercentageStat(numReferenceQueryFailed, numIterations);
  LOG(INFO) << "Total failed functions: "
            << printPercentageStat(numFailed, numIterations);
}

} // namespace facebook::velox::exec::test
