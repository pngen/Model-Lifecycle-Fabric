// Model Lifecycle Fabric — lifecycle client.
//
// A synchronous request/reply client over the framed protocol. Used by the CLI,
// by examples, and by the multiprocess proofs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mlf/protocol.hpp"

namespace mlf {

/// Compile-time mapping from a message structure to its wire type.
template <class T>
struct MessageTraits;

#define MLF_MESSAGE_TRAIT(TYPE, VALUE)                        \
  template <>                                                 \
  struct MessageTraits<TYPE> {                                \
    static constexpr MessageType value = MessageType::VALUE;  \
  }

MLF_MESSAGE_TRAIT(HelloMessage, HELLO);
MLF_MESSAGE_TRAIT(HelloAckMessage, HELLO_ACK);
MLF_MESSAGE_TRAIT(RegisterModelMessage, REGISTER_MODEL);
MLF_MESSAGE_TRAIT(RegisterVersionMessage, REGISTER_VERSION);
MLF_MESSAGE_TRAIT(PublishCompatibilityMessage, PUBLISH_COMPATIBILITY);
MLF_MESSAGE_TRAIT(PublishEvidenceMessage, PUBLISH_READINESS);
MLF_MESSAGE_TRAIT(AttemptRequestMessage, REQUEST_WARM);
MLF_MESSAGE_TRAIT(CompletionMessage, PUBLISH_COMPLETION);
MLF_MESSAGE_TRAIT(FenceMessage, FENCE);
MLF_MESSAGE_TRAIT(CommandMessage, COMMAND);
MLF_MESSAGE_TRAIT(QueryStateMessage, QUERY_STATE);
MLF_MESSAGE_TRAIT(QueryAuthorityMessage, QUERY_AUTHORITY);
MLF_MESSAGE_TRAIT(PromotionMessage, PROMOTE);
MLF_MESSAGE_TRAIT(RolloutPlanMessage, REGISTER_ROLLOUT_PLAN);
MLF_MESSAGE_TRAIT(ProgressMessage, BEGIN_ROLLOUT);
MLF_MESSAGE_TRAIT(RetireMessage, RETIRE);
MLF_MESSAGE_TRAIT(RevalidateMessage, REVALIDATE);
MLF_MESSAGE_TRAIT(RollbackMessage, ROLLBACK);
MLF_MESSAGE_TRAIT(SupersedeMessage, SUPERSEDE_ROLLOUT);
MLF_MESSAGE_TRAIT(ReconcileMessage, RECONCILE);
MLF_MESSAGE_TRAIT(RegisterScopeMessage, REGISTER_SCOPE);

#undef MLF_MESSAGE_TRAIT

class LifecycleClient {
 public:
  explicit LifecycleClient(ProtocolLimits limits = {}) : limits_(limits) {}
  ~LifecycleClient();

  LifecycleClient(const LifecycleClient&) = delete;
  LifecycleClient& operator=(const LifecycleClient&) = delete;

  bool connect(std::string_view host, std::uint16_t port, std::string& error);
  void close();
  [[nodiscard]] bool connected() const noexcept { return channel_.valid(); }

  /// Send a frame of the given type and wait for the reply. The error string is
  /// set when the exchange could not complete; the returned Reply then reports
  /// the corresponding rejection.
  Reply call(MessageType type, const std::vector<std::uint8_t>& payload, std::string& error);

  /// Send a typed message and wait for the reply.
  template <class Message>
  Reply call(const Message& message, std::string& error) {
    std::vector<std::uint8_t> payload;
    const ProtocolStatus status = encode(message, limits_, payload);
    if (status != ProtocolStatus::Ok) {
      Reply reply;
      reply.outcome = outcome_for(status);
      reply.add(reason_for(status), "message could not be encoded");
      error = "message could not be encoded";
      return reply;
    }
    return call(MessageTraits<Message>::value, payload, error);
  }

  /// Send a typed message under an explicit wire type. Needed for messages that
  /// share a structure but differ in intent, such as the health and readiness
  /// publications.
  template <class Message>
  Reply call_as(MessageType type, const Message& message, std::string& error) {
    std::vector<std::uint8_t> payload;
    const ProtocolStatus status = encode(message, limits_, payload);
    if (status != ProtocolStatus::Ok) {
      Reply reply;
      reply.outcome = outcome_for(status);
      reply.add(reason_for(status), "message could not be encoded");
      error = "message could not be encoded";
      return reply;
    }
    return call(type, payload, error);
  }

  /// Convenience: the coordinator handshake used by clients.
  Reply hello(WorkerId worker, WorkerBootId boot, CoordinatorEpoch epoch,
              const std::vector<ScopeId>& scopes, std::string& error);

  [[nodiscard]] ProtocolLimits& limits() noexcept { return limits_; }
  [[nodiscard]] const ProtocolLimits& limits() const noexcept { return limits_; }

 private:
  ProtocolLimits limits_{};
  FrameChannel channel_{};
};

}  // namespace mlf
