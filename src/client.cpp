#include "mlf/client.hpp"

namespace mlf {

LifecycleClient::~LifecycleClient() { close(); }

bool LifecycleClient::connect(std::string_view host, std::uint16_t port, std::string& error) {
  Socket socket;
  const ProtocolStatus status =
      Socket::connect_to(host.empty() ? "127.0.0.1" : host, port, socket, error);
  if (status != ProtocolStatus::Ok) return false;
  channel_ = FrameChannel(std::move(socket), limits_);
  return true;
}

void LifecycleClient::close() {
  if (channel_.valid()) channel_.close();
}

Reply LifecycleClient::call(MessageType type, const std::vector<std::uint8_t>& payload,
                            std::string& error) {
  Reply reply;
  if (!channel_.valid()) {
    reply.outcome = OutcomeCode::Rejected;
    reply.add(ReasonCode::ProtocolDecodeFailure, "client is not connected");
    error = "client is not connected";
    return reply;
  }
  const ProtocolStatus sent = channel_.send(make_frame(type, payload), error);
  if (sent != ProtocolStatus::Ok) {
    reply.outcome = outcome_for(sent);
    reply.add(reason_for(sent), "frame was not delivered");
    return reply;
  }
  Frame response;
  const ProtocolStatus received = channel_.receive(response, error);
  if (received != ProtocolStatus::Ok) {
    reply.outcome = outcome_for(received);
    reply.add(reason_for(received), "no reply from the coordinator");
    return reply;
  }
  if (response.type != MessageType::REPLY) {
    reply.outcome = OutcomeCode::Rejected;
    reply.add(ReasonCode::ProtocolUnknownType,
              std::string("unexpected reply type ") + std::string(to_string(response.type)));
    error = "unexpected reply type";
    return reply;
  }
  Reply decoded;
  const ProtocolStatus status =
      decode(response.payload.data(), response.payload.size(), limits_, decoded);
  if (status != ProtocolStatus::Ok) {
    reply.outcome = outcome_for(status);
    reply.add(reason_for(status), "reply payload could not be decoded");
    error = "reply payload could not be decoded";
    return reply;
  }
  error.clear();
  return decoded;
}

Reply LifecycleClient::hello(WorkerId worker, WorkerBootId boot, CoordinatorEpoch epoch,
                             const std::vector<ScopeId>& scopes, std::string& error) {
  HelloMessage message;
  message.protocol_version = limits_.version;
  message.worker = worker;
  message.boot = boot;
  message.epoch = epoch;
  message.scopes = scopes;
  message.detail = "client handshake";
  return call(message, error);
}

}  // namespace mlf
