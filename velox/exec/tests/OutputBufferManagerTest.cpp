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
#include "velox/exec/OutputBufferManager.h"
#include <gtest/gtest.h>
#include "folly/experimental/EventCount.h"
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/dwio/common/tests/utils/BatchMaker.h"
#include "velox/exec/Task.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/serializers/PrestoSerializer.h"

using namespace facebook::velox;
using namespace facebook::velox::exec;
using namespace facebook::velox::core;

using facebook::velox::test::BatchMaker;

class OutputBufferManagerTest : public testing::Test {
 protected:
  OutputBufferManagerTest() {
    std::vector<std::string> names = {"c0", "c1"};
    std::vector<TypePtr> types = {BIGINT(), VARCHAR()};
    rowType_ = ROW(std::move(names), std::move(types));
  }

  static void SetUpTestCase() {
    memory::MemoryManager::testingSetInstance({});
  }

  void SetUp() override {
    pool_ = facebook::velox::memory::memoryManager()->addLeafPool();
    bufferManager_ = OutputBufferManager::getInstance().lock();
    if (!isRegisteredVectorSerde()) {
      facebook::velox::serializer::presto::PrestoVectorSerde::
          registerVectorSerde();
      bufferManager_->setListenerFactory([]() {
        return std::make_unique<
            serializer::presto::PrestoOutputStreamListener>();
      });
    }
  }

  std::shared_ptr<Task> initializeTask(
      const std::string& taskId,
      const RowTypePtr& rowType,
      PartitionedOutputNode::Kind kind,
      int numDestinations,
      int numDrivers,
      int maxOutputBufferSize = 0) {
    bufferManager_->removeTask(taskId);

    auto planFragment = exec::test::PlanBuilder()
                            .values({std::dynamic_pointer_cast<RowVector>(
                                BatchMaker::createBatch(rowType, 100, *pool_))})
                            .planFragment();
    std::unordered_map<std::string, std::string> configSettings;
    if (maxOutputBufferSize != 0) {
      configSettings[core::QueryConfig::kMaxPartitionedOutputBufferSize] =
          std::to_string(maxOutputBufferSize);
    }
    auto queryCtx = std::make_shared<core::QueryCtx>(
        executor_.get(), core::QueryConfig(std::move(configSettings)));

    auto task =
        Task::create(taskId, std::move(planFragment), 0, std::move(queryCtx));

    bufferManager_->initializeTask(task, kind, numDestinations, numDrivers);
    return task;
  }

  std::unique_ptr<SerializedPage> makeSerializedPage(
      RowTypePtr rowType,
      vector_size_t size) {
    auto vector = std::dynamic_pointer_cast<RowVector>(
        BatchMaker::createBatch(rowType, size, *pool_));
    return toSerializedPage(vector);
  }

  std::unique_ptr<SerializedPage> toSerializedPage(VectorPtr vector) {
    auto data = std::make_unique<VectorStreamGroup>(pool_.get());
    auto size = vector->size();
    auto range = IndexRange{0, size};
    data->createStreamTree(
        std::dynamic_pointer_cast<const RowType>(vector->type()), size);
    data->append(
        std::dynamic_pointer_cast<RowVector>(vector), folly::Range(&range, 1));
    auto listener = bufferManager_->newListener();
    IOBufOutputStream stream(*pool_, listener.get(), data->size());
    data->flush(&stream);
    return std::make_unique<SerializedPage>(stream.getIOBuf(), nullptr, size);
  }

  void enqueue(
      const std::string& taskId,
      const RowTypePtr& rowType,
      vector_size_t size,
      bool expectedBlock = false) {
    enqueue(taskId, 0, rowType, size);
  }

  void enqueue(
      const std::string& taskId,
      int destination,
      const RowTypePtr& rowType,
      vector_size_t size,
      bool expectedBlock = false) {
    ContinueFuture future;
    auto blocked = bufferManager_->enqueue(
        taskId, destination, makeSerializedPage(rowType, size), &future);
    if (!expectedBlock) {
      ASSERT_FALSE(blocked);
    }
    if (blocked) {
      future.wait();
    }
  }

  OutputBuffer::Stats getStats(const std::string& taskId) {
    const auto stats = bufferManager_->stats(taskId);
    EXPECT_TRUE(stats.has_value());
    return stats.value();
  }

  void noMoreData(const std::string& taskId) {
    bufferManager_->noMoreData(taskId);
  }

  void fetch(
      const std::string& taskId,
      int destination,
      int64_t sequence,
      uint64_t maxBytes = 1024,
      int expectedGroups = 1,
      bool expectedEndMarker = false) {
    bool receivedData = false;
    ASSERT_TRUE(bufferManager_->getData(
        taskId,
        destination,
        maxBytes,
        sequence,
        [destination,
         sequence,
         expectedGroups,
         expectedEndMarker,
         &receivedData](
            std::vector<std::unique_ptr<folly::IOBuf>> pages,
            int64_t inSequence) {
          ASSERT_FALSE(receivedData) << "for destination " << destination;
          ASSERT_EQ(pages.size(), expectedGroups)
              << "for destination " << destination;
          for (int i = 0; i < pages.size(); ++i) {
            if (i == pages.size() - 1) {
              ASSERT_EQ(expectedEndMarker, pages[i] == nullptr)
                  << "for destination " << destination;
            } else {
              ASSERT_TRUE(pages[i] != nullptr)
                  << "for destination " << destination;
            }
          }
          ASSERT_EQ(inSequence, sequence) << "for destination " << destination;
          receivedData = true;
        }));
    ASSERT_TRUE(receivedData) << "for destination " << destination;
  }

  void fetchOne(
      const std::string& taskId,
      int destination,
      int64_t sequence,
      uint64_t maxBytes = 1024) {
    fetch(taskId, destination, sequence, maxBytes, 1);
  }

  void
  acknowledge(const std::string& taskId, int destination, int64_t sequence) {
    bufferManager_->acknowledge(taskId, destination, sequence);
  }

  void
  fetchOneAndAck(const std::string& taskId, int destination, int64_t sequence) {
    fetchOne(taskId, destination, sequence);
    acknowledge(taskId, destination, sequence + 1);
  }

  DataAvailableCallback
  receiveEndMarker(int destination, int64_t sequence, bool& receivedEndMarker) {
    return [destination, sequence, &receivedEndMarker](
               std::vector<std::unique_ptr<folly::IOBuf>> pages,
               int64_t inSequence) {
      EXPECT_FALSE(receivedEndMarker) << "for destination " << destination;
      EXPECT_EQ(pages.size(), 1) << "for destination " << destination;
      EXPECT_TRUE(pages[0] == nullptr) << "for destination " << destination;
      EXPECT_EQ(inSequence, sequence) << "for destination " << destination;
      receivedEndMarker = true;
    };
  }

  void
  fetchEndMarker(const std::string& taskId, int destination, int64_t sequence) {
    bool receivedData = false;
    ASSERT_TRUE(bufferManager_->getData(
        taskId,
        destination,
        std::numeric_limits<uint64_t>::max(),
        sequence,
        receiveEndMarker(destination, sequence, receivedData)));
    EXPECT_TRUE(receivedData) << "for destination " << destination;
    bufferManager_->deleteResults(taskId, destination);
  }

  void deleteResults(const std::string& taskId, int destination) {
    bufferManager_->deleteResults(taskId, destination);
  }

  void registerForEndMarker(
      const std::string& taskId,
      int destination,
      int64_t sequence,
      bool& receivedEndMarker) {
    receivedEndMarker = false;
    ASSERT_TRUE(bufferManager_->getData(
        taskId,
        destination,
        std::numeric_limits<uint64_t>::max(),
        sequence,
        receiveEndMarker(destination, sequence, receivedEndMarker)));
    EXPECT_FALSE(receivedEndMarker) << "for destination " << destination;
  }

  DataAvailableCallback receiveData(
      int destination,
      int64_t sequence,
      int expectedGroups,
      bool& receivedData) {
    receivedData = false;
    return [destination, sequence, expectedGroups, &receivedData](
               std::vector<std::unique_ptr<folly::IOBuf>> pages,
               int64_t inSequence) {
      EXPECT_FALSE(receivedData) << "for destination " << destination;
      EXPECT_EQ(pages.size(), expectedGroups)
          << "for destination " << destination;
      for (int i = 0; i < expectedGroups; i++) {
        EXPECT_TRUE(pages[i] != nullptr) << "for destination " << destination;
      }
      EXPECT_EQ(inSequence, sequence) << "for destination " << destination;
      receivedData = true;
    };
  }

  void registerForData(
      const std::string& taskId,
      int destination,
      int64_t sequence,
      int expectedGroups,
      bool& receivedData) {
    receivedData = false;
    ASSERT_TRUE(bufferManager_->getData(
        taskId,
        destination,
        1024,
        sequence,
        receiveData(destination, sequence, expectedGroups, receivedData)));
    EXPECT_FALSE(receivedData) << "for destination " << destination;
  }

  void dataFetcher(
      const std::string& taskId,
      int destination,
      int64_t& fetchedPages,
      bool earlyTermination) {
    folly::Random::DefaultGenerator rng;
    rng.seed(destination);
    int64_t nextSequence{0};
    while (true) {
      if (earlyTermination && folly::Random().oneIn(200)) {
        bufferManager_->deleteResults(taskId, destination);
        return;
      }
      const int64_t maxBytes = folly::Random().oneIn(4, rng) ? 32'000'000 : 1;
      int64_t receivedSequence;
      bool atEnd{false};
      folly::EventCount dataWait;
      auto dataWaitKey = dataWait.prepareWait();
      bufferManager_->getData(
          taskId,
          destination,
          maxBytes,
          nextSequence,
          [&](std::vector<std::unique_ptr<folly::IOBuf>> pages,
              int64_t inSequence) {
            ASSERT_EQ(inSequence, nextSequence);
            for (int i = 0; i < pages.size(); ++i) {
              if (pages[i] != nullptr) {
                ++nextSequence;
              } else {
                ASSERT_EQ(i, pages.size() - 1);
                atEnd = true;
              }
            }
            dataWait.notify();
          });
      dataWait.wait(dataWaitKey);
      if (atEnd) {
        break;
      }
    }
    bufferManager_->deleteResults(taskId, destination);
    fetchedPages = nextSequence;
  }

  // Define the states of output buffer for testing purpose.
  enum class OutputBufferStatus {
    kInitiated,
    kRunning,
    kBlocked,
    kFinished,
    kNoMoreProducer
  };

  void verifyOutputBuffer(
      std::shared_ptr<Task> task,
      OutputBufferStatus outputBufferStatus) {
    TaskStats finishStats = task->taskStats();
    const auto utilization = finishStats.outputBufferUtilization;
    const auto overutilized = finishStats.outputBufferOverutilized;
    if (outputBufferStatus == OutputBufferStatus::kInitiated ||
        outputBufferStatus == OutputBufferStatus::kFinished) {
      // zero utilization on a fresh new output buffer
      ASSERT_EQ(utilization, 0);
      ASSERT_FALSE(overutilized);
    }
    if (outputBufferStatus == OutputBufferStatus::kRunning) {
      // non-blocking running, 0 < utilization < 1
      ASSERT_GT(utilization, 0);
      ASSERT_LT(utilization, 1);
      if (utilization > 0.5) {
        ASSERT_TRUE(overutilized);
      } else {
        ASSERT_FALSE(overutilized);
      }
    }
    if (outputBufferStatus == OutputBufferStatus::kBlocked) {
      // output buffer is over utilized and blocked
      ASSERT_GT(utilization, 0.5);
      ASSERT_TRUE(overutilized);
    }
    if (outputBufferStatus == OutputBufferStatus::kNoMoreProducer) {
      // output buffer is over utilized if no more producer is set.
      ASSERT_TRUE(overutilized);
    }
  }

  std::shared_ptr<folly::Executor> executor_{
      std::make_shared<folly::CPUThreadPoolExecutor>(
          std::thread::hardware_concurrency())};
  std::shared_ptr<facebook::velox::memory::MemoryPool> pool_;
  std::shared_ptr<OutputBufferManager> bufferManager_;
  RowTypePtr rowType_;
};

struct TestParam {
  PartitionedOutputNode::Kind kind;
};

class AllOutputBufferManagerTest
    : public OutputBufferManagerTest,
      public testing::WithParamInterface<PartitionedOutputNode::Kind> {
 public:
  AllOutputBufferManagerTest() : kind_(GetParam()) {}

  static std::vector<PartitionedOutputNode::Kind> getTestParams() {
    static std::vector<PartitionedOutputNode::Kind> params = {
        PartitionedOutputNode::Kind::kBroadcast,
        PartitionedOutputNode::Kind::kPartitioned,
        PartitionedOutputNode::Kind::kArbitrary};
    return params;
  }

 protected:
  PartitionedOutputNode::Kind kind_;
};

TEST_F(OutputBufferManagerTest, arbitrayBuffer) {
  {
    ArbitraryBuffer buffer;
    ASSERT_TRUE(buffer.empty());
    ASSERT_FALSE(buffer.hasNoMoreData());
    ASSERT_EQ(
        buffer.toString(), "[ARBITRARY_BUFFER PAGES[0] NO MORE DATA[false]]");
    VELOX_ASSERT_THROW(buffer.enqueue(nullptr), "Unexpected null page");
    ASSERT_EQ(
        buffer.toString(), "[ARBITRARY_BUFFER PAGES[0] NO MORE DATA[false]]");
    buffer.noMoreData();
    ASSERT_EQ(
        buffer.toString(), "[ARBITRARY_BUFFER PAGES[0] NO MORE DATA[true]]");
  }

  {
    ArbitraryBuffer buffer;
    ASSERT_TRUE(buffer.empty());
    ASSERT_FALSE(buffer.hasNoMoreData());
    auto page1 = makeSerializedPage(rowType_, 100);
    auto* rawPage1 = page1.get();
    buffer.enqueue(std::move(page1));
    ASSERT_FALSE(buffer.empty());
    ASSERT_FALSE(buffer.hasNoMoreData());
    ASSERT_EQ(
        buffer.toString(), "[ARBITRARY_BUFFER PAGES[1] NO MORE DATA[false]]");
    auto page2 = makeSerializedPage(rowType_, 100);
    auto* rawPage2 = page2.get();
    buffer.enqueue(std::move(page2));
    ASSERT_FALSE(buffer.empty());
    ASSERT_FALSE(buffer.hasNoMoreData());
    ASSERT_EQ(
        buffer.toString(), "[ARBITRARY_BUFFER PAGES[2] NO MORE DATA[false]]");

    auto pages = buffer.getPages(1);
    ASSERT_EQ(pages.size(), 1);
    ASSERT_EQ(pages[0].get(), rawPage1);
    auto page3 = makeSerializedPage(rowType_, 100);
    auto* rawPage3 = page3.get();
    buffer.enqueue(std::move(page3));
    ASSERT_EQ(
        buffer.toString(), "[ARBITRARY_BUFFER PAGES[2] NO MORE DATA[false]]");
    buffer.noMoreData();
    ASSERT_FALSE(buffer.empty());
    ASSERT_TRUE(buffer.hasNoMoreData());
    ASSERT_EQ(
        buffer.toString(), "[ARBITRARY_BUFFER PAGES[2] NO MORE DATA[true]]");
    pages = buffer.getPages(1'000'000'000);
    ASSERT_EQ(pages.size(), 3);
    ASSERT_EQ(pages[0].get(), rawPage2);
    ASSERT_EQ(pages[1].get(), rawPage3);
    ASSERT_EQ(pages[2].get(), nullptr);
    ASSERT_TRUE(buffer.empty());
    ASSERT_TRUE(buffer.hasNoMoreData());
    ASSERT_EQ(
        buffer.toString(), "[ARBITRARY_BUFFER PAGES[0] NO MORE DATA[true]]");
    auto page4 = makeSerializedPage(rowType_, 100);
    VELOX_ASSERT_THROW(
        buffer.enqueue(std::move(page4)),
        "Arbitrary buffer has set no more data marker");
    ASSERT_EQ(
        buffer.toString(), "[ARBITRARY_BUFFER PAGES[0] NO MORE DATA[true]]");
    buffer.noMoreData();
    VELOX_ASSERT_THROW(buffer.getPages(0), "maxBytes can't be zero");
    // Verify the end marker is persistent.
    for (int i = 0; i < 3; ++i) {
      pages = buffer.getPages(100);
      ASSERT_EQ(pages.size(), 1);
      ASSERT_EQ(pages[0].get(), nullptr);
    }
  }
}

TEST_F(OutputBufferManagerTest, outputType) {
  ASSERT_EQ(
      PartitionedOutputNode::kindString(
          PartitionedOutputNode::Kind::kPartitioned),
      "PARTITIONED");
  ASSERT_EQ(
      PartitionedOutputNode::kindString(
          PartitionedOutputNode::Kind::kArbitrary),
      "ARBITRARY");
  ASSERT_EQ(
      PartitionedOutputNode::kindString(
          PartitionedOutputNode::Kind::kBroadcast),
      "BROADCAST");
  ASSERT_EQ(
      PartitionedOutputNode::kindString(
          static_cast<PartitionedOutputNode::Kind>(100)),
      "INVALID OUTPUT KIND 100");
}

TEST_F(OutputBufferManagerTest, destinationBuffer) {
  {
    ArbitraryBuffer buffer;
    DestinationBuffer destinationBuffer;
    VELOX_ASSERT_THROW(
        destinationBuffer.loadData(&buffer, 0), "maxBytes can't be zero");
    destinationBuffer.loadData(&buffer, 100);
    std::atomic<bool> notified{false};
    destinationBuffer.getData(
        1'000'000,
        0,
        [&](std::vector<std::unique_ptr<folly::IOBuf>> buffers,
            int64_t sequence) {
          ASSERT_EQ(buffers.size(), 1);
          ASSERT_TRUE(buffers[0].get() == nullptr);
          notified = true;
        });
    ASSERT_TRUE(buffer.empty());
    ASSERT_FALSE(buffer.hasNoMoreData());
    ASSERT_FALSE(notified);
    VELOX_ASSERT_THROW(destinationBuffer.maybeLoadData(&buffer), "");
    buffer.noMoreData();
    destinationBuffer.maybeLoadData(&buffer);
    destinationBuffer.getAndClearNotify().notify();
    ASSERT_TRUE(notified);
  }

  {
    ArbitraryBuffer buffer;
    int64_t expectedNumBytes{0};
    for (int i = 0; i < 10; ++i) {
      auto page = makeSerializedPage(rowType_, 100);
      expectedNumBytes += page->size();
      buffer.enqueue(std::move(page));
    }
    DestinationBuffer destinationBuffer;
    destinationBuffer.loadData(&buffer, 1);
    ASSERT_EQ(
        buffer.toString(), "[ARBITRARY_BUFFER PAGES[9] NO MORE DATA[false]]");
    std::atomic<bool> notified{false};
    int64_t numBytes{0};
    auto buffers = destinationBuffer.getData(
        1'000'000'000,
        0,
        [&](std::vector<std::unique_ptr<folly::IOBuf>> /*unused*/,
            int64_t /*unused*/) { notified = true; });
    for (const auto& buffer : buffers) {
      numBytes += buffer->length();
    }
    ASSERT_GT(numBytes, 0);
    ASSERT_FALSE(notified);
    {
      auto result = destinationBuffer.getAndClearNotify();
      ASSERT_EQ(result.callback, nullptr);
      ASSERT_TRUE(result.data.empty());
      ASSERT_EQ(result.sequence, 0);
    }
    auto pages = destinationBuffer.acknowledge(1, false);
    ASSERT_EQ(pages.size(), 1);

    buffers = destinationBuffer.getData(
        1'000'000,
        1,
        [&](std::vector<std::unique_ptr<folly::IOBuf>> buffers,
            int64_t sequence) {
          ASSERT_EQ(sequence, 1);
          ASSERT_EQ(buffers.size(), 9);
          for (const auto& buffer : buffers) {
            numBytes += buffer->length();
          }
          notified = true;
        });
    ASSERT_TRUE(buffers.empty());
    ASSERT_FALSE(notified);

    destinationBuffer.maybeLoadData(&buffer);
    destinationBuffer.getAndClearNotify().notify();
    ASSERT_TRUE(notified);
    ASSERT_TRUE(buffer.empty());
    ASSERT_FALSE(buffer.hasNoMoreData());
    ASSERT_EQ(numBytes, expectedNumBytes);
  }
}

TEST_F(OutputBufferManagerTest, basicPartitioned) {
  vector_size_t size = 100;
  std::string taskId = "t0";
  auto task = initializeTask(
      taskId, rowType_, PartitionedOutputNode::Kind::kPartitioned, 5, 1);
  verifyOutputBuffer(task, OutputBufferStatus::kInitiated);

  // Duplicateb update buffers with the same settings are allowed and ignored.
  ASSERT_TRUE(bufferManager_->updateOutputBuffers(taskId, 5, true));
  ASSERT_FALSE(bufferManager_->isFinished(taskId));
  // Partitioned output buffer doesn't allow to update with different number of
  // output buffers once created.
  VELOX_ASSERT_THROW(
      bufferManager_->updateOutputBuffers(taskId, 5 + 1, true), "");
  // Partitioned output buffer doesn't expect more output buffers once created.
  VELOX_ASSERT_THROW(bufferManager_->updateOutputBuffers(taskId, 5, false), "");
  VELOX_ASSERT_THROW(
      bufferManager_->updateOutputBuffers(taskId, 5 - 1, true), "");
  VELOX_ASSERT_THROW(
      bufferManager_->updateOutputBuffers(taskId, 5 - 1, false), "");

  // - enqueue one group per destination
  // - fetch and ask one group per destination
  // - enqueue one more group for destinations 0-2
  // - fetch and ask one group for destinations 0-2
  // - register end-marker callback for destination 3 and data callback for 4
  // - enqueue data for 4 and assert callback was called
  // - enqueue end marker for all destinations
  // - assert callback was called for destination 3
  // - fetch end markers for destinations 0-2 and 4

  for (int destination = 0; destination < 5; ++destination) {
    enqueue(taskId, destination, rowType_, size);
  }
  EXPECT_FALSE(bufferManager_->isFinished(taskId));

  for (int destination = 0; destination < 5; destination++) {
    fetchOneAndAck(taskId, destination, 0);
    // Try to re-fetch already ack-ed data - should fail
    EXPECT_THROW(fetchOne(taskId, destination, 0), std::exception);
  }
  EXPECT_FALSE(bufferManager_->isFinished(taskId));

  for (int destination = 0; destination < 3; destination++) {
    enqueue(taskId, destination, rowType_, size);
  }

  for (int destination = 0; destination < 3; destination++) {
    fetchOneAndAck(taskId, destination, 1);
  }

  // destinations 0-2 received and ack-ed 2 groups each
  // destinations 3-4 received and ack-ed 1 group each

  bool receivedEndMarker3;
  registerForEndMarker(taskId, 3, 1, receivedEndMarker3);

  bool receivedData4;
  registerForData(taskId, 4, 1, 1, receivedData4);

  enqueue(taskId, 4, rowType_, size);
  EXPECT_TRUE(receivedData4);

  noMoreData(taskId);
  EXPECT_TRUE(receivedEndMarker3);
  EXPECT_FALSE(bufferManager_->isFinished(taskId));

  for (int destination = 0; destination < 3; destination++) {
    fetchEndMarker(taskId, destination, 2);
  }
  EXPECT_TRUE(task->isRunning());
  verifyOutputBuffer(task, OutputBufferStatus::kNoMoreProducer);

  deleteResults(taskId, 3);
  fetchEndMarker(taskId, 4, 2);
  bufferManager_->removeTask(taskId);
  EXPECT_TRUE(task->isFinished());
}

TEST_F(OutputBufferManagerTest, basicBroadcast) {
  vector_size_t size = 100;

  std::string taskId = "t0";
  auto task = initializeTask(
      taskId, rowType_, PartitionedOutputNode::Kind::kBroadcast, 5, 1);
  verifyOutputBuffer(task, OutputBufferStatus::kInitiated);
  VELOX_ASSERT_THROW(enqueue(taskId, 1, rowType_, size), "Bad destination 1");

  enqueue(taskId, rowType_, size);
  EXPECT_FALSE(bufferManager_->isFinished(taskId));

  for (int destination = 0; destination < 5; destination++) {
    fetchOneAndAck(taskId, destination, 0);
    // Try to re-fetch already ack-ed data - should fail
    EXPECT_THROW(fetchOne(taskId, destination, 0), std::exception);
  }
  EXPECT_FALSE(bufferManager_->isFinished(taskId));

  enqueue(taskId, rowType_, size);

  for (int destination = 0; destination < 5; destination++) {
    fetchOneAndAck(taskId, destination, 1);
  }

  bool receivedEndMarker3;
  registerForEndMarker(taskId, 3, 2, receivedEndMarker3);

  noMoreData(taskId);
  EXPECT_TRUE(receivedEndMarker3);
  EXPECT_FALSE(bufferManager_->isFinished(taskId));

  for (int destination = 0; destination < 5; ++destination) {
    if (destination != 3) {
      fetchEndMarker(taskId, destination, 2);
    }
  }
  EXPECT_FALSE(bufferManager_->isFinished(taskId));
  bufferManager_->updateOutputBuffers(taskId, 5, false);
  EXPECT_FALSE(bufferManager_->isFinished(taskId));
  bufferManager_->updateOutputBuffers(taskId, 6, false);
  verifyOutputBuffer(task, OutputBufferStatus::kNoMoreProducer);

  // Fetch all for the new added destinations.
  fetch(taskId, 5, 0, 1'000'000'000, 3, true);
  VELOX_ASSERT_THROW(
      acknowledge(taskId, 5, 4),
      "(4 vs. 3) Ack received for a not yet produced item");
  acknowledge(taskId, 5, 0);
  acknowledge(taskId, 5, 1);
  acknowledge(taskId, 5, 2);
  acknowledge(taskId, 5, 3);
  EXPECT_FALSE(bufferManager_->isFinished(taskId));
  deleteResults(taskId, 5);
  VELOX_ASSERT_THROW(
      fetch(taskId, 5, 0, 1'000'000'000, 2),
      "getData received after its buffer is deleted. Destination: 5, sequence: 0");

  bufferManager_->updateOutputBuffers(taskId, 7, true);
  EXPECT_FALSE(bufferManager_->isFinished(taskId));
  fetch(taskId, 6, 0, 1'000'000'000, 3, true);
  acknowledge(taskId, 6, 2);
  EXPECT_FALSE(bufferManager_->isFinished(taskId));
  deleteResults(taskId, 6);
  EXPECT_FALSE(bufferManager_->isFinished(taskId));

  EXPECT_TRUE(task->isRunning());
  deleteResults(taskId, 3);
  EXPECT_TRUE(bufferManager_->isFinished(taskId));
  bufferManager_->removeTask(taskId);
  EXPECT_TRUE(task->isFinished());
}

TEST_F(OutputBufferManagerTest, basicArbitrary) {
  const vector_size_t size = 100;
  int numDestinations = 5;
  const std::string taskId = "t0";
  auto task = initializeTask(
      taskId, rowType_, PartitionedOutputNode::Kind::kArbitrary, 5, 1);
  verifyOutputBuffer(task, OutputBufferStatus::kInitiated);

  VELOX_ASSERT_THROW(
      enqueue(taskId, 1, rowType_, size), "(1 vs. 0) Bad destination 1");

  for (int i = 0; i < numDestinations; ++i) {
    enqueue(taskId, rowType_, size);
  }
  EXPECT_FALSE(bufferManager_->isFinished(taskId));

  std::unordered_map<int, int> ackedSeqbyDestination;

  for (int destination = 0; destination < numDestinations; destination++) {
    fetchOne(taskId, destination, 0, 1);
    acknowledge(taskId, destination, 1);
    ackedSeqbyDestination[destination] = 1;
    // Try to re-fetch already ack-ed data - should fail
    VELOX_ASSERT_THROW(fetchOne(taskId, destination, 0), "");
  }
  EXPECT_FALSE(bufferManager_->isFinished(taskId));
  verifyOutputBuffer(task, OutputBufferStatus::kRunning);

  bool receivedData0{false};
  bool receivedData1{false};
  registerForData(taskId, 0, 1, 1, receivedData0);
  registerForData(taskId, 1, 1, 1, receivedData1);
  enqueue(taskId, rowType_, size);
  enqueue(taskId, rowType_, size);
  ASSERT_TRUE(receivedData0);
  ASSERT_TRUE(receivedData1);
  acknowledge(taskId, 0, 2);
  ackedSeqbyDestination[0] = 2;
  acknowledge(taskId, 1, 2);
  ackedSeqbyDestination[1] = 2;
  bool receivedData{false};
  registerForData(taskId, 4, 1, 1, receivedData);
  enqueue(taskId, rowType_, size);
  enqueue(taskId, rowType_, size);
  ASSERT_TRUE(receivedData);
  acknowledge(taskId, 4, 2);
  ackedSeqbyDestination[4] = 2;

  bufferManager_->updateOutputBuffers(taskId, ++numDestinations, false);
  bufferManager_->updateOutputBuffers(taskId, ++numDestinations, false);
  fetchOneAndAck(taskId, numDestinations - 1, 0);
  ackedSeqbyDestination[numDestinations - 1] = 1;

  bufferManager_->updateOutputBuffers(taskId, numDestinations - 1, false);
  VELOX_ASSERT_THROW(
      fetchOneAndAck(taskId, numDestinations - 1, 0),
      "(0 vs. 1) Get received for an already acknowledged item");

  receivedData = false;
  registerForData(taskId, numDestinations - 2, 0, 1, receivedData);
  enqueue(taskId, rowType_, size);
  ASSERT_TRUE(receivedData);
  acknowledge(taskId, numDestinations - 2, 1);
  ackedSeqbyDestination[numDestinations - 2] = 1;

  noMoreData(taskId);
  EXPECT_FALSE(bufferManager_->isFinished(taskId));
  EXPECT_TRUE(task->isRunning());
  for (int i = 0; i < numDestinations; ++i) {
    fetchEndMarker(taskId, i, ackedSeqbyDestination[i]);
  }
  EXPECT_TRUE(bufferManager_->isFinished(taskId));
  EXPECT_FALSE(task->isRunning());

  // NOTE: arbitrary buffer finish condition doesn't depend on no more
  // (destination )buffers update flag.
  bufferManager_->updateOutputBuffers(taskId, numDestinations, true);

  EXPECT_TRUE(bufferManager_->isFinished(taskId));
  bufferManager_->removeTask(taskId);
  EXPECT_TRUE(task->isFinished());
}

TEST_F(OutputBufferManagerTest, broadcastWithDynamicAddedDestination) {
  vector_size_t size = 100;

  std::string taskId = "t0";
  auto task = initializeTask(
      taskId, rowType_, PartitionedOutputNode::Kind::kBroadcast, 5, 1);

  const int numPages = 10;
  for (int i = 0; i < numPages; ++i) {
    enqueue(taskId, rowType_, size);
  }
  EXPECT_FALSE(bufferManager_->isFinished(taskId));

  for (int i = 0; i < numPages; ++i) {
    for (int destination = 0; destination < 5; ++destination) {
      fetchOneAndAck(taskId, destination, i);
    }
  }

  // Dynamic added destination.
  fetch(taskId, 5, 0, 1'000'000'000, 10);
  bufferManager_->updateOutputBuffers(taskId, 4, false);
  bufferManager_->updateOutputBuffers(taskId, 6, false);
  bufferManager_->updateOutputBuffers(taskId, 7, false);

  fetch(taskId, 6, 0, 1'000'000'000, 10);
  fetch(taskId, 7, 0, 1'000'000'000, 10);
  fetch(taskId, 8, 0, 1'000'000'000, 10);

  bufferManager_->updateOutputBuffers(taskId, 7, true);

  VELOX_ASSERT_THROW(fetch(taskId, 10, 0, 1'000'000'000, 10), "");

  ASSERT_FALSE(bufferManager_->isFinished(taskId));
  noMoreData(taskId);
  for (int i = 0; i < 9; ++i) {
    fetchEndMarker(taskId, i, numPages);
  }
  ASSERT_TRUE(bufferManager_->isFinished(taskId));

  bufferManager_->removeTask(taskId);
  EXPECT_TRUE(task->isFinished());
}

TEST_F(OutputBufferManagerTest, arbitraryWithDynamicAddedDestination) {
  const vector_size_t size = 100;
  int numDestinations = 5;
  const std::string taskId = "t0";
  auto task = initializeTask(
      taskId, rowType_, PartitionedOutputNode::Kind::kArbitrary, 5, 1);

  for (int i = 0; i < numDestinations; ++i) {
    enqueue(taskId, rowType_, size);
  }
  std::unordered_map<int, int> ackedSeqbyDestination;

  for (int destination = 0; destination < 5; destination++) {
    fetchOne(taskId, destination, 0, 1);
    acknowledge(taskId, destination, 1);
    ackedSeqbyDestination[destination] = 1;
  }
  EXPECT_FALSE(bufferManager_->isFinished(taskId));

  // Dynamic added destination.
  enqueue(taskId, rowType_, size);
  fetchOneAndAck(taskId, numDestinations, 0);
  ackedSeqbyDestination[numDestinations] = 1;

  bufferManager_->updateOutputBuffers(taskId, numDestinations - 1, false);
  bufferManager_->updateOutputBuffers(taskId, numDestinations, false);
  bufferManager_->updateOutputBuffers(taskId, numDestinations + 1, false);

  enqueue(taskId, rowType_, size);
  fetchOneAndAck(taskId, numDestinations + 10, 0);
  ackedSeqbyDestination[numDestinations + 10] = 1;

  bufferManager_->updateOutputBuffers(taskId, numDestinations, true);

  enqueue(taskId, rowType_, size);
  fetchOneAndAck(taskId, numDestinations + 9, 0);
  ackedSeqbyDestination[numDestinations + 9] = 1;

  VELOX_ASSERT_THROW(
      fetch(taskId, numDestinations + 20, 0, 1'000'000'000, 1), "");

  ASSERT_FALSE(bufferManager_->isFinished(taskId));

  noMoreData(taskId);
  EXPECT_TRUE(task->isRunning());
  for (int i = 0; i <= numDestinations + 10; ++i) {
    fetchEndMarker(taskId, i, ackedSeqbyDestination[i]);
  }
  EXPECT_TRUE(bufferManager_->isFinished(taskId));

  EXPECT_FALSE(task->isRunning());
  EXPECT_TRUE(bufferManager_->isFinished(taskId));
  bufferManager_->removeTask(taskId);
  EXPECT_TRUE(task->isFinished());
}

TEST_P(AllOutputBufferManagerTest, maxBytes) {
  const vector_size_t size = 100;
  const std::string taskId = "t0";
  initializeTask(taskId, rowType_, kind_, 1, 1);

  enqueue(taskId, 0, rowType_, size);
  enqueue(taskId, 0, rowType_, size);
  enqueue(taskId, 0, rowType_, size);

  // fetch up to 1Kb - 1 group
  fetchOne(taskId, 0, 0);
  // re-fetch with larger size limit - 2 groups
  fetch(taskId, 0, 0, std::numeric_limits<int64_t>::max(), 3);
  // re-fetch with 1Kb limit - 1 group
  fetchOneAndAck(taskId, 0, 0);
  fetchOneAndAck(taskId, 0, 1);
  fetchOneAndAck(taskId, 0, 2);

  if (kind_ != PartitionedOutputNode::Kind::kPartitioned) {
    bufferManager_->updateOutputBuffers(taskId, 0, true);
  }
  noMoreData(taskId);
  fetchEndMarker(taskId, 0, 3);
  bufferManager_->removeTask(taskId);
}

TEST_P(AllOutputBufferManagerTest, outputBufferUtilization) {
  const std::string taskId = std::to_string(rand());
  const auto destination = 0;
  auto task = initializeTask(taskId, rowType_, kind_, 1, 1);
  verifyOutputBuffer(task, OutputBufferStatus::kInitiated);
  if (kind_ == facebook::velox::core::PartitionedOutputNode::Kind::kBroadcast) {
    bufferManager_->updateOutputBuffers(taskId, destination, true);
  }

  bool blocked = false;
  do {
    ContinueFuture future;
    blocked = bufferManager_->enqueue(
        taskId, destination, makeSerializedPage(rowType_, 100), &future);
    if (!blocked) {
      verifyOutputBuffer(task, OutputBufferStatus::kRunning);
    }
  } while (!blocked);

  verifyOutputBuffer(task, OutputBufferStatus::kBlocked);

  int group = 0;
  do {
    fetchOneAndAck(taskId, destination, group);
    group++;
  } while (task->taskStats().outputBufferOverutilized);
  verifyOutputBuffer(task, OutputBufferStatus::kRunning);

  ContinueFuture future;
  bufferManager_->enqueue(
      taskId, destination, makeSerializedPage(rowType_, 200), &future);
  verifyOutputBuffer(task, OutputBufferStatus::kBlocked);
  auto oldUtilization = task->taskStats().outputBufferUtilization;

  noMoreData(taskId);
  verifyOutputBuffer(task, OutputBufferStatus::kNoMoreProducer);
  ASSERT_EQ(oldUtilization, task->taskStats().outputBufferUtilization);

  deleteResults(taskId, destination);
  bufferManager_->removeTask(taskId);
  verifyOutputBuffer(task, OutputBufferStatus::kFinished);
}

TEST_P(AllOutputBufferManagerTest, outputBufferStats) {
  const vector_size_t size = 100;
  const std::string taskId = std::to_string(folly::Random::rand32());
  initializeTask(taskId, rowType_, kind_, 1, 1);
  // Check the stats kind.
  ASSERT_EQ(getStats(taskId).kind, kind_);

  const int pageNum = 3;
  int totalSize = 0;
  for (int pageId = 0; pageId < pageNum; pageId++) {
    // Enqueue pages and check stats of buffered data.
    enqueue(taskId, 0, rowType_, size);
    totalSize += size;
    // Force ArbitraryBuffer to load data, otherwise the data would
    // not be buffered in DestinationBuffer.
    if (kind_ == PartitionedOutputNode::Kind::kArbitrary) {
      fetchOne(taskId, 0, pageId);
    }
    auto statsEnqueue = getStats(taskId);
    ASSERT_EQ(statsEnqueue.buffersStats[0].pagesBuffered, 1);
    ASSERT_EQ(statsEnqueue.buffersStats[0].rowsBuffered, size);

    // Ack pages and check stats of sent data.
    fetchOneAndAck(taskId, 0, pageId);
    auto statsAck = getStats(taskId);
    ASSERT_EQ(statsAck.buffersStats[0].pagesSent, pageId + 1);
    ASSERT_EQ(statsAck.buffersStats[0].rowsSent, totalSize);
    ASSERT_EQ(statsAck.buffersStats[0].pagesBuffered, 0);
    ASSERT_EQ(statsAck.buffersStats[0].rowsBuffered, 0);
  }

  // Set outputBuffer to NoMoreBuffers and check stats.
  bufferManager_->updateOutputBuffers(taskId, 1, true);
  auto statsNoMoreBuffers = getStats(taskId);
  ASSERT_TRUE(statsNoMoreBuffers.noMoreBuffers);
  ASSERT_FALSE(statsNoMoreBuffers.noMoreData);

  // Set outputBuffer to noMoreData and check stats.
  noMoreData(taskId);
  auto statsNoMoreData = getStats(taskId);
  ASSERT_TRUE(statsNoMoreData.noMoreData);

  // DeleteResults and check stats.
  fetchEndMarker(taskId, 0, pageNum);
  deleteResults(taskId, 0);
  auto statsDeleteResults = getStats(taskId);
  ASSERT_TRUE(statsDeleteResults.buffersStats[0].finished);
  ASSERT_TRUE(statsDeleteResults.finished);

  // Remove task and check stats.
  bufferManager_->removeTask(taskId);
  ASSERT_FALSE(bufferManager_->stats(taskId).has_value());
}

TEST_F(OutputBufferManagerTest, outOfOrderAcks) {
  const vector_size_t size = 100;
  const std::string taskId = "t0";
  auto task = initializeTask(
      taskId, rowType_, PartitionedOutputNode::Kind::kPartitioned, 5, 1);

  enqueue(taskId, 0, rowType_, size);
  for (int i = 0; i < 10; i++) {
    enqueue(taskId, 1, rowType_, size);
  }

  // fetch first 3 groups
  fetchOne(taskId, 1, 0);
  fetchOne(taskId, 1, 1);
  fetchOne(taskId, 1, 2);

  // send acks out of order: 3, 1, 2
  acknowledge(taskId, 1, 3);
  acknowledge(taskId, 1, 1);
  acknowledge(taskId, 1, 2);

  // fetch and ack remaining 7 groups
  fetch(taskId, 1, 3, std::numeric_limits<uint64_t>::max(), 7);
  acknowledge(taskId, 1, 10);

  noMoreData(taskId);
  fetchEndMarker(taskId, 1, 10);
  task->requestCancel();
  bufferManager_->removeTask(taskId);
}

TEST_F(OutputBufferManagerTest, errorInQueue) {
  auto queue = std::make_shared<ExchangeQueue>();
  queue->setError("Forced failure");

  std::lock_guard<std::mutex> l(queue->mutex());
  ContinueFuture future;
  bool atEnd = false;
  VELOX_ASSERT_THROW(
      queue->dequeueLocked(1, &atEnd, &future), "Forced failure");
}

TEST_F(OutputBufferManagerTest, setQueueErrorWithPendingPages) {
  const uint64_t kBufferSize = 128;
  auto iobuf = folly::IOBuf::create(kBufferSize);
  const std::string payload("setQueueErrorWithPendingPages");
  size_t payloadSize = payload.size();
  std::memcpy(iobuf->writableData(), payload.data(), payloadSize);
  iobuf->append(payloadSize);

  auto page = std::make_unique<SerializedPage>(std::move(iobuf));

  auto queue = std::make_shared<ExchangeQueue>();
  std::vector<ContinuePromise> promises;
  {
    std::lock_guard<std::mutex> l(queue->mutex());
    queue->enqueueLocked(std::move(page), promises);
  }

  queue->setError("Forced failure");

  // Expect a throw on dequeue after the queue has been set error.
  std::lock_guard<std::mutex> l(queue->mutex());
  ContinueFuture future;
  bool atEnd = false;
  VELOX_ASSERT_THROW(
      queue->dequeueLocked(1, &atEnd, &future), "Forced failure");
}

TEST_F(OutputBufferManagerTest, getDataOnFailedTask) {
  // Fetching data on a task which was either never initialized in the buffer
  // manager or was removed by a parallel thread must return false. The `notify`
  // callback must not be registered.
  ASSERT_FALSE(bufferManager_->getData(
      "test.0.1",
      1,
      10,
      1,
      [](std::vector<std::unique_ptr<folly::IOBuf>> pages, int64_t sequence) {
        VELOX_UNREACHABLE();
      }));
}

TEST_F(OutputBufferManagerTest, updateBrodcastBufferOnFailedTask) {
  // Updating broadcast buffer count in the buffer manager for a given unknown
  // task must not throw exception, instead must return FALSE.
  ASSERT_FALSE(bufferManager_->updateOutputBuffers(
      "test.0.1", /* unknown task */
      10,
      false));
}

TEST_P(AllOutputBufferManagerTest, multiFetchers) {
  const std::vector<bool> earlyTerminations = {false, true};
  for (const auto earlyTermination : earlyTerminations) {
    SCOPED_TRACE(fmt::format("earlyTermination {}", earlyTermination));

    const vector_size_t size = 10;
    const std::string taskId = "t0";
    int numPartitions = 10;
    int extendedNumPartitions = numPartitions / 2;
    initializeTask(
        taskId,
        rowType_,
        kind_,
        numPartitions,
        1,
        kind_ == PartitionedOutputNode::Kind::kBroadcast ? 256 << 20 : 0);

    std::vector<std::thread> threads;
    std::vector<int64_t> fetchedPages(numPartitions + extendedNumPartitions, 0);
    for (size_t i = 0; i < numPartitions; ++i) {
      threads.emplace_back([&, i]() {
        dataFetcher(taskId, i, fetchedPages.at(i), earlyTermination);
      });
    }
    folly::Random::DefaultGenerator rng;
    rng.seed(1234);
    const int totalPages = 10'000;
    std::vector<int64_t> producedPages(
        numPartitions + extendedNumPartitions, 0);
    for (int i = 0; i < totalPages; ++i) {
      const int partition = kind_ == PartitionedOutputNode::Kind::kPartitioned
          ? folly::Random().rand32(rng) % numPartitions
          : 0;
      try {
        enqueue(taskId, partition, rowType_, size, true);
      } catch (...) {
        // Early termination might cause the task to fail early.
        ASSERT_TRUE(earlyTermination);
        break;
      }
      ++producedPages[partition];
      if (folly::Random().oneIn(4)) {
        std::this_thread::sleep_for(std::chrono::microseconds(5)); // NOLINT
      }
      if (i == 1000 && (kind_ != PartitionedOutputNode::Kind::kPartitioned)) {
        bufferManager_->updateOutputBuffers(
            taskId, numPartitions + extendedNumPartitions, false);
        for (size_t i = numPartitions;
             i < numPartitions + extendedNumPartitions;
             ++i) {
          threads.emplace_back([&, i]() {
            dataFetcher(taskId, i, fetchedPages.at(i), earlyTermination);
          });
          fetchedPages[i] = 0;
        }
        bufferManager_->updateOutputBuffers(taskId, 0, true);
      }
    }
    noMoreData(taskId);

    for (int i = 0; i < threads.size(); ++i) {
      threads[i].join();
    }

    if (!earlyTermination) {
      if (kind_ == PartitionedOutputNode::Kind::kPartitioned) {
        for (int i = 0; i < numPartitions; ++i) {
          ASSERT_EQ(fetchedPages[i], producedPages[i]);
        }
      } else if (kind_ == PartitionedOutputNode::Kind::kBroadcast) {
        int64_t totalFetchedPages{0};
        for (const auto& pages : fetchedPages) {
          totalFetchedPages += pages;
        }
        ASSERT_EQ(
            totalPages * (numPartitions + extendedNumPartitions),
            totalFetchedPages);
      } else {
        int64_t totalFetchedPages{0};
        for (const auto& pages : fetchedPages) {
          totalFetchedPages += pages;
        }
        ASSERT_EQ(totalPages, totalFetchedPages);
      }
    }
    bufferManager_->removeTask(taskId);
  }
}

VELOX_INSTANTIATE_TEST_SUITE_P(
    AllOutputBufferManagerTestSuite,
    AllOutputBufferManagerTest,
    testing::ValuesIn(AllOutputBufferManagerTest::getTestParams()));
