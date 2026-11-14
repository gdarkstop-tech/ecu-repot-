// HTTP body framing as served by different hosts (Content-Length, chunked, until-close),
// fed in arbitrary pieces the way bytes arrive from a TLS socket.
#include <string.h>
#include <unity.h>

#include <string>

#include "http_body.h"

using namespace robot;

struct Out {
  std::string data;
  size_t cap = 1 << 20;
};

static size_t collect(const uint8_t* p, size_t n, void* ctx) {
  Out* o = static_cast<Out*>(ctx);
  if (o->data.size() + n > o->cap) return 0;
  o->data.append(reinterpret_cast<const char*>(p), n);
  return n;
}

// Feeds `wire` in pieces of `step` bytes; returns the decoder state after the last piece.
static bool feedAll(HttpBodyDecoder& d, const std::string& wire, size_t step, Out& out) {
  for (size_t i = 0; i < wire.size(); i += step) {
    size_t n = wire.size() - i < step ? wire.size() - i : step;
    if (!d.feed(reinterpret_cast<const uint8_t*>(wire.data() + i), n, collect, &out)) return false;
  }
  return true;
}

void setUp() {}
void tearDown() {}

void test_content_length_any_split() {
  for (size_t step : {1u, 3u, 7u, 1000u}) {
    HttpBodyDecoder d;
    Out o;
    d.beginLength(11);
    TEST_ASSERT_TRUE(feedAll(d, "hello world", step, o));
    TEST_ASSERT_TRUE(d.complete());
    TEST_ASSERT_EQUAL_STRING("hello world", o.data.c_str());
  }
  HttpBodyDecoder d;
  Out o;
  d.beginLength(20);
  TEST_ASSERT_TRUE(feedAll(d, "short", 5, o));
  TEST_ASSERT_FALSE(d.finishOnClose());  // connection closed early: incomplete
}

void test_chunked_any_split() {
  const std::string wire = "5\r\nhello\r\n6;name=value\r\n world\r\nA\r\n0123456789\r\n0\r\n\r\n";
  for (size_t step : {1u, 2u, 5u, 1000u}) {
    HttpBodyDecoder d;
    Out o;
    d.beginChunked();
    TEST_ASSERT_TRUE(feedAll(d, wire, step, o));
    TEST_ASSERT_TRUE(d.complete());
    TEST_ASSERT_EQUAL_STRING("hello world0123456789", o.data.c_str());
    TEST_ASSERT_EQUAL_UINT32(21, d.delivered());
  }
}

void test_chunked_with_trailer_and_bare_lf() {
  HttpBodyDecoder d;
  Out o;
  d.beginChunked();
  TEST_ASSERT_TRUE(feedAll(d, "3\nabc\n0\r\nX-Trailer: 1\r\n\r\n", 1, o));
  TEST_ASSERT_TRUE(d.complete());
  TEST_ASSERT_EQUAL_STRING("abc", o.data.c_str());
}

void test_chunked_malformed_is_rejected() {
  const char* bad[] = {"zz\r\nabc\r\n0\r\n\r\n", "3\r\nabcX\r\n0\r\n\r\n", "\r\n", "FFFFFFFF\r\n"};
  for (const char* w : bad) {
    HttpBodyDecoder d;
    Out o;
    d.beginChunked();
    TEST_ASSERT_FALSE_MESSAGE(feedAll(d, w, 1, o), w);
    TEST_ASSERT_TRUE(d.failed());
  }
}

void test_sink_can_refuse_oversized_bodies() {
  HttpBodyDecoder d;
  Out o;
  o.cap = 8;
  d.beginChunked();
  TEST_ASSERT_FALSE(feedAll(d, "10\r\n0123456789abcdef\r\n0\r\n\r\n", 4, o));
  TEST_ASSERT_TRUE(d.failed());
}

void test_until_close_and_empty_length() {
  HttpBodyDecoder d;
  Out o;
  d.beginUntilClose();
  TEST_ASSERT_TRUE(feedAll(d, "anything", 3, o));
  TEST_ASSERT_FALSE(d.complete());
  TEST_ASSERT_TRUE(d.finishOnClose());
  TEST_ASSERT_EQUAL_STRING("anything", o.data.c_str());
  HttpBodyDecoder e;
  e.beginLength(0);
  TEST_ASSERT_TRUE(e.complete());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_content_length_any_split);
  RUN_TEST(test_chunked_any_split);
  RUN_TEST(test_chunked_with_trailer_and_bare_lf);
  RUN_TEST(test_chunked_malformed_is_rejected);
  RUN_TEST(test_sink_can_refuse_oversized_bodies);
  RUN_TEST(test_until_close_and_empty_length);
  return UNITY_END();
}
