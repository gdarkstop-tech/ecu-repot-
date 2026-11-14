// Incremental HTTP/1.1 response-body decoder: Content-Length, chunked, or read-until-close.
//
// Portable (host-tested) so the robot reads gateway replies the same way whatever framing the
// hosting platform uses (serverless platforms such as Vercel may answer with chunked transfer
// encoding). The ESP32 side feeds it bytes from the socket with its own stall timeout and cancel
// checks; the decoder hands payload bytes to a sink, which may refuse them (size cap, cancel).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace robot {

class HttpBodyDecoder {
 public:
  // Returns how many bytes it accepted; fewer than offered aborts decoding.
  using Sink = size_t (*)(const uint8_t* data, size_t len, void* ctx);

  void beginLength(uint32_t contentLength);
  void beginChunked();
  void beginUntilClose();

  // Feeds raw bytes from the socket. Returns false on malformed framing or a refusing sink.
  bool feed(const uint8_t* p, size_t n, Sink sink, void* ctx);
  // Call when the connection closed: true if the body is complete (always, for until-close).
  bool finishOnClose();

  bool complete() const { return st_ == St::Done; }
  bool failed() const { return st_ == St::Failed; }
  uint32_t delivered() const { return delivered_; }

 private:
  enum class St : uint8_t { Length, Size, SizeExt, SizeLF, Data, DataCR, DataLF, Trailer, UntilClose, Done, Failed };
  bool emit(const uint8_t* p, size_t n, Sink sink, void* ctx);
  St st_ = St::Failed;
  uint32_t remaining_ = 0;
  uint32_t chunk_ = 0;
  uint8_t digits_ = 0;
  uint16_t lineLen_ = 0;
  uint32_t delivered_ = 0;
};

}  // namespace robot
