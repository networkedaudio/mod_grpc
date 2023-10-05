// Generated by the gRPC C++ plugin.
// If you make any local change, they will be lost.
// source: stream.proto

#include "stream.pb.h"
#include "stream.grpc.pb.h"

#include <functional>
#include <grpcpp/impl/codegen/async_stream.h>
#include <grpcpp/impl/codegen/async_unary_call.h>
#include <grpcpp/impl/codegen/channel_interface.h>
#include <grpcpp/impl/codegen/client_unary_call.h>
#include <grpcpp/impl/codegen/client_callback.h>
#include <grpcpp/impl/codegen/message_allocator.h>
#include <grpcpp/impl/codegen/method_handler.h>
#include <grpcpp/impl/codegen/rpc_service_method.h>
#include <grpcpp/impl/codegen/server_callback.h>
#include <grpcpp/impl/codegen/server_callback_handlers.h>
#include <grpcpp/impl/codegen/server_context.h>
#include <grpcpp/impl/codegen/service_type.h>
#include <grpcpp/impl/codegen/sync_stream.h>
namespace amd {

static const char* Api_method_names[] = {
  "/amd.Api/StreamPCM",
};

std::unique_ptr< Api::Stub> Api::NewStub(const std::shared_ptr< ::grpc::ChannelInterface>& channel, const ::grpc::StubOptions& options) {
  (void)options;
  std::unique_ptr< Api::Stub> stub(new Api::Stub(channel, options));
  return stub;
}

Api::Stub::Stub(const std::shared_ptr< ::grpc::ChannelInterface>& channel, const ::grpc::StubOptions& options)
  : channel_(channel), rpcmethod_StreamPCM_(Api_method_names[0], options.suffix_for_stats(),::grpc::internal::RpcMethod::BIDI_STREAMING, channel)
  {}

::grpc::ClientReaderWriter< ::amd::StreamPCMRequest, ::amd::StreamPCMResponse>* Api::Stub::StreamPCMRaw(::grpc::ClientContext* context) {
  return ::grpc::internal::ClientReaderWriterFactory< ::amd::StreamPCMRequest, ::amd::StreamPCMResponse>::Create(channel_.get(), rpcmethod_StreamPCM_, context);
}

void Api::Stub::async::StreamPCM(::grpc::ClientContext* context, ::grpc::ClientBidiReactor< ::amd::StreamPCMRequest,::amd::StreamPCMResponse>* reactor) {
  ::grpc::internal::ClientCallbackReaderWriterFactory< ::amd::StreamPCMRequest,::amd::StreamPCMResponse>::Create(stub_->channel_.get(), stub_->rpcmethod_StreamPCM_, context, reactor);
}

::grpc::ClientAsyncReaderWriter< ::amd::StreamPCMRequest, ::amd::StreamPCMResponse>* Api::Stub::AsyncStreamPCMRaw(::grpc::ClientContext* context, ::grpc::CompletionQueue* cq, void* tag) {
  return ::grpc::internal::ClientAsyncReaderWriterFactory< ::amd::StreamPCMRequest, ::amd::StreamPCMResponse>::Create(channel_.get(), cq, rpcmethod_StreamPCM_, context, true, tag);
}

::grpc::ClientAsyncReaderWriter< ::amd::StreamPCMRequest, ::amd::StreamPCMResponse>* Api::Stub::PrepareAsyncStreamPCMRaw(::grpc::ClientContext* context, ::grpc::CompletionQueue* cq) {
  return ::grpc::internal::ClientAsyncReaderWriterFactory< ::amd::StreamPCMRequest, ::amd::StreamPCMResponse>::Create(channel_.get(), cq, rpcmethod_StreamPCM_, context, false, nullptr);
}

Api::Service::Service() {
  AddMethod(new ::grpc::internal::RpcServiceMethod(
      Api_method_names[0],
      ::grpc::internal::RpcMethod::BIDI_STREAMING,
      new ::grpc::internal::BidiStreamingHandler< Api::Service, ::amd::StreamPCMRequest, ::amd::StreamPCMResponse>(
          [](Api::Service* service,
             ::grpc::ServerContext* ctx,
             ::grpc::ServerReaderWriter<::amd::StreamPCMResponse,
             ::amd::StreamPCMRequest>* stream) {
               return service->StreamPCM(ctx, stream);
             }, this)));
}

Api::Service::~Service() {
}

::grpc::Status Api::Service::StreamPCM(::grpc::ServerContext* context, ::grpc::ServerReaderWriter< ::amd::StreamPCMResponse, ::amd::StreamPCMRequest>* stream) {
  (void) context;
  (void) stream;
  return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
}


}  // namespace amd

