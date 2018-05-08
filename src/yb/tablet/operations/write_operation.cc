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

#include "yb/tablet/operations/write_operation.h"

#include <algorithm>
#include <vector>

#include <boost/optional.hpp>

#include "yb/common/wire_protocol.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/numbers.h"
#include "yb/gutil/walltime.h"
#include "yb/rpc/rpc_context.h"
#include "yb/server/hybrid_clock.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_metrics.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/flag_tags.h"
#include "yb/util/locks.h"
#include "yb/util/trace.h"

DEFINE_test_flag(int32, tablet_inject_latency_on_apply_write_txn_ms, 0,
                 "How much latency to inject when a write operation is applied.");
TAG_FLAG(tablet_inject_latency_on_apply_write_txn_ms, runtime);

namespace yb {
namespace tablet {

using std::lock_guard;
using std::mutex;
using std::unique_ptr;
using consensus::ReplicateMsg;
using consensus::DriverType;
using consensus::WRITE_OP;
using tserver::TabletServerErrorPB;
using tserver::WriteRequestPB;
using tserver::WriteResponsePB;
using strings::Substitute;

WriteOperation::WriteOperation(std::unique_ptr<WriteOperationState> state, DriverType type)
  : Operation(std::move(state), type, OperationType::kWrite),
    start_time_(MonoTime::Now()) {
}

consensus::ReplicateMsgPtr WriteOperation::NewReplicateMsg() {
  auto result = std::make_shared<ReplicateMsg>();
  result->set_op_type(WRITE_OP);
  result->set_allocated_write_request(state()->mutable_request());
  return result;
}

Status WriteOperation::Prepare() {//DHQ: 这个里面啥也没做，后么apply直接搞定了
  TRACE_EVENT0("txn", "WriteOperation::Prepare");
  return Status::OK();
}

void WriteOperation::DoStart() {
  TRACE("Start()");
  state()->tablet()->StartOperation(state()); //DHQ: 实际上里面是设置hybrid time
}

// FIXME: Since this is called as a void in a thread-pool callback,
// it seems pointless to return a Status!
Status WriteOperation::Apply() {
  TRACE_EVENT0("txn", "WriteOperation::Apply");
  TRACE("APPLY: Starting");

  if (PREDICT_FALSE(
          ANNOTATE_UNPROTECTED_READ(FLAGS_tablet_inject_latency_on_apply_write_txn_ms) > 0)) {
    TRACE("Injecting $0ms of latency due to --tablet_inject_latency_on_apply_write_txn_ms",
          FLAGS_tablet_inject_latency_on_apply_write_txn_ms);
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_tablet_inject_latency_on_apply_write_txn_ms));
  }

  Tablet* tablet = state()->tablet();

  tablet->ApplyRowOperations(state());

  return Status::OK();
}

void WriteOperation::PreCommit() { //DHQ: only on leader? 
  TRACE_EVENT0("txn", "WriteOperation::PreCommit");
  TRACE("PRECOMMIT: Releasing row and schema locks");
  // Perform early lock release after we've applied all changes
  state()->ReleaseDocDbLocks(tablet()); //DHQ: AcquireLocksAndPerformDocOperations获得的lock，到PreCommit才释放
}

void WriteOperation::Finish(OperationResult result) {
  TRACE_EVENT0("txn", "WriteOperation::Finish");
  if (PREDICT_FALSE(result == Operation::ABORTED)) {
    TRACE("FINISH: aborting operation");
    state()->Abort();
    return;
  }

  DCHECK_EQ(result, Operation::COMMITTED);
  // Now that all of the changes have been applied and the commit is durable
  // make the changes visible to readers.
  TRACE("FINISH: making edits visible");
  state()->Commit();

  TabletMetrics* metrics = tablet()->metrics();
  if (metrics && type() == consensus::LEADER) {
    auto op_duration_usec = MonoTime::Now().GetDeltaSince(start_time_).ToMicroseconds();
    metrics->write_op_duration_client_propagated_consistency->Increment(op_duration_usec);
  }
}

string WriteOperation::ToString() const {
  MonoTime now(MonoTime::Now());
  MonoDelta d = now.GetDeltaSince(start_time_);
  WallTime abs_time = WallTime_Now() - d.ToSeconds();
  string abs_time_formatted;
  StringAppendStrftime(&abs_time_formatted, "%Y-%m-%d %H:%M:%S", (time_t)abs_time, true);
  return Substitute("WriteOperation [type=$0, start_time=$1, state=$2]",
                    DriverType_Name(type()), abs_time_formatted, state()->ToString());
}

WriteOperationState::WriteOperationState(Tablet* tablet,
                                         const tserver::WriteRequestPB *request,
                                         tserver::WriteResponsePB *response)
    : OperationState(tablet),
      // We need to copy over the request from the RPC layer, as we're modifying it in the tablet
      // layer.
      request_(request ? new WriteRequestPB(*request) : nullptr),
      response_(response) {
}

void WriteOperationState::Abort() {
  if (hybrid_time_.is_valid()) {
    tablet()->mvcc_manager()->Aborted(hybrid_time_);
  }

  ReleaseDocDbLocks(tablet());

  // After aborting, we may respond to the RPC and delete the
  // original request, so null them out here.
  ResetRpcFields();
}

void WriteOperationState::Commit() {
  tablet()->mvcc_manager()->Replicated(hybrid_time_);

  // After committing, we may respond to the RPC and delete the
  // original request, so null them out here.
  ResetRpcFields();
}

void WriteOperationState::ReleaseDocDbLocks(Tablet* tablet) {
  // Free DocDB multi-level locks.
  docdb_locks_.Reset();
}

WriteOperationState::~WriteOperationState() {
  Reset();
  // Ownership is with the Round object, if one exists, else with us.
  if (!consensus_round() && request_ != nullptr) {
    delete request_;
  }
}

void WriteOperationState::Reset() {
  hybrid_time_ = HybridTime::kInvalid;
}

void WriteOperationState::ResetRpcFields() {
  std::lock_guard<simple_spinlock> l(mutex_);
  response_ = nullptr;
}

string WriteOperationState::ToString() const {
  string ts_str;
  if (has_hybrid_time()) {
    ts_str = hybrid_time().ToString();
  } else {
    ts_str = "<unassigned>";
  }

  return Substitute("WriteOperationState $0 [op_id=($1), ts=$2]",
                    this,
                    op_id().ShortDebugString(),
                    ts_str);
}

}  // namespace tablet
}  // namespace yb
