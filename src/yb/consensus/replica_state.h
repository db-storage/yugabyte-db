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
#ifndef YB_CONSENSUS_REPLICA_STATE_H
#define YB_CONSENSUS_REPLICA_STATE_H

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "yb/common/hybrid_time.h"
#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus_meta.h"
#include "yb/consensus/consensus_queue.h"
#include "yb/consensus/log_util.h"
#include "yb/consensus/leader_lease.h"
#include "yb/gutil/port.h"
#include "yb/util/locks.h"
#include "yb/util/status.h"
#include "yb/util/enums.h"

namespace yb {

class HostPort;
class ReplicaState;
class ThreadPool;

namespace consensus {

// Class that coordinates access to the replica state (independently of Role).
// This has a 1-1 relationship with RaftConsensus and is essentially responsible for
// keeping state and checking if state changes are viable.
// DHQ: 跟RaftConsensus 1-1,每个Consensus里面有个ReplicaState
// Note that, in the case of a LEADER role, there are two configuration states that
// that are tracked: a pending and a committed configuration. The "active" state is
// considered to be the pending configuration if it is non-null, otherwise the
// committed configuration is the active configuration.
//
// When a replica becomes a leader of a configuration, it sets the pending configuration to
// a new configuration declaring itself as leader and sets its "active" role to LEADER.
// It then starts up ConsensusPeers for each member of the pending configuration and
// tries to push a new configuration to the peers. Once that configuration is
// pushed to a majority of the cluster, it is considered committed and the
// replica flushes that configuration to disk as the committed configuration.
// DHQ: Leader在当选后，会push pending configuration，然后让其变成committed。这个为什么需要呢？每次当选，都是变化？ RaftConfigPB里面实际上还包含了Role，这个肯定是变化的
// Each time an operation is to be performed on the replica the appropriate LockFor*()
// method should be called. The LockFor*() methods check that the replica is in the
// appropriate state to perform the requested operation and returns the lock or return
// Status::IllegalState if that is not the case.
//
// All state reading/writing methods acquire the lock, unless suffixed by "Unlocked", in
// which case a lock should be obtained prior to calling them.
class ReplicaState {//DHQ: 这个应该是对应Consensus的多个Replica的状态，不是一个Replica
 public:
  enum State {
    // State after the replica is built.
    kInitialized,

    // State signaling the replica accepts requests (from clients
    // if leader, from leader if follower)
    kRunning,

    // State signaling that the replica is shutting down and no longer accepting
    // new transactions or commits.
    kShuttingDown,

    // State signaling the replica is shut down and does not accept
    // any more requests.
    kShutDown
  };

  typedef std::unique_lock<std::mutex> UniqueLock;

  typedef std::map<int64_t, scoped_refptr<ConsensusRound> > IndexToRoundMap;

  // Used internally for storing the role + term combination atomically.
  using PackedRoleAndTerm = uint64;

  ReplicaState(ConsensusOptions options, std::string peer_uuid,
               std::unique_ptr<ConsensusMetadata> cmeta,
               ReplicaOperationFactory* operation_factory,
               SafeOpIdWaiter* safe_op_id_waiter);

  CHECKED_STATUS StartUnlocked(const OpId& last_in_wal);

  // Should be used only to assert that the update_lock_ is held.
  bool IsLocked() const WARN_UNUSED_RESULT;

  // Locks a replica in preparation for StartUnlocked(). Makes
  // sure the replica is in kInitialized state.
  CHECKED_STATUS LockForStart(UniqueLock* lock) const WARN_UNUSED_RESULT;

  // Locks a replica down until the critical section of an append completes,
  // i.e. until the replicate message has been assigned an id and placed in
  // the log queue.
  // This also checks that the replica is in the appropriate
  // state (role) to replicate the provided operation, that the operation
  // contains a replicate message and is of the appropriate type, and returns
  // Status::IllegalState if that is not the case.
  CHECKED_STATUS LockForReplicate(UniqueLock* lock, const ReplicateMsg& msg) const;
  CHECKED_STATUS LockForReplicate(UniqueLock* lock) const;

  Status CheckIsActiveLeaderAndHasLease() const;

  // Locks a replica down until an the critical section of an update completes.
  // Further updates from the same or some other leader will be blocked until
  // this completes. This also checks that the replica is in the appropriate
  // state (role) to be updated and returns Status::IllegalState if that
  // is not the case.
  CHECKED_STATUS LockForUpdate(UniqueLock* lock) const WARN_UNUSED_RESULT;

  // Changes the role to non-participant and returns a lock that can be
  // used to make sure no state updates come in until Shutdown() is
  // completed.
  CHECKED_STATUS LockForShutdown(UniqueLock* lock) WARN_UNUSED_RESULT;

  CHECKED_STATUS LockForConfigChange(UniqueLock* lock) const WARN_UNUSED_RESULT;

  // Obtains the lock for a state read, does not check state.
  CHECKED_STATUS LockForRead(UniqueLock* lock) const WARN_UNUSED_RESULT;

  // Obtains the lock so that we can advance the majority replicated
  // index and possibly the committed index.
  // Requires that this peer is leader.
  CHECKED_STATUS LockForMajorityReplicatedIndexUpdate(
      UniqueLock* lock) const WARN_UNUSED_RESULT;

  // Ensure the local peer is the active leader.
  // Returns OK if leader, IllegalState otherwise.
  CHECKED_STATUS CheckActiveLeaderUnlocked(LeaderLeaseCheckMode lease_check_mode) const;

  // Completes the Shutdown() of this replica. No more operations, local
  // or otherwise can happen after this point.
  // Called after the quiescing phase (started with LockForShutdown())
  // finishes.
  CHECKED_STATUS ShutdownUnlocked() WARN_UNUSED_RESULT;

  // Return current consensus state summary.
  ConsensusStatePB ConsensusStateUnlocked(ConsensusConfigType type) const;

  // Returns the currently active Raft role.
  RaftPeerPB::Role GetActiveRoleUnlocked() const;

  // Returns true if there is a configuration change currently in-flight but not yet
  // committed.
  bool IsConfigChangePendingUnlocked() const;

  // Inverse of IsConfigChangePendingUnlocked(): returns OK if there is
  // currently *no* configuration change pending, and IllegalState is there *is* a
  // configuration change pending.
  CHECKED_STATUS CheckNoConfigChangePendingUnlocked() const;

  // Returns true if an operation is in this replica's log, namely:
  // - If the op's index is lower than or equal to our committed index
  // - If the op id matches an inflight op.
  // If an operation with the same index is in our log but the terms
  // are different 'term_mismatch' is set to true, it is false otherwise.
  bool IsOpCommittedOrPending(const OpId& op_id, bool* term_mismatch);

  // Sets the given configuration as pending commit. Does not persist into the peers
  // metadata. In order to be persisted, SetCommittedConfigUnlocked() must be called.
  CHECKED_STATUS SetPendingConfigUnlocked(const RaftConfigPB& new_config) WARN_UNUSED_RESULT;

  // Clears the pending config.
  CHECKED_STATUS ClearPendingConfigUnlocked() WARN_UNUSED_RESULT;

  // Return the pending configuration, or crash if one is not set.
  const RaftConfigPB& GetPendingConfigUnlocked() const;

  // Changes the committed config for this replica. Checks that there is a
  // pending configuration and that it is equal to this one. Persists changes to disk.
  // Resets the pending configuration to null.
  CHECKED_STATUS SetCommittedConfigUnlocked(const RaftConfigPB& new_config);

  // Return the persisted configuration.
  const RaftConfigPB& GetCommittedConfigUnlocked() const;

  // Return the "active" configuration - if there is a pending configuration return it;
  // otherwise return the committed configuration.
  const RaftConfigPB& GetActiveConfigUnlocked() const;

  // Checks if the term change is legal. If so, sets 'current_term'
  // to 'new_term' and sets 'has voted' to no for the current term.
  CHECKED_STATUS SetCurrentTermUnlocked(int64_t new_term) WARN_UNUSED_RESULT;

  // Returns the term set in the last config change round.
  const int64_t GetCurrentTermUnlocked() const;

  // Accessors for the leader of the current term.
  void SetLeaderUuidUnlocked(const std::string& uuid);
  const std::string& GetLeaderUuidUnlocked() const;
  bool HasLeaderUnlocked() const { return !GetLeaderUuidUnlocked().empty(); }
  void ClearLeaderUnlocked() { SetLeaderUuidUnlocked(""); }

  // Return whether this peer has voted in the current term.
  const bool HasVotedCurrentTermUnlocked() const;

  // Record replica's vote for the current term, then flush the consensus
  // metadata to disk.
  CHECKED_STATUS SetVotedForCurrentTermUnlocked(const std::string& uuid) WARN_UNUSED_RESULT;

  // Return replica's vote for the current term.
  // The vote must be set; use HasVotedCurrentTermUnlocked() to check.
  const std::string& GetVotedForCurrentTermUnlocked() const;

  ReplicaOperationFactory* GetReplicaOperationFactoryUnlocked() const;

  // Returns the uuid of the peer to which this replica state belongs.
  // Safe to call with or without locks held.
  const std::string& GetPeerUuid() const;

  const ConsensusOptions& GetOptions() const;

  // Aborts pending operations after, but not including 'index'. The OpId with 'index'
  // will become our new last received id. If there are pending operations with indexes
  // higher than 'index' those operations are aborted.
  CHECKED_STATUS AbortOpsAfterUnlocked(int64_t index);

  // Returns the the ConsensusRound with the provided index, if there is any, or NULL
  // if there isn't.
  scoped_refptr<ConsensusRound> GetPendingOpByIndexOrNullUnlocked(int64_t index);

  // Add 'round' to the set of rounds waiting to be committed.
  CHECKED_STATUS AddPendingOperation(const scoped_refptr<ConsensusRound>& round);

  // Marks ReplicaOperations up to 'id' as majority replicated, meaning the
  // transaction may Apply() (immediately if Prepare() has completed or when Prepare()
  // completes, if not).
  //
  // If this advanced the committed index, sets *committed_index_changed to true.
  CHECKED_STATUS UpdateMajorityReplicatedUnlocked(const OpId& majority_replicated,
                                          OpId* committed_index,
                                          bool* committed_index_changed);

  // Advances the committed index.
  // This is a no-op if the committed index has not changed.
  // Returns in '*committed_index_changed' whether the operation actually advanced
  // the index.
  CHECKED_STATUS AdvanceCommittedIndexUnlocked(const OpId& committed_index,
                                               bool* committed_index_changed = nullptr);

  // Initializes the committed index.
  // Function checks that we are in initial state, then updates committed index.
  CHECKED_STATUS InitCommittedIndexUnlocked(const OpId& committed_index);

  // Returns the watermark below which all operations are known to
  // be committed according to consensus.
  //
  // This must be called under a lock.
  const OpId& GetCommittedOpIdUnlocked() const;

  // Returns true iff an op from the current term has been committed.
  bool AreCommittedAndCurrentTermsSameUnlocked() const;

  // Updates the last received operation.
  // This must be called under a lock.
  void UpdateLastReceivedOpIdUnlocked(const OpId& op_id);

  // Returns the last received op id. This must be called under the lock.
  const OpId& GetLastReceivedOpIdUnlocked() const;

  // Returns the id of the last op received from the current leader.
  const OpId& GetLastReceivedOpIdCurLeaderUnlocked() const;

  // Returns the id of the latest pending transaction (i.e. the one with the
  // latest index). This must be called under the lock.
  OpId GetLastPendingOperationOpIdUnlocked() const;

  // Used by replicas to cancel pending transactions. Pending transaction are those
  // that have completed prepare/replicate but are waiting on the LEADER's commit
  // to complete. This does not cancel transactions being applied.
  CHECKED_STATUS CancelPendingOperations();

  // API to dump pending transactions. Added to debug ENG-520.
  void DumpPendingOperationsUnlocked();

  void NewIdUnlocked(OpId* id);

  // Used when, for some reason, an operation that failed before it could be considered
  // a part of the state machine. Basically restores the id gen to the state it was before
  // generating 'id'. So that we reuse these ids later, when we can actually append to the
  // state machine. This makes the state machine have continuous ids for the same term, even if
  // the queue refused to add any more operations.
  // should_exists indicates whether we expect that this operation is already added.
  // Used for debugging purposes only.
  void CancelPendingOperation(const OpId& id, bool should_exist);

  // Accessors for pending election op id. These must be called under a lock.
  const OpId& GetPendingElectionOpIdUnlocked() { return pending_election_opid_; }
  void SetPendingElectionOpIdUnlocked(const OpId& opid) { pending_election_opid_ = opid; }
  void ClearPendingElectionOpIdUnlocked() { pending_election_opid_.Clear(); }
  bool HasOpIdCommittedUnlocked(const OpId& opid) {
    return (opid.IsInitialized() && OpIdCompare(opid, GetCommittedOpIdUnlocked()) <= 0);
  }

  std::string ToString() const;
  std::string ToStringUnlocked() const;

  // A common prefix that should be in any log messages emitted,
  // identifying the tablet and peer.
  std::string LogPrefix();
  std::string LogPrefixUnlocked() const;

  // A variant of LogPrefix which does not take the lock. This is a slightly
  // less thorough prefix which only includes immutable (and thus thread-safe)
  // information, but does not require the lock.
  std::string LogPrefixThreadSafe() const;

  // Checks that 'current' correctly follows 'previous'. Specifically it checks
  // that the term is the same or higher and that the index is sequential.
  static CHECKED_STATUS CheckOpInSequence(const OpId& previous, const OpId& current);

  // Return the current state of this object.
  // The update_lock_ must be held.
  ReplicaState::State state() const;

  // Update the point in time we have to wait until before starting to act as a leader in case
  // we win an election.
  void UpdateOldLeaderLeaseExpirationUnlocked(
      MonoDelta lease_duration, MicrosTime ht_lease_expiration);
  void UpdateOldLeaderLeaseExpirationUnlocked(
      MonoTime lease_expiration, MicrosTime ht_lease_expiration);

  void SetMajorityReplicatedLeaseExpirationUnlocked(
      const MajorityReplicatedData& majority_replicated_data);

  // Checks two conditions:
  // - That the old leader definitely does not have a lease.
  // - That this leader has a committed lease.
  LeaderLeaseStatus GetLeaderLeaseStatusUnlocked(
      MonoDelta* remaining_old_leader_lease = nullptr) const;

  LeaderLeaseStatus GetHybridTimeLeaseStatusAtUnlocked(MicrosTime micros_time) const;

  // Get the remaining duration of the old leader's lease. Optionally, return the current time in
  // the "now" output parameter. In case the old leader's lease has already expired or is not known,
  // returns an uninitialized MonoDelta value.
  MonoDelta RemainingOldLeaderLeaseDuration(MonoTime* now = nullptr) const;

  MicrosTime old_leader_ht_lease_expiration() const {
    return old_leader_ht_lease_expiration_;
  }

  // A lock-free way to read role and term atomically.
  std::pair<RaftPeerPB::Role, int64_t> GetRoleAndTerm() const;

  bool MajorityReplicatedLeaderLeaseExpired(MonoTime* now = nullptr) const;

  bool MajorityReplicatedHybridTimeLeaseExpiredAt(MicrosTime hybrid_time) const;

  // Get the current majority-replicated hybrid time leader lease expiration time as a microsecond
  // timestamp.
  // @param min_allowed - will wait until the majority-replicated hybrid time leader lease reaches
  //                      at least this microsecond timestamp.
  // @param deadline - won't wait past this deadline.
  // @return leader lease or 0 if timed out.
  MicrosTime MajorityReplicatedHtLeaseExpiration(MicrosTime min_allowed, MonoTime deadline) const;

  // The on-disk size of the consensus metadata.
  uint64_t OnDiskSize() const;

 private:

  template <class Policy>
  LeaderLeaseStatus GetLeaseStatusUnlocked(Policy policy) const;

  // To maintain safety, we need to check that the committed entry is actually
  // present in our log (and as a result, is either already committed locally, which means there
  // is nothing to do, or present in the pending operations map).
  Status CheckOperationExist(const OpId& committed_index, IndexToRoundMap::iterator* end_iter);

  // Apply pending operations beginning at iter up to committed_index.
  // Updates last_committed_index_ to committed_index.
  CHECKED_STATUS ApplyPendingOperations(IndexToRoundMap::iterator iter,
                                        const OpId& committed_index);
  CHECKED_STATUS ApplyPendingOperationsUnlocked(IndexToRoundMap::iterator iter,
                                                const OpId& committed_index);

  void SetLastCommittedIndexUnlocked(const OpId& committed_index);

  // Store role and term in a lock-free way. This is normally only called when the lock is being
  // held anyway, but read without the lock.
  void StoreRoleAndTerm(RaftPeerPB::Role role, int64_t term);

  // Applies committed config change.
  void ApplyConfigChangeUnlocked(const ConsensusRoundPtr& round);

  const ConsensusOptions options_;

  // The UUID of the local peer.
  const std::string peer_uuid_;

  mutable std::mutex update_lock_;
  mutable std::condition_variable cond_;

  // Consensus metadata persistence object.
  std::unique_ptr<ConsensusMetadata> cmeta_;

  // Active role and term. Stored as a separate atomic field for fast read-only access. This is
  // still only modified under the lock.
  std::atomic<PackedRoleAndTerm> role_and_term_;

  // Used by the LEADER. This is the index of the next operation generated
  // by this LEADER.
  int64_t next_index_ = 0;

  // Index=>Round map that manages pending ops, i.e. operations for which we've
  // received a replicate message from the leader but have yet to be committed.
  // The key is the index of the replicate operation.
  IndexToRoundMap pending_operations_;

  // When we receive a message from a remote peer telling us to start a operation, we use
  // this factory to start it.
  ReplicaOperationFactory* operation_factory_;

  // Used to wait for safe op id during apply of committed entries.
  SafeOpIdWaiter* safe_op_id_waiter_;

  // The id of the last received operation, which corresponds to the last entry
  // written to the local log. Operations whose id is lower than or equal to
  // this id do not need to be resent by the leader. This is not guaranteed to
  // be monotonically increasing due to the possibility for log truncation and
  // aborted operations when a leader change occurs.
  OpId last_received_op_id_ = MinimumOpId();

  // Same as last_received_op_id_ but only includes operations sent by the
  // current leader. The "term" in this op may not actually match the current
  // term, since leaders may replicate ops from prior terms.
  //
  // As an implementation detail, this field is reset to MinumumOpId() every
  // time there is a term advancement on the local node, to simplify the logic
  // involved in resetting this every time a new node becomes leader.
  OpId last_received_op_id_current_leader_ = MinimumOpId();

  // The id of the Apply that was last triggered when the last message from the leader
  // was received. Initialized to MinimumOpId().
  OpId last_committed_index_ = MinimumOpId();

  // If set, a leader election is pending upon the specific op id commitment to this peer's log.
  OpId pending_election_opid_;

  State state_ = State::kInitialized;

  // When a follower becomes the leader, it uses this field to wait out the old leader's lease
  // before accepting writes or serving up-to-date reads. This is also used by candidates by
  // granting a vote. We compute the amount of time the new leader has to wait to make sure the old
  // leader's lease has expired.
  //
  // This is marked mutable because it can be reset on to MonoTime::kMin on the read path after the
  // deadline has passed, so that we avoid querying the clock unnecessarily from that point on.
  mutable MonoTime old_leader_lease_expiration_;

  mutable MicrosTime old_leader_ht_lease_expiration_ = HybridTime::kMin.GetPhysicalValueMicros();

  // LEADER only: the latest committed lease expiration deadline for the current leader. The leader
  // is allowed to serve up-to-date reads and accept writes only while the current time is less than
  // this. However, the leader might manage to replicate a lease extension without losing its
  // leadership.
  MonoTime majority_replicated_lease_expiration_;

  // LEADER only: the latest committed hybrid time lease expiration deadline for the current leader.
  // The leader is allowed to add new log entries only when lease of old leader is expired.
  std::atomic<MicrosTime> majority_replicated_ht_lease_expiration_
      {HybridTime::kMin.GetPhysicalValueMicros()};
};

}  // namespace consensus
}  // namespace yb

#endif // YB_CONSENSUS_REPLICA_STATE_H_
