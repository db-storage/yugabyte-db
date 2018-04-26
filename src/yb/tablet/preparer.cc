// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/lockfree/queue.hpp>

#include <gflags/gflags.h>

#include "yb/tablet/preparer.h"
#include "yb/tablet/operations/operation_driver.h"
#include "yb/util/logging.h"
#include "yb/util/threadpool.h"

DEFINE_int32(max_group_replicate_batch_size, 16,
             "Maximum number of operations to submit to consensus for replication in a batch.");

// We have to make the queue length really long. Otherwise we risk crashes on followers when they
// fail to append entries to the queue, as we try to cancel the operation in that case, and it
// is not possible to cancel an already-replicated operation. The proper way to handle that would
// probably be to implement backpressure in UpdateReplica.
//
// Note that the lock-free queue seems to be preallocating memory proportional to the queue size
// (about 64 bytes per entry for 8-byte pointer keys) -- something to keep in mind with a large
// number of tablets.
DEFINE_int32(prepare_queue_max_size, 100000,
             "Maximum number of operations waiting in the per-tablet prepare queue.");

using std::vector;

namespace yb {
class ThreadPool;
class ThreadPoolToken;

namespace tablet {

// ------------------------------------------------------------------------------------------------
// PreparerImpl

class PreparerImpl {
 public:
  explicit PreparerImpl(consensus::Consensus* consensus, ThreadPool* tablet_prepare_pool);
  ~PreparerImpl();
  CHECKED_STATUS Start();
  void Stop();

  CHECKED_STATUS Submit(OperationDriver* operation_driver);

 private:
  using OperationDrivers = std::vector<OperationDriver*>;

  scoped_refptr<yb::Thread> thread_;

  consensus::Consensus* const consensus_;

  // We set this to true to tell the Run function to return. No new tasks will be accepted, but
  // existing tasks will still be processed.
  std::atomic<bool> stop_requested_{false};

  // If true, a task is running for this tablet already.
  // If false, no taska are running for this tablet,
  // and we can submit a task to the thread pool token.
  std::atomic<int> running_{0};

  // This is set to true immediately before the thread exits.
  std::atomic<bool> stopped_{false};

  boost::lockfree::queue<OperationDriver*> queue_;

  // This mutex/condition combination is used in Stop() in case multiple threads are calling that
  // function concurrently. One of them will ask the prepare thread to stop and wait for it, and
  // then will notify other threads that have called Stop().
  std::mutex stop_mtx_;
  std::condition_variable stop_cond_;

  OperationDrivers leader_side_batch_;

  std::unique_ptr<ThreadPoolToken> tablet_prepare_pool_token_;

  // A temporary buffer of rounds to replicate, used to reduce reallocation.
  consensus::ConsensusRounds rounds_to_replicate_;

  void Run();
  void ProcessItem(OperationDriver* item);

  void ProcessAndClearLeaderSideBatch();

  // A wrapper around ProcessAndClearLeaderSideBatch that assumes we are currently holding the
  // mutex.

  void ReplicateSubBatch(OperationDrivers::iterator begin,
                         OperationDrivers::iterator end);
};

PreparerImpl::PreparerImpl(consensus::Consensus* consensus,
                                     ThreadPool* tablet_prepare_pool)
    : consensus_(consensus),
      queue_(FLAGS_prepare_queue_max_size),
      tablet_prepare_pool_token_(tablet_prepare_pool
                                     ->NewToken(ThreadPool::ExecutionMode::SERIAL)) {
}

PreparerImpl::~PreparerImpl() {
  Stop();
}

Status PreparerImpl::Start() {
  return Status::OK();
}

void PreparerImpl::Stop() {
  if (stopped_.load(std::memory_order_acquire)) {
    return;
  }
  stop_requested_ = true;
  {
    std::unique_lock<std::mutex> stop_lock(stop_mtx_);
    stop_cond_.wait(stop_lock, [this] {
      return (!running_.load(std::memory_order_acquire) && queue_.empty());
    });
  }
  stopped_.store(true, std::memory_order_release);
}

Status PreparerImpl::Submit(OperationDriver* operation_driver) {
  if (stop_requested_.load(std::memory_order_acquire)) {
    return STATUS(IllegalState, "Tablet is shutting down");
  }
  if (!queue_.bounded_push(operation_driver)) {
    return STATUS_FORMAT(ServiceUnavailable,
                         "Prepare queue is full (max capacity $0)",
                         FLAGS_prepare_queue_max_size);
  }

  int expected = 0;
  if (!running_.compare_exchange_strong(expected, 1, std::memory_order_release)) {
    // running_ was not 0, so we are not creating a task to process operations.
    return Status::OK();
  }
  // We flipped running_ from 0 to 1. The previously running thread could go back to doing another
  // iteration, but in that case since we are submitting to a token of a thread pool, only one
  // such thread will be running, the other will be in the queue.
  return tablet_prepare_pool_token_->SubmitFunc(std::bind(&PreparerImpl::Run, this));
}

void PreparerImpl::Run() {
  VLOG(1) << "Starting prepare task:" << this;
  for (;;) {
    OperationDriver *item = nullptr;
    while (queue_.pop(item)) {
      ProcessItem(item);
    }
    if (queue_.empty()) {
      // Not processing and queue empty, return from task.
      ProcessAndClearLeaderSideBatch();
      std::unique_lock<std::mutex> stop_lock(stop_mtx_);
      running_--;
      if (!queue_.empty()) {
        // Got more operations, stay in the loop.
        running_++;
        continue;
      }
      if (stop_requested_.load(std::memory_order_acquire)) {
        VLOG(1) << "Prepare task's Run() function is returning because stop is requested.";
        stop_cond_.notify_all();
        return;
      }
      VLOG(1) << "Returning from prepare task after inactivity:" << this;
      return;
    }
  }
}

void PreparerImpl::ProcessItem(OperationDriver* item) {
  CHECK_NOTNULL(item);

  if (item->is_leader_side()) {
    // AlterSchemaOperation::Prepare calls Tablet::CreatePreparedAlterSchema, which acquires the
    // schema lock. Because of this, we must not attempt to process two AlterSchemaOperations in
    // one batch, otherwise we'll deadlock. Furthermore, for simplicity, we choose to process each
    // AlterSchemaOperation in a batch of its own.
    auto operation_type = item->operation_type();
    const bool apply_separately = operation_type == OperationType::kAlterSchema ||
                                  operation_type == OperationType::kEmpty;
    const int64_t bound_term = apply_separately ? -1 : item->consensus_round()->bound_term();

    // Don't add more than the max number of operations to a batch, and also don't add
    // operations bound to different terms, so as not to fail unrelated operations
    // unnecessarily in case of a bound term mismatch.
    if (leader_side_batch_.size() >= FLAGS_max_group_replicate_batch_size ||
        !leader_side_batch_.empty() &&
            bound_term != leader_side_batch_.back()->consensus_round()->bound_term()) {
      ProcessAndClearLeaderSideBatch();//DHQ: 里面做了group replicate，尽量等待和产生Batch
    }
    leader_side_batch_.push_back(item);
    if (apply_separately) {//DHQ: 需要单独apply的，直接处理Batch
      ProcessAndClearLeaderSideBatch();
    }
  } else {
    // We found a non-leader-side operation. We need to process the accumulated batch of
    // leader-side operations first, and then process this other operation.
    ProcessAndClearLeaderSideBatch();
    item->PrepareAndStartTask();
  }
}

void PreparerImpl::ProcessAndClearLeaderSideBatch() {
  if (leader_side_batch_.empty()) {
    return;
  }

  VLOG(1) << "Preparing a batch of " << leader_side_batch_.size() << " leader-side operations";

  auto iter = leader_side_batch_.begin();
  auto replication_subbatch_begin = iter;
  auto replication_subbatch_end = iter;

  // PrepareAndStart does not call Consensus::Replicate anymore as of 07/07/2017, and it is our
  // responsibility to do so in case of success. We call Consensus::ReplicateBatch for batches
  // of consecutive successfully prepared operations.

  while (iter != leader_side_batch_.end()) {
    auto* operation_driver = *iter;

    Status s = operation_driver->PrepareAndStart();

    if (PREDICT_TRUE(s.ok())) {
      replication_subbatch_end = ++iter;
    } else {//DHQ: 跳过Prepare失败的，
      ReplicateSubBatch(replication_subbatch_begin, replication_subbatch_end);//DHQ: failure前面的，作为batch先replicate

      // Handle failure for this operation itself.
      operation_driver->HandleFailure(s);//DHQ: 处理失败的

      // Now we'll start accumulating a new batch.
      replication_subbatch_begin = replication_subbatch_end = ++iter; //DHQ: 越过
    }
  }

  // Replicate the remaining batch. No-op for an empty batch.
  ReplicateSubBatch(replication_subbatch_begin, replication_subbatch_end);

  leader_side_batch_.clear();
}

void PreparerImpl::ReplicateSubBatch(
    OperationDrivers::iterator batch_begin,
    OperationDrivers::iterator batch_end) {
  DCHECK_GE(std::distance(batch_begin, batch_end), 0);
  if (batch_begin == batch_end) {
    return;
  }
  VLOG(1) << "Replicating a sub-batch of " << std::distance(batch_begin, batch_end)
          << " leader-side operations";
  if (VLOG_IS_ON(2)) {
    for (auto batch_iter = batch_begin; batch_iter != batch_end; ++batch_iter) {
      VLOG(2) << "Leader-side operation to be replicated: " << (*batch_iter)->ToString();
    }
  }

  rounds_to_replicate_.clear();
  rounds_to_replicate_.reserve(std::distance(batch_begin, batch_end));
  for (auto batch_iter = batch_begin; batch_iter != batch_end; ++batch_iter) {
    DCHECK_ONLY_NOTNULL(*batch_iter);
    DCHECK_ONLY_NOTNULL((*batch_iter)->consensus_round());
    rounds_to_replicate_.push_back((*batch_iter)->consensus_round());
  }

  const Status s = consensus_->ReplicateBatch(rounds_to_replicate_);
  rounds_to_replicate_.clear();

  if (PREDICT_FALSE(!s.ok())) {
    VLOG(1) << "ReplicateBatch failed with status " << s.ToString()
            << ", treating all " << std::distance(batch_begin, batch_end) << " operations as "
            << "failed with that status";
    // Treat all the operations in the batch as failed.
    for (auto batch_iter = batch_begin; batch_iter != batch_end; ++batch_iter) {
      (*batch_iter)->SetReplicationFailed(s);
      (*batch_iter)->HandleFailure(s);
    }
  }
}

// ------------------------------------------------------------------------------------------------
// Preparer

Preparer::Preparer(consensus::Consensus* consensus, ThreadPool* tablet_prepare_thread)
    : impl_(std::make_unique<PreparerImpl>(consensus, tablet_prepare_thread)) {
}

Preparer::~Preparer() = default;

Status Preparer::Start() {
  VLOG(1) << "Starting the prepare thread";
  return impl_->Start();
}

void Preparer::Stop() {
  VLOG(1) << "Stopping the prepare thread";
  impl_->Stop();
  VLOG(1) << "The prepare thread has stopped";
}

Status Preparer::Submit(OperationDriver* operation_driver) {
  return impl_->Submit(operation_driver);
}

}  // namespace tablet
}  // namespace yb
