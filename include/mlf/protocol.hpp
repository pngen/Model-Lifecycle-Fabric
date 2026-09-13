// Model Lifecycle Fabric — bounded, versioned, framed protocol.
//
// A frame is a fixed 16-byte header, a bounded payload, and a trailing CRC-32
// over everything before it. Decoding validates magic, version, type, length,
// truncation and integrity before any message is interpreted.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mlf/compatibility.hpp"
#include "mlf/engine.hpp"
#include "mlf/identity.hpp"
#include "mlf/provenance.hpp"
#include "mlf/status.hpp"
#include "mlf/version.hpp"

namespace mlf {

/// Frame header size in bytes: magic(4) version(2) type(2) flags(4) length(4).
inline constexpr std::size_t kFrameHeaderSize = 16;
/// Trailing integrity check size in bytes.
inline constexpr std::size_t kFrameTrailerSize = 4;

/// Message type. Stable on the wire; unknown values are rejected.
enum class MessageType : std::uint16_t {
  HELLO = 1,
  HELLO_ACK = 2,
  REGISTER_WORKER = 3,
  REGISTER_MODEL = 4,
  REGISTER_VERSION = 5,
  PUBLISH_COMPATIBILITY = 6,
  PUBLISH_READINESS = 7,
  PUBLISH_HEALTH = 8,
  REQUEST_WARM = 9,
  REQUEST_ACTIVATE = 10,
  REQUEST_DRAIN = 11,
  REQUEST_ROLLBACK = 12,
  ACK_ATTEMPT = 13,
  PUBLISH_COMPLETION = 14,
  QUERY_STATE = 15,
  QUERY_AUTHORITY = 16,
  FENCE = 17,
  PROMOTE = 18,
  REGISTER_ROLLOUT_PLAN = 19,
  BEGIN_ROLLOUT = 20,
  ADVANCE_STAGE = 21,
  FAIL_STAGE = 22,
  RETIRE = 23,
  REVALIDATE = 24,
  ROLLBACK = 25,
  RECONCILE = 26,
  SUPERSEDE_ROLLOUT = 27,
  REPLY = 28,
  COMMAND = 29,
  REGISTER_SCOPE = 30,
};

inline constexpr std::size_t kMessageTypeCount = 30;

[[nodiscard]] std::string_view to_string(MessageType type) noexcept;
[[nodiscard]] bool parse_message_type(std::string_view text, MessageType& out) noexcept;
/// True when the type is a coordinator-to-worker command.
[[nodiscard]] bool is_command(MessageType type) noexcept;

enum class ProtocolStatus : std::uint8_t {
  Ok = 0,
  NeedMoreData,
  WrongMagic,
  UnsupportedVersion,
  UnknownType,
  Oversized,
  Truncated,
  IntegrityFailure,
  DecodeFailure,
  EncodeFailure,
  IoError,
  TooManyPending,
  Closed,
};

[[nodiscard]] std::string_view to_string(ProtocolStatus status) noexcept;
[[nodiscard]] ReasonCode reason_for(ProtocolStatus status) noexcept;
/// Outcome code a rejected frame maps to.
[[nodiscard]] OutcomeCode outcome_for(ProtocolStatus status) noexcept;

/// Limits applied to every frame.
struct ProtocolLimits {
  std::uint16_t version{kProtocolVersion};
  std::size_t max_payload_bytes{262144};
  std::size_t max_pending_frames{64};
  std::size_t max_connections{64};
  std::size_t max_string{256};
  std::size_t max_digest{128};
  std::size_t max_reasons{32};
  std::size_t max_values{64};
};

// ---------------------------------------------------------------------------
// Payload codec
// ---------------------------------------------------------------------------

/// Canonical little-endian payload writer with bounded strings.
class PayloadWriter {
 public:
  explicit PayloadWriter(std::vector<std::uint8_t>& out) : out_(out) {}
  void u8(std::uint8_t value) { out_.push_back(value); }
  void boolean(bool value) { u8(value ? 1u : 0u); }
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i8(std::int8_t value) { u8(static_cast<std::uint8_t>(value)); }
  void f64(double value);
  void str(std::string_view value);
  template <class Tag>
  void id(Id<Tag> value) {
    u64(value.raw());
  }
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return out_; }

 private:
  std::vector<std::uint8_t>& out_;
};

/// Bounds-checked payload reader. Once a read fails the reader stays failed and
/// every later read returns a default, so a decode routine can validate once.
class PayloadReader {
 public:
  PayloadReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] bool exhausted() const noexcept { return position_ == size_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - position_; }
  void fail() noexcept { ok_ = false; }

  std::uint8_t u8();
  bool boolean() { return u8() != 0; }
  std::uint16_t u16();
  std::uint32_t u32();
  std::uint64_t u64();
  std::int8_t i8() { return static_cast<std::int8_t>(u8()); }
  double f64();
  std::string str(std::size_t max_length);
  template <class Tag>
  Id<Tag> id() {
    return Id<Tag>(u64());
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t position_{0};
  bool ok_{true};
};

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

struct Frame {
  MessageType type{MessageType::HELLO};
  std::uint32_t flags{0};
  std::vector<std::uint8_t> payload{};
};

[[nodiscard]] ProtocolStatus encode_frame(const Frame& frame, const ProtocolLimits& limits,
                                          std::vector<std::uint8_t>& out);
[[nodiscard]] ProtocolStatus decode_frame(const std::uint8_t* data, std::size_t size,
                                          const ProtocolLimits& limits, Frame& out,
                                          std::size_t& consumed);

/// Incremental stream decoder. Rejects a bad header as soon as it is readable
/// rather than buffering unbounded input.
class FrameDecoder {
 public:
  explicit FrameDecoder(ProtocolLimits limits = {}) : limits_(limits) {}

  /// Feed bytes. Returns the first status encountered; Ok means the bytes were
  /// accepted into the buffer.
  ProtocolStatus feed(const std::uint8_t* data, std::size_t size);

  /// Pop one decoded frame. Returns NeedMoreData when a whole frame is not yet
  /// available, and a rejection status when the buffered stream is invalid.
  ProtocolStatus next(Frame& out);

  [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] ProtocolStatus failure() const noexcept { return failure_; }
  void reset() noexcept {
    buffer_.clear();
    position_ = 0;
    failed_ = false;
    failure_ = ProtocolStatus::Ok;
  }

 private:
  ProtocolLimits limits_;
  std::vector<std::uint8_t> buffer_{};
  std::size_t position_{0};
  bool failed_{false};
  ProtocolStatus failure_{ProtocolStatus::Ok};
};

// ---------------------------------------------------------------------------
// Bounded outbound queue (backpressure)
// ---------------------------------------------------------------------------

/// Bounded frame queue. Overflow is rejected explicitly and accounted, never
/// silently dropped: incomplete delivery stays visible to the caller.
class FrameQueue {
 public:
  explicit FrameQueue(std::size_t capacity = 64) : capacity_(capacity) {}

  /// Returns TooManyPending when the queue is full.
  ProtocolStatus push(Frame frame);
  [[nodiscard]] bool pop(Frame& out);
  [[nodiscard]] std::size_t size() const noexcept { return queue_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }
  [[nodiscard]] bool empty() const noexcept { return queue_.empty(); }
  void set_capacity(std::size_t capacity) noexcept { capacity_ = capacity; }

 private:
  std::deque<Frame> queue_{};
  std::size_t capacity_{64};
  std::uint64_t rejected_{0};
};

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

/// RAII TCP socket over IPv4 loopback or any interface.
class Socket {
 public:
  Socket() = default;
  ~Socket();
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  /// Bind and listen. Port zero selects an ephemeral port; the chosen port is
  /// reported through local_port().
  [[nodiscard]] static ProtocolStatus listen_on(std::string_view address, std::uint16_t port,
                                                int backlog, Socket& out, std::string& error);
  [[nodiscard]] static ProtocolStatus connect_to(std::string_view address, std::uint16_t port,
                                                 Socket& out, std::string& error);

  [[nodiscard]] ProtocolStatus accept(Socket& out, std::string& error) const;
  [[nodiscard]] ProtocolStatus send_all(const std::uint8_t* data, std::size_t size,
                                        std::string& error);
  [[nodiscard]] ProtocolStatus recv_some(std::uint8_t* data, std::size_t capacity,
                                         std::size_t& received, std::string& error);

  /// Switch between blocking and non-blocking mode. A listening socket must be
  /// non-blocking so that accepting in a loop cannot block after the last
  /// pending connection has been taken.
  [[nodiscard]] ProtocolStatus set_blocking(bool blocking, std::string& error);

  /// Half-close so the peer observes orderly shutdown.
  void shutdown_send();
  void close();
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint16_t local_port() const noexcept;

  /// The native handle, exposed so callers can integrate with platform I/O
  /// multiplexing. Never owned by the caller.
  [[nodiscard]] std::uintptr_t native_handle() const noexcept { return handle_; }

  /// Release ownership of the native handle (used when handing a connection to
  /// another owner).
  [[nodiscard]] std::uintptr_t release() noexcept;

 private:
  std::uintptr_t handle_{kInvalidHandle};
 public:
  static constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(~0ull);
};

/// One framed, bounded, duplex channel over a socket.
class FrameChannel {
 public:
  FrameChannel() = default;
  explicit FrameChannel(Socket socket, ProtocolLimits limits = {})
      : socket_(std::move(socket)), limits_(limits), outbound_(limits.max_pending_frames) {}

  [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }
  [[nodiscard]] Socket& socket() noexcept { return socket_; }
  [[nodiscard]] ProtocolLimits& limits() noexcept { return limits_; }

  ProtocolStatus send(const Frame& frame, std::string& error);
  /// Non-blocking progress: flushes what it can, then reads once.
  ProtocolStatus pump(std::string& error);
  [[nodiscard]] ProtocolStatus receive(Frame& out, std::string& error);
  [[nodiscard]] FrameQueue& outbound() noexcept { return outbound_; }
  [[nodiscard]] std::uint64_t rejected_frames() const noexcept { return outbound_.rejected(); }
  void close();

 private:
  Socket socket_{};
  ProtocolLimits limits_{};
  FrameDecoder decoder_{};
  FrameQueue outbound_{};
  std::vector<std::uint8_t> scratch_{};
};

// ---------------------------------------------------------------------------
// Reply
// ---------------------------------------------------------------------------

/// Generic reply: a structured outcome, named reasons, and bounded typed values.
struct Reply {
  OutcomeCode outcome{OutcomeCode::Rejected};
  std::vector<Reason> reasons{};
  std::vector<std::pair<std::string, std::string>> values{};

  [[nodiscard]] bool ok() const noexcept { return is_positive(outcome); }
  Reply& add(ReasonCode code, std::string detail = {});
  Reply& set(std::string key, std::string value);
  Reply& set(std::string key, std::uint64_t value);
  [[nodiscard]] const std::string* find(std::string_view key) const noexcept;
  [[nodiscard]] std::uint64_t u64(std::string_view key, std::uint64_t fallback = 0) const noexcept;
  [[nodiscard]] std::string render() const;
};

[[nodiscard]] Reply reply_from_decision(const Decision& decision);
[[nodiscard]] Decision decision_from_reply(const Reply& reply);

// ---------------------------------------------------------------------------
// Message payloads
// ---------------------------------------------------------------------------

struct HelloMessage {
  std::uint16_t protocol_version{kProtocolVersion};
  WorkerId worker{};
  WorkerBootId boot{};
  CoordinatorEpoch epoch{};
  std::vector<ScopeId> scopes{};
  std::string detail{};
};

struct HelloAckMessage {
  std::uint16_t protocol_version{kProtocolVersion};
  CoordinatorEpoch epoch{};
  WorkerId worker{};
  WorkerBootId boot{};
  bool accepted{false};
  std::vector<ScopeId> scopes{};
  std::string detail{};
};

struct RegisterModelMessage {
  std::string name{};
  std::string family{};
  Provenance provenance{Provenance::Unknown};
};

struct RegisterVersionMessage {
  ModelId model{};
  std::string label{};
  ArtifactBinding artifact{};
  ModelRequirements requirements{};
  ModelVersionId predecessor{};
  Provenance provenance{Provenance::Unknown};
};

struct PublishCompatibilityMessage {
  ModelVersionId version{};
  EnvironmentProfile environment{};
  CompatibilityOutcome outcome{CompatibilityOutcome::UNKNOWN};
  std::string detail{};
  Provenance provenance{Provenance::Unknown};
};

struct PublishEvidenceMessage {
  ModelVersionId version{};
  ModelGeneration model_generation{};
  ArtifactGeneration artifact_generation{};
  ScopeId scope{};
  RolloutId rollout{};
  RolloutStageId stage{};
  EvidenceKind kind{EvidenceKind::HealthCheck};
  EvidenceVerdict verdict{EvidenceVerdict::Unknown};
  double value{0.0};
  std::string unit{};
  std::uint64_t tick{0};
  std::string detail{};
  Provenance provenance{Provenance::Unknown};
};

struct AttemptRequestMessage {
  AttemptKind kind{AttemptKind::Warm};
  /// Worker that must perform the action. When invalid the coordinator routes
  /// the attempt to the requesting session's own worker lease; a pure client has
  /// none, so such a request is refused rather than routed to the wrong peer.
  WorkerId worker{};
  WorkerBootId boot{};
  ModelId model{};
  ModelVersionId version{};
  ModelGeneration model_generation{};
  ScopeId scope{};
  RolloutId rollout{};
  RolloutGeneration rollout_generation{};
  RolloutStageId stage{};
  RolloutStageGeneration stage_generation{};
  bool idempotent{false};
  std::string detail{};
};

struct CompletionMessage {
  AttemptId attempt{};
  bool success{false};
  bool confirmed{false};
  std::string detail{};
};

struct FenceMessage {
  WorkerId worker{};
  WorkerBootId boot{};
  std::string reason{};
};

struct CommandMessage {
  AttemptKind kind{AttemptKind::Warm};
  AttemptId attempt{};
  ModelId model{};
  ModelVersionId version{};
  ModelGeneration model_generation{};
  ArtifactGeneration artifact_generation{};
  ScopeId scope{};
  RolloutId rollout{};
  RolloutGeneration rollout_generation{};
  std::string detail{};
};

struct QueryStateMessage {
  ModelId model{};
  ModelVersionId version{};
  ScopeId scope{};
};

struct QueryAuthorityMessage {
  ScopeId scope{};
};

struct PromotionMessage {
  PromotionRequest request{};
};

struct RolloutPlanMessage {
  RolloutPlanRequest request{};
};

struct ProgressMessage {
  RolloutProgressRequest request{};
};

struct RetireMessage {
  RetirementRequest request{};
};

struct RevalidateMessage {
  RevalidationRequest request{};
};

struct RollbackMessage {
  RollbackRequest request{};
};

struct SupersedeMessage {
  RolloutId rollout{};
  RolloutGeneration generation{};
  std::string detail{};
};

/// Ask the coordinator to reconcile durable lifecycle truth against the
/// evidence its workers have actually published.
struct ReconcileMessage {
  /// Reserved for future selectors; must be zero for this protocol version.
  std::uint32_t flags{0};
};

/// Resolve a canonical scope path such as "global/cluster:c1" to its identity,
/// creating the path when it does not exist yet. Deterministic and idempotent.
struct RegisterScopeMessage {
  std::string path{};
};

// Wire codecs. Each returns Ok or a rejection status; a rejected decode leaves
// the output untouched where the structure is trivially replaceable.
ProtocolStatus encode(const HelloMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      HelloMessage& out);
ProtocolStatus encode(const HelloAckMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      HelloAckMessage& out);
ProtocolStatus encode(const RegisterModelMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      RegisterModelMessage& out);
ProtocolStatus encode(const RegisterVersionMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      RegisterVersionMessage& out);
ProtocolStatus encode(const PublishCompatibilityMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      PublishCompatibilityMessage& out);
ProtocolStatus encode(const PublishEvidenceMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      PublishEvidenceMessage& out);
ProtocolStatus encode(const AttemptRequestMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      AttemptRequestMessage& out);
ProtocolStatus encode(const CompletionMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      CompletionMessage& out);
ProtocolStatus encode(const FenceMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      FenceMessage& out);
ProtocolStatus encode(const CommandMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      CommandMessage& out);
ProtocolStatus encode(const QueryStateMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      QueryStateMessage& out);
ProtocolStatus encode(const QueryAuthorityMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      QueryAuthorityMessage& out);
ProtocolStatus encode(const PromotionMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      PromotionMessage& out);
ProtocolStatus encode(const RolloutPlanMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      RolloutPlanMessage& out);
ProtocolStatus encode(const ProgressMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      ProgressMessage& out);
ProtocolStatus encode(const RetireMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      RetireMessage& out);
ProtocolStatus encode(const RevalidateMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      RevalidateMessage& out);
ProtocolStatus encode(const RollbackMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      RollbackMessage& out);
ProtocolStatus encode(const SupersedeMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      SupersedeMessage& out);
ProtocolStatus encode(const ReconcileMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      ReconcileMessage& out);
ProtocolStatus encode(const RegisterScopeMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      RegisterScopeMessage& out);
ProtocolStatus encode(const Reply& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out);
ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      Reply& out);

/// Type-dispatched helpers: attach a payload to a frame of the matching type,
/// and decode a frame payload into the matching structure.
[[nodiscard]] Frame make_frame(MessageType type, const std::vector<std::uint8_t>& payload);

}  // namespace mlf
