#include "mlf/protocol.hpp"

#include <algorithm>
#include <cstring>

#include "mlf/state_store.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mlf {
namespace {

constexpr std::uint8_t kFrameMagic[4] = {'M', 'L', 'F', 'P'};

constexpr char kMessageNames[][28] = {
    "INVALID",           "HELLO",       "HELLO_ACK",      "REGISTER_WORKER",
    "REGISTER_MODEL",    "REGISTER_VERSION", "PUBLISH_COMPATIBILITY", "PUBLISH_READINESS",
    "PUBLISH_HEALTH",    "REQUEST_WARM", "REQUEST_ACTIVATE", "REQUEST_DRAIN",
    "REQUEST_ROLLBACK",  "ACK_ATTEMPT", "PUBLISH_COMPLETION", "QUERY_STATE",
    "QUERY_AUTHORITY",   "FENCE",       "PROMOTE",        "REGISTER_ROLLOUT_PLAN",
    "BEGIN_ROLLOUT",     "ADVANCE_STAGE", "FAIL_STAGE",   "RETIRE",
    "REVALIDATE",        "ROLLBACK",    "RECONCILE",      "SUPERSEDE_ROLLOUT",
    "REPLY",             "COMMAND",     "REGISTER_SCOPE"};

std::uint32_t crc32_bytes(const std::uint8_t* data, std::size_t size) noexcept {
  return crc32(data, size);
}

#ifdef _WIN32
bool ensure_winsock() {
  static const bool initialized = []() {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
}
#endif

}  // namespace

std::string_view to_string(MessageType type) noexcept {
  const auto index = static_cast<std::size_t>(type);
  if (index == 0 || index >= kMessageTypeCount + 1) return "INVALID";
  return kMessageNames[index];
}

bool parse_message_type(std::string_view text, MessageType& out) noexcept {
  for (std::size_t i = 1; i <= kMessageTypeCount; ++i) {
    if (text == kMessageNames[i]) {
      out = static_cast<MessageType>(i);
      return true;
    }
  }
  return false;
}

bool is_command(MessageType type) noexcept {
  return type == MessageType::REQUEST_WARM || type == MessageType::REQUEST_ACTIVATE ||
         type == MessageType::REQUEST_DRAIN || type == MessageType::REQUEST_ROLLBACK ||
         type == MessageType::COMMAND;
}

std::string_view to_string(ProtocolStatus status) noexcept {
  switch (status) {
    case ProtocolStatus::Ok:                  return "OK";
    case ProtocolStatus::NeedMoreData:        return "NEED_MORE_DATA";
    case ProtocolStatus::WrongMagic:          return "WRONG_MAGIC";
    case ProtocolStatus::UnsupportedVersion:  return "UNSUPPORTED_VERSION";
    case ProtocolStatus::UnknownType:         return "UNKNOWN_TYPE";
    case ProtocolStatus::Oversized:           return "OVERSIZED";
    case ProtocolStatus::Truncated:           return "TRUNCATED";
    case ProtocolStatus::IntegrityFailure:    return "INTEGRITY_FAILURE";
    case ProtocolStatus::DecodeFailure:       return "DECODE_FAILURE";
    case ProtocolStatus::EncodeFailure:       return "ENCODE_FAILURE";
    case ProtocolStatus::IoError:             return "IO_ERROR";
    case ProtocolStatus::TooManyPending:      return "TOO_MANY_PENDING";
    case ProtocolStatus::Closed:              return "CLOSED";
  }
  return "UNKNOWN";
}

ReasonCode reason_for(ProtocolStatus status) noexcept {
  switch (status) {
    case ProtocolStatus::WrongMagic:         return ReasonCode::ProtocolWrongMagic;
    case ProtocolStatus::UnsupportedVersion: return ReasonCode::ProtocolUnsupportedVersion;
    case ProtocolStatus::UnknownType:        return ReasonCode::ProtocolUnknownType;
    case ProtocolStatus::Oversized:          return ReasonCode::ProtocolOversized;
    case ProtocolStatus::Truncated:          return ReasonCode::ProtocolTruncated;
    case ProtocolStatus::IntegrityFailure:   return ReasonCode::ProtocolIntegrityFailure;
    case ProtocolStatus::DecodeFailure:      return ReasonCode::ProtocolDecodeFailure;
    case ProtocolStatus::TooManyPending:     return ReasonCode::BoundsPayloadSize;
    default:                                 return ReasonCode::ProtocolDecodeFailure;
  }
}

OutcomeCode outcome_for(ProtocolStatus status) noexcept {
  switch (status) {
    case ProtocolStatus::Oversized:
    case ProtocolStatus::TooManyPending:     return OutcomeCode::BOUNDS_EXCEEDED;
    case ProtocolStatus::WrongMagic:
    case ProtocolStatus::UnsupportedVersion:
    case ProtocolStatus::UnknownType:
    case ProtocolStatus::Truncated:
    case ProtocolStatus::IntegrityFailure:
    case ProtocolStatus::DecodeFailure:      return OutcomeCode::Rejected;
    default:                                 return OutcomeCode::Rejected;
  }
}

// ---------------------------------------------------------------------------
// Payload codec
// ---------------------------------------------------------------------------

void PayloadWriter::u16(std::uint16_t value) {
  u8(static_cast<std::uint8_t>(value & 0xffu));
  u8(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void PayloadWriter::u32(std::uint32_t value) {
  for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
}

void PayloadWriter::u64(std::uint64_t value) {
  for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
}

void PayloadWriter::f64(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  u64(bits);
}

void PayloadWriter::str(std::string_view value) {
  u32(static_cast<std::uint32_t>(value.size()));
  for (char c : value) u8(static_cast<std::uint8_t>(c));
}

std::uint8_t PayloadReader::u8() {
  if (!ok_ || remaining() < 1) {
    ok_ = false;
    return 0;
  }
  return data_[position_++];
}

std::uint16_t PayloadReader::u16() {
  std::uint16_t value = 0;
  for (int i = 0; i < 2; ++i) {
    value = static_cast<std::uint16_t>(value | (static_cast<std::uint16_t>(u8()) << (8 * i)));
  }
  return value;
}

std::uint32_t PayloadReader::u32() {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(u8()) << (8 * i);
  return value;
}

std::uint64_t PayloadReader::u64() {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(u8()) << (8 * i);
  return value;
}

double PayloadReader::f64() {
  const std::uint64_t bits = u64();
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::string PayloadReader::str(std::size_t max_length) {
  const std::uint32_t length = u32();
  if (!ok_ || remaining() < length) {
    ok_ = false;
    return {};
  }
  if (length > max_length) {
    ok_ = false;
    return {};
  }
  std::string out(reinterpret_cast<const char*>(data_ + position_), length);
  position_ += length;
  return out;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

ProtocolStatus encode_frame(const Frame& frame, const ProtocolLimits& limits,
                            std::vector<std::uint8_t>& out) {
  if (frame.payload.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  const auto type_index = static_cast<std::uint16_t>(frame.type);
  if (type_index == 0 || type_index > kMessageTypeCount) return ProtocolStatus::UnknownType;

  out.clear();
  out.reserve(kFrameHeaderSize + frame.payload.size() + kFrameTrailerSize);
  out.push_back(kFrameMagic[0]);
  out.push_back(kFrameMagic[1]);
  out.push_back(kFrameMagic[2]);
  out.push_back(kFrameMagic[3]);
  const auto push16 = [&out](std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
  };
  const auto push32 = [&out](std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
    }
  };
  push16(limits.version);
  push16(type_index);
  push32(frame.flags);
  push32(static_cast<std::uint32_t>(frame.payload.size()));
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());
  push32(crc32_bytes(out.data(), out.size()));
  return ProtocolStatus::Ok;
}

ProtocolStatus decode_frame(const std::uint8_t* data, std::size_t size,
                            const ProtocolLimits& limits, Frame& out, std::size_t& consumed) {
  consumed = 0;
  if (data == nullptr) return ProtocolStatus::DecodeFailure;
  if (size < kFrameHeaderSize) return ProtocolStatus::NeedMoreData;
  if (std::memcmp(data, kFrameMagic, sizeof(kFrameMagic)) != 0) return ProtocolStatus::WrongMagic;

  const auto read16 = [data](std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset] | (static_cast<std::uint16_t>(data[offset + 1]) << 8));
  };
  const auto read32 = [data](std::size_t offset) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(data[offset + static_cast<std::size_t>(i)]) << (8 * i);
    return value;
  };

  const std::uint16_t version = read16(4);
  const std::uint16_t type = read16(6);
  const std::uint32_t flags = read32(8);
  const std::uint32_t length = read32(12);

  if (version != limits.version) return ProtocolStatus::UnsupportedVersion;
  if (type == 0 || static_cast<std::size_t>(type) > kMessageTypeCount) {
    return ProtocolStatus::UnknownType;
  }
  if (length > limits.max_payload_bytes) return ProtocolStatus::Oversized;

  const std::size_t total = kFrameHeaderSize + static_cast<std::size_t>(length) + kFrameTrailerSize;
  if (size < total) return ProtocolStatus::NeedMoreData;

  std::uint32_t stored = 0;
  for (int i = 0; i < 4; ++i) {
    stored |= static_cast<std::uint32_t>(data[kFrameHeaderSize + length + static_cast<std::size_t>(i)])
              << (8 * i);
  }
  if (stored != crc32_bytes(data, kFrameHeaderSize + length)) {
    return ProtocolStatus::IntegrityFailure;
  }

  out.type = static_cast<MessageType>(type);
  out.flags = flags;
  out.payload.assign(data + kFrameHeaderSize, data + kFrameHeaderSize + length);
  consumed = total;
  return ProtocolStatus::Ok;
}

ProtocolStatus FrameDecoder::feed(const std::uint8_t* data, std::size_t size) {
  if (failed_) return failure_;
  if (size > limits_.max_payload_bytes + kFrameHeaderSize + kFrameTrailerSize) {
    // A single feed larger than any legal frame cannot become legal by waiting.
    failed_ = true;
    failure_ = ProtocolStatus::Oversized;
    return failure_;
  }
  if (buffer_.size() + size > limits_.max_payload_bytes + kFrameHeaderSize + kFrameTrailerSize) {
    failed_ = true;
    failure_ = ProtocolStatus::Oversized;
    return failure_;
  }
  if (position_ > 0 && position_ == buffer_.size()) {
    buffer_.clear();
    position_ = 0;
  }
  buffer_.insert(buffer_.end(), data, data + size);
  return ProtocolStatus::Ok;
}

ProtocolStatus FrameDecoder::next(Frame& out) {
  if (failed_) return failure_;
  const std::size_t available = buffer_.size() - position_;
  if (available < kFrameHeaderSize) return ProtocolStatus::NeedMoreData;
  std::size_t consumed = 0;
  const ProtocolStatus status =
      decode_frame(buffer_.data() + position_, available, limits_, out, consumed);
  if (status == ProtocolStatus::Ok) {
    position_ += consumed;
    if (position_ == buffer_.size()) {
      buffer_.clear();
      position_ = 0;
    }
    return ProtocolStatus::Ok;
  }
  if (status != ProtocolStatus::NeedMoreData) {
    failed_ = true;
    failure_ = status;
  }
  return status;
}

// ---------------------------------------------------------------------------
// FrameQueue
// ---------------------------------------------------------------------------

ProtocolStatus FrameQueue::push(Frame frame) {
  if (queue_.size() >= capacity_) {
    ++rejected_;
    return ProtocolStatus::TooManyPending;
  }
  queue_.push_back(std::move(frame));
  return ProtocolStatus::Ok;
}

bool FrameQueue::pop(Frame& out) {
  if (queue_.empty()) return false;
  out = std::move(queue_.front());
  queue_.pop_front();
  return true;
}

// ---------------------------------------------------------------------------
// Socket
// ---------------------------------------------------------------------------

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept
    : handle_(other.handle_) {
  other.handle_ = kInvalidHandle;
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalidHandle;
  }
  return *this;
}

bool Socket::valid() const noexcept { return handle_ != kInvalidHandle; }

void Socket::close() {
  if (handle_ == kInvalidHandle) return;
#ifdef _WIN32
  closesocket(static_cast<SOCKET>(handle_));
#else
  ::close(static_cast<int>(handle_));
#endif
  handle_ = kInvalidHandle;
}

std::uintptr_t Socket::release() noexcept {
  const std::uintptr_t value = handle_;
  handle_ = kInvalidHandle;
  return value;
}

ProtocolStatus Socket::set_blocking(bool blocking, std::string& error) {
  if (handle_ == kInvalidHandle) {
    error = "socket is not open";
    return ProtocolStatus::Closed;
  }
#ifdef _WIN32
  u_long mode = blocking ? 0ul : 1ul;
  if (ioctlsocket(static_cast<SOCKET>(handle_), FIONBIO, &mode) != 0) {
    error = "ioctlsocket(FIONBIO) failed";
    return ProtocolStatus::IoError;
  }
#else
  const int flags = ::fcntl(static_cast<int>(handle_), F_GETFL, 0);
  if (flags < 0) {
    error = "fcntl(F_GETFL) failed";
    return ProtocolStatus::IoError;
  }
  const int updated = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
  if (::fcntl(static_cast<int>(handle_), F_SETFL, updated) < 0) {
    error = "fcntl(F_SETFL) failed";
    return ProtocolStatus::IoError;
  }
#endif
  return ProtocolStatus::Ok;
}

void Socket::shutdown_send() {
  if (handle_ == kInvalidHandle) return;
#ifdef _WIN32
  ::shutdown(static_cast<SOCKET>(handle_), SD_SEND);
#else
  ::shutdown(static_cast<int>(handle_), SHUT_WR);
#endif
}

std::uint16_t Socket::local_port() const noexcept {
  if (handle_ == kInvalidHandle) return 0;
  sockaddr_in address{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(address));
  if (getsockname(static_cast<SOCKET>(handle_), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return 0;
  }
#else
  socklen_t length = sizeof(address);
  if (getsockname(static_cast<int>(handle_), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return 0;
  }
#endif
  return ntohs(address.sin_port);
}

ProtocolStatus Socket::listen_on(std::string_view address, std::uint16_t port, int backlog,
                                 Socket& out, std::string& error) {
#ifdef _WIN32
  if (!ensure_winsock()) {
    error = "WSAStartup failed";
    return ProtocolStatus::IoError;
  }
#endif
  const auto handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
  if (handle == INVALID_SOCKET) {
#else
  if (handle < 0) {
#endif
    error = "socket() failed";
    return ProtocolStatus::IoError;
  }
  Socket listening;
  listening.handle_ = static_cast<std::uintptr_t>(handle);

  int reuse = 1;
  static_cast<void>(setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
                               reinterpret_cast<const char*>(&reuse), sizeof(reuse)));

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(port);
  if (address.empty() || address == "127.0.0.1" || address == "localhost") {
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  } else if (address == "0.0.0.0") {
    local.sin_addr.s_addr = htonl(INADDR_ANY);
  } else {
    if (inet_pton(AF_INET, std::string(address).c_str(), &local.sin_addr) != 1) {
      error = "address is not a valid IPv4 literal; refusing to guess";
      return ProtocolStatus::IoError;
    }
  }

  if (::bind(handle, reinterpret_cast<sockaddr*>(&local), static_cast<int>(sizeof(local))) != 0) {
    error = "bind() failed";
    return ProtocolStatus::IoError;
  }
  if (::listen(handle, backlog) != 0) {
    error = "listen() failed";
    return ProtocolStatus::IoError;
  }
  out = std::move(listening);
  return ProtocolStatus::Ok;
}

ProtocolStatus Socket::connect_to(std::string_view address, std::uint16_t port, Socket& out,
                                  std::string& error) {
#ifdef _WIN32
  if (!ensure_winsock()) {
    error = "WSAStartup failed";
    return ProtocolStatus::IoError;
  }
#endif
  const auto handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
  if (handle == INVALID_SOCKET) {
#else
  if (handle < 0) {
#endif
    error = "socket() failed";
    return ProtocolStatus::IoError;
  }
  Socket connection;
  connection.handle_ = static_cast<std::uintptr_t>(handle);

  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(port);
  const std::string host(address.empty() ? "127.0.0.1" : std::string(address));
  if (inet_pton(AF_INET, host.c_str(), &remote.sin_addr) != 1) {
    error = "address is not a valid IPv4 literal";
    return ProtocolStatus::IoError;
  }
  if (::connect(handle, reinterpret_cast<sockaddr*>(&remote), static_cast<int>(sizeof(remote))) != 0) {
    error = "connect() failed";
    return ProtocolStatus::IoError;
  }
  out = std::move(connection);
  return ProtocolStatus::Ok;
}

ProtocolStatus Socket::accept(Socket& out, std::string& error) const {
  if (handle_ == kInvalidHandle) {
    error = "socket is not open";
    return ProtocolStatus::Closed;
  }
  sockaddr_in remote{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(remote));
  const SOCKET accepted =
      ::accept(static_cast<SOCKET>(handle_), reinterpret_cast<sockaddr*>(&remote), &length);
  if (accepted == INVALID_SOCKET) {
#else
  socklen_t length = sizeof(remote);
  const int accepted =
      ::accept(static_cast<int>(handle_), reinterpret_cast<sockaddr*>(&remote), &length);
  if (accepted < 0) {
#endif
    error = "accept() failed";
    return ProtocolStatus::IoError;
  }
  Socket connection;
  connection.handle_ = static_cast<std::uintptr_t>(accepted);
  out = std::move(connection);
  return ProtocolStatus::Ok;
}

ProtocolStatus Socket::send_all(const std::uint8_t* data, std::size_t size, std::string& error) {
  std::size_t sent = 0;
  while (sent < size) {
    const std::size_t chunk = std::min<std::size_t>(size - sent, 1u << 20);
#ifdef _WIN32
    const int written = ::send(static_cast<SOCKET>(handle_),
                               reinterpret_cast<const char*>(data + sent), static_cast<int>(chunk), 0);
#else
    const auto written = ::send(static_cast<int>(handle_), data + sent, chunk, 0);
#endif
    if (written <= 0) {
      error = "send() failed";
      return ProtocolStatus::IoError;
    }
    sent += static_cast<std::size_t>(written);
  }
  return ProtocolStatus::Ok;
}

ProtocolStatus Socket::recv_some(std::uint8_t* data, std::size_t capacity, std::size_t& received,
                                 std::string& error) {
  received = 0;
  if (capacity == 0) return ProtocolStatus::Ok;
#ifdef _WIN32
  const int got = ::recv(static_cast<SOCKET>(handle_), reinterpret_cast<char*>(data),
                         static_cast<int>(std::min<std::size_t>(capacity, 1u << 20)), 0);
  if (got == 0) return ProtocolStatus::Closed;
  if (got < 0) {
    error = "recv() failed";
    return ProtocolStatus::IoError;
  }
  received = static_cast<std::size_t>(got);
#else
  const auto got = ::recv(static_cast<int>(handle_), data,
                          std::min<std::size_t>(capacity, 1u << 20), 0);
  if (got == 0) return ProtocolStatus::Closed;
  if (got < 0) {
    error = "recv() failed";
    return ProtocolStatus::IoError;
  }
  received = static_cast<std::size_t>(got);
#endif
  return ProtocolStatus::Ok;
}

// ---------------------------------------------------------------------------
// FrameChannel
// ---------------------------------------------------------------------------

ProtocolStatus FrameChannel::send(const Frame& frame, std::string& error) {
  if (!socket_.valid()) {
    error = "channel is closed";
    return ProtocolStatus::Closed;
  }
  std::vector<std::uint8_t> encoded;
  const ProtocolStatus status = encode_frame(frame, limits_, encoded);
  if (status != ProtocolStatus::Ok) return status;
  return socket_.send_all(encoded.data(), encoded.size(), error);
}

ProtocolStatus FrameChannel::receive(Frame& out, std::string& error) {
  if (!socket_.valid()) {
    error = "channel is closed";
    return ProtocolStatus::Closed;
  }
  for (;;) {
    const ProtocolStatus decoded = decoder_.next(out);
    if (decoded == ProtocolStatus::Ok) return ProtocolStatus::Ok;
    if (decoded != ProtocolStatus::NeedMoreData) return decoded;

    scratch_.resize(64 * 1024);
    std::size_t received = 0;
    const ProtocolStatus read = socket_.recv_some(scratch_.data(), scratch_.size(), received, error);
    if (read != ProtocolStatus::Ok) return read;
    const ProtocolStatus fed = decoder_.feed(scratch_.data(), received);
    if (fed != ProtocolStatus::Ok) return fed;
  }
}

ProtocolStatus FrameChannel::pump(std::string& error) {
  Frame frame;
  return receive(frame, error);
}

void FrameChannel::close() {
  socket_.shutdown_send();
  socket_.close();
}

// ---------------------------------------------------------------------------
// Reply
// ---------------------------------------------------------------------------

Reply& Reply::add(ReasonCode code, std::string detail) {
  if (code == ReasonCode::None) return *this;
  if (detail.size() > 160) detail.resize(160);
  reasons.emplace_back(code, std::move(detail));
  return *this;
}

Reply& Reply::set(std::string key, std::string value) {
  if (value.size() > 256) value.resize(256);
  for (auto& entry : values) {
    if (entry.first == key) {
      entry.second = std::move(value);
      return *this;
    }
  }
  values.emplace_back(std::move(key), std::move(value));
  return *this;
}

Reply& Reply::set(std::string key, std::uint64_t value) {
  return set(std::move(key), std::to_string(value));
}

const std::string* Reply::find(std::string_view key) const noexcept {
  for (const auto& entry : values) {
    if (entry.first == key) return &entry.second;
  }
  return nullptr;
}

std::uint64_t Reply::u64(std::string_view key, std::uint64_t fallback) const noexcept {
  const std::string* value = find(key);
  if (value == nullptr || value->empty()) return fallback;
  std::uint64_t result = 0;
  for (char c : *value) {
    if (c < '0' || c > '9') return fallback;
    result = result * 10u + static_cast<std::uint64_t>(c - '0');
  }
  return result;
}

std::string Reply::render() const {
  std::string out(to_string(outcome));
  for (const Reason& reason : reasons) {
    out.push_back(' ');
    out.append(to_string(reason.code));
  }
  for (const auto& entry : values) {
    out.push_back(' ');
    out.append(entry.first).push_back('=');
    out.append(entry.second);
  }
  return out;
}

Reply reply_from_decision(const Decision& decision) {
  Reply reply;
  reply.outcome = decision.outcome();
  reply.reasons = decision.reasons();
  return reply;
}

Decision decision_from_reply(const Reply& reply) {
  Decision decision(reply.outcome);
  for (const Reason& reason : reply.reasons) decision.add(reason);
  decision.normalize();
  return decision;
}

Frame make_frame(MessageType type, const std::vector<std::uint8_t>& payload) {
  Frame frame;
  frame.type = type;
  frame.payload = payload;
  return frame;
}

// ---------------------------------------------------------------------------
// Message codecs
// ---------------------------------------------------------------------------

namespace {

void put_binding(PayloadWriter& writer, const ArtifactBinding& binding) {
  writer.id(binding.set_id);
  writer.id(binding.generation);
  writer.str(binding.digest);
}

bool get_binding(PayloadReader& reader, const ProtocolLimits& limits, ArtifactBinding& binding) {
  binding.set_id = reader.id<ArtifactSetIdTag>();
  binding.generation = reader.id<ArtifactGenerationTag>();
  binding.digest = reader.str(limits.max_digest);
  return reader.ok();
}

void put_requirements(PayloadWriter& writer, const ModelRequirements& requirements) {
  writer.str(requirements.runtime);
  writer.str(requirements.runtime_min_version);
  writer.str(requirements.backend);
  writer.str(requirements.architecture);
  writer.str(requirements.precision);
  writer.str(requirements.tokenizer_generation);
  writer.str(requirements.adapter_set);
}

bool get_requirements(PayloadReader& reader, const ProtocolLimits& limits,
                      ModelRequirements& requirements) {
  requirements.runtime = reader.str(limits.max_string);
  requirements.runtime_min_version = reader.str(limits.max_string);
  requirements.backend = reader.str(limits.max_string);
  requirements.architecture = reader.str(limits.max_string);
  requirements.precision = reader.str(limits.max_string);
  requirements.tokenizer_generation = reader.str(limits.max_string);
  requirements.adapter_set = reader.str(limits.max_string);
  return reader.ok();
}

void put_environment(PayloadWriter& writer, const EnvironmentProfile& environment) {
  writer.str(environment.key);
  writer.str(environment.runtime);
  writer.str(environment.runtime_version);
  writer.str(environment.backend);
  writer.str(environment.backend_version);
  writer.str(environment.architecture);
  writer.u32(static_cast<std::uint32_t>(environment.precisions.size()));
  for (const std::string& precision : environment.precisions) writer.str(precision);
  writer.str(environment.tokenizer_generation);
  writer.str(environment.adapter_set);
  writer.u32(environment.compute_capability);
  writer.u8(static_cast<std::uint8_t>(environment.provenance));
}

bool get_environment(PayloadReader& reader, const ProtocolLimits& limits,
                     EnvironmentProfile& environment) {
  environment.key = reader.str(limits.max_string);
  environment.runtime = reader.str(limits.max_string);
  environment.runtime_version = reader.str(limits.max_string);
  environment.backend = reader.str(limits.max_string);
  environment.backend_version = reader.str(limits.max_string);
  environment.architecture = reader.str(limits.max_string);
  const std::uint32_t precision_count = reader.u32();
  if (!reader.ok() || precision_count > 64) {
    reader.fail();
    return false;
  }
  for (std::uint32_t i = 0; i < precision_count; ++i) {
    environment.precisions.insert(reader.str(limits.max_string));
  }
  environment.tokenizer_generation = reader.str(limits.max_string);
  environment.adapter_set = reader.str(limits.max_string);
  environment.compute_capability = reader.u32();
  const std::uint8_t provenance = reader.u8();
  if (!reader.ok() || provenance > 3) {
    reader.fail();
    return false;
  }
  environment.provenance = static_cast<Provenance>(provenance);
  return reader.ok();
}

bool valid_provenance(std::uint8_t raw) noexcept { return raw <= 3; }

template <class T>
ProtocolStatus decode_or(const PayloadReader& reader, const T& value, T& out) {
  if (!reader.ok() || !reader.exhausted()) return ProtocolStatus::DecodeFailure;
  out = value;
  return ProtocolStatus::Ok;
}

}  // namespace

ProtocolStatus encode(const HelloMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.u16(message.protocol_version);
  writer.id(message.worker);
  writer.id(message.boot);
  writer.id(message.epoch);
  writer.u32(static_cast<std::uint32_t>(message.scopes.size()));
  for (ScopeId scope : message.scopes) writer.id(scope);
  writer.str(message.detail);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      HelloMessage& out) {
  PayloadReader reader(data, size);
  HelloMessage value;
  value.protocol_version = reader.u16();
  value.worker = reader.id<WorkerIdTag>();
  value.boot = reader.id<WorkerBootIdTag>();
  value.epoch = reader.id<CoordinatorEpochTag>();
  const std::uint32_t scope_count = reader.u32();
  if (!reader.ok() || scope_count > limits.max_connections * 4) return ProtocolStatus::DecodeFailure;
  for (std::uint32_t i = 0; i < scope_count; ++i) {
    value.scopes.push_back(reader.id<ScopeIdTag>());
  }
  value.detail = reader.str(limits.max_string);
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const HelloAckMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.u16(message.protocol_version);
  writer.id(message.epoch);
  writer.id(message.worker);
  writer.id(message.boot);
  writer.boolean(message.accepted);
  writer.u32(static_cast<std::uint32_t>(message.scopes.size()));
  for (ScopeId scope : message.scopes) writer.id(scope);
  writer.str(message.detail);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      HelloAckMessage& out) {
  PayloadReader reader(data, size);
  HelloAckMessage value;
  value.protocol_version = reader.u16();
  value.epoch = reader.id<CoordinatorEpochTag>();
  value.worker = reader.id<WorkerIdTag>();
  value.boot = reader.id<WorkerBootIdTag>();
  value.accepted = reader.boolean();
  const std::uint32_t scope_count = reader.u32();
  if (!reader.ok() || scope_count > limits.max_connections * 4) return ProtocolStatus::DecodeFailure;
  for (std::uint32_t i = 0; i < scope_count; ++i) {
    value.scopes.push_back(reader.id<ScopeIdTag>());
  }
  value.detail = reader.str(limits.max_string);
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const RegisterModelMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.str(message.name);
  writer.str(message.family);
  writer.u8(static_cast<std::uint8_t>(message.provenance));
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      RegisterModelMessage& out) {
  PayloadReader reader(data, size);
  RegisterModelMessage value;
  value.name = reader.str(limits.max_string);
  value.family = reader.str(limits.max_string);
  const std::uint8_t provenance = reader.u8();
  if (!valid_provenance(provenance)) return ProtocolStatus::DecodeFailure;
  value.provenance = static_cast<Provenance>(provenance);
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const RegisterVersionMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.id(message.model);
  writer.str(message.label);
  put_binding(writer, message.artifact);
  put_requirements(writer, message.requirements);
  writer.id(message.predecessor);
  writer.u8(static_cast<std::uint8_t>(message.provenance));
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      RegisterVersionMessage& out) {
  PayloadReader reader(data, size);
  RegisterVersionMessage value;
  value.model = reader.id<ModelIdTag>();
  value.label = reader.str(limits.max_string);
  if (!get_binding(reader, limits, value.artifact)) return ProtocolStatus::DecodeFailure;
  if (!get_requirements(reader, limits, value.requirements)) return ProtocolStatus::DecodeFailure;
  value.predecessor = reader.id<ModelVersionIdTag>();
  const std::uint8_t provenance = reader.u8();
  if (!valid_provenance(provenance)) return ProtocolStatus::DecodeFailure;
  value.provenance = static_cast<Provenance>(provenance);
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const PublishCompatibilityMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.id(message.version);
  put_environment(writer, message.environment);
  writer.u8(static_cast<std::uint8_t>(message.outcome));
  writer.str(message.detail);
  writer.u8(static_cast<std::uint8_t>(message.provenance));
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      PublishCompatibilityMessage& out) {
  PayloadReader reader(data, size);
  PublishCompatibilityMessage value;
  value.version = reader.id<ModelVersionIdTag>();
  if (!get_environment(reader, limits, value.environment)) return ProtocolStatus::DecodeFailure;
  const std::uint8_t outcome = reader.u8();
  if (outcome >= kCompatibilityOutcomeCount) return ProtocolStatus::DecodeFailure;
  value.outcome = static_cast<CompatibilityOutcome>(outcome);
  value.detail = reader.str(limits.max_string);
  const std::uint8_t provenance = reader.u8();
  if (!valid_provenance(provenance)) return ProtocolStatus::DecodeFailure;
  value.provenance = static_cast<Provenance>(provenance);
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const PublishEvidenceMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.id(message.version);
  writer.id(message.model_generation);
  writer.id(message.artifact_generation);
  writer.id(message.scope);
  writer.id(message.rollout);
  writer.id(message.stage);
  writer.u8(static_cast<std::uint8_t>(message.kind));
  writer.u8(static_cast<std::uint8_t>(message.verdict));
  writer.f64(message.value);
  writer.str(message.unit);
  writer.u64(message.tick);
  writer.str(message.detail);
  writer.u8(static_cast<std::uint8_t>(message.provenance));
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      PublishEvidenceMessage& out) {
  PayloadReader reader(data, size);
  PublishEvidenceMessage value;
  value.version = reader.id<ModelVersionIdTag>();
  value.model_generation = reader.id<ModelGenerationTag>();
  value.artifact_generation = reader.id<ArtifactGenerationTag>();
  value.scope = reader.id<ScopeIdTag>();
  value.rollout = reader.id<RolloutIdTag>();
  value.stage = reader.id<RolloutStageIdTag>();
  const std::uint8_t kind = reader.u8();
  if (kind >= kEvidenceKindCount) return ProtocolStatus::DecodeFailure;
  value.kind = static_cast<EvidenceKind>(kind);
  const std::uint8_t verdict = reader.u8();
  if (verdict > 2) return ProtocolStatus::DecodeFailure;
  value.verdict = static_cast<EvidenceVerdict>(verdict);
  value.value = reader.f64();
  value.unit = reader.str(32);
  value.tick = reader.u64();
  value.detail = reader.str(limits.max_string);
  const std::uint8_t provenance = reader.u8();
  if (!valid_provenance(provenance)) return ProtocolStatus::DecodeFailure;
  value.provenance = static_cast<Provenance>(provenance);
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const AttemptRequestMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.u8(static_cast<std::uint8_t>(message.kind));
  writer.id(message.worker);
  writer.id(message.boot);
  writer.id(message.model);
  writer.id(message.version);
  writer.id(message.model_generation);
  writer.id(message.scope);
  writer.id(message.rollout);
  writer.id(message.rollout_generation);
  writer.id(message.stage);
  writer.id(message.stage_generation);
  writer.boolean(message.idempotent);
  writer.str(message.detail);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      AttemptRequestMessage& out) {
  PayloadReader reader(data, size);
  AttemptRequestMessage value;
  const std::uint8_t kind = reader.u8();
  if (kind >= kAttemptKindCount) return ProtocolStatus::DecodeFailure;
  value.kind = static_cast<AttemptKind>(kind);
  value.worker = reader.id<WorkerIdTag>();
  value.boot = reader.id<WorkerBootIdTag>();
  value.model = reader.id<ModelIdTag>();
  value.version = reader.id<ModelVersionIdTag>();
  value.model_generation = reader.id<ModelGenerationTag>();
  value.scope = reader.id<ScopeIdTag>();
  value.rollout = reader.id<RolloutIdTag>();
  value.rollout_generation = reader.id<RolloutGenerationTag>();
  value.stage = reader.id<RolloutStageIdTag>();
  value.stage_generation = reader.id<RolloutStageGenerationTag>();
  value.idempotent = reader.boolean();
  value.detail = reader.str(limits.max_string);
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const CompletionMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.id(message.attempt);
  writer.boolean(message.success);
  writer.boolean(message.confirmed);
  writer.str(message.detail);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      CompletionMessage& out) {
  PayloadReader reader(data, size);
  CompletionMessage value;
  value.attempt = reader.id<AttemptIdTag>();
  value.success = reader.boolean();
  value.confirmed = reader.boolean();
  value.detail = reader.str(limits.max_string);
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const FenceMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.id(message.worker);
  writer.id(message.boot);
  writer.str(message.reason);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      FenceMessage& out) {
  PayloadReader reader(data, size);
  FenceMessage value;
  value.worker = reader.id<WorkerIdTag>();
  value.boot = reader.id<WorkerBootIdTag>();
  value.reason = reader.str(limits.max_string);
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const CommandMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.u8(static_cast<std::uint8_t>(message.kind));
  writer.id(message.attempt);
  writer.id(message.model);
  writer.id(message.version);
  writer.id(message.model_generation);
  writer.id(message.artifact_generation);
  writer.id(message.scope);
  writer.id(message.rollout);
  writer.id(message.rollout_generation);
  writer.str(message.detail);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      CommandMessage& out) {
  PayloadReader reader(data, size);
  CommandMessage value;
  const std::uint8_t kind = reader.u8();
  if (kind >= kAttemptKindCount) return ProtocolStatus::DecodeFailure;
  value.kind = static_cast<AttemptKind>(kind);
  value.attempt = reader.id<AttemptIdTag>();
  value.model = reader.id<ModelIdTag>();
  value.version = reader.id<ModelVersionIdTag>();
  value.model_generation = reader.id<ModelGenerationTag>();
  value.artifact_generation = reader.id<ArtifactGenerationTag>();
  value.scope = reader.id<ScopeIdTag>();
  value.rollout = reader.id<RolloutIdTag>();
  value.rollout_generation = reader.id<RolloutGenerationTag>();
  value.detail = reader.str(limits.max_string);
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const QueryStateMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.id(message.model);
  writer.id(message.version);
  writer.id(message.scope);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      QueryStateMessage& out) {
  static_cast<void>(limits);
  PayloadReader reader(data, size);
  QueryStateMessage value;
  value.model = reader.id<ModelIdTag>();
  value.version = reader.id<ModelVersionIdTag>();
  value.scope = reader.id<ScopeIdTag>();
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const QueryAuthorityMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.id(message.scope);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      QueryAuthorityMessage& out) {
  static_cast<void>(limits);
  PayloadReader reader(data, size);
  QueryAuthorityMessage value;
  value.scope = reader.id<ScopeIdTag>();
  return decode_or(reader, value, out);
}

namespace {

void put_evidence_requirement(PayloadWriter& writer, const EvidenceRequirement& requirement) {
  writer.u8(static_cast<std::uint8_t>(requirement.kind));
  writer.u8(static_cast<std::uint8_t>(requirement.required_verdict));
  writer.boolean(requirement.require_exact_subject);
  writer.boolean(requirement.require_current_publisher);
}

bool get_evidence_requirement(PayloadReader& reader, EvidenceRequirement& requirement) {
  const std::uint8_t kind = reader.u8();
  const std::uint8_t verdict = reader.u8();
  if (!reader.ok() || kind >= kEvidenceKindCount || verdict > 2) return false;
  requirement.kind = static_cast<EvidenceKind>(kind);
  requirement.required_verdict = static_cast<EvidenceVerdict>(verdict);
  requirement.require_exact_subject = reader.boolean();
  requirement.require_current_publisher = reader.boolean();
  return reader.ok();
}

void put_criterion(PayloadWriter& writer, const Criterion& criterion) {
  writer.u8(static_cast<std::uint8_t>(criterion.kind));
  writer.u8(static_cast<std::uint8_t>(criterion.comparison));
  writer.f64(criterion.threshold);
}

bool get_criterion(PayloadReader& reader, Criterion& criterion) {
  const std::uint8_t kind = reader.u8();
  const std::uint8_t comparison = reader.u8();
  if (!reader.ok() || kind >= kEvidenceKindCount || comparison > 1) return false;
  criterion.kind = static_cast<EvidenceKind>(kind);
  criterion.comparison = static_cast<CriterionComparison>(comparison);
  criterion.threshold = reader.f64();
  return reader.ok();
}

void put_promotion(PayloadWriter& writer, const PromotionRequest& request) {
  writer.id(request.model);
  writer.id(request.candidate);
  writer.id(request.candidate_generation);
  writer.id(request.artifact_generation);
  writer.id(request.compatibility_generation);
  writer.id(request.policy_generation);
  writer.id(request.epoch);
  writer.id(request.scope);
  writer.id(request.scope_generation);
  writer.u8(static_cast<std::uint8_t>(request.strategy));
  writer.id(request.rollback_target);
  writer.id(request.rollback_target_generation);
  writer.id(request.evidence_generation);
  writer.id(request.worker);
  writer.id(request.boot);
  writer.boolean(request.administrative_approval);
  writer.boolean(request.request_immediate_cutover);
  writer.str(request.environment_key);
  writer.boolean(request.environment.has_value());
  if (request.environment.has_value()) put_environment(writer, *request.environment);
}

bool get_promotion(PayloadReader& reader, const ProtocolLimits& limits, PromotionRequest& request) {
  request.model = reader.id<ModelIdTag>();
  request.candidate = reader.id<ModelVersionIdTag>();
  request.candidate_generation = reader.id<ModelGenerationTag>();
  request.artifact_generation = reader.id<ArtifactGenerationTag>();
  request.compatibility_generation = reader.id<CompatibilityGenerationTag>();
  request.policy_generation = reader.id<PolicyGenerationTag>();
  request.epoch = reader.id<CoordinatorEpochTag>();
  request.scope = reader.id<ScopeIdTag>();
  request.scope_generation = reader.id<ScopeGenerationTag>();
  const std::uint8_t strategy = reader.u8();
  if (!reader.ok() || strategy >= kRolloutStrategyCount) return false;
  request.strategy = static_cast<RolloutStrategy>(strategy);
  request.rollback_target = reader.id<ModelVersionIdTag>();
  request.rollback_target_generation = reader.id<ModelGenerationTag>();
  request.evidence_generation = reader.id<EvidenceGenerationTag>();
  request.worker = reader.id<WorkerIdTag>();
  request.boot = reader.id<WorkerBootIdTag>();
  request.administrative_approval = reader.boolean();
  request.request_immediate_cutover = reader.boolean();
  request.environment_key = reader.str(limits.max_string);
  const bool has_environment = reader.boolean();
  if (!reader.ok()) return false;
  if (has_environment) {
    EnvironmentProfile environment;
    if (!get_environment(reader, limits, environment)) return false;
    request.environment = std::move(environment);
  }
  return reader.ok();
}

void put_progress(PayloadWriter& writer, const RolloutProgressRequest& request) {
  writer.id(request.rollout);
  writer.id(request.rollout_generation);
  writer.id(request.stage);
  writer.id(request.stage_generation);
  writer.id(request.scope_generation);
  writer.id(request.epoch);
  writer.id(request.worker);
  writer.id(request.boot);
  writer.boolean(request.administrative_approval);
  writer.str(request.detail);
}

bool get_progress(PayloadReader& reader, const ProtocolLimits& limits,
                  RolloutProgressRequest& request) {
  request.rollout = reader.id<RolloutIdTag>();
  request.rollout_generation = reader.id<RolloutGenerationTag>();
  request.stage = reader.id<RolloutStageIdTag>();
  request.stage_generation = reader.id<RolloutStageGenerationTag>();
  request.scope_generation = reader.id<ScopeGenerationTag>();
  request.epoch = reader.id<CoordinatorEpochTag>();
  request.worker = reader.id<WorkerIdTag>();
  request.boot = reader.id<WorkerBootIdTag>();
  request.administrative_approval = reader.boolean();
  request.detail = reader.str(limits.max_string);
  return reader.ok();
}

}  // namespace

ProtocolStatus encode(const PromotionMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  put_promotion(writer, message.request);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      PromotionMessage& out) {
  PayloadReader reader(data, size);
  PromotionMessage value;
  if (!get_promotion(reader, limits, value.request)) return ProtocolStatus::DecodeFailure;
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const RolloutPlanMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  const RolloutPlanRequest& request = message.request;
  writer.id(request.model);
  writer.id(request.candidate);
  writer.id(request.candidate_generation);
  writer.id(request.artifact_generation);
  writer.id(request.compatibility_generation);
  writer.id(request.policy_generation);
  writer.id(request.root_scope);
  writer.id(request.root_scope_generation);
  writer.u8(static_cast<std::uint8_t>(request.strategy));
  writer.id(request.previous);
  writer.id(request.previous_generation);
  writer.id(request.rollback_target);
  writer.id(request.rollback_target_generation);
  writer.u32(request.max_blast_radius_percent);
  writer.boolean(request.manual_progression);
  writer.id(request.epoch);
  writer.id(request.worker);
  writer.id(request.boot);
  writer.id(request.promotion);
  writer.id(request.promotion_generation);
  writer.u32(static_cast<std::uint32_t>(request.cohorts.size()));
  for (const CohortSpec& cohort : request.cohorts) {
    writer.str(cohort.name);
    writer.u32(static_cast<std::uint32_t>(cohort.scopes.size()));
    for (ScopeId scope : cohort.scopes) writer.id(scope);
    writer.u32(cohort.traffic_percent);
    writer.u32(cohort.max_blast_radius_percent);
  }
  writer.u32(static_cast<std::uint32_t>(request.stages.size()));
  for (const StageSpec& stage : request.stages) {
    writer.str(stage.name);
    writer.u32(static_cast<std::uint32_t>(stage.cohort_indices.size()));
    for (std::size_t index : stage.cohort_indices) writer.u32(static_cast<std::uint32_t>(index));
    writer.u32(static_cast<std::uint32_t>(stage.required_evidence.size()));
    for (const EvidenceRequirement& requirement : stage.required_evidence) {
      put_evidence_requirement(writer, requirement);
    }
    writer.u32(static_cast<std::uint32_t>(stage.acceptance.size()));
    for (const Criterion& criterion : stage.acceptance) put_criterion(writer, criterion);
    writer.u32(static_cast<std::uint32_t>(stage.failure.size()));
    for (const Criterion& criterion : stage.failure) put_criterion(writer, criterion);
    writer.boolean(stage.require_residency_before_entry);
    writer.boolean(stage.require_drain_after_commit);
  }
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      RolloutPlanMessage& out) {
  PayloadReader reader(data, size);
  RolloutPlanMessage value;
  RolloutPlanRequest& request = value.request;
  request.model = reader.id<ModelIdTag>();
  request.candidate = reader.id<ModelVersionIdTag>();
  request.candidate_generation = reader.id<ModelGenerationTag>();
  request.artifact_generation = reader.id<ArtifactGenerationTag>();
  request.compatibility_generation = reader.id<CompatibilityGenerationTag>();
  request.policy_generation = reader.id<PolicyGenerationTag>();
  request.root_scope = reader.id<ScopeIdTag>();
  request.root_scope_generation = reader.id<ScopeGenerationTag>();
  const std::uint8_t strategy = reader.u8();
  if (!reader.ok() || strategy >= kRolloutStrategyCount) return ProtocolStatus::DecodeFailure;
  request.strategy = static_cast<RolloutStrategy>(strategy);
  request.previous = reader.id<ModelVersionIdTag>();
  request.previous_generation = reader.id<ModelGenerationTag>();
  request.rollback_target = reader.id<ModelVersionIdTag>();
  request.rollback_target_generation = reader.id<ModelGenerationTag>();
  request.max_blast_radius_percent = reader.u32();
  request.manual_progression = reader.boolean();
  request.epoch = reader.id<CoordinatorEpochTag>();
  request.worker = reader.id<WorkerIdTag>();
  request.boot = reader.id<WorkerBootIdTag>();
  request.promotion = reader.id<PromotionIdTag>();
  request.promotion_generation = reader.id<PromotionGenerationTag>();
  const std::uint32_t cohort_count = reader.u32();
  if (!reader.ok() || cohort_count > limits.max_values * 8) return ProtocolStatus::DecodeFailure;
  for (std::uint32_t i = 0; i < cohort_count; ++i) {
    CohortSpec cohort;
    cohort.name = reader.str(limits.max_string);
    const std::uint32_t scope_count = reader.u32();
    if (!reader.ok() || scope_count > limits.max_values * 16) return ProtocolStatus::DecodeFailure;
    for (std::uint32_t s = 0; s < scope_count; ++s) {
      cohort.scopes.push_back(reader.id<ScopeIdTag>());
    }
    cohort.traffic_percent = reader.u32();
    cohort.max_blast_radius_percent = reader.u32();
    if (!reader.ok() || cohort.traffic_percent > 100 || cohort.max_blast_radius_percent > 100) {
      return ProtocolStatus::DecodeFailure;
    }
    request.cohorts.push_back(std::move(cohort));
  }
  const std::uint32_t stage_count = reader.u32();
  if (!reader.ok() || stage_count > limits.max_values) return ProtocolStatus::DecodeFailure;
  for (std::uint32_t i = 0; i < stage_count; ++i) {
    StageSpec stage;
    stage.name = reader.str(limits.max_string);
    const std::uint32_t index_count = reader.u32();
    if (!reader.ok() || index_count > limits.max_values * 8) return ProtocolStatus::DecodeFailure;
    for (std::uint32_t k = 0; k < index_count; ++k) {
      stage.cohort_indices.push_back(reader.u32());
    }
    const std::uint32_t requirement_count = reader.u32();
    if (!reader.ok() || requirement_count > 64) return ProtocolStatus::DecodeFailure;
    for (std::uint32_t k = 0; k < requirement_count; ++k) {
      EvidenceRequirement requirement;
      if (!get_evidence_requirement(reader, requirement)) return ProtocolStatus::DecodeFailure;
      stage.required_evidence.push_back(requirement);
    }
    const std::uint32_t acceptance = reader.u32();
    if (!reader.ok() || acceptance > 64) return ProtocolStatus::DecodeFailure;
    for (std::uint32_t k = 0; k < acceptance; ++k) {
      Criterion criterion;
      if (!get_criterion(reader, criterion)) return ProtocolStatus::DecodeFailure;
      stage.acceptance.push_back(criterion);
    }
    const std::uint32_t failure = reader.u32();
    if (!reader.ok() || failure > 64) return ProtocolStatus::DecodeFailure;
    for (std::uint32_t k = 0; k < failure; ++k) {
      Criterion criterion;
      if (!get_criterion(reader, criterion)) return ProtocolStatus::DecodeFailure;
      stage.failure.push_back(criterion);
    }
    stage.require_residency_before_entry = reader.boolean();
    stage.require_drain_after_commit = reader.boolean();
    request.stages.push_back(std::move(stage));
  }
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const ProgressMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  put_progress(writer, message.request);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      ProgressMessage& out) {
  PayloadReader reader(data, size);
  ProgressMessage value;
  if (!get_progress(reader, limits, value.request)) return ProtocolStatus::DecodeFailure;
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const RetireMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.id(message.request.model);
  writer.id(message.request.version);
  writer.id(message.request.generation);
  writer.id(message.request.policy_generation);
  writer.id(message.request.epoch);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      RetireMessage& out) {
  static_cast<void>(limits);
  PayloadReader reader(data, size);
  RetireMessage value;
  value.request.model = reader.id<ModelIdTag>();
  value.request.version = reader.id<ModelVersionIdTag>();
  value.request.generation = reader.id<ModelGenerationTag>();
  value.request.policy_generation = reader.id<PolicyGenerationTag>();
  value.request.epoch = reader.id<CoordinatorEpochTag>();
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const RevalidateMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.id(message.request.model);
  writer.id(message.request.version);
  writer.id(message.request.generation);
  writer.u16(static_cast<std::uint16_t>(message.request.cause));
  writer.str(message.request.detail);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      RevalidateMessage& out) {
  PayloadReader reader(data, size);
  RevalidateMessage value;
  value.request.model = reader.id<ModelIdTag>();
  value.request.version = reader.id<ModelVersionIdTag>();
  value.request.generation = reader.id<ModelGenerationTag>();
  value.request.cause = static_cast<ReasonCode>(reader.u16());
  value.request.detail = reader.str(limits.max_string);
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const RollbackMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  const RollbackRequest& request = message.request;
  writer.id(request.model);
  writer.id(request.candidate);
  writer.id(request.candidate_generation);
  writer.id(request.rollout);
  writer.id(request.rollout_generation);
  writer.id(request.target);
  writer.id(request.target_generation);
  writer.id(request.target_artifact_generation);
  writer.id(request.scope);
  writer.id(request.scope_generation);
  writer.id(request.policy_generation);
  writer.id(request.epoch);
  writer.id(request.worker);
  writer.id(request.boot);
  writer.boolean(request.automatic);
  writer.str(request.detail);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      RollbackMessage& out) {
  PayloadReader reader(data, size);
  RollbackMessage value;
  RollbackRequest& request = value.request;
  request.model = reader.id<ModelIdTag>();
  request.candidate = reader.id<ModelVersionIdTag>();
  request.candidate_generation = reader.id<ModelGenerationTag>();
  request.rollout = reader.id<RolloutIdTag>();
  request.rollout_generation = reader.id<RolloutGenerationTag>();
  request.target = reader.id<ModelVersionIdTag>();
  request.target_generation = reader.id<ModelGenerationTag>();
  request.target_artifact_generation = reader.id<ArtifactGenerationTag>();
  request.scope = reader.id<ScopeIdTag>();
  request.scope_generation = reader.id<ScopeGenerationTag>();
  request.policy_generation = reader.id<PolicyGenerationTag>();
  request.epoch = reader.id<CoordinatorEpochTag>();
  request.worker = reader.id<WorkerIdTag>();
  request.boot = reader.id<WorkerBootIdTag>();
  request.automatic = reader.boolean();
  request.detail = reader.str(limits.max_string);
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const SupersedeMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.id(message.rollout);
  writer.id(message.generation);
  writer.str(message.detail);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      SupersedeMessage& out) {
  PayloadReader reader(data, size);
  SupersedeMessage value;
  value.rollout = reader.id<RolloutIdTag>();
  value.generation = reader.id<RolloutGenerationTag>();
  value.detail = reader.str(limits.max_string);
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const ReconcileMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.u32(message.flags);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      ReconcileMessage& out) {
  static_cast<void>(limits);
  PayloadReader reader(data, size);
  ReconcileMessage value;
  value.flags = reader.u32();
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const RegisterScopeMessage& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  out.clear();
  PayloadWriter writer(out);
  writer.str(message.path);
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      RegisterScopeMessage& out) {
  PayloadReader reader(data, size);
  RegisterScopeMessage value;
  value.path = reader.str(limits.max_string * 8);
  return decode_or(reader, value, out);
}

ProtocolStatus encode(const Reply& message, const ProtocolLimits& limits,
                      std::vector<std::uint8_t>& out) {
  if (message.reasons.size() > limits.max_reasons || message.values.size() > limits.max_values) {
    return ProtocolStatus::Oversized;
  }
  out.clear();
  PayloadWriter writer(out);
  writer.u16(static_cast<std::uint16_t>(message.outcome));
  writer.u32(static_cast<std::uint32_t>(message.reasons.size()));
  for (const Reason& reason : message.reasons) {
    writer.u16(static_cast<std::uint16_t>(reason.code));
    writer.str(reason.detail);
  }
  writer.u32(static_cast<std::uint32_t>(message.values.size()));
  for (const auto& entry : message.values) {
    writer.str(entry.first);
    writer.str(entry.second);
  }
  out = writer.bytes();
  if (out.size() > limits.max_payload_bytes) return ProtocolStatus::Oversized;
  return ProtocolStatus::Ok;
}

ProtocolStatus decode(const std::uint8_t* data, std::size_t size, const ProtocolLimits& limits,
                      Reply& out) {
  PayloadReader reader(data, size);
  Reply value;
  const std::uint16_t outcome = reader.u16();
  if (outcome > static_cast<std::uint16_t>(OutcomeCode::UNSUPPORTED)) {
    return ProtocolStatus::DecodeFailure;
  }
  value.outcome = static_cast<OutcomeCode>(outcome);
  const std::uint32_t reason_count = reader.u32();
  if (!reader.ok() || reason_count > limits.max_reasons) return ProtocolStatus::DecodeFailure;
  for (std::uint32_t i = 0; i < reason_count; ++i) {
    Reason reason;
    reason.code = static_cast<ReasonCode>(reader.u16());
    reason.detail = reader.str(limits.max_string);
    value.reasons.push_back(std::move(reason));
  }
  const std::uint32_t value_count = reader.u32();
  if (!reader.ok() || value_count > limits.max_values) return ProtocolStatus::DecodeFailure;
  for (std::uint32_t i = 0; i < value_count; ++i) {
    std::string key = reader.str(limits.max_string);
    std::string entry = reader.str(limits.max_string);
    value.values.emplace_back(std::move(key), std::move(entry));
  }
  return decode_or(reader, value, out);
}

}  // namespace mlf
