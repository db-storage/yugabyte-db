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

#include <list>
#include <string>
#include <vector>

#include <gflags/gflags.h>

#include "yb/gutil/casts.h"
#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rpc/acceptor.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/service_if.h"
#include "yb/rpc/service_pool.h"
#include "yb/rpc/thread_pool.h"
#include "yb/server/rpc_server.h"
#include "yb/util/flag_tags.h"
#include "yb/util/status.h"

using yb::rpc::Messenger;
using yb::rpc::ServiceIf;
using std::shared_ptr;
using std::string;
using std::vector;
using strings::Substitute;
using std::unique_ptr;
using std::make_unique;

DEFINE_string(rpc_bind_addresses, "0.0.0.0",
              "Comma-separated list of addresses to bind to for RPC connections. "
              "Currently, ephemeral ports (i.e. port 0) are not allowed.");
TAG_FLAG(rpc_bind_addresses, stable);

DEFINE_bool(rpc_server_allow_ephemeral_ports, false,
            "Allow binding to ephemeral ports. This can cause problems, so currently "
            "only allowed in tests.");
TAG_FLAG(rpc_server_allow_ephemeral_ports, unsafe);

DEFINE_int32(rpc_queue_limit, 5000, "Queue limit for rpc server");
DEFINE_int32(rpc_workers_limit, 128, "Workers limit for rpc server");
DECLARE_int32(rpc_default_keepalive_time_ms);

namespace yb {

RpcServerOptions::RpcServerOptions()
  : rpc_bind_addresses(FLAGS_rpc_bind_addresses),
    default_port(0),
    queue_limit(FLAGS_rpc_queue_limit),
    workers_limit(FLAGS_rpc_workers_limit),
    connection_keepalive_time_ms(FLAGS_rpc_default_keepalive_time_ms) {
}

RpcServer::RpcServer(const std::string& name, RpcServerOptions opts)
    : name_(name),
      server_state_(UNINITIALIZED),
      options_(std::move(opts)),
      normal_thread_pool_(CreateThreadPool(name, ServicePriority::kNormal)) {}

RpcServer::~RpcServer() {
  Shutdown();
}

string RpcServer::ToString() const {
  // TODO: include port numbers, etc.
  return "RpcServer";
}

Status RpcServer::Init(const shared_ptr<Messenger>& messenger) {
  CHECK_EQ(server_state_, UNINITIALIZED);
  messenger_ = messenger;

  RETURN_NOT_OK(HostPort::ParseStrings(options_.rpc_bind_addresses,
                                       options_.default_port,
                                       &rpc_host_port_));

  RETURN_NOT_OK(ParseAddressList(options_.rpc_bind_addresses,
                                 options_.default_port,
                                 &rpc_bind_addresses_));
  for (const auto& addr : rpc_bind_addresses_) {
    if (IsPrivilegedPort(addr.port())) {
      LOG(WARNING) << "May be unable to bind to privileged port for address " << addr;
    }

    // Currently, we can't support binding to ephemeral ports outside of
    // unit tests, because consensus caches RPC ports of other servers
    // across restarts. See KUDU-334.
    if (addr.port() == 0 && !FLAGS_rpc_server_allow_ephemeral_ports) {
      LOG(FATAL) << "Binding to ephemeral ports not supported (RPC address "
                 << "configured to " << addr << ")";
    }
  }

  server_state_ = INITIALIZED;
  return Status::OK();
}

Status RpcServer::RegisterService(size_t queue_limit,
                                  unique_ptr<rpc::ServiceIf> service,
                                  ServicePriority priority) {//DHQ: 这个相当于跟一个thread_pool关联起来
  CHECK(server_state_ == INITIALIZED ||
        server_state_ == BOUND) << "bad state: " << server_state_;
  const scoped_refptr<MetricEntity>& metric_entity = messenger_->metric_entity();
  string service_name = service->service_name();

  rpc::ThreadPool* thread_pool = normal_thread_pool_.get();
  if (priority == ServicePriority::kHigh) {
    if (!high_priority_thread_pool_) {
      high_priority_thread_pool_ = CreateThreadPool(name_, ServicePriority::kHigh);
    }
    thread_pool = high_priority_thread_pool_.get();
  }

  scoped_refptr<rpc::ServicePool> service_pool =
    new rpc::ServicePool(queue_limit, thread_pool, std::move(service), metric_entity);//DHQ: 这个应该才运行起来了
  RETURN_NOT_OK(messenger_->RegisterService(service_name, service_pool));
  return Status::OK();
}

Status RpcServer::Bind() {
  CHECK_EQ(server_state_, INITIALIZED);

  rpc_bound_addresses_.resize(rpc_bind_addresses_.size());
  for (size_t i = 0; i != rpc_bind_addresses_.size(); ++i) {
    RETURN_NOT_OK(messenger_->ListenAddress(rpc_bind_addresses_[i], &rpc_bound_addresses_[i]));
  }

  server_state_ = BOUND;
  return Status::OK();
}

Status RpcServer::Start() {
  if (server_state_ == INITIALIZED) {
    RETURN_NOT_OK(Bind());
  }
  CHECK_EQ(server_state_, BOUND);
  server_state_ = STARTED;

  RETURN_NOT_OK(messenger_->StartAcceptor());
  string bound_addrs_str;
  for (const auto& bind_addr : rpc_bound_addresses_) {
    if (!bound_addrs_str.empty()) bound_addrs_str += ", ";
    bound_addrs_str += yb::ToString(bind_addr);
  }
  LOG(INFO) << "RPC server started. Bound to: " << bound_addrs_str;

  return Status::OK();
}

void RpcServer::Shutdown() {
  normal_thread_pool_->Shutdown();
  if (high_priority_thread_pool_) {
    high_priority_thread_pool_->Shutdown();
  }

  if (messenger_) {
    messenger_->ShutdownAcceptor();
    WARN_NOT_OK(messenger_->UnregisterAllServices(), "Unable to unregister our services");
  }
}

const rpc::ServicePool* RpcServer::service_pool(const string& service_name) const {
  return down_cast<rpc::ServicePool*>(messenger_->rpc_service(service_name).get());
}

unique_ptr<rpc::ThreadPool> RpcServer::CreateThreadPool(
    string name_prefix, ServicePriority priority) {
  string name = priority == ServicePriority::kHigh ? Format("$0-high-pri", name_prefix)
                                                   : name_prefix;
  VLOG(1) << "Creating thread pool '" << name << "'";
  return make_unique<rpc::ThreadPool>(name, options_.queue_limit, options_.workers_limit);
}

} // namespace yb
