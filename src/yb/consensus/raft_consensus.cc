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

#include "yb/consensus/raft_consensus.h"

#include <algorithm>
#include <iostream>
#include <mutex>

#include <boost/optional.hpp>
#include <gflags/gflags.h>

#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus_peers.h"
#include "yb/consensus/leader_election.h"
#include "yb/consensus/log.h"
#include "yb/consensus/peer_manager.h"
#include "yb/consensus/quorum_util.h"
#include "yb/consensus/replica_state.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/stringprintf.h"
#include "yb/server/clock.h"
#include "yb/server/metadata.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/enums.h"
#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/mem_tracker.h"
#include "yb/util/metrics.h"
#include "yb/util/random.h"
#include "yb/util/random_util.h"
#include "yb/util/threadpool.h"
#include "yb/util/tostring.h"
#include "yb/util/trace.h"
#include "yb/util/url-coding.h"
#include "yb/util/format.h"
#include "yb/util/tsan_util.h"

DEFINE_int32(raft_heartbeat_interval_ms, yb::NonTsanVsTsan(500, 1000),
             "The heartbeat interval for Raft replication. The leader produces heartbeats "
             "to followers at this interval. The followers expect a heartbeat at this interval "
             "and consider a leader to have failed if it misses several in a row.");
TAG_FLAG(raft_heartbeat_interval_ms, advanced);

// Defaults to be the same value as the leader heartbeat interval.
DEFINE_int32(leader_failure_monitor_check_mean_ms, -1,
             "The mean failure-checking interval of the randomized failure monitor. If this "
             "is configured to -1 (the default), uses the value of 'raft_heartbeat_interval_ms'.");
TAG_FLAG(leader_failure_monitor_check_mean_ms, experimental);

// Defaults to half of the mean (above).
DEFINE_int32(leader_failure_monitor_check_stddev_ms, -1,
             "The standard deviation of the failure-checking interval of the randomized "
             "failure monitor. If this is configured to -1 (the default), this is set to "
             "half of the mean check interval.");
TAG_FLAG(leader_failure_monitor_check_stddev_ms, experimental);

DEFINE_double(leader_failure_max_missed_heartbeat_periods, 6.0,
              "Maximum heartbeat periods that the leader can fail to heartbeat in before we "
              "consider the leader to be failed. The total failure timeout in milliseconds is "
              "raft_heartbeat_interval_ms times leader_failure_max_missed_heartbeat_periods. "
              "The value passed to this flag may be fractional.");
TAG_FLAG(leader_failure_max_missed_heartbeat_periods, advanced);

DEFINE_int32(leader_failure_exp_backoff_max_delta_ms, 20 * 1000,
             "Maximum time to sleep in between leader election retries, in addition to the "
             "regular timeout. When leader election fails the interval in between retries "
             "increases exponentially, up to this value.");
TAG_FLAG(leader_failure_exp_backoff_max_delta_ms, experimental);

DEFINE_bool(enable_leader_failure_detection, true,
            "Whether to enable failure detection of tablet leaders. If enabled, attempts will be "
            "made to elect a follower as a new leader when the leader is detected to have failed.");
TAG_FLAG(enable_leader_failure_detection, unsafe);

DEFINE_bool(do_not_start_election_test_only, false,
            "Do not start election even if leader failure is detected. To be used only for unit "
            "testing purposes.");
TAG_FLAG(do_not_start_election_test_only, unsafe);
TAG_FLAG(do_not_start_election_test_only, hidden);

DEFINE_bool(evict_failed_followers, true,
            "Whether to evict followers from the Raft config that have fallen "
            "too far behind the leader's log to catch up normally or have been "
            "unreachable by the leader for longer than "
            "follower_unavailable_considered_failed_sec");
TAG_FLAG(evict_failed_followers, advanced);

DEFINE_bool(follower_reject_update_consensus_requests, false,
            "Whether a follower will return an error for all UpdateConsensus() requests. "
            "Warning! This is only intended for testing.");
TAG_FLAG(follower_reject_update_consensus_requests, unsafe);

DEFINE_bool(follower_fail_all_prepare, false,
            "Whether a follower will fail preparing all operations. "
            "Warning! This is only intended for testing.");
TAG_FLAG(follower_fail_all_prepare, unsafe);

DEFINE_int32(after_stepdown_delay_election_multiplier, 5,
             "After a peer steps down as a leader, the factor with which to multiply "
             "leader_failure_max_missed_heartbeat_periods to get the delay time before starting a "
             "new election.");
TAG_FLAG(after_stepdown_delay_election_multiplier, advanced);
TAG_FLAG(after_stepdown_delay_election_multiplier, hidden);

DECLARE_int32(memory_limit_warn_threshold_percentage);

DEFINE_int32(inject_delay_leader_change_role_append_secs, 0,
              "Amount of time to delay leader from sending replicate of change role. To be used "
              "for unit testing purposes only.");
TAG_FLAG(inject_delay_leader_change_role_append_secs, unsafe);
TAG_FLAG(inject_delay_leader_change_role_append_secs, hidden);

DEFINE_double(return_error_on_change_config, 0.0,
              "Fraction of the time when ChangeConfig will return an error."
              "Warning! This is only intended for testing.");
TAG_FLAG(return_error_on_change_config, unsafe);
TAG_FLAG(return_error_on_change_config, hidden);

METRIC_DEFINE_counter(tablet, follower_memory_pressure_rejections,
                      "Follower Memory Pressure Rejections",
                      yb::MetricUnit::kRequests,
                      "Number of RPC requests rejected due to "
                      "memory pressure while FOLLOWER.");
METRIC_DEFINE_gauge_int64(tablet, raft_term,
                          "Current Raft Consensus Term",
                          yb::MetricUnit::kUnits,
                          "Current Term of the Raft Consensus algorithm. This number increments "
                          "each time a leader election is started.");

DEFINE_int32(leader_lease_duration_ms, yb::consensus::kDefaultLeaderLeaseDurationMs,
             "Leader lease duration. A leader keeps establishing a new lease or extending the "
             "existing one with every UpdateConsensus. A new server is not allowed to serve as a "
             "leader (i.e. serve up-to-date read requests or acknowledge write requests) until a "
             "lease of this duration has definitely expired on the old leader's side.");

DEFINE_int32(ht_lease_duration_ms, 2000,
             "Hybrid time leader lease duration. A leader keeps establishing a new lease or "
             "extending the existing one with every UpdateConsensus. A new server is not allowed "
             "to add entries to RAFT log until a lease of the old leader is expired. 0 to disable."
             );

DEFINE_int32(min_leader_stepdown_retry_interval_ms,
             20 * 1000,
             "Minimum amount of time between successive attempts to perform the leader stepdown "
             "for the same combination of tablet and intended (target) leader. This is needed "
             "to avoid infinite leader stepdown loops when the current leader never has a chance "
             "to update the intended leader with its latest records.");

namespace {

// Return the mean interval at which to check for failures of the
// leader.
int GetFailureMonitorCheckMeanMs() {
  int val = FLAGS_leader_failure_monitor_check_mean_ms;
  if (val < 0) {
    val = FLAGS_raft_heartbeat_interval_ms;
  }
  return val;
}

// Return the standard deviation for the interval at which to check
// for failures of the leader.
int GetFailureMonitorCheckStddevMs() {
  int val = FLAGS_leader_failure_monitor_check_stddev_ms;
  if (val < 0) {
    val = GetFailureMonitorCheckMeanMs() / 2;
  }
  return val;
}

} // anonymous namespace

namespace yb {
namespace consensus {

using log::LogEntryBatch;
using std::shared_ptr;
using std::unique_ptr;
using strings::Substitute;
using tserver::TabletServerErrorPB;
using yb::util::to_underlying;

// Special string that represents any known leader to the failure detector.
static const char* const kTimerId = "election-timer";

scoped_refptr<RaftConsensus> RaftConsensus::Create(
    const ConsensusOptions& options,
    std::unique_ptr<ConsensusMetadata> cmeta,
    const RaftPeerPB& local_peer_pb,
    const scoped_refptr<MetricEntity>& metric_entity,
    const scoped_refptr<server::Clock>& clock,
    ReplicaOperationFactory* operation_factory,
    const shared_ptr<rpc::Messenger>& messenger,
    const scoped_refptr<log::Log>& log,
    const shared_ptr<MemTracker>& parent_mem_tracker,
    const Callback<void(std::shared_ptr<StateChangeContext> context)> mark_dirty_clbk,
    TableType table_type,
    LostLeadershipListener lost_leadership_listener,
    ThreadPool* raft_pool) {
  gscoped_ptr<PeerProxyFactory> rpc_factory(new RpcPeerProxyFactory(messenger));

  // The message queue that keeps track of which operations need to be replicated
  // where.
  gscoped_ptr<PeerMessageQueue> queue(
      new PeerMessageQueue(metric_entity,
                           log,
                           local_peer_pb,
                           options.tablet_id,
                           clock,
                           raft_pool->NewToken(ThreadPool::ExecutionMode::SERIAL)));//DHQ: 从thread_pool创建一个新的token

  DCHECK(local_peer_pb.has_permanent_uuid());
  const string& peer_uuid = local_peer_pb.permanent_uuid();

  // A single Raft thread pool token is shared between RaftConsensus and
  // PeerManager. Because PeerManager is owned by RaftConsensus, it receives a
  // raw pointer to the token, to emphasize that RaftConsensus is responsible
  // for destroying the token.
  unique_ptr<ThreadPoolToken> raft_pool_token(raft_pool->NewToken(
      ThreadPool::ExecutionMode::CONCURRENT));//DHQ: Consensus和PeerManager共享ThreadPoolToken。二者是从属关系。

  // A manager for the set of peers that actually send the operations both remotely
  // and to the local wal.
  gscoped_ptr<PeerManager> peer_manager(
    new PeerManager(options.tablet_id,
                    peer_uuid,
                    rpc_factory.get(),
                    queue.get(),
                    raft_pool_token.get(),
                    log));

  return make_scoped_refptr(new RaftConsensus(
      options,
      std::move(cmeta),
      rpc_factory.Pass(), //DHQ: Pass类似于move?
      queue.Pass(),
      peer_manager.Pass(),
      std::move(raft_pool_token),
      metric_entity,
      peer_uuid,
      clock,
      operation_factory,
      log,
      parent_mem_tracker,
      mark_dirty_clbk,
      table_type,
      std::move(lost_leadership_listener)));
}

RaftConsensus::RaftConsensus(
    const ConsensusOptions& options, std::unique_ptr<ConsensusMetadata> cmeta,
    gscoped_ptr<PeerProxyFactory> proxy_factory,
    gscoped_ptr<PeerMessageQueue> queue, gscoped_ptr<PeerManager> peer_manager,
    unique_ptr<ThreadPoolToken> raft_pool_token,
    const scoped_refptr<MetricEntity>& metric_entity,
    const std::string& peer_uuid, const scoped_refptr<server::Clock>& clock,
    ReplicaOperationFactory* operation_factory, const scoped_refptr<log::Log>& log,
    shared_ptr<MemTracker> parent_mem_tracker,
    Callback<void(std::shared_ptr<StateChangeContext> context)> mark_dirty_clbk,
    TableType table_type,
    LostLeadershipListener lost_leadership_listener)
    : raft_pool_token_(std::move(raft_pool_token)),
      log_(log),
      clock_(clock),
      peer_proxy_factory_(proxy_factory.Pass()),
      peer_manager_(peer_manager.Pass()),
      queue_(queue.Pass()),
      rng_(GetRandomSeed32()),
      failure_monitor_(GetRandomSeed32(), GetFailureMonitorCheckMeanMs(),
                       GetFailureMonitorCheckStddevMs()),
      failure_detector_(new TimedFailureDetector(MonoDelta::FromMilliseconds(
          FLAGS_raft_heartbeat_interval_ms *
          FLAGS_leader_failure_max_missed_heartbeat_periods))),
      withhold_votes_until_(MonoTime::Min()),
      withhold_election_start_until_(MonoTime::Min().ToUint64()),
      mark_dirty_clbk_(std::move(mark_dirty_clbk)),
      shutdown_(false),
      follower_memory_pressure_rejections_(metric_entity->FindOrCreateCounter(
          &METRIC_follower_memory_pressure_rejections)),
      term_metric_(metric_entity->FindOrCreateGauge(&METRIC_raft_term,
                                                    cmeta->current_term())),
      parent_mem_tracker_(std::move(parent_mem_tracker)),
      table_type_(table_type),
      lost_leadership_listener_(std::move(lost_leadership_listener)) {
  DCHECK_NOTNULL(log_.get());

  state_.reset(new ReplicaState(options,
                                peer_uuid,
                                std::move(cmeta),
                                DCHECK_NOTNULL(operation_factory),
                                this));

  peer_manager_->SetConsensus(this);
}

RaftConsensus::~RaftConsensus() {
  Shutdown();
}

Status RaftConsensus::Start(const ConsensusBootstrapInfo& info) {
  RETURN_NOT_OK(ExecuteHook(PRE_START));

  // This just starts the monitor thread -- no failure detector is registered yet.
  if (FLAGS_enable_leader_failure_detection) {
    RETURN_NOT_OK(failure_monitor_.Start());
  }

  // Register the failure detector instance with the monitor.
  // We still have not enabled failure detection for the leader election timer.
  // That happens separately via the helper functions
  // EnsureFailureDetector(Enabled/Disabled)Unlocked();
  RETURN_NOT_OK(failure_monitor_.MonitorFailureDetector(state_->GetOptions().tablet_id,
                                                        failure_detector_));

  {
    ReplicaState::UniqueLock lock;
    RETURN_NOT_OK(state_->LockForStart(&lock));
    state_->ClearLeaderUnlocked();

    RETURN_NOT_OK_PREPEND(state_->StartUnlocked(info.last_id),
                          "Unable to start RAFT ReplicaState");

    LOG_WITH_PREFIX_UNLOCKED(INFO) << "Replica starting. Triggering "
                                   << info.orphaned_replicates.size()
                                   << " pending operations. Active config: "
                                   << state_->GetActiveConfigUnlocked().ShortDebugString();
    for (const auto& replicate : info.orphaned_replicates) {
      ReplicateMsgPtr replicate_ptr = std::make_shared<ReplicateMsg>(*replicate);
      RETURN_NOT_OK(StartReplicaOperationUnlocked(replicate_ptr, HybridTime::kInvalid));
    }

    RETURN_NOT_OK(state_->InitCommittedIndexUnlocked(info.last_committed_id));

    queue_->Init(state_->GetLastReceivedOpIdUnlocked());
  }

  {
    ReplicaState::UniqueLock lock;
    RETURN_NOT_OK(state_->LockForConfigChange(&lock));

    RETURN_NOT_OK(EnsureFailureDetectorEnabledUnlocked());

    // If this is the first term expire the FD immediately so that we have a fast first
    // election, otherwise we just let the timer expire normally.
    if (state_->GetCurrentTermUnlocked() == 0) {
      // Initialize the failure detector timeout to some time in the past so that
      // the next time the failure detector monitor runs it triggers an election
      // (unless someone else requested a vote from us first, which resets the
      // election timer). We do it this way instead of immediately running an
      // election to get a higher likelihood of enough servers being available
      // when the first one attempts an election to avoid multiple election
      // cycles on startup, while keeping that "waiting period" random.
      if (PREDICT_TRUE(FLAGS_enable_leader_failure_detection)) {
        LOG_WITH_PREFIX_UNLOCKED(INFO) << "Consensus starting up: Expiring fail detector timer "
                                          "to make a prompt election more likely";
      }
      RETURN_NOT_OK(ExpireFailureDetectorUnlocked());
    }

    RETURN_NOT_OK(BecomeReplicaUnlocked());
  }

  RETURN_NOT_OK(ExecuteHook(POST_START));//DHQ: 应该是用户注册的hook，不是consensus自身需要的。而是对上接口

  // The context tracks that the current caller does not hold the lock for consensus state.
  // So mark dirty callback, e.g., consensus->ConsensusState() for master consensus callback of
  // SysCatalogStateChanged, can get the lock when needed.
  auto context = std::make_shared<StateChangeContext>(StateChangeReason::CONSENSUS_STARTED, false);
  // Report become visible to the Master.
  MarkDirty(context);

  return Status::OK();
}

bool RaftConsensus::IsRunning() const {
  ReplicaState::UniqueLock lock;
  Status s = state_->LockForRead(&lock);
  if (PREDICT_FALSE(!s.ok())) return false;
  return state_->state() == ReplicaState::kRunning;
}

Status RaftConsensus::EmulateElection() {
  ReplicaState::UniqueLock lock;
  RETURN_NOT_OK(state_->LockForConfigChange(&lock));

  LOG_WITH_PREFIX_UNLOCKED(INFO) << "Emulating election...";

  // Assume leadership of new term.
  RETURN_NOT_OK(IncrementTermUnlocked());
  SetLeaderUuidUnlocked(state_->GetPeerUuid());
  return BecomeLeaderUnlocked();
}

Status RaftConsensus::DoStartElection(
    ElectionMode mode,
    const bool pending_commit,
    const OpId& must_be_committed_opid,
    const std::string& originator_uuid,
    TEST_SuppressVoteRequest suppress_vote_request) {
  TRACE_EVENT2("consensus", "RaftConsensus::StartElection",
               "peer", peer_uuid(),
               "tablet", tablet_id());
  if (FLAGS_do_not_start_election_test_only) {
    LOG(INFO) << "Election start skipped as do_not_start_election_test_only flag is set to true.";
    return Status::OK();
  }
  LeaderElectionPtr election;
  {
    ReplicaState::UniqueLock lock;
    RETURN_NOT_OK(state_->LockForConfigChange(&lock));

    RaftPeerPB::Role active_role = state_->GetActiveRoleUnlocked();
    if (active_role == RaftPeerPB::LEADER) {
      LOG_WITH_PREFIX_UNLOCKED(INFO) << "Not starting election -- already leader";
      return Status::OK();
    } else if (active_role == RaftPeerPB::LEARNER) {
      LOG_WITH_PREFIX_UNLOCKED(INFO) << "Not starting election -- role is LEARNER, pending="
                                     << state_->IsConfigChangePendingUnlocked()
                                     <<", active_role=" << active_role;
      return Status::OK();
    } else if (PREDICT_FALSE(active_role == RaftPeerPB::NON_PARTICIPANT)) {
      // Avoid excessive election noise while in this state.
      RETURN_NOT_OK(SnoozeFailureDetectorUnlocked());
      return STATUS(IllegalState, "Not starting election: Node is currently "
                                  "a non-participant in the raft config",
                                  state_->GetActiveConfigUnlocked().ShortDebugString());
    }

    // Default is to start the election now. But if we are starting a pending election, see if
    // there is an op id pending upon indeed and if it has been committed to the log. The op id
    // could have been cleared if the pending election has already been started or another peer
    // has jumped before we can start.
    bool start_now = true;
    if (pending_commit) {
      const auto required_id =
          must_be_committed_opid.IsInitialized() ? must_be_committed_opid
                                                 : state_->GetPendingElectionOpIdUnlocked();
      const Status advance_committed_index_status =
          state_->AdvanceCommittedIndexUnlocked(required_id);
      if (!advance_committed_index_status.ok()) {
        LOG(WARNING) << "Starting an election but the latest committed OpId is not "
                        "present in this peer's log: "
                     << required_id.ShortDebugString() << ". "
                     << "Status: " << advance_committed_index_status.ToString();
      }
      start_now = state_->HasOpIdCommittedUnlocked(required_id);
    }

    if (start_now) {
      if (state_->HasLeaderUnlocked()) {
        LOG_WITH_PREFIX_UNLOCKED(INFO)
            << "Fail of leader " << state_->GetLeaderUuidUnlocked()
            << " detected. Triggering leader election, mode=" << mode;
      } else {
        LOG_WITH_PREFIX_UNLOCKED(INFO)
            << "Triggering leader election, mode=" << mode;
      }

      // Increment the term.
      RETURN_NOT_OK(IncrementTermUnlocked());

      // Snooze to avoid the election timer firing again as much as possible.
      // We do not disable the election timer while running an election.
      RETURN_NOT_OK(EnsureFailureDetectorEnabledUnlocked());

      MonoDelta timeout = LeaderElectionExpBackoffDeltaUnlocked();
      RETURN_NOT_OK(SnoozeFailureDetectorUnlocked(timeout, ALLOW_LOGGING));

      const RaftConfigPB& active_config = state_->GetActiveConfigUnlocked();
      LOG_WITH_PREFIX_UNLOCKED(INFO) << "Starting election with config: "
                                     << active_config.ShortDebugString();

      // Initialize the VoteCounter.
      int num_voters = CountVoters(active_config);
      int majority_size = MajoritySize(num_voters);
      auto counter = std::make_unique<VoteCounter>(num_voters, majority_size);

      // Vote for ourselves.
      // TODO: Consider using a separate Mutex for voting, which must sync to disk.
      RETURN_NOT_OK(state_->SetVotedForCurrentTermUnlocked(state_->GetPeerUuid()));
      bool duplicate;
      RETURN_NOT_OK(counter->RegisterVote(state_->GetPeerUuid(), VOTE_GRANTED, &duplicate));
      CHECK(!duplicate) << state_->LogPrefixUnlocked()
                        << "Inexplicable duplicate self-vote for term "
                        << state_->GetCurrentTermUnlocked();

      VoteRequestPB request;
      request.set_ignore_live_leader(mode == ELECT_EVEN_IF_LEADER_IS_ALIVE);
      request.set_candidate_uuid(state_->GetPeerUuid());
      request.set_candidate_term(state_->GetCurrentTermUnlocked());
      request.set_tablet_id(state_->GetOptions().tablet_id);
      *request.mutable_candidate_status()->mutable_last_received() =
        state_->GetLastReceivedOpIdUnlocked();

      election.reset(new LeaderElection(
          active_config,
          peer_proxy_factory_.get(),
          request,
          std::move(counter),
          timeout,
          suppress_vote_request,
          Bind(&RaftConsensus::ElectionCallback, this, originator_uuid)));

      // Clear the pending election op id so that we won't start the same pending election again.
      state_->ClearPendingElectionOpIdUnlocked();

    } else if (pending_commit && must_be_committed_opid.IsInitialized()) {
      // Queue up the pending op id if specified.
      state_->SetPendingElectionOpIdUnlocked(must_be_committed_opid);
      LOG(INFO) << "Leader election is pending upon log commitment of OpId "
                << must_be_committed_opid.ShortDebugString();
    }
  }

  // Start the election outside the lock.
  if (election) {
    election->Run();
  }

  return Status::OK();
}

Status RaftConsensus::WaitUntilLeaderForTests(const MonoDelta& timeout) {
  MonoTime deadline = MonoTime::Now();
  deadline.AddDelta(timeout);
  while (MonoTime::Now().ComesBefore(deadline)) {
    if (leader_status() == LeaderStatus::LEADER_AND_READY) {
      return Status::OK();
    }
    SleepFor(MonoDelta::FromMilliseconds(10));
  }

  return STATUS(TimedOut, Substitute("Peer $0 is not leader of tablet $1 after $2. Role: $3",
                                     peer_uuid(), tablet_id(), timeout.ToString(), role()));
}

string RaftConsensus::ServersInTransitionMessage() {
  string err_msg;
  const RaftConfigPB& active_config = state_->GetActiveConfigUnlocked();
  const RaftConfigPB& committed_config = state_->GetCommittedConfigUnlocked();
  int servers_in_transition = CountServersInTransition(active_config);
  int committed_servers_in_transition = CountServersInTransition(committed_config);
  LOG(INFO) << Substitute("Active config has $0 and committed has $1 servers in transition.",
                          servers_in_transition, committed_servers_in_transition);
  if (servers_in_transition != 0 || committed_servers_in_transition != 0) {
    err_msg = Substitute("Leader not ready to step down as there are $0 active config peers"
                         " in transition, $1 in committed. Configs:\nactive=$2\ncommit=$3",
                         servers_in_transition, committed_servers_in_transition,
                         active_config.ShortDebugString(), committed_config.ShortDebugString());
    LOG(INFO) << err_msg;
  }
  return err_msg;
}

Status RaftConsensus::StepDown(const LeaderStepDownRequestPB* req, LeaderStepDownResponsePB* resp) {
  TRACE_EVENT0("consensus", "RaftConsensus::StepDown");
  ReplicaState::UniqueLock lock;
  RETURN_NOT_OK(state_->LockForConfigChange(&lock));

  // A sanity check that this request was routed to the correct RaftConsensus.
  const auto& tablet_id = req->tablet_id();
  if (tablet_id != this->tablet_id()) {
    resp->mutable_error()->set_code(TabletServerErrorPB::UNKNOWN_ERROR);
    const auto msg = Format(
        "Received a leader stepdown operation for wrong tablet id: $0, must be: $1",
        tablet_id, this->tablet_id());
    LOG_WITH_PREFIX_UNLOCKED(ERROR) << msg;
    StatusToPB(STATUS(IllegalState, msg), resp->mutable_error()->mutable_status());
    return Status::OK();
  }

  if (state_->GetActiveRoleUnlocked() != RaftPeerPB::LEADER) {
    resp->mutable_error()->set_code(TabletServerErrorPB::NOT_THE_LEADER);
    StatusToPB(STATUS(IllegalState, "Not currently leader"),
               resp->mutable_error()->mutable_status());
    // We return OK so that the tablet service won't overwrite the error code.
    return Status::OK();
  }

  // The leader needs to be ready to perform a step down. There should be no PRE_VOTER in both
  // active and committed configs - ENG-557.
  string err_msg = ServersInTransitionMessage();
  if (!err_msg.empty()) {
    resp->mutable_error()->set_code(TabletServerErrorPB::LEADER_NOT_READY_TO_STEP_DOWN);
    StatusToPB(STATUS(IllegalState, err_msg), resp->mutable_error()->mutable_status());
    return Status::OK();
  }

  std::string new_leader_uuid;
  // If a new leader is nominated, find it among peers to send RunLeaderElection request.
  // See https://ramcloud.stanford.edu/~ongaro/thesis.pdf, section 3.10 for this mechanism
  // to transfer the leadership.
  if (req->has_new_leader_uuid()) {//DHQ: 让对方发起选举。。
    new_leader_uuid = req->new_leader_uuid();
    if (!queue_->CanPeerBecomeLeader(new_leader_uuid)) {
      resp->mutable_error()->set_code(TabletServerErrorPB::LEADER_NOT_READY_TO_STEP_DOWN);
      StatusToPB(
          STATUS(IllegalState, "Suggested peer is not caught up yet"),
          resp->mutable_error()->mutable_status());
      // We return OK so that the tablet service won't overwrite the error code.
      return Status::OK();
    }
    const auto& local_peer_uuid = state_->GetPeerUuid();
    const auto leadership_transfer_description =
        Format("tablet $0 from $1 to $2", tablet_id, local_peer_uuid, new_leader_uuid);
    if (new_leader_uuid == protege_leader_uuid_ && election_lost_by_protege_at_) {
      const MonoDelta time_since_election_loss_by_protege =
          MonoTime::Now() - election_lost_by_protege_at_;
      if (time_since_election_loss_by_protege.ToMilliseconds() <
              FLAGS_min_leader_stepdown_retry_interval_ms) {
        LOG(INFO) << "Rejecting leader stepdown request for " << leadership_transfer_description
                  << " because the intended leader already lost an election only "
                  << ToString(time_since_election_loss_by_protege) << " ago (within "
                  << FLAGS_min_leader_stepdown_retry_interval_ms << " ms).";
        resp->mutable_error()->set_code(TabletServerErrorPB::LEADER_NOT_READY_TO_STEP_DOWN);
        resp->set_time_since_election_failure_ms(
            time_since_election_loss_by_protege.ToMilliseconds());
        StatusToPB(STATUS(IllegalState, "Suggested peer lost an election recently"),
                   resp->mutable_error()->mutable_status());
        // We return OK so that the tablet service won't overwrite the error code.
        return Status::OK();
      }
      election_lost_by_protege_at_ = MonoTime();
    }
    bool new_leader_found = false;
    const RaftConfigPB& active_config = state_->GetActiveConfigUnlocked();
    for (const RaftPeerPB& peer : active_config.peers()) {
      if (peer.member_type() == RaftPeerPB::VOTER &&
          peer.permanent_uuid() == new_leader_uuid) {
        auto election_state = std::make_shared<RunLeaderElectionState>();
        RETURN_NOT_OK(peer_proxy_factory_->NewProxy(peer, &election_state->proxy));
        election_state->req.set_originator_uuid(req->dest_uuid());
        election_state->req.set_dest_uuid(new_leader_uuid);
        election_state->req.set_tablet_id(tablet_id);
        election_state->req.mutable_committed_index()->CopyFrom(
            state_->GetCommittedOpIdUnlocked());
        election_state->proxy->RunLeaderElectionAsync(
            &election_state->req, &election_state->resp, &election_state->rpc,
            std::bind(&RaftConsensus::RunLeaderElectionResponseRpcCallback, this,
                election_state));
        new_leader_found = true;
        LOG(INFO) << "Transferring leadership of " << leadership_transfer_description;
        break;
      }
    }
    if (!new_leader_found) {
      LOG(WARNING) << "New leader " << new_leader_uuid << " not found among " << tablet_id
                   << " tablet peers.";
      resp->mutable_error()->set_code(TabletServerErrorPB::LEADER_NOT_READY_TO_STEP_DOWN);
      StatusToPB(STATUS(IllegalState, "New leader not found among peers"),
                 resp->mutable_error()->mutable_status());
      // We return OK so that the tablet service won't overwrite the error code.
      return Status::OK();
    }
  }

  RETURN_NOT_OK(BecomeReplicaUnlocked());

  WithholdElectionAfterStepDown(new_leader_uuid);

  return Status::OK();
}

Status RaftConsensus::ElectionLostByProtege(const std::string& election_lost_by_uuid) {
  if (election_lost_by_uuid.empty()) {
    return STATUS(InvalidArgument, "election_lost_by_uuid could not be empty");
  }

  auto start_election = false;
  {
    ReplicaState::UniqueLock lock;
    RETURN_NOT_OK(state_->LockForConfigChange(&lock));
    if (election_lost_by_uuid == protege_leader_uuid_) {
      LOG_WITH_PREFIX_UNLOCKED(INFO) << "Our protege " << election_lost_by_uuid
                                     << ", lost election. Has leader: "
                                     << state_->HasLeaderUnlocked();
      withhold_election_start_until_.store(MonoTime::Min().ToUint64(), std::memory_order_relaxed);
      election_lost_by_protege_at_ = MonoTime::Now();

      start_election = !state_->HasLeaderUnlocked();
    }
  }

  if (start_election) {
    return StartElection(NORMAL_ELECTION);
  }

  return Status::OK();
}

void RaftConsensus::WithholdElectionAfterStepDown(const std::string& protege_uuid) {
  DCHECK(state_->IsLocked());
  protege_leader_uuid_ = protege_uuid;
  auto timeout = MonoDelta::FromMilliseconds(
      FLAGS_after_stepdown_delay_election_multiplier *
      FLAGS_leader_failure_max_missed_heartbeat_periods *
      FLAGS_raft_heartbeat_interval_ms);
  auto deadline = MonoTime::Now() + timeout;
  withhold_election_start_until_.store(deadline.ToUint64(), std::memory_order_release);
  election_lost_by_protege_at_ = MonoTime();
}

void RaftConsensus::RunLeaderElectionResponseRpcCallback(
    shared_ptr<RunLeaderElectionState> election_state) {
  // Check for RPC errors.
  if (!election_state->rpc.status().ok()) {
    LOG(WARNING) << "RPC error from RunLeaderElection() call to peer "
                 << election_state->req.dest_uuid() << ": "
                 << election_state->rpc.status().ToString();
  // Check for tablet errors.
  } else if (election_state->resp.has_error()) {
    LOG(WARNING) << "Tablet error from RunLeaderElection() call to peer "
                 << election_state->req.dest_uuid() << ": "
                 << StatusFromPB(election_state->resp.error().status()).ToString();
  }
}
//DHQ: KTimerId就是leader election用的。超时则触发选举. 应该是在一段时间没有收到leader消息后触发这个函数执行
void RaftConsensus::ReportFailureDetected(const std::string& name, const Status& msg) {
  DCHECK_EQ(name, kTimerId);

  MonoTime now;
  const auto kMinTime = MonoTime::Min().ToUint64();
  for (;;) {
    // Do not start election for an extended period of time if we were recently stepped down.
    auto old_value = withhold_election_start_until_.load(std::memory_order_acquire);

    if (old_value == kMinTime) {
      break;
    }

    if (!now.Initialized()) {
      now = MonoTime::Now();
    }

    if (now < MonoTime::FromUint64(old_value)) {
      LOG(INFO) << "Skipping election due to delayed timeout.";
      return;
    }

    // If we ever stepped down and then delayed election start did get scheduled, reset that we
    // are out of that extra delay mode.
    if (withhold_election_start_until_.compare_exchange_weak(
        old_value, kMinTime, std::memory_order_release)) {
      break;
    }
  }

  // Start an election.
  LOG_WITH_PREFIX(INFO) << "ReportFailDetected: Starting NORMAL_ELECTION...";
  Status s = StartElection(NORMAL_ELECTION);
  if (PREDICT_FALSE(!s.ok())) {
    LOG_WITH_PREFIX(WARNING) << "Failed to trigger leader election: " << s.ToString();
  }
}

Status RaftConsensus::BecomeLeaderUnlocked() {
  DCHECK(state_->IsLocked());
  TRACE_EVENT2("consensus", "RaftConsensus::BecomeLeaderUnlocked",
               "peer", peer_uuid(),
               "tablet", tablet_id());
  LOG_WITH_PREFIX_UNLOCKED(INFO) << "Becoming Leader. State: " << state_->ToStringUnlocked();

  // Disable FD while we are leader.
  RETURN_NOT_OK(EnsureFailureDetectorDisabledUnlocked());

  // Don't vote for anyone if we're a leader.
  withhold_votes_until_ = MonoTime::Max();

  leader_no_op_committed_ = false;
  queue_->RegisterObserver(this); //DHQ: Leader上有用的Ovserver，上层知道commit了
  RETURN_NOT_OK(RefreshConsensusQueueAndPeersUnlocked());

  // Initiate a NO_OP operation that is sent at the beginning of every term
  // change in raft.
  auto replicate = std::make_shared<ReplicateMsg>();
  replicate->set_op_type(NO_OP);
  replicate->mutable_noop_request(); // Define the no-op request field.
  LOG(INFO) << "Sending NO_OP at op " << state_->GetCommittedOpIdUnlocked();
  // This committed OpId is used for tablet bootstrap for RocksDB-backed tables.
  replicate->mutable_committed_op_id()->CopyFrom(state_->GetCommittedOpIdUnlocked());

  // TODO: We should have no-ops (?) and config changes be COMMIT_WAIT
  // operations. See KUDU-798.
  // Note: This hybrid_time has no meaning from a serialization perspective
  // because this method is not executed on the TabletPeer's prepare thread.
  replicate->set_hybrid_time(clock_->Now().ToUint64());

  scoped_refptr<ConsensusRound> round(new ConsensusRound(this, replicate));
  round->SetConsensusReplicatedCallback(Bind(&RaftConsensus::NonTxRoundReplicationFinished,
                                             Unretained(this),
                                             Unretained(round.get()),
                                             Bind(&DoNothingStatusCB)));
  RETURN_NOT_OK(AppendNewRoundToQueueUnlocked(round));

  return Status::OK();
}

Status RaftConsensus::BecomeReplicaUnlocked() {
  LOG_WITH_PREFIX_UNLOCKED(INFO) << "Becoming Follower/Learner. State: "
                                 << state_->ToStringUnlocked();

  if (state_->GetActiveRoleUnlocked() == RaftPeerPB::LEADER) {
    WithholdElectionAfterStepDown(std::string());
  }

  state_->ClearLeaderUnlocked();

  // FD should be running while we are a follower.
  RETURN_NOT_OK(EnsureFailureDetectorEnabledUnlocked());

  // Now that we're a replica, we can allow voting for other nodes.
  withhold_votes_until_ = MonoTime::Min();

  const Status unregister_observer_status = queue_->UnRegisterObserver(this);
  if (!unregister_observer_status.IsNotFound()) {
    RETURN_NOT_OK(unregister_observer_status);
  }
  // Deregister ourselves from the queue. We don't care what get's replicated, since
  // we're stepping down.
  queue_->SetNonLeaderMode();

  peer_manager_->Close();

  if (lost_leadership_listener_) {
    lost_leadership_listener_();
  }

  return Status::OK();
}

Status RaftConsensus::Replicate(const ConsensusRoundPtr& round) {
  return ReplicateBatch({ round });
}

Status RaftConsensus::ReplicateBatch(const ConsensusRounds& rounds) {
  RETURN_NOT_OK(ExecuteHook(PRE_REPLICATE));
  {
    ReplicaState::UniqueLock lock;
#ifndef NDEBUG
    for (const auto& round : rounds) {
      DCHECK(!round->replicate_msg()->has_id()) << "Should not have an OpId yet: "
                                                << round->replicate_msg()->DebugString();
    }
#endif
    RETURN_NOT_OK(state_->LockForReplicate(&lock));//DHQ: Conesensus与 state_ 是一一对应的
    auto current_term = state_->GetCurrentTermUnlocked();

    for (const auto& round : rounds) {
      RETURN_NOT_OK(round->CheckBoundTerm(current_term));
    }
    RETURN_NOT_OK(AppendNewRoundsToQueueUnlocked(rounds));
  }

  peer_manager_->SignalRequest(RequestTriggerMode::NON_EMPTY_ONLY);//DHQ: Replicate调用这个，然后调用对应Peer的SignalRequest
  RETURN_NOT_OK(ExecuteHook(POST_REPLICATE));
  return Status::OK();
}

Status RaftConsensus::CheckLeadershipAndBindTerm(const scoped_refptr<ConsensusRound>& round) {
  // We are using a lock-free GetRoleAndTerm, and therefore we might be in the middle of an
  // operation that is holding the consensus state lock and is about to modify the role + term
  // atomic field (e.g. if we've stopped being leader, and optionally, if the term has increased).
  // However, we'll handle that in CheckBoundTerm, which is only executed while holding the state
  // lock, and that error should be a rare occurrence.
  const auto role_and_term = state_->GetRoleAndTerm();
  const auto role = role_and_term.first;
  const auto term = role_and_term.second;
  if (role != RaftPeerPB::Role::RaftPeerPB_Role_LEADER) {
    // OK to take the lock here, because this error case should be rare.
    ReplicaState::UniqueLock lock;
    RETURN_NOT_OK(state_->LockForReplicate(&lock, *round->replicate_msg()));
    const ConsensusStatePB cstate = state_->ConsensusStateUnlocked(CONSENSUS_CONFIG_ACTIVE);
    return STATUS(IllegalState, Substitute("Replica $0 is not leader of this config. Role: $1. "
                                           "Consensus state: $2",
                                           peer_uuid(),
                                           RaftPeerPB::Role_Name(role),
                                           cstate.ShortDebugString()));
  }

  round->BindToTerm(term);
  return Status::OK();
}

Status RaftConsensus::AppendNewRoundToQueueUnlocked(const scoped_refptr<ConsensusRound>& round) { //DHQ: 只有一个round，原来仅用于ConfChange，但是我们split时，也需要用到这个
  return AppendNewRoundsToQueueUnlocked({ round });
}

Status RaftConsensus::AppendNewRoundsToQueueUnlocked(
    const std::vector<scoped_refptr<ConsensusRound>>& rounds) {
  for (auto iter = rounds.begin(); iter != rounds.end(); ++iter) {
    const ConsensusRoundPtr& round = *iter;

    state_->NewIdUnlocked(round->replicate_msg()->mutable_id());

    ReplicateMsg* const replicate_msg = round->replicate_msg().get();

    // In YB tables we include the last committed id into every REPLICATE log record so we can
    // perform local bootstrap more efficiently.
    replicate_msg->mutable_committed_op_id()->CopyFrom(state_->GetCommittedOpIdUnlocked());

    // We use this callback to transform write operations by substituting the hybrid_time into
    // the write batch inside the write operation.
    //
    // TODO: we could allocate multiple HybridTimes in batch, only reading system clock once.
    auto* const append_cb = round->append_callback();
    if (append_cb != nullptr) {
      append_cb->HandleConsensusAppend();
    }

    Status s = state_->AddPendingOperation(round);//DHQ: PendingOperation,加到 state_ 上
    if (!s.ok()) {
      // Iterate rounds in the reverse order and release ids. We add one to the iterator before
      // converting to a reverse iterator to get a reverse iterator staring with the current
      // element.
      for (;;) {
        RollbackIdAndDeleteOpId((**iter).replicate_msg(), false /* should_exists */);
        if (iter == rounds.begin()) {
          break;
        }
        --iter;
      }
      return s;
    }
  }

  std::vector<ReplicateMsgPtr> replicate_msgs;
  replicate_msgs.reserve(rounds.size());
  for (const auto& round : rounds) {
    replicate_msgs.push_back(round->replicate_msg());
  }
  Status s = queue_->AppendOperations(replicate_msgs, Bind(DoNothingStatusCB));//DHQ: Operation 加到queue, 里面先启动追加到本地log_cache_

  // Handle Status::ServiceUnavailable(), which means the queue is full.
  // TODO: what are we doing about other errors here? Should we also release OpIds in those cases?
  if (PREDICT_FALSE(s.IsServiceUnavailable())) {
    for (auto iter = replicate_msgs.rbegin(); iter != replicate_msgs.rend(); ++iter) {
      RollbackIdAndDeleteOpId(*iter, true /* should_exists */);
      LOG_WITH_PREFIX_UNLOCKED(WARNING) << ": Could not append replicate request "
                   << "to the queue. Queue is Full. "
                   << "Queue metrics: " << queue_->ToString();

      // TODO Possibly evict a dangling peer from the configuration here.
      // TODO count of number of ops failed due to consensus queue overflow.
    }
  }

  RETURN_NOT_OK_PREPEND(s, "Unable to append operations to consensus queue");
  state_->UpdateLastReceivedOpIdUnlocked(rounds.back()->id());
  return Status::OK();
}

void RaftConsensus::UpdateMajorityReplicated(
    const MajorityReplicatedData& majority_replicated_data,
    OpId* committed_index) {
  ReplicaState::UniqueLock lock;
  Status s = state_->LockForMajorityReplicatedIndexUpdate(&lock);
  if (PREDICT_FALSE(!s.ok())) {
    LOG_WITH_PREFIX(WARNING)
        << "Unable to take state lock to update committed index: "
        << s.ToString();
    return;
  }

  state_->SetMajorityReplicatedLeaseExpirationUnlocked(majority_replicated_data);
  leader_lease_wait_cond_.notify_all();

  VLOG_WITH_PREFIX_UNLOCKED(1) << "Marking majority replicated up to "
      << majority_replicated_data.op_id.ShortDebugString();
  TRACE("Marking majority replicated up to $0", majority_replicated_data.op_id.ShortDebugString());
  bool committed_index_changed = false;
  s = state_->UpdateMajorityReplicatedUnlocked(
      majority_replicated_data.op_id, committed_index, &committed_index_changed);
  if (majority_replicated_listener_) {
    majority_replicated_listener_();
  }
  if (PREDICT_FALSE(!s.ok())) {
    string msg = Substitute("Unable to mark committed up to $0: $1",
                            majority_replicated_data.op_id.ShortDebugString(),
                            s.ToString());
    TRACE(msg);
    LOG_WITH_PREFIX_UNLOCKED(WARNING) << msg;
    return;
  }

  if (committed_index_changed &&
      state_->GetActiveRoleUnlocked() == RaftPeerPB::LEADER) {
    lock.unlock();
    // No need to hold the lock while calling SignalRequest.
    peer_manager_->SignalRequest(RequestTriggerMode::NON_EMPTY_ONLY);
  }
}

void RaftConsensus::NotifyTermChange(int64_t term) {
  ReplicaState::UniqueLock lock;
  Status s = state_->LockForConfigChange(&lock);
  if (PREDICT_FALSE(!s.ok())) {
    LOG(WARNING) << state_->LogPrefixThreadSafe() << "Unable to lock ReplicaState for config change"
                 << " when notified of new term " << term << ": " << s.ToString();
    return;
  }
  WARN_NOT_OK(HandleTermAdvanceUnlocked(term), "Couldn't advance consensus term.");
}
//DHQ: Follower失败处理
void RaftConsensus::NotifyFailedFollower(const string& uuid,
                                         int64_t term,
                                         const std::string& reason) {
  // Common info used in all of the log messages within this method.
  string fail_msg = Substitute("Processing failure of peer $0 in term $1 ($2): ",
                               uuid, term, reason);

  if (!FLAGS_evict_failed_followers) {
    LOG(INFO) << state_->LogPrefixThreadSafe() << fail_msg
              << "Eviction of failed followers is disabled. Doing nothing.";
    return;
  }

  RaftConfigPB committed_config;
  {
    ReplicaState::UniqueLock lock;
    Status s = state_->LockForRead(&lock);
    if (PREDICT_FALSE(!s.ok())) {
      LOG(WARNING) << state_->LogPrefixThreadSafe() << fail_msg
                   << "Unable to lock ReplicaState for read: " << s.ToString();
      return;
    }

    int64_t current_term = state_->GetCurrentTermUnlocked();
    if (current_term != term) {
      LOG_WITH_PREFIX_UNLOCKED(INFO) << fail_msg << "Notified about a follower failure in "
                                     << "previous term " << term << ", but a leader election "
                                     << "likely occurred since the failure was detected. "
                                     << "Doing nothing.";
      return;
    }

    if (state_->IsConfigChangePendingUnlocked()) {
      LOG_WITH_PREFIX_UNLOCKED(INFO) << fail_msg << "There is already a config change operation "
                                     << "in progress. Unable to evict follower until it completes. "
                                     << "Doing nothing.";
      return;
    }
    committed_config = state_->GetCommittedConfigUnlocked();
  }

  // Run config change on thread pool after dropping ReplicaState lock.
  WARN_NOT_OK(raft_pool_token_->SubmitClosure(Bind(&RaftConsensus::TryRemoveFollowerTask,
                                               this, uuid, committed_config, reason)),
              state_->LogPrefixThreadSafe() + "Unable to start RemoteFollowerTask");
}

void RaftConsensus::TryRemoveFollowerTask(const string& uuid,
                                          const RaftConfigPB& committed_config,
                                          const std::string& reason) {
  ChangeConfigRequestPB req;
  req.set_tablet_id(tablet_id());
  req.mutable_server()->set_permanent_uuid(uuid);
  req.set_type(REMOVE_SERVER);
  req.set_cas_config_opid_index(committed_config.opid_index());
  LOG(INFO) << state_->LogPrefixThreadSafe() << "Attempting to remove follower "
            << uuid << " from the Raft config at commit index "
            << committed_config.opid_index() << ". Reason: " << reason;
  boost::optional<TabletServerErrorPB::Code> error_code;
  WARN_NOT_OK(ChangeConfig(req, Bind(&DoNothingStatusCB), &error_code),
              state_->LogPrefixThreadSafe() + "Unable to remove follower " + uuid);//DHQ: 走ChangeConfig流程
}

Status RaftConsensus::Update(ConsensusRequestPB* request,
                             ConsensusResponsePB* response) {//DHQ: 这个就是AppendEntries, TabletServiceImpl::UpdateTransaction调用

  if (PREDICT_FALSE(FLAGS_follower_reject_update_consensus_requests)) {
    return STATUS(IllegalState, "Rejected: --follower_reject_update_consensus_requests "
                                "is set to true.");
  }

  RETURN_NOT_OK(ExecuteHook(PRE_UPDATE));
  response->set_responder_uuid(state_->GetPeerUuid());

  VLOG_WITH_PREFIX(2) << "Replica received request: " << request->ShortDebugString();

  // see var declaration
  std::lock_guard<simple_spinlock> lock(update_lock_);
  Status s = UpdateReplica(request, response);//DHQ:调用Replica(follower)的处理函数
  if (PREDICT_FALSE(VLOG_IS_ON(1))) {
    if (request->ops_size() == 0) {
      VLOG_WITH_PREFIX(1) << "Replica replied to status only request. Replica: "
                          << state_->ToString() << ". Response: " << response->ShortDebugString();
    }
  }
  RETURN_NOT_OK(s);

  RETURN_NOT_OK(ExecuteHook(POST_UPDATE));
  return Status::OK();
}

// Helper function to check if the op is a non-Operation op.
static bool IsConsensusOnlyOperation(OperationType op_type) {//DHQ: 这个都是raft自身产生的，与应用无关
  return op_type == NO_OP || op_type == CHANGE_CONFIG_OP;
}

// Helper to check if the op is Change Config op.
static bool IsChangeConfigOperation(OperationType op_type) {
  return op_type == CHANGE_CONFIG_OP;
}

Status RaftConsensus::StartReplicaOperationUnlocked(
    const ReplicateMsgPtr& msg, HybridTime propagated_safe_time) {
  if (IsConsensusOnlyOperation(msg->op_type())) {
    return StartConsensusOnlyRoundUnlocked(msg);
  }

  if (PREDICT_FALSE(FLAGS_follower_fail_all_prepare)) {
    return STATUS(IllegalState, "Rejected: --follower_fail_all_prepare "
                                "is set to true.");
  }

  VLOG_WITH_PREFIX_UNLOCKED(1) << "Starting operation: " << msg->id().ShortDebugString();
  scoped_refptr<ConsensusRound> round(new ConsensusRound(this, msg)); //DHQ: 创建了ConsensuRound
  ConsensusRound* round_ptr = round.get();
  RETURN_NOT_OK(state_->GetReplicaOperationFactoryUnlocked()->
      StartReplicaOperation(round, propagated_safe_time));//DHQ: follower，也需要启动一个Operation. StartReplicaOperation应该是TabletPeer的函数
  return state_->AddPendingOperation(round_ptr);//DHQ: 对于非leader，主要是插入消息id
}

std::string RaftConsensus::LeaderRequest::OpsRangeString() const {
  std::string ret;
  ret.reserve(100);
  ret.push_back('[');
  if (!messages.empty()) {
    const OpId& first_op = (*messages.begin())->id();
    const OpId& last_op = (*messages.rbegin())->id();
    strings::SubstituteAndAppend(&ret, "$0.$1-$2.$3",
                                 first_op.term(), first_op.index(),
                                 last_op.term(), last_op.index());
  }
  ret.push_back(']');
  return ret;
}

void RaftConsensus::DeduplicateLeaderRequestUnlocked(ConsensusRequestPB* rpc_req,
                                                     LeaderRequest* deduplicated_req) {
  const OpId& last_committed = state_->GetCommittedOpIdUnlocked();

  // The leader's preceding id.
  deduplicated_req->preceding_opid = &rpc_req->preceding_id();

  int64_t dedup_up_to_index = state_->GetLastReceivedOpIdUnlocked().index();

  deduplicated_req->first_message_idx = -1;

  // In this loop we discard duplicates and advance the leader's preceding id
  // accordingly.
  for (int i = 0; i < rpc_req->ops_size(); i++) {
    ReplicateMsg* leader_msg = rpc_req->mutable_ops(i);

    if (leader_msg->id().index() <= last_committed.index()) {
      VLOG_WITH_PREFIX_UNLOCKED(2) << "Skipping op id " << leader_msg->id()
                                   << " (already committed)";
      deduplicated_req->preceding_opid = &leader_msg->id();
      continue;
    }

    if (leader_msg->id().index() <= dedup_up_to_index) {
      // If the index is uncommitted and below our match index, then it must be in the
      // pendings set.
      scoped_refptr<ConsensusRound> round =
          state_->GetPendingOpByIndexOrNullUnlocked(leader_msg->id().index());
      DCHECK(round);

      // If the OpIds match, i.e. if they have the same term and id, then this is just
      // duplicate, we skip...
      if (OpIdEquals(round->replicate_msg()->id(), leader_msg->id())) {
        VLOG_WITH_PREFIX_UNLOCKED(2) << "Skipping op id " << leader_msg->id()
                                     << " (already replicated)";
        deduplicated_req->preceding_opid = &leader_msg->id();
        continue;
      }

      // ... otherwise we must adjust our match index, i.e. all messages from now on
      // are "new"
      dedup_up_to_index = leader_msg->id().index();
    }

    if (deduplicated_req->first_message_idx == -1) {
      deduplicated_req->first_message_idx = i;
    }
    deduplicated_req->messages.emplace_back(leader_msg);
  }

  if (deduplicated_req->messages.size() != rpc_req->ops_size()) {
    LOG_WITH_PREFIX_UNLOCKED(INFO) << "Deduplicated request from leader. Original: "
                          << rpc_req->preceding_id() << "->" << OpsRangeString(*rpc_req)
                          << "   Dedup: " << *deduplicated_req->preceding_opid << "->"
                          << deduplicated_req->OpsRangeString();
  }
}

Status RaftConsensus::HandleLeaderRequestTermUnlocked(const ConsensusRequestPB* request,
                                                      ConsensusResponsePB* response) {
  // Do term checks first:
  if (PREDICT_FALSE(request->caller_term() != state_->GetCurrentTermUnlocked())) {

    // If less, reject.
    if (request->caller_term() < state_->GetCurrentTermUnlocked()) {
      string msg = Substitute("Rejecting Update request from peer $0 for earlier term $1. "
                              "Current term is $2. Ops: $3",

                              request->caller_uuid(),
                              request->caller_term(),
                              state_->GetCurrentTermUnlocked(),
                              OpsRangeString(*request));
      LOG_WITH_PREFIX_UNLOCKED(INFO) << msg;
      FillConsensusResponseError(response,
                                 ConsensusErrorPB::INVALID_TERM,
                                 STATUS(IllegalState, msg));
      return Status::OK();
    } else {
      RETURN_NOT_OK(HandleTermAdvanceUnlocked(request->caller_term()));
    }
  }
  return Status::OK();
}

Status RaftConsensus::EnforceLogMatchingPropertyMatchesUnlocked(const LeaderRequest& req,
                                                                ConsensusResponsePB* response) {

  bool term_mismatch;
  if (state_->IsOpCommittedOrPending(*req.preceding_opid, &term_mismatch)) {
    return Status::OK();
  }

  string error_msg = Substitute(
    "Log matching property violated."
    " Preceding OpId in replica: $0. Preceding OpId from leader: $1. ($2 mismatch)",
    state_->GetLastReceivedOpIdUnlocked().ShortDebugString(),
    req.preceding_opid->ShortDebugString(),
    term_mismatch ? "term" : "index");

  FillConsensusResponseError(response,
                             ConsensusErrorPB::PRECEDING_ENTRY_DIDNT_MATCH,
                             STATUS(IllegalState, error_msg));

  LOG_WITH_PREFIX_UNLOCKED(INFO) << "Refusing update from remote peer "
                        << req.leader_uuid << ": " << error_msg;

  // If the terms mismatch we abort down to the index before the leader's preceding,
  // since we know that is the last opid that has a chance of not being overwritten.
  // Aborting preemptively here avoids us reporting a last received index that is
  // possibly higher than the leader's causing an avoidable cache miss on the leader's
  // queue.
  //
  // TODO: this isn't just an optimization! if we comment this out, we get
  // failures on raft_consensus-itest a couple percent of the time! Should investigate
  // why this is actually critical to do here, as opposed to just on requests that
  // append some ops.
  if (term_mismatch) {
    return state_->AbortOpsAfterUnlocked(req.preceding_opid->index() - 1);
  }

  return Status::OK();
}

Status RaftConsensus::CheckLeaderRequestOpIdSequence(
    const LeaderRequest& deduped_req,
    ConsensusRequestPB* request) {
  Status sequence_check_status;
  const OpId* prev = deduped_req.preceding_opid;
  for (const auto& message : deduped_req.messages) {
    sequence_check_status = ReplicaState::CheckOpInSequence(*prev, message->id());
    if (PREDICT_FALSE(!sequence_check_status.ok())) {
      LOG(ERROR) << "Leader request contained out-of-sequence messages. Status: "
          << sequence_check_status.ToString() << ". Leader Request: "
          << request->ShortDebugString();
      break;
    }
    prev = &message->id();
  }

  // We only release the messages from the request after the above check so that that we can print
  // the original request, if it fails.
  if (!deduped_req.messages.empty()) {
    // We take ownership of the deduped ops.
    DCHECK_GE(deduped_req.first_message_idx, 0);
    request->mutable_ops()->ExtractSubrange(
        deduped_req.first_message_idx,
        deduped_req.messages.size(),
        nullptr);
  }

  return sequence_check_status;
}

Status RaftConsensus::CheckLeaderRequestUnlocked(ConsensusRequestPB* request,
                                                 ConsensusResponsePB* response,
                                                 LeaderRequest* deduped_req) {

  DeduplicateLeaderRequestUnlocked(request, deduped_req);

  // This is an additional check for KUDU-639 that makes sure the message's index
  // and term are in the right sequence in the request, after we've deduplicated
  // them. We do this before we change any of the internal state.
  //
  // TODO move this to raft_consensus-state or whatever we transform that into.
  // We should be able to do this check for each append, but right now the way
  // we initialize raft_consensus-state is preventing us from doing so.
  RETURN_NOT_OK(CheckLeaderRequestOpIdSequence(*deduped_req, request));

  RETURN_NOT_OK(HandleLeaderRequestTermUnlocked(request, response));

  if (response->status().has_error()) {
    return Status::OK();
  }

  RETURN_NOT_OK(EnforceLogMatchingPropertyMatchesUnlocked(*deduped_req, response));

  if (response->status().has_error()) {
    return Status::OK();
  }

  // If the first of the messages to apply is not in our log, either it follows the last
  // received message or it replaces some in-flight.
  if (!deduped_req->messages.empty()) {
    OpId first_id = deduped_req->messages[0]->id();
    bool term_mismatch;
    if (state_->IsOpCommittedOrPending(first_id, &term_mismatch)) {
      return STATUS_FORMAT(IllegalState,
                           "First deduped message $0 is committed or pending",
                           first_id);
    }

    // If the index is in our log but the terms are not the same abort down to the leader's
    // preceding id.
    if (term_mismatch) {
      RETURN_NOT_OK(state_->AbortOpsAfterUnlocked(deduped_req->preceding_opid->index()));
    }
  }

  // If all of the above logic was successful then we can consider this to be
  // the effective leader of the configuration. If they are not currently marked as
  // the leader locally, mark them as leader now.
  const string& caller_uuid = request->caller_uuid();
  if (PREDICT_FALSE(state_->HasLeaderUnlocked() &&
                    state_->GetLeaderUuidUnlocked() != caller_uuid)) {
    LOG_WITH_PREFIX_UNLOCKED(FATAL) << "Unexpected new leader in same term! "
        << "Existing leader UUID: " << state_->GetLeaderUuidUnlocked() << ", "
        << "new leader UUID: " << caller_uuid;
  }
  if (PREDICT_FALSE(!state_->HasLeaderUnlocked())) {
    SetLeaderUuidUnlocked(caller_uuid);
  }

  return Status::OK();
}

Status RaftConsensus::UpdateReplica(ConsensusRequestPB* request,
                                    ConsensusResponsePB* response) {
  TRACE_EVENT2("consensus", "RaftConsensus::UpdateReplica",
               "peer", peer_uuid(),
               "tablet", tablet_id());

  if (request->has_propagated_hybrid_time()) {//DHQ: request带了hybrid_time
    clock_->Update(HybridTime(request->propagated_hybrid_time()));
  }

  Synchronizer log_synchronizer;
  StatusCallback sync_status_cb = log_synchronizer.AsStatusCallback();

  // The ordering of the following operations is crucial, read on for details.
  //
  // The main requirements explained in more detail below are:
  //
  //   1) We must enqueue the prepares before we write to our local log.
  //   2) If we were able to enqueue a prepare then we must be able to log it.
  //   3) If we fail to enqueue a prepare, we must not attempt to enqueue any
  //      later-indexed prepare or apply.
  //
  // See below for detailed rationale.
  //
  // The steps are:
  //
  // 0 - Dedup
  //
  // We make sure that we don't do anything on Replicate operations we've already received in a
  // previous call. This essentially makes this method idempotent.
  //
  // 1 - We mark as many pending operations as committed as we can.
  //
  // We may have some pending operations that, according to the leader, are now
  // committed. We Apply them early, because:
  // - Soon (step 2) we may reject the call due to excessive memory pressure. One
  //   way to relieve the pressure is by flushing the MRS, and applying these
  //   operations may unblock an in-flight Flush().
  // - The Apply and subsequent Prepares (step 2) can take place concurrently.
  //
  // 2 - We enqueue the Prepare of the operations.
  //
  // The actual prepares are enqueued in order but happen asynchronously so we don't
  // have decoding/acquiring locks on the critical path.
  //
  // We need to do this now for a number of reasons:
  // - Prepares, by themselves, are inconsequential, i.e. they do not mutate the
  //   state machine so, were we to crash afterwards, having the prepares in-flight
  //   won't hurt.
  // - Prepares depend on factors external to consensus (the operation drivers and
  //   the tablet peer) so if for some reason they cannot be enqueued we must know
  //   before we try write them to the WAL. Once enqueued, we assume that prepare will
  //   always succeed on a replica operation (because the leader already prepared them
  //   successfully, and thus we know they are valid).
  // - The prepares corresponding to every operation that was logged must be in-flight
  //   first. This because should we need to abort certain operations (say a new leader
  //   says they are not committed) we need to have those prepares in-flight so that
  //   the operations can be continued (in the abort path).
  // - Failure to enqueue prepares is OK, we can continue and let the leader know that
  //   we only went so far. The leader will re-send the remaining messages.
  // - Prepares represent new operations, and operations consume memory. Thus, if the
  //   overall memory pressure on the server is too high, we will reject the prepares.
  //
  // 3 - We enqueue the writes to the WAL.
  //
  // We enqueue writes to the WAL, but only the operations that were successfully
  // enqueued for prepare (for the reasons introduced above). This means that even
  // if a prepare fails to enqueue, if any of the previous prepares were successfully
  // submitted they must be written to the WAL.
  // If writing to the WAL fails, we're in an inconsistent state and we crash. In this
  // case, no one will ever know of the operations we previously prepared so those are
  // inconsequential.
  //
  // 4 - We mark the operations as committed.
  //
  // For each operation which has been committed by the leader, we update the
  // operation state to reflect that. If the logging has already succeeded for that
  // operation, this will trigger the Apply phase. Otherwise, Apply will be triggered
  // when the logging completes. In both cases the Apply phase executes asynchronously.
  // This must, of course, happen after the prepares have been triggered as the same batch
  // can both replicate/prepare and commit/apply an operation.
  //
  // Currently, if a prepare failed to enqueue we still trigger all applies for operations
  // with an id lower than it (if we have them). This is important now as the leader will
  // not re-send those commit messages. This will be moot when we move to the commit
  // commitIndex way of doing things as we can simply ignore the applies as we know
  // they will be triggered with the next successful batch.
  //
  // 5 - We wait for the writes to be durable.
  //
  // Before replying to the leader we wait for the writes to be durable. We then
  // just update the last replicated watermark and respond.
  //
  // TODO - These failure scenarios need to be exercised in an unit
  //        test. Moreover we need to add more fault injection spots (well that
  //        and actually use them) for each of these steps.
  TRACE("Updating replica for $0 ops", request->ops_size());

  // The deduplicated request.
  LeaderRequest deduped_req;

  // Start an election after the writes are committed?
  bool start_election = false;

  {
    ReplicaState::UniqueLock lock;//DHQ: 这个是ReplicaState, 都借用这个lock?
    RETURN_NOT_OK(state_->LockForUpdate(&lock));

    deduped_req.leader_uuid = request->caller_uuid();

    RETURN_NOT_OK(CheckLeaderRequestUnlocked(request, response, &deduped_req));

    if (response->status().has_error()) {
      // We had an error, like an invalid term, we still fill the response.
      FillConsensusResponseOKUnlocked(response);
      return Status::OK();
    }

    // Snooze the failure detector as soon as we decide to accept the message.
    // We are guaranteed to be acting as a FOLLOWER at this point by the above
    // sanity check.
    RETURN_NOT_OK(SnoozeFailureDetectorUnlocked());

    // Update the expiration time of the current leader's lease, so that when this follower becomes
    // a leader, it can wait out the time interval while the old leader might still be active.
    if (request->has_leader_lease_duration_ms()) {
      state_->UpdateOldLeaderLeaseExpirationUnlocked(
          MonoDelta::FromMilliseconds(request->leader_lease_duration_ms()),
          request->ht_lease_expiration());
    }

    // Also prohibit voting for anyone for the minimum election timeout.
    withhold_votes_until_ = MonoTime::Now() + MinimumElectionTimeout();

    // 1 - Early commit pending (and committed) operations
    RETURN_NOT_OK(EarlyCommitUnlocked(*request, deduped_req));

    // 2 - Enqueue the prepares
    auto result = EnqueuePreparesUnlocked(*request, &deduped_req, response);//DHQ: 让Prepare异步执行
    RETURN_NOT_OK(result);
    if (!result.get()) {
      return Status::OK();
    }

    // 3 - Enqueue the writes.
    OpId last_from_leader = EnqueueWritesUnlocked(deduped_req, sync_status_cb);

    // 4 - Mark operations as committed
    RETURN_NOT_OK(MarkOperationsAsCommittedUnlocked(*request, deduped_req, last_from_leader));//DHQ: Leader告诉我们，哪些已经commit了

    // Fill the response with the current state. We will not mutate anymore state until
    // we actually reply to the leader, we'll just wait for the messages to be durable.
    FillConsensusResponseOKUnlocked(response);

    // Check if there is an election pending and the op id pending upon has just been committed.
    if (state_->HasOpIdCommittedUnlocked(state_->GetPendingElectionOpIdUnlocked())) {
      start_election = true;
    }
  }
  // Release the lock while we wait for the log append to finish so that commits can go through.
  // We'll re-acquire it before we update the state again.

  RETURN_NOT_OK(WaitWritesUnlocked(deduped_req, &log_synchronizer));//DHQ: 这个做同步等待 log完成？

  if (PREDICT_FALSE(VLOG_IS_ON(2))) {
    VLOG_WITH_PREFIX(2) << "Replica updated."
        << state_->ToString() << " Request: " << request->ShortDebugString();
  }

  // If an election pending on a specific op id and it has just been committed, start it now.
  // StartElection will ensure the pending election will be started just once only even if
  // UpdateReplica happens in multiple threads in parallel.
  if (start_election) {
    RETURN_NOT_OK(StartElection(consensus::Consensus::ELECT_EVEN_IF_LEADER_IS_ALIVE, true));
  }

  TRACE("UpdateReplicas() finished");
  return Status::OK();
}

Status RaftConsensus::EarlyCommitUnlocked(const ConsensusRequestPB& request,
                                          const LeaderRequest& deduped_req) {
  // What should we commit?
  // 1. As many pending operations as we can, except...
  // 2. ...if we commit beyond the preceding index, we'd regress KUDU-639
  //    ("Leader doesn't overwrite demoted follower's log properly"), and...
  // 3. ...the leader's committed index is always our upper bound.
  OpId early_apply_up_to = state_->GetLastPendingOperationOpIdUnlocked();
  CopyIfOpIdLessThan(*deduped_req.preceding_opid, &early_apply_up_to);
  CopyIfOpIdLessThan(request.committed_index(), &early_apply_up_to);

  VLOG_WITH_PREFIX_UNLOCKED(1) << "Early marking committed up to " <<
                               early_apply_up_to.ShortDebugString();
  TRACE("Early marking committed up to $0.$1", early_apply_up_to.term(), early_apply_up_to.index());
  return state_->AdvanceCommittedIndexUnlocked(early_apply_up_to);
}

Result<bool> RaftConsensus::EnqueuePreparesUnlocked(const ConsensusRequestPB& request,
                                                    LeaderRequest* deduped_req_ptr,
                                                    ConsensusResponsePB* response) {
  LeaderRequest& deduped_req = *deduped_req_ptr;
  TRACE("Triggering prepare for $0 ops", deduped_req.messages.size());

  Status prepare_status;
  auto iter = deduped_req.messages.begin();

  if (PREDICT_TRUE(deduped_req.messages.size() > 0)) {
    // TODO Temporary until the leader explicitly propagates the safe hybrid_time.
    // TODO: what if there is a failure here because the updated time is too far in the future?
    clock_->Update(HybridTime(deduped_req.messages.back()->hybrid_time()));

    // This request contains at least one message, and is likely to increase
    // our memory pressure.
    double capacity_pct;
    if (parent_mem_tracker_->AnySoftLimitExceeded(&capacity_pct)) {
      follower_memory_pressure_rejections_->Increment();
      string msg = StringPrintf(
          "Soft memory limit exceeded (at %.2f%% of capacity)",
          capacity_pct);
      if (capacity_pct >= FLAGS_memory_limit_warn_threshold_percentage) {
        YB_LOG_EVERY_N_SECS(WARNING, 1) << "Rejecting consensus request: " << msg
                                      << THROTTLE_MSG;
      } else {
        YB_LOG_EVERY_N_SECS(INFO, 1) << "Rejecting consensus request: " << msg
                                   << THROTTLE_MSG;
      }
      return STATUS(ServiceUnavailable, msg);
    }
  }

  HybridTime propagated_safe_time;
  if (request.has_propagated_safe_time()) {
    propagated_safe_time = HybridTime(request.propagated_safe_time());
    if (deduped_req.messages.empty()) {
      state_->GetReplicaOperationFactoryUnlocked()->SetPropagatedSafeTime(
          propagated_safe_time);
    }
  }

  if (iter != deduped_req.messages.end()) {
    for (;;) {
      const ReplicateMsgPtr& msg = *iter;
      ++iter;
      bool last = iter == deduped_req.messages.end();
      prepare_status = StartReplicaOperationUnlocked(
          msg, last ? propagated_safe_time : HybridTime::kInvalid);
      if (PREDICT_FALSE(!prepare_status.ok())) {
        --iter;
        LOG(WARNING) << "StartReplicaOperationUnlocked failed: " << prepare_status.ToString();
        break;
      }
      if (last) {
        break;
      }
    }
  }

  // If we stopped before reaching the end we failed to prepare some message(s) and need
  // to perform cleanup, namely trimming deduped_req.messages to only contain the messages
  // that were actually prepared, and deleting the other ones since we've taken ownership
  // when we first deduped.
  if (iter != deduped_req.messages.end()) {
    {
      const ReplicateMsgPtr msg = *iter;
      LOG_WITH_PREFIX_UNLOCKED(WARNING) << "Could not prepare operation for op: "
          << msg->id() << ". Suppressed " << (deduped_req.messages.end() - iter - 1) <<
          " other warnings. Status for this op: " << prepare_status.ToString();
      deduped_req.messages.erase(iter, deduped_req.messages.end());
    }

    // If this is empty, it means we couldn't prepare a single de-duped message. There is nothing
    // else we can do. The leader will detect this and retry later.
    if (deduped_req.messages.empty()) {
      auto msg = Format("Rejecting Update request from peer $0 for term $1. "
                        "Could not prepare a single operation due to: $2",
                        request.caller_uuid(),
                        request.caller_term(),
                        prepare_status);
      LOG_WITH_PREFIX_UNLOCKED(INFO) << msg;
      FillConsensusResponseError(response, ConsensusErrorPB::CANNOT_PREPARE,
                                 STATUS(IllegalState, msg));
      FillConsensusResponseOKUnlocked(response);
      return false;
    }
  }

  return true;
}

OpId RaftConsensus::EnqueueWritesUnlocked(const LeaderRequest& deduped_req,
                                          const StatusCallback& sync_status_cb) {
  // Now that we've triggered the prepares enqueue the operations to be written
  // to the WAL.
  if (PREDICT_TRUE(!deduped_req.messages.empty())) {
    // Trigger the log append asap, if fsync() is on this might take a while
    // and we can't reply until this is done.
    //
    // Since we've prepared, we need to be able to append (or we risk trying to apply
    // later something that wasn't logged). We crash if we can't.
    CHECK_OK(queue_->AppendOperations(deduped_req.messages, sync_status_cb));

    return deduped_req.messages.back()->id();
  } else {
    return *deduped_req.preceding_opid;
  }
}

Status RaftConsensus::WaitWritesUnlocked(const LeaderRequest& deduped_req,
                                         Synchronizer* log_synchronizer) {
  // Update the last replicated op id
  if (deduped_req.messages.size() > 0) {

    // 5 - We wait for the writes to be durable.

    // Note that this is safe because dist consensus now only supports a single outstanding
    // request at a time and this way we can allow commits to proceed while we wait.
    TRACE("Waiting on the replicates to finish logging");
    TRACE_EVENT0("consensus", "Wait for log");
    for (;;) {
      Status s = log_synchronizer->WaitFor(
        MonoDelta::FromMilliseconds(FLAGS_raft_heartbeat_interval_ms));
      // If just waiting for our log append to finish lets snooze the timer.
      // We don't want to fire leader election because we're waiting on our own log.
      if (!s.IsTimedOut()) {
        if (s.ok()) {
          break;
        }
        return s;
      }
      RETURN_NOT_OK(SnoozeFailureDetectorUnlocked());
    }
    TRACE("finished");
  }
  return Status::OK();
}

Status RaftConsensus::MarkOperationsAsCommittedUnlocked(const ConsensusRequestPB& request,
                                                          const LeaderRequest& deduped_req,
                                                          OpId last_from_leader) {
  // Choose the last operation to be applied. This will either be 'committed_index', if
  // no prepare enqueuing failed, or the minimum between 'committed_index' and the id of
  // the last successfully enqueued prepare, if some prepare failed to enqueue.
  OpId apply_up_to;
  if (last_from_leader.index() < request.committed_index().index()) {
    // we should never apply anything later than what we received in this request
    apply_up_to = last_from_leader;

    VLOG_WITH_PREFIX_UNLOCKED(2) << "Received commit index "
        << request.committed_index() << " from the leader but only"
        << " marked up to " << apply_up_to << " as committed.";
  } else {
    apply_up_to = request.committed_index();
  }

  // We can now update the last received watermark.
  //
  // We do it here (and before we actually hear back from the wal whether things
  // are durable) so that, if we receive another, possible duplicate, message
  // that exercises this path we don't handle these messages twice.
  //
  // If any messages failed to be started locally, then we already have removed them
  // from 'deduped_req' at this point. So, we can simply update our last-received
  // watermark to the last message that remains in 'deduped_req'.
  //
  // It's possible that the leader didn't send us any new data -- it might be a completely
  // duplicate request. In that case, we don't need to update LastReceived at all.
  if (!deduped_req.messages.empty()) {
    OpId last_appended = deduped_req.messages.back()->id();
    TRACE(Substitute("Updating last received op as $0", last_appended.ShortDebugString()));
    state_->UpdateLastReceivedOpIdUnlocked(last_appended);
  } else if (state_->GetLastReceivedOpIdUnlocked().index() < deduped_req.preceding_opid->index()) {
    return STATUS_FORMAT(InvalidArgument,
                         "Bad preceding_opid: $0, last received: $1",
                         *deduped_req.preceding_opid,
                         state_->GetLastReceivedOpIdUnlocked());
  }

  VLOG_WITH_PREFIX_UNLOCKED(1) << "Marking committed up to " << apply_up_to.ShortDebugString();
  TRACE(Substitute("Marking committed up to $0", apply_up_to.ShortDebugString()));
  return state_->AdvanceCommittedIndexUnlocked(apply_up_to);
}

void RaftConsensus::FillConsensusResponseOKUnlocked(ConsensusResponsePB* response) {
  TRACE("Filling consensus response to leader.");
  response->set_responder_term(state_->GetCurrentTermUnlocked());
  response->mutable_status()->mutable_last_received()->CopyFrom(
      state_->GetLastReceivedOpIdUnlocked());
  response->mutable_status()->mutable_last_received_current_leader()->CopyFrom(
      state_->GetLastReceivedOpIdCurLeaderUnlocked());
  response->mutable_status()->set_last_committed_idx(
      state_->GetCommittedOpIdUnlocked().index());
}

void RaftConsensus::FillConsensusResponseError(ConsensusResponsePB* response,
                                               ConsensusErrorPB::Code error_code,
                                               const Status& status) {
  ConsensusErrorPB* error = response->mutable_status()->mutable_error();
  error->set_code(error_code);
  StatusToPB(status, error->mutable_status());
}

Status RaftConsensus::RequestVote(const VoteRequestPB* request, VoteResponsePB* response) {
  TRACE_EVENT2("consensus", "RaftConsensus::RequestVote",
               "peer", peer_uuid(),
               "tablet", tablet_id());
  response->set_responder_uuid(state_->GetPeerUuid());

  // We must acquire the update lock in order to ensure that this vote action
  // takes place between requests.
  // Lock ordering: The update lock must be acquired before the ReplicaState lock.
  std::unique_lock<simple_spinlock> update_guard(update_lock_, std::defer_lock);
  if (FLAGS_enable_leader_failure_detection) {
    update_guard.try_lock();
  } else {
    // If failure detection is not enabled, then we can't just reject the vote,
    // because there will be no automatic retry later. So, block for the lock.
    update_guard.lock();
  }
  if (!update_guard.owns_lock()) {
    // There is another vote or update concurrent with the vote. In that case, that
    // other request is likely to reset the timer, and we'll end up just voting
    // "NO" after waiting. To avoid starving RPC handlers and causing cascading
    // timeouts, just vote a quick NO.
    //
    // We still need to take the state lock in order to respond with term info, etc.
    ReplicaState::UniqueLock state_guard;
    RETURN_NOT_OK(state_->LockForConfigChange(&state_guard));
    return RequestVoteRespondIsBusy(request, response);
  }

  // Acquire the replica state lock so we can read / modify the consensus state.
  ReplicaState::UniqueLock state_guard;
  RETURN_NOT_OK(state_->LockForConfigChange(&state_guard));

  // If the node is not in the configuration, allow the vote (this is required by Raft)
  // but log an informational message anyway.
  if (!IsRaftConfigMember(request->candidate_uuid(), state_->GetActiveConfigUnlocked())) {
    LOG_WITH_PREFIX_UNLOCKED(INFO) << "Handling vote request from an unknown peer "
                                   << request->candidate_uuid();
  }

  // If we've heard recently from the leader, then we should ignore the request.
  // It might be from a "disruptive" server. This could happen in a few cases:
  //
  // 1) Network partitions
  // If the leader can talk to a majority of the nodes, but is partitioned from a
  // bad node, the bad node's failure detector will trigger. If the bad node is
  // able to reach other nodes in the cluster, it will continuously trigger elections.
  //
  // 2) An abandoned node
  // It's possible that a node has fallen behind the log GC mark of the leader. In that
  // case, the leader will stop sending it requests. Eventually, the the configuration
  // will change to eject the abandoned node, but until that point, we don't want the
  // abandoned follower to disturb the other nodes.
  //
  // See also https://ramcloud.stanford.edu/~ongaro/thesis.pdf
  // section 4.2.3.
  MonoTime now = MonoTime::Now();
  if (!request->ignore_live_leader() && now < withhold_votes_until_) {
    return RequestVoteRespondLeaderIsAlive(request, response);
  }

  // Candidate is running behind.
  if (request->candidate_term() < state_->GetCurrentTermUnlocked()) {
    return RequestVoteRespondInvalidTerm(request, response);
  }

  // We already voted this term.
  if (request->candidate_term() == state_->GetCurrentTermUnlocked() &&
      state_->HasVotedCurrentTermUnlocked()) {

    // Already voted for the same candidate in the current term.
    if (state_->GetVotedForCurrentTermUnlocked() == request->candidate_uuid()) {
      return RequestVoteRespondVoteAlreadyGranted(request, response);
    }

    // Voted for someone else in current term.
    return RequestVoteRespondAlreadyVotedForOther(request, response);
  }

  // The term advanced.
  if (request->candidate_term() > state_->GetCurrentTermUnlocked()) {
    RETURN_NOT_OK_PREPEND(HandleTermAdvanceUnlocked(request->candidate_term()),
        Substitute("Could not step down in RequestVote. Current term: $0, candidate term: $1",
                   state_->GetCurrentTermUnlocked(), request->candidate_term()));
  }

  // Candidate must have last-logged OpId at least as large as our own to get our vote.
  consensus::OpId local_last_logged_opid;
  GetLatestOpIdFromLog().ToPB(&local_last_logged_opid);
  if (OpIdLessThan(request->candidate_status().last_received(), local_last_logged_opid)) {
    return RequestVoteRespondLastOpIdTooOld(local_last_logged_opid, request, response);
  }

  // Clear the pending election op id if any before granting the vote. If another peer jumps in
  // before we can catch up and start the election, let's not disrupt the quorum with another
  // election.
  state_->ClearPendingElectionOpIdUnlocked();

  auto remaining_old_leader_lease = state_->RemainingOldLeaderLeaseDuration();
  if (remaining_old_leader_lease.Initialized()) {
    response->set_remaining_leader_lease_duration_ms(
        remaining_old_leader_lease.ToMilliseconds());
  }

  auto old_leader_ht_lease_expiration = state_->old_leader_ht_lease_expiration();
  if (old_leader_ht_lease_expiration != HybridTime::kMin.GetPhysicalValueMicros()) {
    response->set_leader_ht_lease_expiration(old_leader_ht_lease_expiration);
  }

  // Passed all our checks. Vote granted.
  return RequestVoteRespondVoteGranted(request, response);
}

Status RaftConsensus::IsLeaderReadyForChangeConfigUnlocked(ChangeConfigType type,
                                                           const string& server_uuid) {
  const RaftConfigPB& active_config = state_->GetActiveConfigUnlocked();
  int servers_in_transition = 0;
  if (type == ADD_SERVER) {
    servers_in_transition = CountServersInTransition(active_config);//DHQ: PRE_VOTER和PRE_OBSERVER的个数
  } else if (type == REMOVE_SERVER) {
    // If we are trying to remove the server in transition, then servers_in_transition shouldn't
    // count it so we can proceed with the operation.
    servers_in_transition = CountServersInTransition(active_config, server_uuid);
  }

  // Check that all the following requirements are met:
  // 1. We are required by Raft to reject config change operations until we have
  //    committed at least one operation in our current term as leader.
  //    See https://groups.google.com/forum/#!topic/raft-dev/t4xj6dJTP6E
  // 2. Ensure there is no other pending change config.
  // 3. There are no peers that are in the process of becoming VOTERs or OBSERVERs.
  if (!state_->AreCommittedAndCurrentTermsSameUnlocked() ||
      state_->IsConfigChangePendingUnlocked() ||  //DHQ: check pending Config change inside
      servers_in_transition != 0) {
    return STATUS(IllegalState, Substitute("Leader is not ready for Config Change, can try again. "
                                           "Num peers in transit = $0. Type=$1. Has opid=$2.\n"
                                           "  Committed config: $3.\n  Pending config: $4.",
                                           servers_in_transition, ChangeConfigType_Name(type),
                                           active_config.has_opid_index(),
                                           state_->GetCommittedConfigUnlocked().ShortDebugString(),
                                           state_->IsConfigChangePendingUnlocked() ?
                                             state_->GetPendingConfigUnlocked().ShortDebugString() :
                                             ""));
  }

  return Status::OK();
}

Status RaftConsensus::ChangeConfig(const ChangeConfigRequestPB& req,
                                   const StatusCallback& client_cb,
                                   boost::optional<TabletServerErrorPB::Code>* error_code) {
  if (PREDICT_FALSE(!req.has_type())) {
    return STATUS(InvalidArgument, "Must specify 'type' argument to ChangeConfig()",
                                   req.ShortDebugString());
  }
  if (PREDICT_FALSE(!req.has_server())) {
    *error_code = TabletServerErrorPB::INVALID_CONFIG;
    return STATUS(InvalidArgument, "Must specify 'server' argument to ChangeConfig()",
                                   req.ShortDebugString());
  }
  LOG(INFO) << "Received ChangeConfig request " << req.ShortDebugString();
  ChangeConfigType type = req.type();

  if (PREDICT_FALSE(FLAGS_return_error_on_change_config != 0.0 && type == CHANGE_ROLE)) {
    DCHECK(FLAGS_return_error_on_change_config >= 0.0 &&
           FLAGS_return_error_on_change_config <= 1.0);
    if (clock_->Now().ToUint64() % 100 < 100 * FLAGS_return_error_on_change_config) {
      return STATUS(IllegalState, "Returning error for unit test");
    }
  }
  RaftPeerPB* new_peer = nullptr;
  const RaftPeerPB& server = req.server();
  if (!server.has_permanent_uuid()) {
    return STATUS(InvalidArgument, Substitute("server must have permanent_uuid specified",
                                              req.ShortDebugString()));
  }
  {
    ReplicaState::UniqueLock lock;
    RETURN_NOT_OK(state_->LockForConfigChange(&lock));
    Status s = state_->CheckActiveLeaderUnlocked(LeaderLeaseCheckMode::DONT_NEED_LEASE);
    if (!s.ok()) {
      *error_code = TabletServerErrorPB::NOT_THE_LEADER;
      return s;
    }

    s = IsLeaderReadyForChangeConfigUnlocked(type, server.permanent_uuid());
    if (!s.ok()) {
      LOG(INFO) << "Returning not ready for " << ChangeConfigType_Name(type)
                << " due to error " << s.ToString();
      *error_code = TabletServerErrorPB::LEADER_NOT_READY_CHANGE_CONFIG;
      return s;
    }

    const RaftConfigPB& committed_config = state_->GetCommittedConfigUnlocked();

    // Support atomic ChangeConfig requests.
    if (req.has_cas_config_opid_index()) {//DHQ: 这个atomic，要求上次的commit的opid是一定的。
      if (committed_config.opid_index() != req.cas_config_opid_index()) {
        *error_code = TabletServerErrorPB::CAS_FAILED;
        return STATUS(IllegalState, Substitute("Request specified cas_config_opid_index "
                                               "of $0 but the committed config has opid_index "
                                               "of $1",
                                               req.cas_config_opid_index(),
                                               committed_config.opid_index()));
      }
    }

    RaftConfigPB new_config = committed_config;
    new_config.clear_opid_index();
    const string& server_uuid = server.permanent_uuid();
    switch (type) {
      case ADD_SERVER:
        // Ensure the server we are adding is not already a member of the configuration.
        if (IsRaftConfigMember(server_uuid, committed_config)) {
          *error_code = TabletServerErrorPB::ADD_CHANGE_CONFIG_ALREADY_PRESENT;
          return STATUS(IllegalState,
              Substitute("Server with UUID $0 is already a member of the config. RaftConfig: $1",
                        server_uuid, committed_config.ShortDebugString()));
        }
        if (!server.has_member_type()) {
          return STATUS(InvalidArgument,
                        Substitute("Server must have member_type specified. Request: $0",
                                   req.ShortDebugString()));
        }
        if (server.member_type() != RaftPeerPB::PRE_VOTER &&
            server.member_type() != RaftPeerPB::PRE_OBSERVER) {
          return STATUS(InvalidArgument,
              Substitute("Server with UUID $0 must be of member_type PRE_VOTER or PRE_OBSERVER. "
                         "member_type received: $1", server_uuid,
                         RaftPeerPB::MemberType_Name(server.member_type())));
        }
        if (!server.has_last_known_addr()) {
          return STATUS(InvalidArgument, "server must have last_known_addr specified",
                                         req.ShortDebugString());
        }
        new_peer = new_config.add_peers();
        *new_peer = server;
        break;

      case REMOVE_SERVER:
        if (server_uuid == peer_uuid()) {
          *error_code = TabletServerErrorPB::LEADER_NEEDS_STEP_DOWN;
          return STATUS(InvalidArgument,
              Substitute("Cannot remove peer $0 from the config because it is the leader. "
                         "Force another leader to be elected to remove this server. "
                         "Active consensus state: $1",
                         server_uuid,
                         state_->ConsensusStateUnlocked(CONSENSUS_CONFIG_ACTIVE)
                            .ShortDebugString()));
        }
        if (!RemoveFromRaftConfig(&new_config, server_uuid)) {
          *error_code = TabletServerErrorPB::REMOVE_CHANGE_CONFIG_NOT_PRESENT;
          return STATUS(NotFound,
              Substitute("Server with UUID $0 not a member of the config. RaftConfig: $1",
                        server_uuid, committed_config.ShortDebugString()));
        }
        break;

      case CHANGE_ROLE://DHQ: 
        if (server_uuid == peer_uuid()) {
          return STATUS(InvalidArgument,
              Substitute("Cannot change role of  peer $0 from the config because it is the leader. "
                         "Force another leader to be elected to change the role of this server. "
                         "Active consensus state: $1",
                         server_uuid,
                         state_->ConsensusStateUnlocked(CONSENSUS_CONFIG_ACTIVE)
                            .ShortDebugString()));
        }
        VLOG(3) << "config before CHANGE_ROLE: " << new_config.DebugString();

        if (!GetMutableRaftConfigMember(&new_config, server_uuid, &new_peer).ok()) {
          return STATUS(NotFound,
            Substitute("Server with UUID $0 not a member of the config. RaftConfig: $1",
                       server_uuid, new_config.ShortDebugString()));
        }
        if (new_peer->member_type() != RaftPeerPB::PRE_OBSERVER &&
            new_peer->member_type() != RaftPeerPB::PRE_VOTER) {
          return STATUS(IllegalState, Substitute("Cannot change role of server with UUID $0 "
                                                 "because its member type is $1",
                                                 server_uuid, new_peer->member_type()));
        }
        if (new_peer->member_type() == RaftPeerPB::PRE_OBSERVER) {
          new_peer->set_member_type(RaftPeerPB::OBSERVER);
        } else {
          new_peer->set_member_type(RaftPeerPB::VOTER);
        }

        VLOG(3) << "config after CHANGE_ROLE: " << new_config.DebugString();
        break;
      default:
        return STATUS(InvalidArgument, Substitute("Unsupported type $0",
                                                  ChangeConfigType_Name(type)));
    }

    auto cc_replicate = std::make_shared<ReplicateMsg>();
    cc_replicate->set_op_type(CHANGE_CONFIG_OP);//DHQ: 设置 req 类型
    ChangeConfigRecordPB* cc_req = cc_replicate->mutable_change_config_record();
    cc_req->set_tablet_id(tablet_id());
    *cc_req->mutable_old_config() = committed_config;
    *cc_req->mutable_new_config() = new_config;
    // TODO: We should have no-ops (?) and config changes be COMMIT_WAIT
    // operations. See KUDU-798.
    // Note: This hybrid_time has no meaning from a serialization perspective
    // because this method is not executed on the TabletPeer's prepare thread.
    cc_replicate->set_hybrid_time(clock_->Now().ToUint64());
    cc_replicate->mutable_committed_op_id()->CopyFrom(state_->GetCommittedOpIdUnlocked());

    auto context =
      std::make_shared<StateChangeContext>(StateChangeReason::LEADER_CONFIG_CHANGE_COMPLETE,
                                           *cc_req,
                                           (type == REMOVE_SERVER) ? server_uuid : "");

    RETURN_NOT_OK(
        ReplicateConfigChangeUnlocked(cc_replicate,
                                      new_config,
                                      type,
                                      Bind(&RaftConsensus::MarkDirtyOnSuccess,
                                           Unretained(this),
                                           context,
                                           client_cb)));
  }

  peer_manager_->SignalRequest(RequestTriggerMode::NON_EMPTY_ONLY);

  return Status::OK();
}

void RaftConsensus::Shutdown() {
  // Avoid taking locks if already shut down so we don't violate
  // ThreadRestrictions assertions in the case where the RaftConsensus
  // destructor runs on the reactor thread due to an election callback being
  // the last outstanding reference.
  if (shutdown_.Load(kMemOrderAcquire)) return;

  CHECK_OK(ExecuteHook(PRE_SHUTDOWN));

  {
    ReplicaState::UniqueLock lock;
    // Transition to kShuttingDown state.
    CHECK_OK(state_->LockForShutdown(&lock));
    LOG_WITH_PREFIX_UNLOCKED(INFO) << "Raft consensus shutting down.";
  }

  // Close the peer manager.
  peer_manager_->Close();

  // We must close the queue after we close the peers.
  queue_->Close();

  CHECK_OK(state_->CancelPendingOperations());

  {
    ReplicaState::UniqueLock lock;
    CHECK_OK(state_->LockForShutdown(&lock));
    CHECK_EQ(ReplicaState::kShuttingDown, state_->state());
    CHECK_OK(state_->ShutdownUnlocked());
    LOG_WITH_PREFIX_UNLOCKED(INFO) << "Raft consensus is shut down!";
  }

  // Shut down things that might acquire locks during destruction.
  raft_pool_token_->Shutdown();
  failure_monitor_.Shutdown();

  CHECK_OK(ExecuteHook(POST_SHUTDOWN));

  shutdown_.Store(true, kMemOrderRelease);
}

RaftPeerPB::Role RaftConsensus::GetActiveRole() const {
  ReplicaState::UniqueLock lock;
  CHECK_OK(state_->LockForRead(&lock));
  return state_->GetActiveRoleUnlocked();
}

yb::OpId RaftConsensus::GetLatestOpIdFromLog() {
  return log_->GetLatestEntryOpId();
}
//DHQ: ConsensusOnly,指noop或者confchange
Status RaftConsensus::StartConsensusOnlyRoundUnlocked(const ReplicateMsgPtr& msg) {
  OperationType op_type = msg->op_type();
  if (!IsConsensusOnlyOperation(op_type)) {
    return STATUS_FORMAT(InvalidArgument,
                         "Expected a consensus-only op type, got $0: $1",
                         OperationType_Name(op_type),
                         *msg);
  }
  VLOG_WITH_PREFIX_UNLOCKED(1) << "Starting consensus round: "
                               << msg->id().ShortDebugString();
  scoped_refptr<ConsensusRound> round(new ConsensusRound(this, msg));
  std::shared_ptr<StateChangeContext> context = nullptr;

  // We are here for NO_OP or CHANGE_CONFIG_OP type ops. We need to set the change record for an
  // actual config change operation. The NO_OP does not update the config, as it is used for a new
  // leader election term change replicate message, which keeps the same config.
  if (IsChangeConfigOperation(op_type)) {
    context =
      std::make_shared<StateChangeContext>(StateChangeReason::FOLLOWER_CONFIG_CHANGE_COMPLETE,
                                           msg->change_config_record());
  } else {
    context = std::make_shared<StateChangeContext>(StateChangeReason::FOLLOWER_NO_OP_COMPLETE);
  }

  round->SetConsensusReplicatedCallback(Bind(&RaftConsensus::NonTxRoundReplicationFinished,
                                             Unretained(this),
                                             Unretained(round.get()),
                                             Bind(&RaftConsensus::MarkDirtyOnSuccess,
                                                  Unretained(this),
                                                  context,
                                                  Bind(&DoNothingStatusCB))));
  return state_->AddPendingOperation(round);
}

Status RaftConsensus::WaitForLeaderLeaseImprecise(MonoTime deadline) {
  MonoTime now;
  while ((now = MonoTime::Now()) < deadline) {
    MonoDelta remaining_old_leader_lease;
    LeaderLeaseStatus leader_lease_status;
    {
      ReplicaState::UniqueLock lock;
      RETURN_NOT_OK(state_->LockForRead(&lock));
      leader_lease_status = state_->GetLeaderLeaseStatusUnlocked(&remaining_old_leader_lease);
    }
    switch (leader_lease_status) {
      case LeaderLeaseStatus::HAS_LEASE:
        return Status::OK();
      case LeaderLeaseStatus::NO_MAJORITY_REPLICATED_LEASE:
        {
          std::unique_lock<decltype(leader_lease_wait_mtx_)> lock(leader_lease_wait_mtx_);
          // Because we're not taking the same lock (leader_lease_wait_mtx_) when we check the
          // leader lease status, there is a possibility of a race condition when we miss the
          // notification and by this point we already have a lease. Rather than re-taking the
          // ReplicaState lock and re-checking, here we simply block for up to 100ms in that case,
          // because this function is currently (08/14/2017) only used in a context when it is OK,
          // such as catalog manager initialization.
          leader_lease_wait_cond_.wait_for(lock,
              max(MonoDelta::FromMilliseconds(100), deadline - now).ToSteadyDuration());
        }
        continue;
      case LeaderLeaseStatus::OLD_LEADER_MAY_HAVE_LEASE:
        if (now + remaining_old_leader_lease > deadline) {
          return STATUS_FORMAT(
              TimedOut,
              "Old leader still has lease for $0 but we only have $1 left to wait",
              remaining_old_leader_lease, deadline - now);
        }
        SleepFor(remaining_old_leader_lease);
        continue;
    }
    FATAL_INVALID_ENUM_VALUE(LeaderLeaseStatus, leader_lease_status);
  }
  return STATUS_FORMAT(TimedOut, "Waited for $0 to acquire a leader lease", deadline);
}

Status RaftConsensus::CheckIsActiveLeaderAndHasLease() const {
  return state_->CheckIsActiveLeaderAndHasLease();
}

MicrosTime RaftConsensus::MajorityReplicatedHtLeaseExpiration(
    MicrosTime min_allowed, MonoTime deadline) const {
  return state_->MajorityReplicatedHtLeaseExpiration(min_allowed, deadline);
}

std::string RaftConsensus::GetRequestVoteLogPrefixUnlocked() const {
  return state_->LogPrefixUnlocked() + "Leader election vote request";
}

void RaftConsensus::FillVoteResponseVoteGranted(VoteResponsePB* response) {
  response->set_responder_term(state_->GetCurrentTermUnlocked());
  response->set_vote_granted(true);
}

void RaftConsensus::FillVoteResponseVoteDenied(ConsensusErrorPB::Code error_code,
                                               VoteResponsePB* response) {
  response->set_responder_term(state_->GetCurrentTermUnlocked());
  response->set_vote_granted(false);
  response->mutable_consensus_error()->set_code(error_code);
}

Status RaftConsensus::RequestVoteRespondInvalidTerm(const VoteRequestPB* request,
                                                    VoteResponsePB* response) {
  FillVoteResponseVoteDenied(ConsensusErrorPB::INVALID_TERM, response);
  string msg = Substitute("$0: Denying vote to candidate $1 for earlier term $2. "
                          "Current term is $3.",
                          GetRequestVoteLogPrefixUnlocked(),
                          request->candidate_uuid(),
                          request->candidate_term(),
                          state_->GetCurrentTermUnlocked());
  LOG(INFO) << msg;
  StatusToPB(STATUS(InvalidArgument, msg), response->mutable_consensus_error()->mutable_status());
  return Status::OK();
}

Status RaftConsensus::RequestVoteRespondVoteAlreadyGranted(const VoteRequestPB* request,
                                                           VoteResponsePB* response) {
  FillVoteResponseVoteGranted(response);
  LOG(INFO) << Substitute("$0: Already granted yes vote for candidate $1 in term $2. "
                          "Re-sending same reply.",
                          GetRequestVoteLogPrefixUnlocked(),
                          request->candidate_uuid(),
                          request->candidate_term());
  return Status::OK();
}

Status RaftConsensus::RequestVoteRespondAlreadyVotedForOther(const VoteRequestPB* request,
                                                             VoteResponsePB* response) {
  FillVoteResponseVoteDenied(ConsensusErrorPB::ALREADY_VOTED, response);
  string msg = Substitute("$0: Denying vote to candidate $1 in current term $2: "
                          "Already voted for candidate $3 in this term.",
                          GetRequestVoteLogPrefixUnlocked(),
                          request->candidate_uuid(),
                          state_->GetCurrentTermUnlocked(),
                          state_->GetVotedForCurrentTermUnlocked());
  LOG(INFO) << msg;
  StatusToPB(STATUS(InvalidArgument, msg), response->mutable_consensus_error()->mutable_status());
  return Status::OK();
}

Status RaftConsensus::RequestVoteRespondLastOpIdTooOld(const OpId& local_last_logged_opid,
                                                       const VoteRequestPB* request,
                                                       VoteResponsePB* response) {
  FillVoteResponseVoteDenied(ConsensusErrorPB::LAST_OPID_TOO_OLD, response);
  string msg = Substitute("$0: Denying vote to candidate $1 for term $2 because "
                          "replica has last-logged OpId of $3, which is greater than that of the "
                          "candidate, which has last-logged OpId of $4.",
                          GetRequestVoteLogPrefixUnlocked(),
                          request->candidate_uuid(),
                          request->candidate_term(),
                          local_last_logged_opid.ShortDebugString(),
                          request->candidate_status().last_received().ShortDebugString());
  LOG(INFO) << msg;
  StatusToPB(STATUS(InvalidArgument, msg), response->mutable_consensus_error()->mutable_status());
  return Status::OK();
}

Status RaftConsensus::RequestVoteRespondLeaderIsAlive(const VoteRequestPB* request,
                                                      VoteResponsePB* response) {
  FillVoteResponseVoteDenied(ConsensusErrorPB::LEADER_IS_ALIVE, response);
  string msg = Substitute("$0: Denying vote to candidate $1 for term $2 because "
                          "replica is either leader or believes a valid leader to "
                          "be alive.",
                          GetRequestVoteLogPrefixUnlocked(),
                          request->candidate_uuid(),
                          request->candidate_term());
  LOG(INFO) << msg;
  StatusToPB(STATUS(InvalidArgument, msg), response->mutable_consensus_error()->mutable_status());
  return Status::OK();
}

Status RaftConsensus::RequestVoteRespondIsBusy(const VoteRequestPB* request,
                                               VoteResponsePB* response) {
  FillVoteResponseVoteDenied(ConsensusErrorPB::CONSENSUS_BUSY, response);
  string msg = Substitute("$0: Denying vote to candidate $1 for term $2 because "
                          "replica is already servicing an update from a current leader "
                          "or another vote.",
                          GetRequestVoteLogPrefixUnlocked(),
                          request->candidate_uuid(),
                          request->candidate_term());
  LOG(INFO) << msg;
  StatusToPB(STATUS(ServiceUnavailable, msg),
             response->mutable_consensus_error()->mutable_status());
  return Status::OK();
}

Status RaftConsensus::RequestVoteRespondVoteGranted(const VoteRequestPB* request,
                                                    VoteResponsePB* response) {
  // We know our vote will be "yes", so avoid triggering an election while we
  // persist our vote to disk. We use an exponential backoff to avoid too much
  // split-vote contention when nodes display high latencies.
  MonoDelta additional_backoff = LeaderElectionExpBackoffDeltaUnlocked();
  RETURN_NOT_OK(SnoozeFailureDetectorUnlocked(additional_backoff, ALLOW_LOGGING));

  // Persist our vote to disk.
  RETURN_NOT_OK(state_->SetVotedForCurrentTermUnlocked(request->candidate_uuid()));

  FillVoteResponseVoteGranted(response);

  // Give peer time to become leader. Snooze one more time after persisting our
  // vote. When disk latency is high, this should help reduce churn.
  RETURN_NOT_OK(SnoozeFailureDetectorUnlocked(additional_backoff, DO_NOT_LOG));

  LOG(INFO) << Substitute("$0: Granting yes vote for candidate $1 in term $2.",
                          GetRequestVoteLogPrefixUnlocked(),
                          request->candidate_uuid(),
                          state_->GetCurrentTermUnlocked());
  return Status::OK();
}

RaftPeerPB::Role RaftConsensus::GetRoleUnlocked() const {
  DCHECK(state_->IsLocked());
  return state_->GetActiveRoleUnlocked();
}

RaftPeerPB::Role RaftConsensus::role() const {
  ReplicaState::UniqueLock lock;
  CHECK_OK(state_->LockForRead(&lock));
  return GetRoleUnlocked();
}

Consensus::LeaderStatus RaftConsensus::leader_status() const {
  ReplicaState::UniqueLock lock;
  CHECK_OK(state_->LockForRead(&lock));

  if (GetRoleUnlocked() != RaftPeerPB::LEADER) {
    return LeaderStatus::NOT_LEADER;
  }

  if (!leader_no_op_committed_) {
    // This will cause the client to retry on the same server (won't try to find the new leader).
    return LeaderStatus::LEADER_BUT_NOT_READY;
  }

  MonoDelta remaining_old_leader_lease;
  const auto lease_status = state_->GetLeaderLeaseStatusUnlocked(&remaining_old_leader_lease);
  switch (lease_status) {
    case LeaderLeaseStatus::OLD_LEADER_MAY_HAVE_LEASE:
      // Will retry on the same server.
      VLOG(1) << "Old leader lease might still be active for "
              << remaining_old_leader_lease.ToString();
      return LeaderStatus::LEADER_BUT_NOT_READY;

    case LeaderLeaseStatus::NO_MAJORITY_REPLICATED_LEASE:
      // Will retry to look up the leader, because it might have changed.
      return LeaderStatus::NOT_LEADER;

    case LeaderLeaseStatus::HAS_LEASE:
      return LeaderStatus::LEADER_AND_READY;
  }

  FATAL_INVALID_ENUM_VALUE(LeaderLeaseStatus, lease_status);
}

std::string RaftConsensus::LogPrefixUnlocked() {
  return state_->LogPrefixUnlocked();
}

std::string RaftConsensus::LogPrefix() {
  return state_->LogPrefix();
}

void RaftConsensus::SetLeaderUuidUnlocked(const string& uuid) {
  state_->SetLeaderUuidUnlocked(uuid);
  auto context = std::make_shared<StateChangeContext>(StateChangeReason::NEW_LEADER_ELECTED, uuid);
  MarkDirty(context);
}

Status RaftConsensus::ReplicateConfigChangeUnlocked(const ReplicateMsgPtr& replicate_ref,
                                                    const RaftConfigPB& new_config,
                                                    ChangeConfigType type,
                                                    const StatusCallback& client_cb) {
  scoped_refptr<ConsensusRound> round(new ConsensusRound(this, replicate_ref));
  round->SetConsensusReplicatedCallback(Bind(&RaftConsensus::NonTxRoundReplicationFinished,
                                             Unretained(this),
                                             Unretained(round.get()),
                                             client_cb));
  LOG(INFO) << "Setting replicate pending config " << new_config.ShortDebugString()
            << ", type = " << ChangeConfigType_Name(type);

  RETURN_NOT_OK(state_->SetPendingConfigUnlocked(new_config));//DHQ: 本地保存下Pending的Config

  if (type == CHANGE_ROLE && PREDICT_FALSE(FLAGS_inject_delay_leader_change_role_append_secs)) {
    LOG(INFO) << "Adding change role sleep for "
              << FLAGS_inject_delay_leader_change_role_append_secs << " secs.";
    SleepFor(MonoDelta::FromSeconds(FLAGS_inject_delay_leader_change_role_append_secs));
  }

  // Set as pending.
  RETURN_NOT_OK(RefreshConsensusQueueAndPeersUnlocked());
  auto status = AppendNewRoundToQueueUnlocked(round);//DHQ: 加到queue里面
  if (!status.ok()) {
    // We could just cancel pending config, because there is could be only one pending config.
    auto clear_status = state_->ClearPendingConfigUnlocked();
    if (!clear_status.ok()) {
      LOG(WARNING) << "Could not clear pending config: " << clear_status;
    }
  }
  return status;
}

Status RaftConsensus::RefreshConsensusQueueAndPeersUnlocked() {
  DCHECK_EQ(RaftPeerPB::LEADER, state_->GetActiveRoleUnlocked());
  const RaftConfigPB& active_config = state_->GetActiveConfigUnlocked();

  // Change the peers so that we're able to replicate messages remotely and
  // locally. Peer manager connections are updated using the active config. Connections to peers
  // that are not part of active_config are closed. New connections are created for those peers
  // that are present in active_config but have no connections. When the queue is in LEADER
  // mode, it checks that all registered peers are a part of the active config.
  peer_manager_->ClosePeersNotInConfig(active_config);
  queue_->SetLeaderMode(state_->GetCommittedOpIdUnlocked(),
                        state_->GetCurrentTermUnlocked(),
                        active_config);
  RETURN_NOT_OK(peer_manager_->UpdateRaftConfig(active_config));
  return Status::OK();
}

string RaftConsensus::peer_uuid() const {
  return state_->GetPeerUuid();
}

string RaftConsensus::tablet_id() const {
  return state_->GetOptions().tablet_id;
}

ConsensusStatePB RaftConsensus::ConsensusState(
    ConsensusConfigType type,
    LeaderLeaseStatus* leader_lease_status) const {
  ReplicaState::UniqueLock lock;
  CHECK_OK(state_->LockForRead(&lock));
  return ConsensusStateUnlocked(type, leader_lease_status);
}

ConsensusStatePB RaftConsensus::ConsensusStateUnlocked(
    ConsensusConfigType type,
    LeaderLeaseStatus* leader_lease_status) const {
  CHECK(state_->IsLocked());
  if (leader_lease_status) {
    if (GetRoleUnlocked() == RaftPeerPB_Role_LEADER) {
      *leader_lease_status = state_->GetLeaderLeaseStatusUnlocked();
    } else {
      // We'll still return a valid value if we're not a leader.
      *leader_lease_status = LeaderLeaseStatus::NO_MAJORITY_REPLICATED_LEASE;
    }
  }
  return state_->ConsensusStateUnlocked(type);
}

RaftConfigPB RaftConsensus::CommittedConfig() const {
  ReplicaState::UniqueLock lock;
  CHECK_OK(state_->LockForRead(&lock));
  return state_->GetCommittedConfigUnlocked();
}

void RaftConsensus::DumpStatusHtml(std::ostream& out) const {
  out << "<h1>Raft Consensus State</h1>" << std::endl;

  out << "<h2>State</h2>" << std::endl;
  out << "<pre>" << EscapeForHtmlToString(queue_->ToString()) << "</pre>" << std::endl;

  // Dump the queues on a leader.
  RaftPeerPB::Role role;
  {
    ReplicaState::UniqueLock lock;
    CHECK_OK(state_->LockForRead(&lock));
    role = state_->GetActiveRoleUnlocked();
  }
  if (role == RaftPeerPB::LEADER) {
    out << "<h2>Queue overview</h2>" << std::endl;
    out << "<pre>" << EscapeForHtmlToString(queue_->ToString()) << "</pre>" << std::endl;
    out << "<hr/>" << std::endl;
    out << "<h2>Queue details</h2>" << std::endl;
    queue_->DumpToHtml(out);
  }
}

ReplicaState* RaftConsensus::GetReplicaStateForTests() {
  return state_.get();
}

void RaftConsensus::ElectionCallback(const std::string& originator_uuid,
                                     const ElectionResult& result) {
  // The election callback runs on a reactor thread, so we need to defer to our
  // threadpool. If the threadpool is already shut down for some reason, it's OK --
  // we're OK with the callback never running.
  WARN_NOT_OK(raft_pool_token_->SubmitClosure(
              Bind(&RaftConsensus::DoElectionCallback, this, originator_uuid, result)),
              state_->LogPrefixThreadSafe() + "Unable to run election callback");
}

void RaftConsensus::NotifyOriginatorAboutLostElection(const std::string& originator_uuid) {
  if (originator_uuid.empty()) {
    return;
  }

  ReplicaState::UniqueLock lock;
  Status s = state_->LockForConfigChange(&lock);
  if (PREDICT_FALSE(!s.ok())) {
    LOG_WITH_PREFIX(INFO) << "Unable to notify originator about lost election, lock failed: "
                          << s.ToString();
    return;
  }

  const RaftConfigPB& active_config = state_->GetActiveConfigUnlocked();
  for (const RaftPeerPB& peer : active_config.peers()) {
    if (peer.permanent_uuid() == originator_uuid) {
      gscoped_ptr<PeerProxy> proxy;
      auto status = peer_proxy_factory_->NewProxy(peer, &proxy);
      if (!status.ok()) {
        LOG_WITH_PREFIX_UNLOCKED(INFO) << "Unable to notify originator about lost election, "
                                       << "failed to create proxy: " << s.ToString();
        return;
      }
      LeaderElectionLostRequestPB req;
      req.set_dest_uuid(originator_uuid);
      req.set_election_lost_by_uuid(state_->GetPeerUuid());
      req.set_tablet_id(state_->GetOptions().tablet_id);
      auto resp = std::make_shared<LeaderElectionLostResponsePB>();
      auto rpc = std::make_shared<rpc::RpcController>();
      auto log_prefix = state_->LogPrefixUnlocked();
      proxy->LeaderElectionLostAsync(&req, resp.get(), rpc.get(), [log_prefix, resp, rpc] {
        if (!rpc->status().ok()) {
          LOG(WARNING) << log_prefix << "Notify about lost election RPC failure: "
                       << rpc->status().ToString();
        } else if (resp->has_error()) {
          LOG(WARNING) << log_prefix << "Notify about lost election failed: "
                       << StatusFromPB(resp->error().status()).ToString();
        }
      });
      return;
    }
  }
  LOG_WITH_PREFIX_UNLOCKED(WARNING) << "Failed to find originators peer: " << originator_uuid
                                    << ", config: " << active_config.ShortDebugString();
}

void RaftConsensus::DoElectionCallback(const std::string& originator_uuid,
                                       const ElectionResult& result) {
  // Snooze to avoid the election timer firing again as much as possible.
  {
    ReplicaState::UniqueLock lock;
    CHECK_OK(state_->LockForRead(&lock));
    // We need to snooze when we win and when we lose:
    // - When we win because we're about to disable the timer and become leader.
    // - When we loose or otherwise we can fall into a cycle, where everyone keeps
    //   triggering elections but no election ever completes because by the time they
    //   finish another one is triggered already.
    // We ignore the status as we don't want to fail if we the timer is
    // disabled.
    ignore_result(SnoozeFailureDetectorUnlocked(LeaderElectionExpBackoffDeltaUnlocked(),
                                                ALLOW_LOGGING));
  }
  if (result.decision == VOTE_DENIED) {
    LOG_WITH_PREFIX(INFO) << "Leader election lost for term " << result.election_term
                             << ". Reason: "
                             << (!result.message.empty() ? result.message : "None given")
                             << ". Originator: " << originator_uuid;
    NotifyOriginatorAboutLostElection(originator_uuid);
    return;
  }

  ReplicaState::UniqueLock lock;
  Status s = state_->LockForConfigChange(&lock);
  if (PREDICT_FALSE(!s.ok())) {
    LOG_WITH_PREFIX(INFO) << "Received election callback for term "
                          << result.election_term << " while not running: "
                          << s.ToString();
    return;
  }

  if (result.election_term != state_->GetCurrentTermUnlocked()) {
    LOG_WITH_PREFIX_UNLOCKED(INFO) << "Leader election decision for defunct term "
                                   << result.election_term << ": "
                                   << (result.decision == VOTE_GRANTED ? "won" : "lost");
    return;
  }

  const RaftConfigPB& active_config = state_->GetActiveConfigUnlocked();
  if (!IsRaftConfigVoter(state_->GetPeerUuid(), active_config)) {
    LOG_WITH_PREFIX_UNLOCKED(WARNING) << "Leader election decision while not in active config. "
                                      << "Result: Term " << result.election_term << ": "
                                      << (result.decision == VOTE_GRANTED ? "won" : "lost")
                                      << ". RaftConfig: " << active_config.ShortDebugString();
    return;
  }

  if (state_->GetActiveRoleUnlocked() == RaftPeerPB::LEADER) {
    LOG_WITH_PREFIX_UNLOCKED(DFATAL) << "Leader election callback while already leader! "
                          "Result: Term " << result.election_term << ": "
                          << (result.decision == VOTE_GRANTED ? "won" : "lost");
    return;
  }

  LOG_WITH_PREFIX_UNLOCKED(INFO) << "Leader election won for term " << result.election_term;

  if (result.old_leader_lease_expiration) {
    // Voters told us about the old leader's lease that we have to wait out.
    state_->UpdateOldLeaderLeaseExpirationUnlocked(
        result.old_leader_lease_expiration,
        result.old_leader_ht_lease_expiration);
  }

  // Convert role to LEADER.
  SetLeaderUuidUnlocked(state_->GetPeerUuid());

  // TODO: BecomeLeaderUnlocked() can fail due to state checks during shutdown.
  // It races with the above state check.
  // This could be a problem during tablet deletion.
  auto status = BecomeLeaderUnlocked();
  if (!status.ok()) {
    LOG_WITH_PREFIX_UNLOCKED(DFATAL) << "Failed to become leader: " << status.ToString();
  }
}

Status RaftConsensus::GetLastOpId(OpIdType type, OpId* id) {
  ReplicaState::UniqueLock lock;
  RETURN_NOT_OK(state_->LockForRead(&lock));
  if (type == RECEIVED_OPID) {
    *DCHECK_NOTNULL(id) = state_->GetLastReceivedOpIdUnlocked();
  } else if (type == COMMITTED_OPID) {
    *DCHECK_NOTNULL(id) = state_->GetCommittedOpIdUnlocked();
  } else {
    return STATUS(InvalidArgument, "Unsupported OpIdType", OpIdType_Name(type));
  }
  return Status::OK();
}

void RaftConsensus::MarkDirty(std::shared_ptr<StateChangeContext> context) {
  LOG(INFO) << "Calling mark dirty synchronously for reason code " << context->reason;
  mark_dirty_clbk_.Run(context);
}

void RaftConsensus::MarkDirtyOnSuccess(std::shared_ptr<StateChangeContext> context,
                                       const StatusCallback& client_cb,
                                       const Status& status) {
  if (PREDICT_TRUE(status.ok())) {
    MarkDirty(context);
  }
  client_cb.Run(status);
}

void RaftConsensus::NonTxRoundReplicationFinished(ConsensusRound* round,
                                                  const StatusCallback& client_cb,
                                                  const Status& status) {
  DCHECK(state_->IsLocked());
  OperationType op_type = round->replicate_msg()->op_type();
  string op_type_str = OperationType_Name(op_type);
  if (!IsConsensusOnlyOperation(op_type)) {
    LOG(ERROR) << "Unexpected op type: " << op_type_str;
    return;
  }
  if (!status.ok()) {
    // TODO: Do something with the status on failure?
    LOG(INFO) << state_->LogPrefixThreadSafe() << op_type_str << " replication failed: "
              << status.ToString();

    // Clear out the pending state (ENG-590).
    if (IsChangeConfigOperation(op_type)) {
      Status s = state_->ClearPendingConfigUnlocked();
      if (!s.ok()) {
        LOG(WARNING) << "Could not clear pending state : " << s.ToString();
      }
    }
  }

  client_cb.Run(status);

  // Set 'Leader is ready to serve' flag only for commited NoOp operation
  // and only if the term is up-to-date.
  if (op_type == NO_OP && round->id().has_term() &&
      round->id().term() == state_->GetCurrentTermUnlocked()) {
    leader_no_op_committed_ = true;
  }
}

Status RaftConsensus::EnsureFailureDetectorEnabledUnlocked() {
  if (PREDICT_FALSE(!FLAGS_enable_leader_failure_detection)) {
    return Status::OK();
  }
  if (failure_detector_->IsTracking(kTimerId)) {
    return Status::OK();
  }
  return failure_detector_->Track(kTimerId,
                                  MonoTime::Now(),
                                  // Unretained to avoid a circular ref.
                                  Bind(&RaftConsensus::ReportFailureDetected, Unretained(this)));
}

Status RaftConsensus::EnsureFailureDetectorDisabledUnlocked() {
  if (PREDICT_FALSE(!FLAGS_enable_leader_failure_detection)) {
    return Status::OK();
  }

  if (!failure_detector_->IsTracking(kTimerId)) {
    return Status::OK();
  }
  return failure_detector_->UnTrack(kTimerId);
}

Status RaftConsensus::ExpireFailureDetectorUnlocked() {
  if (PREDICT_FALSE(!FLAGS_enable_leader_failure_detection)) {
    return Status::OK();
  }

  return failure_detector_->MessageFrom(kTimerId, MonoTime::Min());
}

Status RaftConsensus::SnoozeFailureDetectorUnlocked() {
  return SnoozeFailureDetectorUnlocked(MonoDelta::FromMicroseconds(0), DO_NOT_LOG);
}

Status RaftConsensus::SnoozeFailureDetectorUnlocked(const MonoDelta& additional_delta,
                                                    AllowLogging allow_logging) {
  if (PREDICT_FALSE(!FLAGS_enable_leader_failure_detection)) {
    return Status::OK();
  }

  MonoTime time = MonoTime::Now();
  time.AddDelta(additional_delta);

  if (allow_logging == ALLOW_LOGGING) {
    LOG_WITH_PREFIX_UNLOCKED(INFO) << "Snoozing fail detection for election timeout "
                                   << "plus an additional " + additional_delta.ToString();
  }

  return failure_detector_->MessageFrom(kTimerId, time);
}

MonoDelta RaftConsensus::MinimumElectionTimeout() const {
  int32_t failure_timeout = FLAGS_leader_failure_max_missed_heartbeat_periods *
      FLAGS_raft_heartbeat_interval_ms;

  return MonoDelta::FromMilliseconds(failure_timeout);
}

MonoDelta RaftConsensus::LeaderElectionExpBackoffDeltaUnlocked() {
  // Compute a backoff factor based on how many leader elections have
  // taken place since a leader was successfully elected.
  int term_difference = state_->GetCurrentTermUnlocked() -
    state_->GetCommittedOpIdUnlocked().term();
  double backoff_factor = pow(1.1, term_difference);
  double min_timeout = MinimumElectionTimeout().ToMilliseconds();
  double max_timeout = std::min<double>(
      min_timeout * backoff_factor,
      FLAGS_leader_failure_exp_backoff_max_delta_ms);
  if (max_timeout < min_timeout) {
    LOG(INFO) << "Resetting max_timeout from " <<  max_timeout << " to " << min_timeout
              << ", max_delta_flag=" << FLAGS_leader_failure_exp_backoff_max_delta_ms;
    max_timeout = min_timeout;
  }
  // Randomize the timeout between the minimum and the calculated value.
  // We do this after the above capping to the max. Otherwise, after a
  // churny period, we'd end up highly likely to backoff exactly the max
  // amount.
  double timeout = min_timeout + (max_timeout - min_timeout) * rng_.NextDoubleFraction();
  DCHECK_GE(timeout, min_timeout);

  return MonoDelta::FromMilliseconds(timeout);
}

Status RaftConsensus::IncrementTermUnlocked() {
  return HandleTermAdvanceUnlocked(state_->GetCurrentTermUnlocked() + 1);
}

Status RaftConsensus::HandleTermAdvanceUnlocked(ConsensusTerm new_term) {
  if (new_term <= state_->GetCurrentTermUnlocked()) {
    return STATUS(IllegalState, Substitute("Can't advance term to: $0 current term: $1 is higher.",
                                           new_term, state_->GetCurrentTermUnlocked()));
  }

  if (state_->GetActiveRoleUnlocked() == RaftPeerPB::LEADER) {
    LOG_WITH_PREFIX_UNLOCKED(INFO) << "Stepping down as leader of term "
                                   << state_->GetCurrentTermUnlocked()
                                   << " since new term is " << new_term;

    RETURN_NOT_OK(BecomeReplicaUnlocked());
  }

  LOG_WITH_PREFIX_UNLOCKED(INFO) << "Advancing to term " << new_term;
  RETURN_NOT_OK(state_->SetCurrentTermUnlocked(new_term));
  term_metric_->set_value(new_term);
  return Status::OK();
}

void RaftConsensus::RollbackIdAndDeleteOpId(const ReplicateMsgPtr& replicate_msg,
                                            bool should_exists) {
  std::unique_ptr<OpId> op_id(replicate_msg->release_id());
  state_->CancelPendingOperation(*op_id, should_exists);
}

uint64_t RaftConsensus::OnDiskSize() const {
  return state_->OnDiskSize();
}

yb::OpId RaftConsensus::WaitForSafeOpIdToApply(const yb::OpId& op_id) {
  return log_->WaitForSafeOpIdToApply(op_id);
}

void RaftConsensus::SetPropagatedSafeTimeProvider(std::function<HybridTime()> provider) {
  queue_->SetPropagatedSafeTimeProvider(std::move(provider));
}

void RaftConsensus::SetMajorityReplicatedListener(std::function<void()> updater) {
  majority_replicated_listener_ = std::move(updater);
}

}  // namespace consensus
}  // namespace yb
