// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/rpc/grpc_server.h"

#include <grpcpp/ext/channelz_service_plugin.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/impl/service_type.h>

#include <algorithm>
#include <boost/asio/detail/socket_holder.hpp>
#include <memory>
#include <string>
#include <utility>

#include "ray/common/ray_config.h"
#include "ray/rpc/common.h"
#include "ray/util/thread_utils.h"

namespace ray {
namespace rpc {

void GrpcServer::Init() {
  RAY_CHECK(num_threads_ > 0) << "Num of threads in gRPC must be greater than 0";
  cqs_.resize(num_threads_);
  // Enable built in health check implemented by gRPC:
  //   https://github.com/grpc/grpc/blob/master/doc/health-checking.md
  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  grpc::channelz::experimental::InitChannelzService();
}

void GrpcServer::Shutdown() {
  if (!is_shutdown_) {
    // Drain the executor threads.
    // Shutdown the server with an immediate deadline.
    // TODO(edoakes): do we want to do this in all cases?
    server_->Shutdown(gpr_now(GPR_CLOCK_REALTIME));
    for (const auto &cq : cqs_) {
      cq->Shutdown();
    }
    for (auto &polling_thread : polling_threads_) {
      polling_thread.join();
    }
    is_shutdown_ = true;
    RAY_LOG(DEBUG) << "gRPC server of " << name_ << " shutdown.";
    server_.reset();
  }
}

void GrpcServer::Run() {
  uint32_t specified_port = port_;
  std::string server_address((listen_to_localhost_only_ ? "127.0.0.1:" : "0.0.0.0:") +
                             std::to_string(port_));
  grpc::ServerBuilder builder;
  // Disable the SO_REUSEPORT option. We don't need it in ray. If the option is enabled
  // (default behavior in grpc), we may see multiple workers listen on the same port and
  // the requests sent to this port may be handled by any of the workers.
  builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
  builder.AddChannelArgument(GRPC_ARG_MAX_SEND_MESSAGE_LENGTH,
                             RayConfig::instance().max_grpc_message_size());
  builder.AddChannelArgument(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH,
                             RayConfig::instance().max_grpc_message_size());
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, keepalive_time_ms_);
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,
                             RayConfig::instance().grpc_keepalive_timeout_ms());
  builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 0);
  builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
  builder.AddChannelArgument(GRPC_ARG_HTTP2_WRITE_BUFFER_SIZE,
                             RayConfig::instance().grpc_stream_buffer_size());
  // NOTE(rickyyx): This argument changes how frequent the gRPC server expects a keepalive
  // ping from the client. See https://github.com/grpc/grpc/blob/HEAD/doc/keepalive.md#faq
  // We set this to 1min because GCS gRPC client currently sends keepalive every 1min:
  // https://github.com/ray-project/ray/blob/releases/2.0.0/python/ray/_private/gcs_utils.py#L72
  // Setting this value larger will trigger GOAWAY from the gRPC server to be sent to the
  // client to back-off keepalive pings. (https://github.com/ray-project/ray/issues/25367)
  builder.AddChannelArgument(
      GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS,
      // If the `client_keepalive_time` is  smaller than this, the client will receive
      // "too many pings" error and crash.
      std::min(static_cast<int64_t>(60000),
               RayConfig::instance().grpc_client_keepalive_time_ms()));
  if (RayConfig::instance().USE_TLS()) {
    // Create credentials from locations specified in config
    std::string rootcert = ReadCert(RayConfig::instance().TLS_CA_CERT());
    std::string servercert = ReadCert(RayConfig::instance().TLS_SERVER_CERT());
    std::string serverkey = ReadCert(RayConfig::instance().TLS_SERVER_KEY());
    grpc::SslServerCredentialsOptions::PemKeyCertPair pkcp = {serverkey, servercert};
    grpc::SslServerCredentialsOptions ssl_opts(
        GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY);
    ssl_opts.pem_root_certs = rootcert;
    ssl_opts.pem_key_cert_pairs.push_back(pkcp);

    // Create server credentials
    std::shared_ptr<grpc::ServerCredentials> server_creds;
    server_creds = grpc::SslServerCredentials(ssl_opts);
    builder.AddListeningPort(server_address, server_creds, &port_);
  } else {
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials(), &port_);
  }
  // Register all the services to this server.
  if (grpc_services_.empty() && services_.empty()) {
    RAY_LOG(WARNING) << "No service is found when start grpc server " << name_;
  }
  for (auto &service : grpc_services_) {
    builder.RegisterService(service.get());
  }
  for (auto &user_service : services_) {
    builder.RegisterService(&user_service->GetGrpcService());
  }
  // Get hold of the completion queue used for the asynchronous communication
  // with the gRPC runtime.
  for (int i = 0; i < num_threads_; i++) {
    cqs_[i] = builder.AddCompletionQueue();
  }
  // Build and start server.
  server_ = builder.BuildAndStart();

  RAY_CHECK(server_)
      << "Failed to start the grpc server. The specified port is " << specified_port
      << ". This means that Ray's core components will not be able to function "
      << "correctly. If the server startup error message is `Address already in use`, "
      << "it indicates the server fails to start because the port is already used by "
      << "other processes (such as --node-manager-port, --object-manager-port, "
      << "--gcs-server-port, and ports between --min-worker-port, --max-worker-port). "
      << "Try running sudo lsof -i :" << specified_port
      << " to check if there are other processes listening to the port.";
  RAY_CHECK(port_ > 0);
  RAY_LOG(INFO) << name_ << " server started, listening on port " << port_ << ".";

  // Create calls for all the server call factories
  //
  // NOTE: That ServerCallFactory is created for every thread processing respective
  //       CompletionQueue
  for (auto &entry : server_call_factories_) {
    // Derive target max inflight RPCs buffer based on `gcs_max_active_rpcs_per_handler`
    //
    // NOTE: For these handlers that have set it to -1, we set default (per
    //       thread) buffer at 32, though it doesn't have any impact on concurrency
    //       (since we're recreating new instance of `ServerCall` as soon as one
    //       gets occupied therefore not serving as back-pressure mechanism)
    size_t buffer_size;
    if (entry->GetMaxActiveRPCs() != -1) {
      buffer_size = std::max<size_t>(
          1, static_cast<size_t>(entry->GetMaxActiveRPCs() / num_threads_));
    } else {
      buffer_size = 32;
    }

    for (size_t j = 0; j < buffer_size; j++) {
      // Create pending `ServerCall` ready to accept incoming requests
      entry->CreateCall();
    }
  }
  // Start threads that polls incoming requests.
  for (int i = 0; i < num_threads_; i++) {
    polling_threads_.emplace_back(&GrpcServer::PollEventsFromCompletionQueue, this, i);
  }
  // Set the server as running.
  is_shutdown_ = false;
}

void GrpcServer::RegisterService(std::unique_ptr<grpc::Service> &&grpc_service) {
  grpc_services_.push_back(std::move(grpc_service));
}

void GrpcServer::RegisterService(std::unique_ptr<GrpcService> &&service,
                                 bool token_auth) {
  for (int i = 0; i < num_threads_; i++) {
    if (token_auth && cluster_id_.IsNil()) {
      RAY_LOG(FATAL) << "Expected cluster ID for token auth!";
    }
    service->InitServerCallFactories(cqs_[i], &server_call_factories_, cluster_id_);
  }
  services_.push_back(std::move(service));
}

void GrpcServer::PollEventsFromCompletionQueue(int index) {
  SetThreadName("server.poll" + std::to_string(index));
  void *tag;
  bool ok;

  // Keep reading events from the `CompletionQueue` until it's shutdown.
  while (true) {
    auto deadline = gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                                 gpr_time_from_millis(250, GPR_TIMESPAN));
    auto status = cqs_[index]->AsyncNext(&tag, &ok, deadline);
    if (status == grpc::CompletionQueue::SHUTDOWN) {
      // If the completion queue status is SHUTDOWN, meaning the queue has been
      // drained. We can now exit the loop.
      break;
    } else if (status == grpc::CompletionQueue::TIMEOUT) {
      continue;
    }
    auto *server_call = static_cast<ServerCall *>(tag);
    bool delete_call = false;
    // A new call is needed after the server sends a reply, no matter the reply is
    // successful or failed.
    bool need_new_call = false;
    if (ok) {
      switch (server_call->GetState()) {
      case ServerCallState::PENDING:
        // We've received a new incoming request. Now this call object is used to
        // track this request.
        server_call->HandleRequest();
        break;
      case ServerCallState::SENDING_REPLY:
        // GRPC has sent reply successfully, invoking the callback.
        server_call->OnReplySent();
        // The rpc call has finished and can be deleted now.
        delete_call = true;
        // A new call should be suplied.
        need_new_call = true;
        break;
      default:
        RAY_LOG(FATAL) << "Shouldn't reach here.";
        break;
      }
    } else {
      // `ok == false` will occur in two situations:

      // First, server has sent reply to client and failed, the server call's status is
      // SENDING_REPLY. This can happen, for example, when the client deadline has
      // exceeded or the client side is dead.
      if (server_call->GetState() == ServerCallState::SENDING_REPLY) {
        server_call->OnReplyFailed();
        // A new call should be suplied.
        need_new_call = true;
      }
      // Second, the server has been shut down, the server call's status is PENDING.
      // And don't need to do anything other than deleting this call.
      // See
      // https://grpc.github.io/grpc/cpp/classgrpc_1_1_completion_queue.html#a86d9810ced694e50f7987ac90b9f8c1a
      // for more details.
      delete_call = true;
    }
    if (delete_call) {
      if (need_new_call && server_call->GetServerCallFactory().GetMaxActiveRPCs() != -1) {
        // Create a new `ServerCall` to accept the next incoming request.
        server_call->GetServerCallFactory().CreateCall();
      }
      delete server_call;
    }
  }
}
}  // namespace rpc
}  // namespace ray
