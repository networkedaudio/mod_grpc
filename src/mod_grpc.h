#ifndef MOD_GRPC_LIBRARY_H
#define MOD_GRPC_LIBRARY_H

extern "C" {
#include <switch.h>
}

#include <thread>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>

#include "generated/fs.grpc.pb.h"
#include "Cluster.h"

#define GRPC_SUCCESS_ORIGINATE "grpc_originate_success"

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerCompletionQueue;
using grpc::Status;

namespace mod_grpc {
    SWITCH_MODULE_LOAD_FUNCTION(mod_grpc_load);
    SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_grpc_shutdown);

    // Logic and data behind the server's behavior.
    class ApiServiceImpl final : public fs::Api::Service {
        Status Originate(ServerContext* context, const fs::OriginateRequest* request,
                         fs::OriginateResponse* reply) override;

        Status Execute(ServerContext* context, const fs::ExecuteRequest* request,
                        fs::ExecuteResponse* reply) override;

        Status SetVariables(ServerContext* context, const fs::SetVariablesRequest* request,
                            fs::SetVariablesResponse* reply) override;

        Status Bridge(ServerContext* context, const fs::BridgeRequest* request,
                      fs::BridgeResponse* reply) override;

        Status Hangup(ServerContext* context, const fs::HangupRequest* request,
                        fs::HangupResponse* reply) override;

        Status HangupMatchingVars(ServerContext* context, const fs::HangupMatchingVarsReqeust* request,
                                  fs::HangupMatchingVarsResponse* reply) override;

        Status Queue(ServerContext* context, const fs::QueueRequest* request,
                                  fs::QueueResponse* reply) override;

        Status HangupMany(ServerContext* context, const fs::HangupManyRequest* request,
                      fs::HangupManyResponse* reply) override;

        Status Hold(ServerContext* context, const fs::HoldRequest* request,
                      fs::HoldResponse* reply) override;

        Status UnHold(ServerContext* context, const fs::UnHoldRequest* request,
                      fs::UnHoldResponse* reply) override;

    };

    struct Config {
        char const *consul_address;
        int consul_tts_sec;
        int consul_deregister_critical_tts_sec;
        char const *grpc_host;
        int grpc_port;
    };

    Config loadConfig();

    class ServerImpl final {
    public:
        explicit ServerImpl(Config config_);
        ~ServerImpl() = default;
        void Run();
        void Shutdown();
    private:
        void initServer();
        std::unique_ptr<Server> server_;
        ApiServiceImpl api_;
        Cluster *cluster_;
        std::string server_address_;
        std::thread thread_;
    };

    ServerImpl *server_;

    extern "C" {
    SWITCH_MODULE_DEFINITION(mod_grpc, mod_grpc_load, mod_grpc_shutdown, nullptr);
    };
}


#endif