#include "mod_grpc.h"

#include <iostream>
#include <thread>
#include <sstream>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>
#include "generated/fs.grpc.pb.h"

#include "Cluster.h"
#include "switch_core.h"

using namespace std;


using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerCompletionQueue;
using grpc::Status;
using grpc::StatusCode;

namespace mod_grpc {

    Status ApiServiceImpl::Originate(ServerContext *ctx, const fs::OriginateRequest *request,
                                     fs::OriginateResponse *reply) {
        switch_channel_t *caller_channel;
        switch_event_t *var_event = NULL;
        switch_core_session_t *caller_session = nullptr;
        uint32_t timeout = 60;
        switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;
        const char *dp, *context;
        const char *separator;

        if (request->timeout()) {
            timeout = static_cast<uint32_t>(request->timeout());
        }

        if (!request->endpoints_size()) {
            //err
        }

        context = request->context().c_str();
        if (!context) {
            context = "default";
        }

        dp = request->dialplan().c_str();
        if (!dp) {
            dp = "XML";
        }

        switch (request->strategy()) {
            case fs::OriginateRequest_Strategy_FAILOVER:
                separator = "|";
                break;
            case fs::OriginateRequest_Strategy_MULTIPLE:
                separator = ":_:";
                break;
            default:
                separator = ",";
                break;
        }

        std::stringstream aleg;

        for (size_t i = 0; i < request->endpoints().size(); ++i) {
            if (i != 0)
                aleg << separator;
            aleg << request->endpoints()[i];
        }

        if (request->variables_size() > 0) {
            if (switch_event_create_plain(&var_event, SWITCH_EVENT_CHANNEL_DATA) != SWITCH_STATUS_SUCCESS) {
                std::string msg("Can't create variable event");
                return Status(StatusCode::INTERNAL, msg);
            }

            for (const auto &kv: request->variables()) {
                switch_event_add_header_string(var_event, SWITCH_STACK_BOTTOM, kv.first.c_str(), kv.second.c_str());
            }
        }

        // todo check version
        if (switch_ivr_originate(nullptr, &caller_session, &cause, aleg.str().c_str(), timeout, nullptr,
                                 request->callername().c_str(), request->callernumber().c_str(), nullptr, var_event,
                                 SOF_NONE, nullptr, nullptr) != SWITCH_STATUS_SUCCESS
            || !caller_session) {
            reply->mutable_error()->set_type(fs::ErrorExecute_Type_ERROR);
            reply->mutable_error()->set_message(switch_channel_cause2str(cause));
            reply->set_error_code(static_cast<::google::protobuf::int32>(cause));

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Originate error %s\n",
                              switch_channel_cause2str(cause));
            goto done;
        }

        caller_channel = switch_core_session_get_channel(caller_session);
        switch_channel_set_variable(caller_channel, GRPC_SUCCESS_ORIGINATE, "true");

        if (!request->extensions().empty()) {
            switch_caller_extension_t *extension = nullptr;
            switch_core_session_reset(caller_session, SWITCH_TRUE, SWITCH_TRUE);
            switch_channel_clear_flag(caller_channel, CF_ORIGINATING);

            /* clear all state handlers */
            switch_channel_clear_state_handler(caller_channel, NULL);

            if ((extension = switch_caller_extension_new(caller_session,
                                                         request->callername().c_str(),
                                                         request->callernumber().c_str())) == 0) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Memory Error!\n");
                abort();
            }

            for (const auto &ex : request->extensions()) {
                switch_caller_extension_add_application(caller_session, extension,
                                                        ex.appname().c_str(),
                                                        ex.args().c_str());
            }

            switch_channel_set_caller_extension(caller_channel, extension);
            switch_channel_set_state(caller_channel, CS_RESET);
            switch_channel_wait_for_state(caller_channel, nullptr, CS_RESET);
            switch_channel_set_state(caller_channel, CS_EXECUTE);
        } else {
            switch_ivr_session_transfer(caller_session, request->destination().c_str(), dp, context);
        }

        reply->set_uuid(switch_core_session_get_uuid(caller_session));

        switch_core_session_rwunlock(caller_session);

        done:
        if (var_event) {
            switch_event_destroy(&var_event);
        }
        return Status::OK;
    }

    Status ApiServiceImpl::Execute(ServerContext *context, const fs::ExecuteRequest *request,
                                   fs::ExecuteResponse *reply) {
        switch_stream_handle_t stream = {0};

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Receive execute cmd: %s [%s]\n",
                          request->command().c_str(), request->args().c_str());


        if (request->command().length() <= 3) {
            std::string msg("Length of `Command` cannot be less than 3 characters");
            return Status(StatusCode::INVALID_ARGUMENT, msg);
        }

        SWITCH_STANDARD_STREAM(stream);

        if (switch_api_execute(request->command().c_str(), request->args().c_str(), nullptr, &stream) ==
            SWITCH_STATUS_FALSE) {
            std::string msg("Command cannot be execute");
            return Status(StatusCode::INTERNAL, msg);
        } else if (stream.data) {
            auto result = std::string((const char *) stream.data);

            if (!result.compare(0, 4, "-ERR")) {
                reply->mutable_error()->set_type(fs::ErrorExecute_Type_ERROR);
                reply->mutable_error()->set_message(result.substr(4));

            } else if (!result.compare(0, 6, "-USAGE")) {
                reply->mutable_error()->set_type(fs::ErrorExecute_Type_USAGE);
                reply->mutable_error()->set_message(result.substr(6));
            } else {
                reply->set_data(result);
            }

        } else {
            reply->set_data("todo: empty response");
        }
        switch_safe_free(stream.data);
        return Status::OK;
    }

    Status ApiServiceImpl::SetVariables(ServerContext *context, const fs::SetVariablesRequest *request,
                                        fs::SetVariablesResponse *reply) {

        switch_core_session_t *psession = nullptr;

        if (request->uuid().empty() || request->variables().empty()) {
            std::string msg("uuid or variables is required");
            return Status(StatusCode::INVALID_ARGUMENT, msg);
        }

        if ((psession = switch_core_session_locate(request->uuid().c_str()))) {
            switch_channel_t *channel = switch_core_session_get_channel(psession);

            for (const auto &kv: request->variables()) {
                switch_channel_set_variable(channel, kv.first.c_str(), kv.second.c_str());
            }
            switch_core_session_rwunlock(psession);
        } else {
            reply->mutable_error()->set_type(fs::ErrorExecute_Type_ERROR);
            reply->mutable_error()->set_message("no such channel");
        }

        return Status::OK;

    }

    Status ApiServiceImpl::Bridge(ServerContext *context, const fs::BridgeRequest *request,
                                  fs::BridgeResponse *reply) {
        int bridged = 1;

        if (switch_ivr_uuid_bridge(request->leg_a_id().c_str(), request->leg_b_id().c_str()) != SWITCH_STATUS_SUCCESS) {
            bridged = 0;
        }

        if (bridged) {
            switch_core_session_t *session;
            session = switch_core_session_locate(request->leg_b_id().c_str());
            if (session) {
                switch_channel_t *channel = switch_core_session_get_channel(session);
                if (switch_channel_wait_for_flag(channel, CF_BRIDGED, SWITCH_TRUE, 3000, nullptr) !=
                    SWITCH_STATUS_SUCCESS) {
                    bridged = 0;
                }

                switch_core_session_rwunlock(session);
            } else {
                bridged = 0;
            }
        }

        if (!bridged) {
            reply->mutable_error()->set_message("not found call id");
        }

        return Status::OK;
    }

    Status ApiServiceImpl::Hangup(ServerContext *context, const fs::HangupRequest *request,
                                  fs::HangupResponse *reply) {
        switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;
        switch_core_session_t *session;

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Receive hangup %s [%s]\n",
                          request->uuid().c_str(), request->cause().c_str());

        if (!request->cause().empty()) {
            cause = switch_channel_str2cause(request->cause().c_str());
        }

        if (zstr(request->uuid().c_str()) || !(session = switch_core_session_locate(request->uuid().c_str()))) {
            reply->mutable_error()->set_message("No such channel!");
            reply->mutable_error()->set_type(fs::ErrorExecute_Type_ERROR);
        } else {
            switch_channel_t *channel = switch_core_session_get_channel(session);
            for (const auto &kv: request->variables()) {
                switch_channel_set_variable(channel, kv.first.c_str(), kv.second.c_str());

                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Set hangup var %s [%s = %s]\n",
                                  request->uuid().c_str(), kv.first.c_str(), kv.second.c_str());
            }

            if (request->reporting()) {
                switch_channel_set_variable(channel, "cc_reporting_at", std::to_string(unixTimestamp()).c_str());
            }

            switch_channel_set_variable(channel, "grpc_send_hangup", "1");

            switch_channel_hangup(channel, cause);
            switch_core_session_rwunlock(session);
        }

        return Status::OK;
    }

    Status ApiServiceImpl::HangupMatchingVars(ServerContext *context, const fs::HangupMatchingVarsReqeust *request,
                                              fs::HangupMatchingVarsResponse *reply) {
        switch_call_cause_t cause = SWITCH_CAUSE_MANAGER_REQUEST;
        switch_event_t *vars = nullptr;
        uint32_t count = 0;

        if (request->variables().empty()) {
            std::string msg("variables is required");
            return Status(StatusCode::INVALID_ARGUMENT, msg);
        }

        if (!request->cause().empty()) {
            cause = switch_channel_str2cause(request->cause().c_str());
        }

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Receive hangup matching variables request\n");

        switch_event_create(&vars, SWITCH_EVENT_CLONE);

        for (const auto &kv: request->variables()) {
            switch_event_add_header_string(vars, SWITCH_STACK_BOTTOM, kv.first.c_str(), kv.second.c_str());
        }

        count = switch_core_session_hupall_matching_vars_ans(vars, cause,
                                                             static_cast<switch_hup_type_t>(SHT_UNANSWERED |
                                                                                            SHT_ANSWERED));
        reply->set_count(count);

        if (vars) {
            switch_event_destroy(&vars);
        }
        return Status::OK;
    }

    Status ApiServiceImpl::Queue(ServerContext *context, const fs::QueueRequest *request, fs::QueueResponse *reply) {
        switch_core_session_t *psession = nullptr;
        if ((psession = switch_core_session_force_locate(request->id().c_str()))) {
            switch_media_flag_t flags = SMF_ECHO_ALEG;
            switch_channel_t *channel = switch_core_session_get_channel(psession);

            if (!switch_channel_up(channel)) {
                switch_core_session_rwunlock(psession);
                return Status::CANCELLED;
            }

            if (!request->playback_file().empty()) {
                flags |= SMF_PRIORITY; // FIXME add parameter
                if (switch_ivr_broadcast(request->id().c_str(), request->playback_file().c_str(), flags) !=
                    SWITCH_STATUS_SUCCESS) {
                    switch_core_session_rwunlock(psession);
                    return Status::CANCELLED;
                }
            }

            if (!request->variables().empty()) {
                for (const auto &kv: request->variables()) {
                    switch_channel_set_variable_var_check(channel, kv.first.c_str(), kv.second.c_str(), SWITCH_FALSE);
                }
            }

            switch_core_session_rwunlock(psession);

        } else {
            reply->mutable_error()->set_message("No such channel!");
            reply->mutable_error()->set_type(fs::ErrorExecute_Type_ERROR);
            return Status::CANCELLED;
        }

        return Status::OK;
    }

    Status ApiServiceImpl::HangupMany(ServerContext *context, const fs::HangupManyRequest *request,
                                      fs::HangupManyResponse *reply) {
        switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;

        if (!request->cause().empty()) {
            cause = switch_channel_str2cause(request->cause().c_str());
        }

        for (const auto &id: request->id()) {
            switch_core_session_t *session;

            if (!id.empty() && (session = switch_core_session_locate(id.c_str()))) {
                switch_channel_t *channel = switch_core_session_get_channel(session);
                switch_channel_set_variable(channel, "grpc_send_hangup", "1");

                switch_channel_hangup(channel, cause);
                switch_core_session_rwunlock(session);

                reply->add_id(id);
            }
        }

        return Status::OK;
    }

    Status ApiServiceImpl::Hold(ServerContext *context, const fs::HoldRequest *request, fs::HoldResponse *reply) {
        for (const auto &id: request->id()) {
            switch_core_session_t *session;

            if (!id.empty() && (session = switch_core_session_locate(id.c_str()))) {
                if (!switch_channel_test_flag(switch_core_session_get_channel(session), CF_PROTO_HOLD)) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Receive Hold request %s\n", id.c_str());
                    switch_core_media_toggle_hold(session, 1);
                    reply->add_id(id);
                }

                switch_core_session_rwunlock(session);
            }
        }

        return Status::OK;
    }

    Status ApiServiceImpl::UnHold(ServerContext *context, const fs::UnHoldRequest *request, fs::UnHoldResponse *reply) {
        for (const auto &id: request->id()) {
            switch_core_session_t *session;

            if (!id.empty() && (session = switch_core_session_locate(id.c_str()))) {
                if (switch_channel_test_flag(switch_core_session_get_channel(session), CF_PROTO_HOLD)) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Receive UnHold request %s\n", id.c_str());
                    switch_core_media_toggle_hold(session, 0);
                    reply->add_id(id);
                }

                switch_core_session_rwunlock(session);
            }
        }

        return Status::OK;
    }

    Status ApiServiceImpl::BridgeCall(ServerContext *context, const fs::BridgeCallRequest *request,
                                      fs::BridgeCallResponse *reply) {
        switch_status_t status = SWITCH_STATUS_TERM;
        switch_core_session_t *leg_a_s = nullptr, *leg_b_s = nullptr;
        switch_channel_t *chan_a_s = nullptr, *chan_b_s = nullptr;

        const char *cc_to_agent_id = nullptr;
        const char *cc_from_attempt_id = nullptr;
        const char *cc_to_attempt_id = nullptr;
        const char *pa = nullptr;
        const char *pb = nullptr;

        if (request->leg_a_id().empty() || request->leg_b_id().empty()) {
            std::string msg("leg_a_id or leg_b_id is required");
            return Status(StatusCode::INVALID_ARGUMENT, msg);
        }

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Receive bridgeCall request %s & %s\n",
                          request->leg_a_id().c_str(), request->leg_b_id().c_str());

        leg_a_s = switch_core_session_locate(request->leg_a_id().c_str());
        leg_b_s = switch_core_session_locate(request->leg_b_id().c_str());

        if (leg_a_s) {
            chan_a_s = switch_core_session_get_channel(leg_a_s);
        }

        if (leg_b_s) {
            chan_b_s = switch_core_session_get_channel(leg_b_s);
        }

        if (chan_a_s) {
            pa = switch_channel_get_partner_uuid(chan_a_s);
        }

        if (chan_b_s) {
            pb = switch_channel_get_partner_uuid(chan_b_s);
        }

        if (leg_a_s && leg_b_s && chan_a_s && chan_b_s && pa && pb
            && switch_channel_test_flag(chan_a_s, CF_BRIDGED) && switch_channel_test_flag(chan_b_s, CF_BRIDGED)) {

            if (switch_channel_get_partner_uuid(chan_a_s)) {
                switch_channel_set_variable_partner(chan_a_s, "wbt_transfer_from", request->leg_b_id().c_str());

                cc_to_agent_id = switch_channel_get_variable_dup(chan_a_s, "cc_agent_id", SWITCH_FALSE, -1);
                cc_from_attempt_id = switch_channel_get_variable_dup(chan_a_s, "cc_attempt_id", SWITCH_FALSE, -1);
            }

            if (cc_to_agent_id) {
                switch_channel_set_variable_partner(chan_b_s, "wbt_transfer_to_agent", cc_to_agent_id);
            }

            if (cc_from_attempt_id) {
                switch_channel_set_variable_partner(chan_b_s, "wbt_transfer_from_attempt", cc_from_attempt_id);
            }

            switch_channel_set_variable_partner(chan_b_s, "wbt_transfer_to", request->leg_a_id().c_str());

            if (cc_from_attempt_id &&
                (cc_to_attempt_id = switch_channel_get_variable_partner(chan_b_s, "cc_attempt_id"))) {
                switch_channel_set_variable_partner(chan_a_s, "wbt_transfer_to_attempt",
                                                    std::string(cc_to_attempt_id).c_str());
            }

            if ((status = switch_ivr_uuid_bridge(request->leg_a_id().c_str(), request->leg_b_id().c_str())) !=
                SWITCH_STATUS_SUCCESS) {
                // todo clean variables - transfer fail
            } else {

                if (!switch_core_media_bug_count(leg_a_s, "session_record")) {
                    switch_core_session_t *pbs = nullptr;
                    if ((pbs = switch_core_session_locate(pb))) {
                        if (switch_core_media_bug_count(pbs, "session_record")) {
                            char *path = switch_channel_expand_variables(chan_b_s, this->transfer_template.c_str());
                            switch_ivr_record_session_event(leg_b_s, path, 0, nullptr, nullptr);

                            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Transfer recordings after bridge: %s & %s [%s]\n",
                                              request->leg_a_id().c_str(), request->leg_b_id().c_str(), path);
                        }
                        switch_core_session_rwunlock(pbs);
                    }
                }

                reply->set_uuid(request->leg_b_id());
            }
        }

        if (leg_a_s) {
            switch_core_session_rwunlock(leg_a_s);
        }

        if (leg_b_s) {
            switch_core_session_rwunlock(leg_b_s);
        }

        if (status != SWITCH_STATUS_SUCCESS) {
            reply->mutable_error()->set_type(fs::ErrorExecute_Type_ERROR);
            reply->mutable_error()->set_message("Invalid id");
        }

        return Status::OK;
    }

    Status ApiServiceImpl::StopPlayback(ServerContext *context, const fs::StopPlaybackRequest *request,
                                        fs::StopPlaybackResponse *reply) {
        switch_core_session_t *session;

        if (!request->id().empty() && (session = switch_core_session_locate(request->id().c_str()))) {
            switch_channel_t *channel = switch_core_session_get_channel(session);
//            switch_channel_clear_flag(channel, CF_HOLD);
            if (switch_channel_test_flag(channel, CF_BROADCAST)) {
                switch_channel_stop_broadcast(channel);
                switch_channel_wait_for_flag(channel, CF_BROADCAST, SWITCH_FALSE, 5000, NULL);
            }

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "executed stop_playback\n");
            switch_core_session_rwunlock(session);
        }

        return Status::OK;
    }

    Status ApiServiceImpl::SetProfileVar(ServerContext *context, const fs::SetProfileVarRequest *request,
                                         fs::SetProfileVarResponse *reply) {
        switch_core_session_t *session;
        if (!request->id().empty() && (session = switch_core_session_locate(request->id().c_str()))) {
            switch_channel_t *channel = switch_core_session_get_channel(session);
            if (request->variables_size() > 0) {
                for (const auto &kv: request->variables()) {
                    switch_channel_set_profile_var(channel, kv.first.c_str(), kv.second.c_str());
                }
            }
            switch_core_session_rwunlock(session);
        } else {
            return Status::CANCELLED;
        }

        return Status::OK;
    }

    Status ApiServiceImpl::ConfirmPush(ServerContext *context, const fs::ConfirmPushRequest *request,
                                       fs::ConfirmPushResponse *reply) {

        if (request->id().empty()) {
            reply->mutable_error()->set_type(fs::ErrorExecute_Type_ERROR);
            reply->mutable_error()->set_message("bad request: no call id");
            return Status::OK;
        }

        switch_core_session_t *session;
        session = switch_core_session_locate(request->id().c_str());
        if (session) {
            switch_channel_t *channel = switch_core_session_get_channel(session);
            //todo fixme CF_SLA_INTERCEPT -> CF_DEVICES_CHANGED
            switch_channel_set_flag(channel, CF_SLA_INTERCEPT);
            switch_core_session_rwunlock(session);
        } else {
            reply->mutable_error()->set_type(fs::ErrorExecute_Type_ERROR);
            reply->mutable_error()->set_message("No such channel!");
        }

        return Status::OK;
    }

    ServerImpl::ServerImpl(Config config_) {
        if (!config_.grpc_host) {
            char ipV4_[80];
            switch_find_local_ip(ipV4_, sizeof(ipV4_), nullptr, AF_INET);
            config_.grpc_host = std::string(ipV4_).c_str();
        }
        server_address_ = std::string(config_.grpc_host) + ":" + std::to_string(config_.grpc_port);

        if (config_.consul_address) {
            cluster_ = new Cluster(config_.consul_address, config_.grpc_host, config_.grpc_port,
                                   config_.consul_tts_sec, config_.consul_deregister_critical_tts_sec);
        }

        this->push_wait_callback = config_.push_wait_callback;
        this->push_fcm_uri = config_.push_fcm_uri ? std::string(config_.push_fcm_uri) : "https://fcm.googleapis.com/fcm/send";
        if (config_.push_fcm_auth != nullptr) {
            this->push_fcm_auth = "Authorization: " + std::string(config_.push_fcm_auth);
        }
        this->push_fcm_enabled = config_.push_fcm_enabled && !this->push_fcm_auth.empty() && !this->push_fcm_uri.empty();

        this->push_apn_uri = config_.push_apn_uri ? std::string(config_.push_apn_uri) : "https://api.sandbox.push.apple.com:443/3/device";
        if (config_.push_apn_cert_file) {
            this->push_apn_cert_file = std::string(config_.push_apn_cert_file);
        }
        if (config_.push_apn_key_file) {
            this->push_apn_key_file = std::string(config_.push_apn_key_file);
        }
        if (config_.push_apn_key_pass) {
            this->push_apn_key_pass = std::string(config_.push_apn_key_pass);
        }
        if (config_.push_apn_topic) {
            this->push_apn_topic = std::string(config_.push_apn_topic);
        }
        this->push_apn_enabled = config_.push_apn_enabled && !this->push_apn_cert_file.empty() && !this->push_apn_key_pass.empty()
                && !this->push_apn_key_file.empty() && !this->push_apn_topic.empty();

        this->transfer_recordings_template = config_.transfer_recordings_template;
    }

    void ServerImpl::Run() {
        initServer();
        thread_ = std::thread([&]() {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Server listening on %s\n",
                              server_address_.c_str());
            server_->Wait();
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Server shutdown\n");
        });
    }

    void ServerImpl::Shutdown() {

        if (server_) {
            server_->Shutdown();
        }

        if (thread_.joinable()) {
            thread_.join();
        }

        delete cluster_;
        server_.reset();
    }

    void ServerImpl::initServer() {
        api_.transfer_template = this->transfer_recordings_template;

        ServerBuilder builder;
        // Listen on the given address without any authentication mechanism.
        builder.AddListeningPort(server_address_, grpc::InsecureServerCredentials());
        // Register "service" as the instance through which we'll communicate with
        // clients. In this case it corresponds to an *synchronous* service.
        builder.RegisterService(&api_);
        // Finally assemble the server.
        server_ = builder.BuildAndStart();

        if (!server_) {
            this->Shutdown();
            throw "Can't bind to address: " + server_address_;
        }
    }

    Config loadConfig() {

        auto config = Config();
        static switch_xml_config_item_t instructions[] = {
                SWITCH_CONFIG_ITEM(
                        "grpc_host",
                        SWITCH_CONFIG_STRING,
                        CONFIG_RELOADABLE,
                        &config.grpc_host,
                        nullptr,
                        nullptr, "grpc_host", "GRPC server address"),
                SWITCH_CONFIG_ITEM(
                        "consul_ttl_sec",
                        SWITCH_CONFIG_INT,
                        CONFIG_RELOADABLE,
                        &config.consul_tts_sec,
                        (void *) 60,
                        nullptr, nullptr, "GRPC TTL"),
                SWITCH_CONFIG_ITEM(
                        "consul_deregister_critical_ttl_sec",
                        SWITCH_CONFIG_INT,
                        CONFIG_RELOADABLE,
                        &config.consul_deregister_critical_tts_sec,
                        (void *) 120,
                        nullptr, nullptr, "GRPC degeregister critical TTL"),
                SWITCH_CONFIG_ITEM(
                        "grpc_port",
                        SWITCH_CONFIG_INT,
                        CONFIG_RELOADABLE,
                        &config.grpc_port,
                        (void *) 50051,
                        nullptr, nullptr, "GRPC server port"),
                SWITCH_CONFIG_ITEM(
                        "consul_address",
                        SWITCH_CONFIG_STRING,
                        CONFIG_RELOADABLE,
                        &config.consul_address,
                        nullptr,
                        nullptr, "consul_address", "Consul address"),
                SWITCH_CONFIG_ITEM(
                        "push_wait_callback",
                        SWITCH_CONFIG_INT,
                        CONFIG_RELOADABLE,
                        &config.push_wait_callback,
                        2000,
                        nullptr, "push_wait_callback", "Push wait callback time"),
                SWITCH_CONFIG_ITEM(
                        "push_fcm_enabled",
                        SWITCH_CONFIG_BOOL,
                        CONFIG_RELOADABLE,
                        &config.push_fcm_enabled,
                        0,
                        nullptr, "push_fcm_enabled", "Enable FCM"),
                SWITCH_CONFIG_ITEM(
                        "push_fcm_auth",
                        SWITCH_CONFIG_STRING,
                        CONFIG_RELOADABLE,
                        &config.push_fcm_auth,
                        nullptr,
                        nullptr, "key=AAAAfEvtHJI:....", "FCM Authorization"),
                SWITCH_CONFIG_ITEM(
                        "push_fcm_uri",
                        SWITCH_CONFIG_STRING,
                        CONFIG_RELOADABLE,
                        &config.push_fcm_uri,
                        "https://fcm.googleapis.com/fcm/send",
                        nullptr, "https://fcm.googleapis.com/fcm/send", "FCM endpoint uri"),
                SWITCH_CONFIG_ITEM(
                        "push_apn_enabled",
                        SWITCH_CONFIG_BOOL,
                        CONFIG_RELOADABLE,
                        &config.push_apn_enabled,
                        0,
                        nullptr, "push_apn_enabled", "Enable APN"),
                SWITCH_CONFIG_ITEM(
                        "push_apn_uri",
                        SWITCH_CONFIG_STRING,
                        CONFIG_RELOADABLE,
                        &config.push_apn_uri,
                        "https://api.sandbox.push.apple.com:443/3/device",
                        nullptr, "https://api.sandbox.push.apple.com:443/3/device", "APN endpoint uri"),
                SWITCH_CONFIG_ITEM(
                        "push_apn_cert_file",
                        SWITCH_CONFIG_STRING,
                        CONFIG_RELOADABLE,
                        &config.push_apn_cert_file,
                        nullptr,
                        nullptr, "/tmp/webitel.webitel-ios.cer", "APN certificate file"),
                SWITCH_CONFIG_ITEM(
                        "push_apn_key_file",
                        SWITCH_CONFIG_STRING,
                        CONFIG_RELOADABLE,
                        &config.push_apn_key_file,
                        nullptr,
                        nullptr, "/tmp/webitel.key.pem", "APN key file"),
                SWITCH_CONFIG_ITEM(
                        "push_apn_key_pass",
                        SWITCH_CONFIG_STRING,
                        CONFIG_RELOADABLE,
                        &config.push_apn_key_pass,
                        nullptr,
                        nullptr, "PASSWORD", "APN key password"),
                SWITCH_CONFIG_ITEM(
                        "push_apn_topic",
                        SWITCH_CONFIG_STRING,
                        CONFIG_RELOADABLE,
                        &config.push_apn_topic,
                        "apns-topic: com.webitel.webitel-ios.voip",
                        nullptr, "apns-topic: com.webitel.webitel-ios.voip", "APN topic header"),
                SWITCH_CONFIG_ITEM(
                        "transfer_recordings_template",
                        SWITCH_CONFIG_STRING,
                        CONFIG_RELOADABLE,
                        &config.transfer_recordings_template,
                        "http_cache://http://$${cdr_url}/sys/recordings?domain=${sip_h_X-Webitel-Domain-Id}&id=${wbt_parent_id}&name=transfer-${wbt_parent_id}.mp3",
                        nullptr, "http_cache://http://$${cdr_url}/sys/recordings?domain=${sip_h_X-Webitel-Domain-Id}&id=${wbt_parent_id}&name=transfer-${wbt_parent_id}.mp3", "Transfer recordings template"),
                SWITCH_CONFIG_ITEM_END()
        };

        if (switch_xml_config_parse_module_settings("grpc.conf", SWITCH_FALSE, instructions) != SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                              "Can't load grpc.conf. Use default GRPC config\n");
        }

        return config;
    }

    std::string toJson(const PushData *data) {
        return "{\"type\":\"call\", \"call_id\":\"" + data->call_id + "\",\"from_number\":\"" + data->from_number + "\",\"from_name\":\"" + data->from_name + "\"}";
    }

    static size_t writeCallback(char *contents, size_t size, size_t nmemb, void *userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    long ServerImpl::SendPushAPN(const char *devices, const PushData *data) {
        switch_CURL  *cli = switch_curl_easy_init();
        long response_code = -1;

        if (cli) {
            switch_CURLcode res;
            switch_curl_slist_t *headers = nullptr;

            switch_curl_easy_setopt(cli, CURLOPT_URL,  (this->push_apn_uri + "/" + std::string(devices)).c_str());
            headers = switch_curl_slist_append(headers, "Content-Type: application/json");
            headers = switch_curl_slist_append(headers, this->push_apn_topic.c_str());
            headers = switch_curl_slist_append(headers, (std::string("apns-expiration: ") +
                std::to_string((long)((unixTimestamp() + this->push_wait_callback + 2000) / 1000))
            ).c_str());

            switch_curl_easy_setopt(cli, CURLOPT_HTTPHEADER, headers);

            std::string body = "{"
                               "\"aps\":" + toJson(data) + ","
                               "\"priority\":10}";

            /* HTTP/2 please */
            curl_easy_setopt(cli, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);

            /* cert is stored PEM coded in file... */
            curl_easy_setopt(cli, CURLOPT_SSLCERTTYPE, "DER");

            /* set the cert for client authentication */
            curl_easy_setopt(cli, CURLOPT_SSLCERT, this->push_apn_cert_file.c_str());

            curl_easy_setopt(cli, CURLOPT_SSLKEYTYPE, "PEM");
            curl_easy_setopt(cli, CURLOPT_KEYPASSWD, this->push_apn_key_pass.c_str());
            /* set the file with the certs vaildating the server */
            curl_easy_setopt(cli, CURLOPT_SSLKEY, this->push_apn_key_file.c_str());

            /* disconnect if we cannot validate server's cert */
            curl_easy_setopt(cli, CURLOPT_SSL_VERIFYPEER, 1L);

            #ifdef DEBUG_CURL
            switch_curl_easy_setopt(cli, CURLOPT_VERBOSE, 1L);
            #endif
            switch_curl_easy_setopt(cli, CURLOPT_CUSTOMREQUEST, "POST");

            switch_curl_easy_setopt(cli, CURLOPT_POSTFIELDS, body.c_str());

//            std::string readBuffer;
//            switch_curl_easy_setopt(cli, CURLOPT_WRITEFUNCTION, writeCallback);
//            switch_curl_easy_setopt(cli, CURLOPT_WRITEDATA, &readBuffer);

//            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
//                              "body\n%s\n", body.c_str());

            res = switch_curl_easy_perform(cli);
            if(res == CURLE_OK) {
                curl_easy_getinfo(cli, CURLINFO_RESPONSE_CODE, &response_code);
            }

//            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
//                              "response (%s)\n%s\n",(this->push_apn_uri + "/" + std::string(devices)).c_str(), readBuffer.c_str());

            switch_curl_easy_cleanup(cli);
            switch_curl_slist_free_all(headers);
        };


        return response_code;
    }

    std::string ReplaceAll(std::string str, const std::string& from, const std::string& to) {
        size_t start_pos = 0;
        while((start_pos = str.find(from, start_pos)) != std::string::npos) {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length(); // Handles case where 'to' is a substring of 'from'
        }
        return str;
    }

    long ServerImpl::SendPushFCM(const char *devices, const PushData *data) {
        switch_CURL  *cli = switch_curl_easy_init();
        long response_code = -1;

        if (cli) {
            switch_CURLcode res;
            switch_curl_slist_t *headers = nullptr;
            switch_curl_easy_setopt(cli, CURLOPT_URL,  this->push_fcm_uri.c_str());
            headers = switch_curl_slist_append(headers, "Content-Type: application/json");
            headers = switch_curl_slist_append(headers, this->push_fcm_auth.c_str());
            switch_curl_easy_setopt(cli, CURLOPT_HTTPHEADER, headers);

            std::string body = "{"
                               "\"time_to_live\":" + std::to_string(int(this->push_wait_callback / 1000) + 2) + ","
                               "\"data\":" + toJson(data) + ","
                               "\"registration_ids\":[\"" + ReplaceAll(devices, "::", "\",\"") + "\"],"
                                                                                   "\"priority\":10}";

//#ifdef DEBUG_CURL
            switch_curl_easy_setopt(cli, CURLOPT_VERBOSE, 1L);
//#endif
            switch_curl_easy_setopt(cli, CURLOPT_CUSTOMREQUEST, "POST");

            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                              "body\n%s\n", body.c_str());

            switch_curl_easy_setopt(cli, CURLOPT_POSTFIELDS, body.c_str());

            res = switch_curl_easy_perform(cli);
            if(res == CURLE_OK) {
                curl_easy_getinfo(cli, CURLINFO_RESPONSE_CODE, &response_code);
            }

            switch_curl_easy_cleanup(cli);
            switch_curl_slist_free_all(headers);
        };

        return response_code;
    }

    bool ServerImpl::UseFCM() const {
        return this->push_fcm_enabled;
    }

    bool ServerImpl::UseAPN() const {
        return this->push_apn_enabled;
    }

    int ServerImpl::PushWaitCallback() const {
        return this->push_wait_callback;
    }

    static void split_str(const std::string& str, const std::string& delimiter, std::vector<std::string> &out) {
        size_t pos = 0;
        std::string s = str;
        while ((pos = s.find(delimiter)) != std::string::npos) {
            out.push_back(s.substr(0, pos));
            s.erase(0, pos + delimiter.length());
        }

        out.push_back(s);
    }

    static switch_status_t wbt_outgoing_channel(switch_core_session_t * session, switch_event_t * event, switch_caller_profile_t * cp, switch_core_session_t * peer_session, switch_originate_flag_t flag) {
        if (!peer_session) {

            return SWITCH_STATUS_SUCCESS;
        }

        switch_channel_t *channel = switch_core_session_get_channel(peer_session);
        auto dir = switch_event_get_header(event, "sip_h_X-Webitel-Direction");
        if (!dir || strcmp(dir, "internal") != 0) {

            return SWITCH_STATUS_SUCCESS;
        }
        auto uuid = switch_channel_get_uuid(channel);
        if (!uuid) {
            //todo
            return SWITCH_STATUS_SUCCESS;
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(peer_session), SWITCH_LOG_DEBUG, "checking tweaks for %s\n", uuid);
        const char *wbt_push_fcm = switch_event_get_header(event, "wbt_push_fcm");
        const char *wbt_push_apn = switch_event_get_header(event, "wbt_push_apn");
        const char *wbt_from_name = switch_event_get_header(event, "wbt_from_name");
        const char *wbt_from_number = switch_event_get_header(event, "wbt_from_number");
        int send = 0;
        PushData data;
        data.call_id = std::string(uuid);
        data.from_name = wbt_from_name ? std::string(wbt_from_name) : "";
        data.from_number = wbt_from_number ? std::string(wbt_from_number) : "";
        if (wbt_push_fcm && server_->UseFCM()) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(peer_session), SWITCH_LOG_DEBUG, "start request FCM %s\n", uuid);
            auto res = server_->SendPushFCM(wbt_push_fcm, &data);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(peer_session), SWITCH_LOG_DEBUG, "stop request FCM %s [%ld]\n", uuid, res);
            if (res == 200) {
                send++;
            }
        }
        if (wbt_push_apn && server_->UseAPN()) {
            std::vector <std::string> out;
            split_str(wbt_push_apn, "::", out);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(peer_session), SWITCH_LOG_DEBUG, "start APN request %s tokens[%s]\n", uuid, wbt_push_apn);
            for (const auto &token: out) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(peer_session), SWITCH_LOG_DEBUG, "start APN request %s\n", uuid);
                auto res = server_->SendPushAPN(token.c_str(), &data);
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(peer_session), SWITCH_LOG_DEBUG, "stop APN request %s [%ld]\n", uuid, res);
                if (res == 200) {
                    send++;
                }
            }
        }

        if (send && server_->PushWaitCallback() > 0) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(peer_session), SWITCH_LOG_DEBUG, "start wait callback %s [%d]\n", uuid, server_->PushWaitCallback());
            switch_channel_wait_for_flag(channel, CF_SLA_INTERCEPT, SWITCH_TRUE, server_->PushWaitCallback(), NULL); //10s
            switch_channel_clear_flag(channel, CF_SLA_INTERCEPT);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(peer_session), SWITCH_LOG_DEBUG, "stop wait callback %s\n", uuid);
        }
        return SWITCH_STATUS_SUCCESS;
    }

    static switch_status_t wbt_tweaks_on_init(switch_core_session_t *session) {
        switch_core_event_hook_add_outgoing_channel(session, wbt_outgoing_channel);
        return SWITCH_STATUS_SUCCESS;
    }

    static switch_status_t wbt_tweaks_on_reporting(switch_core_session_t *session) {
        double talk = 0;
        switch_caller_profile_t *cp = nullptr;

        auto channel = switch_core_session_get_channel(session);
        auto profile = switch_channel_get_caller_profile(channel);

        for (cp = profile; cp; cp = cp->next) {
            if (cp->times && cp->times->bridged) {
                switch_time_t t2 = cp->times->transferred;
                if (t2 == 0) {
                    t2 = cp->times->hungup ? cp->times->hungup : switch_micro_time_now();
                }
                talk += difftime(t2,
                                 (switch_time_t)(cp->times->bridged));
            }
        }
        switch_channel_set_variable(channel, WBT_TALK_SEC, std::to_string(static_cast<int>(talk / 1000000)).c_str());
        return SWITCH_STATUS_SUCCESS;
    }

    SWITCH_STANDARD_APP(wbr_queue_function) {
        switch_channel_t *channel = switch_core_session_get_channel(session);

        if (zstr(data)) {
            return;
        }

        while (switch_channel_ready(channel)) {
            switch_status_t pstatus = switch_ivr_play_file(session, NULL, data,
                                                           nullptr);

            if (pstatus == SWITCH_STATUS_BREAK || pstatus == SWITCH_STATUS_TIMEOUT) {
                break;
            }
        }
    }

    SWITCH_STANDARD_APP(wbt_queue_playback_function) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        switch_status_t status = SWITCH_STATUS_SUCCESS;
        const char *file = data;

        while (switch_channel_ready(channel)) {
            status = switch_ivr_play_file(session, NULL, file, NULL);

            if (status != SWITCH_STATUS_SUCCESS) {
                break;
            }
        }

        switch (status) {
            case SWITCH_STATUS_SUCCESS:
            case SWITCH_STATUS_BREAK:
                switch_channel_set_variable(channel, SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE, "FILE PLAYED");
                break;
            case SWITCH_STATUS_NOTFOUND:
                switch_channel_set_variable(channel, SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE, "FILE NOT FOUND");
                break;
            default:
                switch_channel_set_variable(channel, SWITCH_CURRENT_APPLICATION_RESPONSE_VARIABLE, "PLAYBACK ERROR");
                break;
        }

    }

    SWITCH_STANDARD_APP(wbt_blind_transfer_function) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "BLIND TRANSFER\n");
    }


    SWITCH_MODULE_LOAD_FUNCTION(mod_grpc_load) {
        try {
            *module_interface = switch_loadable_module_create_module_interface(pool, modname);
            switch_application_interface_t *app_interface;
            switch_core_add_state_handler(&wbt_state_handlers);
            SWITCH_ADD_APP(app_interface, "wbt_queue", "wbt_queue", "wbt_queue", wbr_queue_function, "", SAF_NONE);
            SWITCH_ADD_APP(app_interface, "wbt_blind_transfer", "wbt_blind_transfer", "wbt_blind_transfer",
                           wbt_blind_transfer_function, "", SAF_NONE);
            SWITCH_ADD_APP(app_interface, "wbt_queue_playback", "wbt_queue_playback", "wbt_queue_playback",
                           wbt_queue_playback_function, "", SAF_NONE);
            server_ = new ServerImpl(loadConfig());
            server_->Run();
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Module loaded completed FCM=%d APN=%d\n", server_->UseFCM(), server_->UseAPN());
            return SWITCH_STATUS_SUCCESS;
        } catch (std::string &err) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error loading GRPC module: %s\n",
                              err.c_str());
            return SWITCH_STATUS_GENERR;
        } catch (std::exception &ex) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error loading GRPC module: %s\n",
                              ex.what());
            return SWITCH_STATUS_GENERR;
        } catch (...) { // Exceptions must not propogate to C caller
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error loading GRPC module\n");
            return SWITCH_STATUS_GENERR;
        }

    }

    SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_grpc_shutdown) {
        try {
            switch_core_remove_state_handler(&wbt_state_handlers);
            server_->Shutdown();
            delete server_;
            google::protobuf::ShutdownProtobufLibrary(); //FIXME CRASH ???
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Module shutting down completed\n");
        } catch (std::exception &ex) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error shutting down GRPC module: %s\n",
                              ex.what());
        } catch (...) { // Exceptions must not propogate to C caller
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                              "Unknown error shutting down GRPC module\n");
        }
        return SWITCH_STATUS_SUCCESS;
    }

}