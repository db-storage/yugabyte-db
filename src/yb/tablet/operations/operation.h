// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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

#ifndef YB_TABLET_OPERATIONS_OPERATION_H
#define YB_TABLET_OPERATIONS_OPERATION_H

#include <mutex>
#include <string>

#include <boost/optional/optional.hpp>

#include "yb/common/hybrid_time.h"
#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus.h"
#include "yb/util/auto_release_pool.h"
#include "yb/util/locks.h"
#include "yb/util/status.h"
#include "yb/util/memory/arena.h"

namespace yb {

namespace tablet {

class Tablet;
class OperationCompletionCallback;
class OperationState;

YB_DEFINE_ENUM(OperationType,
               (kWrite)(kAlterSchema)(kUpdateTransaction)(kSnapshot)(kTruncate)(kEmpty));

// Base class for transactions.  There are different implementations for different types (Write,
// AlterSchema, etc.) OperationDriver implementations use Operations along with Consensus to execute
// and replicate operations in a consensus configuration.
class Operation {
 public:
  enum TraceType {
    NO_TRACE_TXNS = 0,
    TRACE_TXNS = 1
  };

  enum OperationResult {
    COMMITTED,
    ABORTED
  };

  Operation(std::unique_ptr<OperationState> state,
            consensus::DriverType type,
            OperationType operation_type);

  // Returns the OperationState for this transaction.
  virtual OperationState* state() { return state_.get(); }
  virtual const OperationState* state() const { return state_.get(); }

  // Returns whether this transaction is being executed on the leader or on a replica.
  consensus::DriverType type() const { return type_; }

  // Returns this transaction's type.
  OperationType operation_type() const { return operation_type_; }

  // Builds the ReplicateMsg for this transaction.
  virtual consensus::ReplicateMsgPtr NewReplicateMsg() = 0;

  // Executes the prepare phase of this transaction. The actual actions of this phase depend on the
  // transaction type, but usually are limited to what can be done without actually changing shared
  // data structures (such as the RocksDB memtable) and without side-effects.
  virtual CHECKED_STATUS Prepare() = 0;

  // Actually starts an operation, assigning a hybrid_time to the transaction.  LEADER replicas
  // execute this in or right after Prepare(), while FOLLOWER/LEARNER replicas execute this right
  // before the Apply() phase as the transaction's hybrid_time is only available on the LEADER's
  // commit message.  Once Started(), state might have leaked to other replicas/local log and the
  // transaction can't be cancelled without issuing an abort message.
  void Start();

  // Executes the Apply() phase of the transaction, the actual actions of this phase depend on the
  // transaction type, but usually this is the method where data-structures are changed.
  virtual CHECKED_STATUS Apply() = 0;

  // Executed after Apply() but before the commit is submitted to consensus.  Some transactions use
  // this to perform pre-commit actions (e.g. write transactions perform early lock release on this
  // hook).  Default implementation does nothing.
  virtual void PreCommit() {}

  // Executed after the transaction has been applied and the commit message has been appended to the
  // log (though it might not be durable yet), or if the transaction was aborted.  Implementations
  // are expected to perform cleanup on this method, the driver will reply to the client after this
  // method call returns.  'result' will be either COMMITTED or ABORTED, letting implementations
  // know what was the final status of the transaction.
  virtual void Finish(OperationResult result) {}

  // Each implementation should have its own ToString() method.
  virtual std::string ToString() const = 0;

  virtual ~Operation() {}

 private:
  virtual void DoStart() = 0;

  // A private version of this transaction's transaction state so that we can use base
  // OperationState methods on destructors.
  std::unique_ptr<OperationState> state_;//DHQ: OperationState
  const consensus::DriverType type_;
  const OperationType operation_type_;
};

class OperationState {
 public:
  OperationState(const OperationState&) = delete;
  void operator=(const OperationState&) = delete;

  // Returns the request PB associated with this transaction. May be NULL if the transaction's state
  // has been reset.
  virtual const google::protobuf::Message* request() const { return nullptr; }

  // Sets the ConsensusRound for this transaction, if this transaction is being executed through the
  // consensus system.
  void set_consensus_round(const scoped_refptr<consensus::ConsensusRound>& consensus_round) {
    consensus_round_ = consensus_round;
    op_id_ = consensus_round_->id();
    UpdateRequestFromConsensusRound();
  }

  // Each subclass should provide a way to update the internal reference to the Message* request, so
  // we can avoid copying the request object all the time.
  virtual void UpdateRequestFromConsensusRound() = 0;

  // Returns the ConsensusRound being used, if this transaction is being executed through the
  // consensus system or NULL if it's not.
  consensus::ConsensusRound* consensus_round() {
    return consensus_round_.get();
  }

  Tablet* tablet() const {
    return tablet_;
  }

  void set_completion_callback(std::unique_ptr<OperationCompletionCallback> completion_clbk) {
    completion_clbk_ = std::move(completion_clbk);
  }

  // Returns the completion callback.
  OperationCompletionCallback* completion_callback() {
    return DCHECK_NOTNULL(completion_clbk_.get());
  }

  // Sets a heap object to be managed by this transaction's AutoReleasePool.
  template<class T>
  T* AddToAutoReleasePool(T* t) {
    return pool_.Add(t);
  }

  // Sets an array heap object to be managed by this transaction's AutoReleasePool.
  template<class T>
  T* AddArrayToAutoReleasePool(T* t) {
    return pool_.AddArray(t);
  }

  // Return the arena associated with this transaction.  NOTE: this is not a thread-safe arena!
  Arena* arena();

  // Each implementation should have its own ToString() method.
  virtual std::string ToString() const = 0;

  // Sets the hybrid_time for the transaction
  void set_hybrid_time(const HybridTime& hybrid_time);

  // If this operation does not have hybrid time yet, then it will be inited from clock.
  void TrySetHybridTimeFromClock();

  HybridTime hybrid_time() const {
    std::lock_guard<simple_spinlock> l(mutex_);
    DCHECK(hybrid_time_.is_valid());
    return hybrid_time_;
  }

  HybridTime hybrid_time_even_if_unset() const {
    std::lock_guard<simple_spinlock> l(mutex_);
    return hybrid_time_;
  }

  bool has_hybrid_time() const {
    std::lock_guard<simple_spinlock> l(mutex_);
    return hybrid_time_.is_valid();
  }

  consensus::OpId* mutable_op_id() {
    return &op_id_;
  }

  const consensus::OpId& op_id() const {
    return op_id_;
  }

  virtual ~OperationState();

 protected:
  explicit OperationState(Tablet* tablet);

  // The tablet peer that is coordinating this transaction.
  Tablet* const tablet_;

  // Optional callback to be called once the transaction completes.
  std::unique_ptr<OperationCompletionCallback> completion_clbk_;

  AutoReleasePool pool_;

  // This transaction's hybrid_time. Protected by mutex_.
  HybridTime hybrid_time_;

  // The clock error when hybrid_time_ was read.
  uint64_t hybrid_time_error_;

  boost::optional<Arena> arena_;

  // This OpId stores the canonical "anchor" OpId for this transaction.
  consensus::OpId op_id_;

  scoped_refptr<consensus::ConsensusRound> consensus_round_;

  // Lock that protects access to operation state.
  mutable simple_spinlock mutex_;
};

// A parent class for the callback that gets called when transactions complete.
//
// This must be set in the OperationState if the transaction initiator is to be notified of when a
// transaction completes. The callback belongs to the transaction context and is deleted along with
// it.
//
// NOTE: this is a concrete class so that we can use it as a default implementation which avoids
// callers having to keep checking for NULL.
class OperationCompletionCallback {
 public:

  OperationCompletionCallback();

  // Allows to set an error for this transaction and a mapping to a server level code.  Calling this
  // method does not mean the transaction is completed.
  void set_error(const Status& status, tserver::TabletServerErrorPB::Code code);

  void set_error(const Status& status);

  bool has_error() const;

  const Status& status() const;

  const tserver::TabletServerErrorPB::Code error_code() const;

  // Subclasses should override this.
  virtual void OperationCompleted();

  void CompleteWithStatus(const Status& status) {
    set_error(status);
    OperationCompleted();
  }

  virtual ~OperationCompletionCallback();

 protected:
  Status status_;
  tserver::TabletServerErrorPB::Code code_;
};

// OperationCompletionCallback implementation that can be waited on.  Helper to make async
// transactions, sync.  This is templated to accept any response PB that has a TabletServerError
// 'error' field and to set the error before performing the latch countdown.  The callback does
// *not* take ownership of either latch or response.
template<class ResponsePB>
class LatchOperationCompletionCallback : public OperationCompletionCallback {
 public:
  explicit LatchOperationCompletionCallback(CountDownLatch* latch,
                                            ResponsePB* response)
    : latch_(DCHECK_NOTNULL(latch)),
      response_(DCHECK_NOTNULL(response)) {
  }

  virtual void OperationCompleted() override {
    if (!status_.ok()) {
      StatusToPB(status_, response_->mutable_error()->mutable_status());
    }
    latch_->CountDown();
  }

 private:
  CountDownLatch* latch_;
  ResponsePB* response_;
};

}  // namespace tablet
}  // namespace yb

#endif  // YB_TABLET_OPERATIONS_OPERATION_H
