// Protocol hardening: malformed, truncated, stale and conflicting frames.
#include "mlf/coordinator.hpp"

#include "mlf_test.hpp"
#include "test_process_util.hpp"
#include "scenario.hpp"

using namespace mlf;

namespace {

/// Bring a coordinator up on an ephemeral loopback port and return its port.
/// Bring an in-process coordinator up on an ephemeral loopback port.
bool start_coordinator(mlftest::InProcessCoordinator& coordinator, std::uint16_t& port) {
  std::string error;
  if (!coordinator.start(error)) return false;
  port = coordinator.port();
  return port != 0;
}

std::vector<std::uint8_t> encode_hello() {
  HelloMessage hello;
  hello.worker = WorkerId{};
  hello.boot = WorkerBootId{};
  std::vector<std::uint8_t> payload;
  static_cast<void>(encode(hello, ProtocolLimits{}, payload));
  return payload;
}

}  // namespace

MLF_TEST(protocol, frame_round_trip) {
  Frame frame;
  frame.type = MessageType::QUERY_STATE;
  frame.flags = 7;
  frame.payload = {1, 2, 3, 4, 5};
  std::vector<std::uint8_t> encoded;
  MLF_CHECK(encode_frame(frame, ProtocolLimits{}, encoded) == ProtocolStatus::Ok);
  Frame decoded;
  std::size_t consumed = 0;
  MLF_CHECK(decode_frame(encoded.data(), encoded.size(), ProtocolLimits{}, decoded, consumed) ==
            ProtocolStatus::Ok);
  MLF_CHECK(decoded.type == MessageType::QUERY_STATE);
  MLF_CHECK_EQ(decoded.flags, 7u);
  MLF_CHECK_EQ(decoded.payload.size(), 5u);
  MLF_CHECK_EQ(consumed, encoded.size());
}

MLF_TEST(protocol, wrong_magic_is_rejected) {
  Frame frame;
  frame.type = MessageType::HELLO;
  std::vector<std::uint8_t> encoded;
  MLF_CHECK(encode_frame(frame, ProtocolLimits{}, encoded) == ProtocolStatus::Ok);
  encoded[0] = 'X';
  Frame decoded;
  std::size_t consumed = 0;
  MLF_CHECK(decode_frame(encoded.data(), encoded.size(), ProtocolLimits{}, decoded, consumed) ==
            ProtocolStatus::WrongMagic);
}

MLF_TEST(protocol, unsupported_version_is_rejected) {
  ProtocolLimits limits;
  Frame frame;
  frame.type = MessageType::HELLO;
  std::vector<std::uint8_t> encoded;
  MLF_CHECK(encode_frame(frame, limits, encoded) == ProtocolStatus::Ok);
  encoded[4] = 99;
  Frame decoded;
  std::size_t consumed = 0;
  MLF_CHECK(decode_frame(encoded.data(), encoded.size(), limits, decoded, consumed) ==
            ProtocolStatus::UnsupportedVersion);
}

MLF_TEST(protocol, unknown_type_is_rejected) {
  Frame frame;
  frame.type = MessageType::HELLO;
  std::vector<std::uint8_t> encoded;
  MLF_CHECK(encode_frame(frame, ProtocolLimits{}, encoded) == ProtocolStatus::Ok);
  encoded[6] = 200;
  encoded[7] = 0;
  Frame decoded;
  std::size_t consumed = 0;
  MLF_CHECK(decode_frame(encoded.data(), encoded.size(), ProtocolLimits{}, decoded, consumed) ==
            ProtocolStatus::UnknownType);
}

MLF_TEST(protocol, oversized_declared_length_is_rejected) {
  ProtocolLimits limits;
  limits.max_payload_bytes = 64;
  Frame frame;
  frame.type = MessageType::HELLO;
  std::vector<std::uint8_t> encoded;
  MLF_CHECK(encode_frame(frame, limits, encoded) == ProtocolStatus::Ok);
  encoded[12] = 0xff;
  encoded[13] = 0xff;
  encoded[14] = 0x00;
  encoded[15] = 0x00;
  Frame decoded;
  std::size_t consumed = 0;
  MLF_CHECK(decode_frame(encoded.data(), encoded.size(), limits, decoded, consumed) ==
            ProtocolStatus::Oversized);
}

MLF_TEST(protocol, truncation_is_detected_at_every_prefix) {
  Frame frame;
  frame.type = MessageType::REGISTER_MODEL;
  frame.payload = {9, 8, 7, 6, 5, 4, 3, 2, 1};
  std::vector<std::uint8_t> encoded;
  MLF_CHECK(encode_frame(frame, ProtocolLimits{}, encoded) == ProtocolStatus::Ok);
  for (std::size_t prefix = 0; prefix < encoded.size(); ++prefix) {
    Frame decoded;
    std::size_t consumed = 0;
    const ProtocolStatus status =
        decode_frame(encoded.data(), prefix, ProtocolLimits{}, decoded, consumed);
    MLF_CHECK_MSG(status == ProtocolStatus::NeedMoreData || status == ProtocolStatus::Truncated,
                  "prefix " + std::to_string(prefix));
  }
}

MLF_TEST(protocol, corrupted_payload_fails_the_integrity_check) {
  Frame frame;
  frame.type = MessageType::QUERY_AUTHORITY;
  frame.payload = {1, 2, 3, 4};
  std::vector<std::uint8_t> encoded;
  MLF_CHECK(encode_frame(frame, ProtocolLimits{}, encoded) == ProtocolStatus::Ok);
  encoded[kFrameHeaderSize] ^= 0xFF;
  Frame decoded;
  std::size_t consumed = 0;
  MLF_CHECK(decode_frame(encoded.data(), encoded.size(), ProtocolLimits{}, decoded, consumed) ==
            ProtocolStatus::IntegrityFailure);

  // Corrupting the trailer has the same effect.
  std::vector<std::uint8_t> second;
  MLF_CHECK(encode_frame(frame, ProtocolLimits{}, second) == ProtocolStatus::Ok);
  second.back() ^= 0x01;
  MLF_CHECK(decode_frame(second.data(), second.size(), ProtocolLimits{}, decoded, consumed) ==
            ProtocolStatus::IntegrityFailure);
}

MLF_TEST(protocol, incremental_decoder_reports_each_frame_once) {
  FrameQueue queue(4);
  std::vector<std::uint8_t> stream;
  for (int i = 0; i < 3; ++i) {
    Frame frame;
    frame.type = MessageType::HELLO;
    frame.payload = {static_cast<std::uint8_t>(i)};
    std::vector<std::uint8_t> encoded;
    static_cast<void>(encode_frame(frame, ProtocolLimits{}, encoded));
    stream.insert(stream.end(), encoded.begin(), encoded.end());
  }
  FrameDecoder decoder;
  int decoded = 0;
  for (std::size_t i = 0; i < stream.size(); ++i) {
    MLF_CHECK(decoder.feed(&stream[i], 1) == ProtocolStatus::Ok);
    Frame frame;
    while (decoder.next(frame) == ProtocolStatus::Ok) ++decoded;
  }
  MLF_CHECK_EQ(decoded, 3);
}

MLF_TEST(protocol, bounded_queue_rejects_explicitly_and_accounts_loss) {
  FrameQueue queue(2);
  Frame frame;
  frame.type = MessageType::HELLO;
  MLF_CHECK(queue.push(frame) == ProtocolStatus::Ok);
  MLF_CHECK(queue.push(frame) == ProtocolStatus::Ok);
  MLF_CHECK(queue.push(frame) == ProtocolStatus::TooManyPending);
  MLF_CHECK_EQ(queue.rejected(), 1u);
  Frame popped;
  MLF_CHECK(queue.pop(popped));
  MLF_CHECK(queue.push(frame) == ProtocolStatus::Ok);
}

MLF_TEST(protocol, reply_round_trip_preserves_reasons_and_values) {
  Reply reply;
  reply.outcome = OutcomeCode::PROMOTION_BLOCKED;
  reply.add(ReasonCode::EvidenceMissing, "health");
  reply.add(ReasonCode::RollbackTargetRequired);
  reply.set("model_id", 42u);
  reply.set("label", std::string("2.0.0"));
  std::vector<std::uint8_t> payload;
  MLF_CHECK(encode(reply, ProtocolLimits{}, payload) == ProtocolStatus::Ok);
  Reply decoded;
  MLF_CHECK(decode(payload.data(), payload.size(), ProtocolLimits{}, decoded) ==
            ProtocolStatus::Ok);
  MLF_CHECK(decoded.outcome == OutcomeCode::PROMOTION_BLOCKED);
  MLF_CHECK_EQ(decoded.reasons.size(), 2u);
  MLF_CHECK_EQ(decoded.u64("model_id"), 42u);
  MLF_CHECK(decoded.find("label") != nullptr);
  MLF_CHECK_EQ(*decoded.find("label"), std::string("2.0.0"));
}

MLF_TEST(protocol, every_message_codec_round_trips) {
  ProtocolLimits limits;

  RegisterModelMessage model;
  model.name = "m";
  model.family = "f";
  model.provenance = Provenance::Real;
  {
    std::vector<std::uint8_t> payload;
    MLF_CHECK(encode(model, limits, payload) == ProtocolStatus::Ok);
    RegisterModelMessage decoded;
    MLF_CHECK(decode(payload.data(), payload.size(), limits, decoded) == ProtocolStatus::Ok);
    MLF_CHECK_EQ(decoded.name, std::string("m"));
  }

  RegisterVersionMessage version;
  version.model = ModelId(3);
  version.label = "1.0";
  version.artifact.set_id = ArtifactSetId(1);
  version.artifact.generation = ArtifactGeneration(1);
  version.artifact.digest = "sha256:" + std::string(64, 'a');
  version.requirements.runtime = "r";
  version.requirements.backend = "b";
  version.predecessor = ModelVersionId(2);
  version.provenance = Provenance::Synthetic;
  {
    std::vector<std::uint8_t> payload;
    MLF_CHECK(encode(version, limits, payload) == ProtocolStatus::Ok);
    RegisterVersionMessage decoded;
    MLF_CHECK(decode(payload.data(), payload.size(), limits, decoded) == ProtocolStatus::Ok);
    MLF_CHECK(decoded.artifact.digest == version.artifact.digest);
    MLF_CHECK(decoded.predecessor == version.predecessor);
  }

  PublishEvidenceMessage evidence;
  evidence.version = ModelVersionId(1);
  evidence.kind = EvidenceKind::Readiness;
  evidence.verdict = EvidenceVerdict::Satisfied;
  evidence.value = 0.75;
  evidence.unit = "ratio";
  evidence.tick = 9;
  evidence.provenance = Provenance::Real;
  {
    std::vector<std::uint8_t> payload;
    MLF_CHECK(encode(evidence, limits, payload) == ProtocolStatus::Ok);
    PublishEvidenceMessage decoded;
    MLF_CHECK(decode(payload.data(), payload.size(), limits, decoded) == ProtocolStatus::Ok);
    MLF_CHECK(decoded.kind == EvidenceKind::Readiness);
    MLF_CHECK_EQ(decoded.tick, 9u);
  }

  FenceMessage fence;
  fence.worker = WorkerId(4);
  fence.boot = WorkerBootId(5);
  fence.reason = "test";
  {
    std::vector<std::uint8_t> payload;
    MLF_CHECK(encode(fence, limits, payload) == ProtocolStatus::Ok);
    FenceMessage decoded;
    MLF_CHECK(decode(payload.data(), payload.size(), limits, decoded) == ProtocolStatus::Ok);
    MLF_CHECK(decoded.boot == fence.boot);
  }
}

MLF_TEST(protocol, a_malformed_hello_is_decoded_as_a_rejection) {
  std::vector<std::uint8_t> payload = encode_hello();
  payload.resize(2);
  HelloMessage decoded;
  MLF_CHECK(decode(payload.data(), payload.size(), ProtocolLimits{}, decoded) ==
            ProtocolStatus::DecodeFailure);
}

MLF_TEST(protocol, a_handshake_with_the_wrong_version_is_refused_by_the_coordinator) {
  mlftest::InProcessCoordinator coordinator;
  std::uint16_t port = 0;
  if (!start_coordinator(coordinator, port)) {
    MLF_CHECK_MSG(false, "coordinator did not start");
  }

  LifecycleClient client;
  std::string error;
  MLF_CHECK(client.connect("127.0.0.1", port, error));
  HelloMessage hello;
  hello.protocol_version = 999;
  const Reply reply = client.call(hello, error);
  MLF_CHECK(!reply.ok());
  MLF_CHECK(reply.reasons.size() >= 1);
  client.close();
  coordinator.stop();
}

MLF_TEST(protocol, a_second_hello_on_the_same_connection_is_refused) {
  mlftest::InProcessCoordinator coordinator;
  std::uint16_t port = 0;
  if (!start_coordinator(coordinator, port)) {
    MLF_CHECK_MSG(false, "coordinator did not start");
  }
  LifecycleClient client;
  std::string error;
  MLF_CHECK(client.connect("127.0.0.1", port, error));
  MLF_CHECK(client.hello(WorkerId{}, WorkerBootId{}, CoordinatorEpoch{}, {}, error).ok());
  HelloMessage hello;
  const Reply duplicate = client.call(hello, error);
  MLF_CHECK(!duplicate.ok());
  MLF_CHECK(duplicate.reasons.front().code == ReasonCode::ProtocolDuplicateHello);
  client.close();
  coordinator.stop();
}

MLF_TEST(protocol, a_worker_registration_without_hello_is_refused) {
  mlftest::InProcessCoordinator coordinator;
  std::uint16_t port = 0;
  if (!start_coordinator(coordinator, port)) {
    MLF_CHECK_MSG(false, "coordinator did not start");
  }
  LifecycleClient client;
  std::string error;
  MLF_CHECK(client.connect("127.0.0.1", port, error));
  HelloMessage registration;
  registration.worker = WorkerId(1);
  registration.boot = WorkerBootId(1);
  const Reply reply =
      client.call_as(MessageType::REGISTER_WORKER, registration, error);
  MLF_CHECK(!reply.ok());
  MLF_CHECK(reply.reasons.front().code == ReasonCode::ProtocolInvalidTransition);
  client.close();
  coordinator.stop();
}

MLF_TEST(protocol, a_completion_for_an_unknown_attempt_is_refused) {
  mlftest::InProcessCoordinator coordinator;
  std::uint16_t port = 0;
  if (!start_coordinator(coordinator, port)) {
    MLF_CHECK_MSG(false, "coordinator did not start");
  }
  LifecycleClient client;
  std::string error;
  MLF_CHECK(client.connect("127.0.0.1", port, error));
  MLF_CHECK(client.hello(WorkerId{}, WorkerBootId{}, CoordinatorEpoch{}, {}, error).ok());
  CompletionMessage completion;
  completion.attempt = AttemptId(4242);
  completion.success = true;
  completion.confirmed = true;
  const Reply reply = client.call(completion, error);
  MLF_CHECK(!reply.ok());
  MLF_CHECK(reply.reasons.front().code == ReasonCode::AttemptUnknown);
  client.close();
  coordinator.stop();
}

MLF_TEST(protocol, a_client_session_cannot_register_a_worker_with_a_stale_epoch) {
  mlftest::InProcessCoordinator coordinator;
  std::uint16_t port = 0;
  if (!start_coordinator(coordinator, port)) {
    MLF_CHECK_MSG(false, "coordinator did not start");
  }
  LifecycleClient client;
  std::string error;
  MLF_CHECK(client.connect("127.0.0.1", port, error));
  const Reply hello =
      client.hello(WorkerId(1), WorkerBootId(1), CoordinatorEpoch{}, {}, error);
  MLF_CHECK(hello.ok());
  HelloMessage registration;
  registration.worker = WorkerId(1);
  registration.boot = WorkerBootId(1);
  const Reply registered =
      client.call_as(MessageType::REGISTER_WORKER, registration, error);
  MLF_CHECK(registered.ok());
  MLF_CHECK_EQ(registered.u64("worker_boot_id"), 1u);
  client.close();
  coordinator.stop();
}