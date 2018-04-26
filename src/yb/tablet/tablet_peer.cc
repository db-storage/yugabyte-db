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

#include "yb/tablet/tablet_peer.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <gflags/gflags.h>

#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/log.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/consensus/log_util.h"
#include "yb/consensus/opid_util.h"
#include "yb/consensus/quorum_util.h"
#include "yb/consensus/raft_consensus.h"

#include "yb/docdb/consensus_frontier.h"
#include "yb/docdb/docdb.h"

#include "yb/gutil/mathlimits.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/sysinfo.h"

#include "yb/rocksdb/db/memtable.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet.pb.h"
#include "yb/tablet/tablet_bootstrap_if.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_metrics.h"
#include "yb/tablet/tablet_peer_mm_ops.h"

#include "yb/tablet/operations/alter_schema_operation.h"
#include "yb/tablet/operations/operation_driver.h"
#include "yb/tablet/operations/truncate_operation.h"
#include "yb/tablet/operations/write_operation.h"
#include "yb/tablet/operations/update_txn_operation.h"

#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/stopwatch.h"
#include "yb/util/threadpool.h"
#include "yb/util/trace.h"

using std::shared_ptr;
using std::string;

namespace yb {
namespace tablet {

METRIC_DEFINE_histogram(tablet, op_prepare_queue_length, "Operation Prepare Queue Length",
                        MetricUnit::kTasks,
                        "Number of operations waiting to be prepared within this tablet. "
                        "High queue lengths indicate that the server is unable to process "
                        "operations as fast as they are being written to the WAL.",
                        10000, 2);

METRIC_DEFINE_histogram(tablet, op_prepare_queue_time, "Operation Prepare Queue Time",
                        MetricUnit::kMicroseconds,
                        "Time that operations spent waiting in the prepare queue before being "
                        "processed. High queue times indicate that the server is unable to "
                        "process operations as fast as they are being written to the WAL.",
                        10000000, 2);

METRIC_DEFINE_histogram(tablet, op_prepare_run_time, "Operation Prepare Run Time",
                        MetricUnit::kMicroseconds,
                        "Time that operations spent being prepared in the tablet. "
                        "High values may indicate that the server is under-provisioned or "
                        "that operations are experiencing high contention with one another for "
                        "locks.",
                        10000000, 2);

using consensus::Consensus;
using consensus::ConsensusBootstrapInfo;
using consensus::ConsensusMetadata;
using consensus::ConsensusOptions;
using consensus::ConsensusRound;
using consensus::StateChangeContext;
using consensus::StateChangeReason;
using consensus::OpId;
using consensus::RaftConfigPB;
using consensus::RaftPeerPB;
using consensus::RaftConsensus;
using consensus::ReplicateMsg;
using consensus::OpIdType;
using log::Log;
using log::LogAnchorRegistry;
using rpc::Messenger;
using strings::Substitute;
using tserver::TabletServerErrorPB;

// ============================================================================
//  Tablet Peer
// ============================================================================
TabletPeer::TabletPeer(
    const scoped_refptr<TabletMetadata>& meta,
    const consensus::RaftPeerPB& local_peer_pb,
    ThreadPool* apply_pool,
    Callback<void(std::shared_ptr<StateChangeContext> context)> mark_dirty_clbk)
  : meta_(meta),
    tablet_id_(meta->tablet_id()),
    local_peer_pb_(local_peer_pb),
    state_(NOT_STARTED),
    status_listener_(new TabletStatusListener(meta)),
    apply_pool_(apply_pool),
    log_anchor_registry_(new LogAnchorRegistry()), //TabletPeer 有个log_anchor_retistry_，不是log层的
    mark_dirty_clbk_(std::move(mark_dirty_clbk)) {}

TabletPeer::~TabletPeer() {
  std::lock_guard<simple_spinlock> lock(lock_);
  // We should either have called Shutdown(), or we should have never called
  // Init().
  CHECK(!tablet_)
      << "TabletPeer not fully shut down. State: "
      << TabletStatePB_Name(state_);
}

Status TabletPeer::InitTabletPeer(const shared_ptr<TabletClass> &tablet,
                                  const std::shared_future<client::YBClientPtr> &client_future,
                                  const scoped_refptr<server::Clock> &clock,
                                  const shared_ptr<Messenger> &messenger,
                                  const scoped_refptr<Log> &log,
                                  const scoped_refptr<MetricEntity> &metric_entity,
                                  ThreadPool* raft_pool,
                                  ThreadPool* tablet_prepare_pool) {

  DCHECK(tablet) << "A TabletPeer must be provided with a Tablet";
  DCHECK(log) << "A TabletPeer must be provided with a Log";

  {
    std::lock_guard<simple_spinlock> lock(lock_);
    CHECK_EQ(BOOTSTRAPPING, state_);
    tablet_ = tablet;
    client_future_ = client_future;
    clock_ = clock;
    messenger_ = messenger;
    log_ = log;

    tablet->SetMemTableFlushFilterFactory([log] { //DHQ: 这个就是操作Frontiers的地方, log_作为参数传递
      auto index = log->GetLatestEntryOpId().index;
      return [index] (const rocksdb::MemTable& memtable) -> Result<bool> {//DHQ: return a Lambda(can be casted to std::function)
        auto frontiers = memtable.Frontiers();
        if (frontiers) {
          const auto& largest = down_cast<const docdb::ConsensusFrontier&>(frontiers->Largest());
          // We can only flush this memtable if all operations written to it have also been written
          // to the log (maybe not synced, if durable_wal_write is disabled, but that's OK).
          return largest.op_id().index <= index; //DHQ: 跟已经写log的op_id相比，必须要先写完log才能持久化到rocksdb.实际上不可能在写log之前就发生这个flush
        }//DHQ: 事实上，下面的就是错误，不应该存在的情况。
        // This is a degenerate case that should ideally never occur. An empty memtable got into the
        // list of immutable memtables. We say it is OK to flush it and move on.
        static const char* error_msg =
            "A memtable with no frontiers set found when deciding what memtables to "
            "flush! This should not happen.";
        LOG(ERROR) << error_msg << " Stack trace:\n" << GetStackTrace();
        return STATUS(IllegalState, error_msg);
      };
    });

    ConsensusOptions options;
    options.tablet_id = meta_->tablet_id();

    TRACE("Creating consensus instance");

    std::unique_ptr<ConsensusMetadata> cmeta;
    RETURN_NOT_OK(ConsensusMetadata::Load(meta_->fs_manager(), tablet_id_,
                                          meta_->fs_manager()->uuid(), &cmeta));//DHQ: 需要load一些数据

    consensus_ = RaftConsensus::Create(
        options,
        std::move(cmeta),
        local_peer_pb_,
        metric_entity,
        clock_,
        this,
        messenger_,
        log_.get(),
        tablet_->mem_tracker(),
        mark_dirty_clbk_,
        tablet_->table_type(),
        std::bind(&Tablet::LostLeadership, tablet.get()),//DHQ: 有个LostLeadership注册
        raft_pool);

    auto ht_lease_provider = [this](MicrosTime min_allowed, MonoTime deadline) {
      MicrosTime lease_micros {
          consensus_->MajorityReplicatedHtLeaseExpiration(min_allowed, deadline) };
      if (!lease_micros) {
        return HybridTime::kInvalid;
      }
      if (lease_micros >= kMaxHybridTimePhysicalMicros) {
        // This could happen when leader leases are disabled.
        return HybridTime::kMax;
      }
      return HybridTime(lease_micros, /* logical */ 0);
    };
    tablet_->SetHybridTimeLeaseProvider(ht_lease_provider);

    auto* mvcc_manager = tablet_->mvcc_manager();
    consensus_->SetPropagatedSafeTimeProvider([mvcc_manager, ht_lease_provider] {
      // Get the current majority-replicated HT leader lease without any waiting.
      auto ht_lease = ht_lease_provider(/* min_allowed */ 0, /* deadline */ MonoTime::kMax);
      if (!ht_lease) {
        return HybridTime::kInvalid;
      }
      return mvcc_manager->SafeTime(ht_lease);
    });

    consensus_->SetMajorityReplicatedListener([mvcc_manager, ht_lease_provider] {//DHQ: Lambda
      auto ht_lease = ht_lease_provider(0, MonoTime::kMax);
      if (ht_lease) {
        mvcc_manager->UpdatePropagatedSafeTimeOnLeader(ht_lease);
      }
    });

    prepare_thread_ = std::make_unique<Preparer>(consensus_.get(), tablet_prepare_pool);//DHQ: 初始化prepare_thread_
  }

  RETURN_NOT_OK(prepare_thread_->Start());

  if (tablet_->metrics() != nullptr) {
    TRACE("Starting instrumentation");
    operation_tracker_.StartInstrumentation(tablet_->GetMetricEntity());
  }
  operation_tracker_.StartMemoryTracking(tablet_->mem_tracker());

  if (tablet_->transaction_coordinator()) {
    tablet_->transaction_coordinator()->Start();
  }

  TRACE("TabletPeer::Init() finished");
  VLOG(2) << "T " << tablet_id() << " P " << consensus_->peer_uuid() << ": Peer Initted";
  return Status::OK();
}

Status TabletPeer::Start(const ConsensusBootstrapInfo& bootstrap_info) {
  std::lock_guard<simple_spinlock> l(state_change_lock_);
  TRACE("Starting consensus");

  VLOG(2) << "T " << tablet_id() << " P " << consensus_->peer_uuid() << ": Peer starting";

  VLOG(2) << "RaftConfig before starting: " << consensus_->CommittedConfig().DebugString();

  RETURN_NOT_OK(consensus_->Start(bootstrap_info)); //DHQ: 调用consensus_的Start()
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    CHECK_EQ(state_, BOOTSTRAPPING);
    state_ = RUNNING;
  }

  // The context tracks that the current caller does not hold the lock for consensus state.
  // So mark dirty callback, e.g., consensus->ConsensusState() for master consensus callback of
  // SysCatalogStateChanged, can get the lock when needed.
  auto context =
      std::make_shared<StateChangeContext>(StateChangeReason::TABLET_PEER_STARTED, false);
  // Because we changed the tablet state, we need to re-report the tablet to the master.
  mark_dirty_clbk_.Run(context);//TODO: 这个或许我们需要使用下？ 触发我们的HB?

  return Status::OK();
}

const consensus::RaftConfigPB TabletPeer::RaftConfig() const {
  CHECK(consensus_) << "consensus is null";
  return consensus_->CommittedConfig();
}

void TabletPeer::Shutdown() {

  LOG(INFO) << "Initiating TabletPeer shutdown for tablet: " << tablet_id_;
  if (tablet_) {
    tablet_->SetShutdownRequestedFlag();
  }

  {
    std::unique_lock<simple_spinlock> lock(lock_);
    if (state_ == QUIESCING || state_ == SHUTDOWN) {
      lock.unlock();
      WaitUntilShutdown();
      return;
    }
    state_ = QUIESCING;
  }

  std::lock_guard<simple_spinlock> l(state_change_lock_);
  // Even though Tablet::Shutdown() also unregisters its ops, we have to do it here
  // to ensure that any currently running operation finishes before we proceed with
  // the rest of the shutdown sequence. In particular, a maintenance operation could
  // indirectly end up calling into the log, which we are about to shut down.
  UnregisterMaintenanceOps();

  if (consensus_) consensus_->Shutdown();

  // TODO: KUDU-183: Keep track of the pending tasks and send an "abort" message.
  LOG_SLOW_EXECUTION(WARNING, 1000,
      Substitute("TabletPeer: tablet $0: Waiting for Operations to complete", tablet_id())) {
    operation_tracker_.WaitForAllToFinish();
  }

  if (prepare_thread_) {
    prepare_thread_->Stop();
  }

  if (log_) {
    WARN_NOT_OK(log_->Close(), "Error closing the Log.");
  }

  if (VLOG_IS_ON(1)) {
    VLOG(1) << "TabletPeer: tablet " << tablet_id() << " shut down!";
  }

  if (tablet_) {
    tablet_->Shutdown();
  }

  // Only mark the peer as SHUTDOWN when all other components have shut down.
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    // Release mem tracker resources.
    consensus_.reset();
    tablet_.reset();
    state_ = SHUTDOWN;
  }
}

void TabletPeer::WaitUntilShutdown() {
  while (true) {
    {
      std::lock_guard<simple_spinlock> lock(lock_);
      if (state_ == SHUTDOWN) {
        return;
      }
    }
    SleepFor(MonoDelta::FromMilliseconds(10));
  }
}

Status TabletPeer::CheckRunning() const {
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    if (state_ != RUNNING) {
      return STATUS(IllegalState, Substitute("The tablet is not in a running state: $0",
                                             TabletStatePB_Name(state_)));
    }
  }
  return Status::OK();
}

Status TabletPeer::CheckShutdownOrNotStarted() const {
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    if (state_ != SHUTDOWN && state_ != NOT_STARTED) {
      return STATUS(IllegalState, Substitute("The tablet is not in a shutdown state: $0",
                                             TabletStatePB_Name(state_)));
    }
  }
  return Status::OK();
}

Status TabletPeer::WaitUntilConsensusRunning(const MonoDelta& timeout) {
  MonoTime start(MonoTime::Now());

  int backoff_exp = 0;
  const int kMaxBackoffExp = 8;
  while (true) {
    bool has_consensus = false;
    TabletStatePB cached_state;
    {
      std::lock_guard<simple_spinlock> lock(lock_);
      cached_state = state_;
      if (consensus_) {
        has_consensus = true; // consensus_ is a set-once object.
      }
    }
    if (cached_state == QUIESCING || cached_state == SHUTDOWN) {
      return STATUS(IllegalState,
          Substitute("The tablet is already shutting down or shutdown. State: $0",
                     TabletStatePB_Name(cached_state)));
    }
    if (cached_state == RUNNING && has_consensus && consensus_->IsRunning()) {
      break;
    }
    MonoTime now(MonoTime::Now());
    MonoDelta elapsed(now.GetDeltaSince(start));
    if (elapsed.MoreThan(timeout)) {
      return STATUS(TimedOut, Substitute("Consensus is not running after waiting for $0. State; $1",
                                         elapsed.ToString(), TabletStatePB_Name(cached_state)));
    }
    SleepFor(MonoDelta::FromMilliseconds(1 << backoff_exp));
    backoff_exp = std::min(backoff_exp + 1, kMaxBackoffExp);
  }
  return Status::OK();
}
//DHQ: WriteOperation在此创建
Status TabletPeer::SubmitWrite(std::unique_ptr<WriteOperationState> state) {
  auto operation = std::make_unique<WriteOperation>(std::move(state), consensus::LEADER);
  RETURN_NOT_OK(CheckRunning());

  HybridTime restart_read_ht;
  RETURN_NOT_OK(tablet_->AcquireLocksAndPerformDocOperations(operation->state(), &restart_read_ht)); //DHQ: //TODO: 这里获取的lock，啥时候释放？
  // If a restart read is required, then we return this fact to caller and don't perform the write
  // operation.
  if (restart_read_ht.is_valid()) {
    auto restart_time = operation->state()->response()->mutable_restart_read_time();
    restart_time->set_read_ht(restart_read_ht.ToUint64());
    restart_time->set_local_limit_ht(
        tablet_->SafeTime(RequireLease::kTrue).ToUint64());
    // Global limit is ignored by caller, so we don't set it.
    operation->state()->completion_callback()->OperationCompleted();
    return Status::OK();
  }
  auto driver = VERIFY_RESULT(NewLeaderOperationDriver(std::move(operation)));
  driver->ExecuteAsync();
  return Status::OK();
}

void TabletPeer::Submit(std::unique_ptr<Operation> operation) {
  auto status = CheckRunning();

  if (status.ok()) {
    auto driver = NewLeaderOperationDriver(std::move(operation));
    if (driver.ok()) {
      (**driver).ExecuteAsync();
    } else {
      status = driver.status();
    }
  }
  if (!status.ok()) {
    operation->state()->completion_callback()->CompleteWithStatus(status);
  }
}

void TabletPeer::SubmitUpdateTransaction(std::unique_ptr<UpdateTxnOperationState> state) {
  Submit(std::make_unique<tablet::UpdateTxnOperation>(std::move(state), consensus::LEADER));
}

HybridTime TabletPeer::Now() {
  return clock_->Now();
}

void TabletPeer::UpdateClock(HybridTime hybrid_time) {
  clock_->Update(hybrid_time);
}

std::unique_ptr<UpdateTxnOperationState> TabletPeer::CreateUpdateTransactionState(
    tserver::TransactionStatePB* request) {
  auto result = std::make_unique<UpdateTxnOperationState>(tablet());
  result->TakeRequest(request);
  return result;
}

void TabletPeer::GetTabletStatusPB(TabletStatusPB* status_pb_out) const {
  std::lock_guard<simple_spinlock> lock(lock_);
  DCHECK(status_pb_out != nullptr);
  DCHECK(status_listener_.get() != nullptr);
  status_pb_out->set_tablet_id(status_listener_->tablet_id());
  status_pb_out->set_table_name(status_listener_->table_name());
  status_pb_out->set_last_status(status_listener_->last_status());
  status_listener_->partition().ToPB(status_pb_out->mutable_partition());
  status_pb_out->set_state(state_);
  status_pb_out->set_tablet_data_state(meta_->tablet_data_state());
  status_pb_out->set_estimated_on_disk_size(OnDiskSize());
}

Status TabletPeer::RunLogGC() {
  if (!CheckRunning().ok()) {
    return Status::OK();
  }
  int64_t min_log_index = 0;
  int32_t num_gced = 0;
  RETURN_NOT_OK(GetEarliestNeededLogIndex(&min_log_index));
  return log_->GC(min_log_index, &num_gced);
}

string TabletPeer::HumanReadableState() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  TabletDataState data_state = meta_->tablet_data_state();
  // If failed, any number of things could have gone wrong.
  if (state_ == FAILED) {
    return Substitute("$0 ($1): $2", TabletStatePB_Name(state_),
                      TabletDataState_Name(data_state),
                      error_.ToString());
  // If it's remotely bootstrapping, or tombstoned, that is the important thing
  // to show.
  } else if (data_state != TABLET_DATA_READY) {
    return TabletDataState_Name(data_state);
  }
  // Otherwise, the tablet's data is in a "normal" state, so we just display
  // the runtime state (BOOTSTRAPPING, RUNNING, etc).
  return TabletStatePB_Name(state_);
}

namespace {

consensus::OperationType MapOperationTypeToPB(OperationType operation_type) {
  switch (operation_type) {
    case OperationType::kWrite:
      return consensus::WRITE_OP;

    case OperationType::kAlterSchema:
      return consensus::ALTER_SCHEMA_OP;

    case OperationType::kUpdateTransaction:
      return consensus::UPDATE_TRANSACTION_OP;

    case OperationType::kSnapshot:
      return consensus::SNAPSHOT_OP;

    case OperationType::kTruncate:
      return consensus::TRUNCATE_OP;

    case OperationType::kEmpty:
      LOG(FATAL) << "OperationType::kEmpty cannot be converted to consensus::OperationType";
  }
  FATAL_INVALID_ENUM_VALUE(OperationType, operation_type);
}

} // namespace

void TabletPeer::GetInFlightOperations(Operation::TraceType trace_type,
                                       vector<consensus::OperationStatusPB>* out) const {
  for (const auto& driver : operation_tracker_.GetPendingOperations()) {
    if (driver->state() == nullptr) {
      continue;
    }
    auto op_type = driver->operation_type();
    if (op_type == OperationType::kEmpty) {
      // This is a special-purpose in-memory-only operation for updating propagated safe time on
      // a follower.
      continue;
    }

    consensus::OperationStatusPB status_pb;
    status_pb.mutable_op_id()->CopyFrom(driver->GetOpId());
    status_pb.set_operation_type(MapOperationTypeToPB(op_type));
    status_pb.set_description(driver->ToString());
    int64_t running_for_micros =
        MonoTime::Now().GetDeltaSince(driver->start_time()).ToMicroseconds();
    status_pb.set_running_for_micros(running_for_micros);
    if (trace_type == Operation::TRACE_TXNS) {
      status_pb.set_trace_buffer(driver->trace()->DumpToString(true));
    }
    out->push_back(status_pb);
  }
}

Status TabletPeer::GetEarliestNeededLogIndex(int64_t* min_index) const {
  // First, we anchor on the last OpId in the Log to establish a lower bound
  // and avoid racing with the other checks. This limits the Log GC candidate
  // segments before we check the anchors.
  *min_index = log_->GetLatestEntryOpId().index;

  // If we never have written to the log, no need to proceed.
  if (*min_index == 0) {
    return Status::OK();
  }

  // Next, we interrogate the anchor registry.
  // Returns OK if minimum known, NotFound if no anchors are registered.
  {//DHQ: 我们在做split时，应该也有这种需求，把开始执行split后的log pin住。
    int64_t min_anchor_index;
    Status s = log_anchor_registry_->GetEarliestRegisteredLogIndex(&min_anchor_index);
    if (PREDICT_FALSE(!s.ok())) {
      DCHECK(s.IsNotFound()) << "Unexpected error calling LogAnchorRegistry: " << s.ToString();
    } else {
      *min_index = std::min(*min_index, min_anchor_index);
    }
  }

  // Next, interrogate the OperationTracker.
  for (const auto& driver : operation_tracker_.GetPendingOperations()) {
    OpId tx_op_id = driver->GetOpId();
    // A operation which doesn't have an opid hasn't been submitted for replication yet and
    // thus has no need to anchor the log.
    if (tx_op_id.IsInitialized()) {
      *min_index = std::min(*min_index, tx_op_id.index());
    }
  }

  auto* transaction_coordinator = tablet()->transaction_coordinator();
  if (transaction_coordinator) {
    *min_index = std::min(*min_index, transaction_coordinator->PrepareGC());
  }

  int64_t last_committed_write_index = tablet_->last_committed_write_index();
  Result<yb::OpId> max_persistent_op_id = tablet_->MaxPersistentOpId();
  RETURN_NOT_OK(max_persistent_op_id);
  int64_t max_persistent_index = max_persistent_op_id->index;
  // Check whether we had writes after last persistent entry.
  // Note that last_committed_write_index could be zero if logs were cleaned before restart.
  // So correct check is 'less', and NOT 'not equals to'.
  if (max_persistent_index < last_committed_write_index) {
    *min_index = std::min(*min_index, max_persistent_index);
  }

  // We keep at least one committed operation in the log so that we can always recover safe time
  // during bootstrap.
  OpId committed_op_id;
  const Status get_committed_op_id_status =
      consensus()->GetLastOpId(OpIdType::COMMITTED_OPID, &committed_op_id);
  if (!get_committed_op_id_status.IsNotFound()) {
    // NotFound is returned by local consensus. We should get rid of this logic once local
    // consensus is gone.
    RETURN_NOT_OK(get_committed_op_id_status);
    *min_index = std::min(*min_index, static_cast<int64_t>(committed_op_id.index()));
  }

  return Status::OK();
}

Status TabletPeer::GetMaxIndexesToSegmentSizeMap(MaxIdxToSegmentSizeMap* idx_size_map) const {
  RETURN_NOT_OK(CheckRunning());
  int64_t min_op_idx;
  RETURN_NOT_OK(GetEarliestNeededLogIndex(&min_op_idx));
  log_->GetMaxIndexesToSegmentSizeMap(min_op_idx, idx_size_map);
  return Status::OK();
}

Status TabletPeer::GetGCableDataSize(int64_t* retention_size) const {
  RETURN_NOT_OK(CheckRunning());
  int64_t min_op_idx;
  RETURN_NOT_OK(GetEarliestNeededLogIndex(&min_op_idx));
  log_->GetGCableDataSize(min_op_idx, retention_size);//DHQ: 应该是顺序GC的。
  return Status::OK();
}

std::unique_ptr<Operation> TabletPeer::CreateOperation(consensus::ReplicateMsg* replicate_msg) {
  switch (replicate_msg->op_type()) {
    case consensus::WRITE_OP:
      DCHECK(replicate_msg->has_write_request()) << "WRITE_OP replica"
          " operation must receive a WriteRequestPB";
      return std::make_unique<WriteOperation>(
          std::make_unique<WriteOperationState>(tablet()), consensus::REPLICA);

    case consensus::ALTER_SCHEMA_OP:
      DCHECK(replicate_msg->has_alter_schema_request()) << "ALTER_SCHEMA_OP replica"
          " operation must receive an AlterSchemaRequestPB";
      return std::make_unique<AlterSchemaOperation>(
          std::make_unique<AlterSchemaOperationState>(tablet(), log()), consensus::REPLICA);

    case consensus::UPDATE_TRANSACTION_OP:
      DCHECK(replicate_msg->has_transaction_state()) << "UPDATE_TRANSACTION_OP replica"
          " operation must receive an TransactionStatePB";
      return std::make_unique<UpdateTxnOperation>(
          std::make_unique<UpdateTxnOperationState>(tablet()), consensus::REPLICA);

    case consensus::TRUNCATE_OP:
      DCHECK(replicate_msg->has_truncate_request()) << "TRUNCATE_OP replica"
          " operation must receive an TruncateRequestPB";
      return std::make_unique<TruncateOperation>(
          std::make_unique<TruncateOperationState>(tablet()), consensus::REPLICA);

    case consensus::SNAPSHOT_OP: FALLTHROUGH_INTENDED;
    case consensus::UNKNOWN_OP: FALLTHROUGH_INTENDED;
    case consensus::NO_OP: FALLTHROUGH_INTENDED;
    case consensus::CHANGE_CONFIG_OP:
      FATAL_INVALID_ENUM_VALUE(consensus::OperationType, replicate_msg->op_type());
  }
  FATAL_INVALID_ENUM_VALUE(consensus::OperationType, replicate_msg->op_type());
}

Status TabletPeer::StartReplicaOperation(//DHQ: Replica，应该是follower
    const scoped_refptr<ConsensusRound>& round, HybridTime propagated_safe_time) {//DHQ: 这种operation，先做好分配，等commit后，执行后续action
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    if (state_ != RUNNING && state_ != BOOTSTRAPPING) {
      return STATUS(IllegalState, TabletStatePB_Name(state_));
    }
  }

  consensus::ReplicateMsg* replicate_msg = round->replicate_msg().get();
  DCHECK(replicate_msg->has_hybrid_time());
  auto operation = CreateOperation(replicate_msg);//DHQ: 针对各种消息，产生相应的operation

  // TODO(todd) Look at wiring the stuff below on the driver
  OperationState* state = operation->state();
  // It's imperative that we set the round here on any type of operation, as this
  // allows us to keep the reference to the request in the round instead of copying it.
  state->set_consensus_round(round);
  HybridTime ht(replicate_msg->hybrid_time());
  state->set_hybrid_time(ht);
  clock_->Update(ht);

  // This sets the monotonic counter to at least replicate_msg.monotonic_counter() atomically.
  tablet_->UpdateMonotonicCounter(replicate_msg->monotonic_counter());

  OperationDriverPtr driver = VERIFY_RESULT(NewReplicaOperationDriver(std::move(operation)));

  // Unretained is required to avoid a refcount cycle.
  state->consensus_round()->SetConsensusReplicatedCallback(
      Bind(&OperationDriver::ReplicationFinished, Unretained(driver.get())));

  if (propagated_safe_time) {
    driver->SetPropagatedSafeTime(propagated_safe_time, tablet_->mvcc_manager());
  }
  driver->ExecuteAsync();//DHQ: 启动执行
  return Status::OK();
}

void TabletPeer::SetPropagatedSafeTime(HybridTime ht) {
  auto driver = NewReplicaOperationDriver(nullptr);
  if (!driver.ok()) {
    LOG(ERROR) << "Failed to create operation driver to set propagated hybrid time";
    return;
  }
  (**driver).SetPropagatedSafeTime(ht, tablet_->mvcc_manager());
  (**driver).ExecuteAsync();
}

const std::string& TabletPeer::permanent_uuid() const {
  if (cached_permanent_uuid_initialized_.load(std::memory_order_acquire)) {
    return cached_permanent_uuid_;
  }
  std::lock_guard<simple_spinlock> lock(lock_);
  if (tablet_ == nullptr) {
    static std::string empty_string;
    return empty_string;
  }

  if (!cached_permanent_uuid_initialized_.load(std::memory_order_acquire)) {
    cached_permanent_uuid_ = tablet_->metadata()->fs_manager()->uuid();
    cached_permanent_uuid_initialized_.store(true, std::memory_order_release);
  }
  return cached_permanent_uuid_;
}

consensus::Consensus* TabletPeer::consensus() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  return consensus_.get();
}

scoped_refptr<consensus::Consensus> TabletPeer::shared_consensus() const {
  std::lock_guard<simple_spinlock> lock(lock_);
  return consensus_;
}

Result<OperationDriverPtr> TabletPeer::NewLeaderOperationDriver(//DHQ: leader的
    std::unique_ptr<Operation> operation) {
  return NewOperationDriver(std::move(operation), consensus::LEADER);
}

Result<OperationDriverPtr> TabletPeer::NewReplicaOperationDriver(//DHQ: follower的
    std::unique_ptr<Operation> operation) {
  return NewOperationDriver(std::move(operation), consensus::REPLICA);
}

Result<OperationDriverPtr> TabletPeer::NewOperationDriver(std::unique_ptr<Operation> operation,
                                                          consensus::DriverType type) {
  auto operation_driver = CreateOperationDriver();
  RETURN_NOT_OK(operation_driver->Init(std::move(operation), type));
  return operation_driver;
}

void TabletPeer::RegisterMaintenanceOps(MaintenanceManager* maint_mgr) {
  // Taking state_change_lock_ ensures that we don't shut down concurrently with
  // this last start-up task.
  std::lock_guard<simple_spinlock> l(state_change_lock_);

  if (state() != RUNNING) {
    LOG(WARNING) << "Not registering maintenance operations for " << tablet_
                 << ": tablet not in RUNNING state";
    return;
  }

  DCHECK(maintenance_ops_.empty());

  gscoped_ptr<MaintenanceOp> log_gc(new LogGCOp(this));
  maint_mgr->RegisterOp(log_gc.get());
  maintenance_ops_.push_back(log_gc.release());
}

void TabletPeer::UnregisterMaintenanceOps() {
  DCHECK(state_change_lock_.is_locked());
  for (MaintenanceOp* op : maintenance_ops_) {
    op->Unregister();
  }
  STLDeleteElements(&maintenance_ops_);
}

uint64_t TabletPeer::OnDiskSize() const {
  uint64_t ret = 0;

  if (consensus_) {
    ret += consensus_->OnDiskSize();
  }

  if (tablet_) {
    ret += tablet_->GetTotalSSTFileSizes();//DHQ: 获取SST总大小的
  }

  if (log_) {
    ret += log_->OnDiskSize();
  }

  return ret;
}

scoped_refptr<OperationDriver> TabletPeer::CreateOperationDriver() {
  return scoped_refptr<OperationDriver>(new OperationDriver(
      &operation_tracker_,
      consensus_.get(),
      log_.get(),
      prepare_thread_.get(),
      apply_pool_,
      &operation_order_verifier_,
      tablet_->table_type()));
}

consensus::Consensus::LeaderStatus TabletPeer::LeaderStatus() const {
  scoped_refptr<consensus::Consensus> consensus;
  {
    std::lock_guard<simple_spinlock> lock(lock_);
    consensus = consensus_;
  }
  return consensus ? consensus->leader_status() : consensus::Consensus::LeaderStatus::NOT_LEADER;
}

HybridTime TabletPeer::HtLeaseExpiration() const {
  HybridTime result(consensus_->MajorityReplicatedHtLeaseExpiration(0, MonoTime::kMax), 0);
  return std::max(result, tablet_->mvcc_manager()->LastReplicatedHybridTime());
}

TableType TabletPeer::table_type() {
  // TODO: what if tablet is not set?
  return tablet()->table_type();
}

}  // namespace tablet
}  // namespace yb
