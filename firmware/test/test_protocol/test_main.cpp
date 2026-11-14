#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "debug_protocol.h"
#include "line_framer.h"
#include "utf8.h"

using namespace robot;

void setUp() {}
void tearDown() {}

static ParseError parse(const char* s, DebugCommand& c, const char** detail = nullptr) {
  return parseCommand(s, strlen(s), c, detail);
}

void test_valid_face_command() {
  DebugCommand c;
  TEST_ASSERT_EQUAL((int)ParseError::None, (int)parse("{\"v\":1,\"id\":7,\"cmd\":\"face\",\"expr\":\"happy\",\"ms\":1500}", c));
  TEST_ASSERT_EQUAL((int)CmdType::Face, (int)c.type);
  TEST_ASSERT_EQUAL_UINT32(7, c.id);
  TEST_ASSERT_EQUAL((int)Expression::Happy, (int)c.expr);
  TEST_ASSERT_EQUAL_UINT32(1500, c.ms);
}

void test_bad_json_and_non_object() {
  DebugCommand c;
  TEST_ASSERT_EQUAL((int)ParseError::BadJson, (int)parse("{\"cmd\":", c));
  TEST_ASSERT_EQUAL((int)ParseError::BadJson, (int)parse("hello", c));
  TEST_ASSERT_EQUAL((int)ParseError::NotObject, (int)parse("[1,2]", c));
}

void test_missing_and_unknown_cmd() {
  DebugCommand c;
  const char* d = nullptr;
  TEST_ASSERT_EQUAL((int)ParseError::MissingCmd, (int)parse("{\"id\":1}", c));
  TEST_ASSERT_EQUAL((int)ParseError::MissingCmd, (int)parse("{\"cmd\":5}", c));
  TEST_ASSERT_EQUAL((int)ParseError::UnknownCmd, (int)parse("{\"cmd\":\"launch_rockets\"}", c, &d));
  TEST_ASSERT_EQUAL_STRING("cmd", d);
}

void test_version_check() {
  DebugCommand c;
  TEST_ASSERT_EQUAL((int)ParseError::UnsupportedVersion, (int)parse("{\"v\":2,\"cmd\":\"ping\"}", c));
  TEST_ASSERT_EQUAL((int)ParseError::None, (int)parse("{\"cmd\":\"ping\"}", c));  // v optional
}

void test_id_must_be_unsigned() {
  DebugCommand c;
  TEST_ASSERT_EQUAL((int)ParseError::BadParam, (int)parse("{\"id\":-1,\"cmd\":\"ping\"}", c));
  TEST_ASSERT_EQUAL((int)ParseError::BadParam, (int)parse("{\"id\":\"x\",\"cmd\":\"ping\"}", c));
}

void test_state_command_and_alias() {
  DebugCommand c;
  TEST_ASSERT_EQUAL((int)ParseError::None, (int)parse("{\"cmd\":\"state\",\"state\":\"thinking\"}", c));
  TEST_ASSERT_EQUAL((int)RobotState::Processing, (int)c.state);
  const char* d = nullptr;
  TEST_ASSERT_EQUAL((int)ParseError::BadParam, (int)parse("{\"cmd\":\"state\",\"state\":\"dancing\"}", c, &d));
  TEST_ASSERT_EQUAL_STRING("state", d);
}

void test_boot_expression_not_requestable() {
  DebugCommand c;
  TEST_ASSERT_EQUAL((int)ParseError::BadParam, (int)parse("{\"cmd\":\"face\",\"expr\":\"boot\"}", c));
}

void test_negative_or_string_ms_rejected_and_large_clamped() {
  DebugCommand c;
  TEST_ASSERT_EQUAL((int)ParseError::BadParam, (int)parse("{\"cmd\":\"face\",\"expr\":\"sad\",\"ms\":-5}", c));
  TEST_ASSERT_EQUAL((int)ParseError::BadParam, (int)parse("{\"cmd\":\"face\",\"expr\":\"sad\",\"ms\":\"5\"}", c));
  TEST_ASSERT_EQUAL((int)ParseError::None, (int)parse("{\"cmd\":\"face\",\"expr\":\"sad\",\"ms\":99999999}", c));
  TEST_ASSERT_EQUAL_UINT32(kMaxDurationMs, c.ms);
}

void test_show_requires_kind_and_value() {
  DebugCommand c;
  TEST_ASSERT_EQUAL((int)ParseError::None,
                    (int)parse("{\"cmd\":\"show\",\"kind\":\"letter\",\"value\":\"B\",\"caption\":\"Ball\"}", c));
  TEST_ASSERT_EQUAL((int)ContentKind::Letter, (int)c.kind);
  TEST_ASSERT_EQUAL_STRING("B", c.value);
  TEST_ASSERT_EQUAL_STRING("Ball", c.text);
  TEST_ASSERT_EQUAL((int)ParseError::BadParam, (int)parse("{\"cmd\":\"show\",\"kind\":\"letter\"}", c));
  TEST_ASSERT_EQUAL((int)ParseError::BadParam, (int)parse("{\"cmd\":\"show\",\"kind\":\"none\",\"value\":\"x\"}", c));
}

void test_text_truncated_at_utf8_boundary() {
  // 100 Arabic letters (2 bytes each) = 200 bytes > 160 limit.
  char line[400];
  size_t p = (size_t)snprintf(line, sizeof(line), "{\"cmd\":\"text\",\"text\":\"");
  for (int i = 0; i < 100; i++) { line[p++] = (char)0xD8; line[p++] = (char)0xA8; }  // U+0628
  p += (size_t)snprintf(line + p, sizeof(line) - p, "\"}");
  DebugCommand c;
  TEST_ASSERT_EQUAL((int)ParseError::None, (int)parseCommand(line, p, c, nullptr));
  TEST_ASSERT_TRUE(c.truncated);
  TEST_ASSERT_EQUAL(160, (int)strlen(c.text));
  TEST_ASSERT_TRUE(utf8IsValid(c.text));
}

void test_invalid_utf8_rejected() {
  const char line[] = "{\"cmd\":\"text\",\"text\":\"\xC3\x28\"}";
  DebugCommand c;
  TEST_ASSERT_EQUAL((int)ParseError::InvalidUtf8, (int)parseCommand(line, strlen(line), c, nullptr));
}

void test_config_value_never_silently_truncated() {
  char line[600];
  char big[300];
  memset(big, 'k', sizeof(big) - 1);
  big[sizeof(big) - 1] = 0;
  snprintf(line, sizeof(line), "{\"cmd\":\"config\",\"key\":\"device_token\",\"value\":\"%s\"}", big);
  DebugCommand c;
  const char* d = nullptr;
  TEST_ASSERT_EQUAL((int)ParseError::BadParam, (int)parseCommand(line, strlen(line), c, &d));
  TEST_ASSERT_EQUAL_STRING("value", d);
  TEST_ASSERT_EQUAL((int)ParseError::None,
                    (int)parse("{\"cmd\":\"config\",\"key\":\"wifi_pass\",\"value\":\"\"}", c));  // clearing allowed
}

void test_mute_requires_bool() {
  DebugCommand c;
  TEST_ASSERT_EQUAL((int)ParseError::None, (int)parse("{\"cmd\":\"mute\",\"on\":false}", c));
  TEST_ASSERT_FALSE(c.on);
  TEST_ASSERT_EQUAL((int)ParseError::BadParam, (int)parse("{\"cmd\":\"mute\",\"on\":\"yes\"}", c));
  TEST_ASSERT_EQUAL((int)ParseError::BadParam, (int)parse("{\"cmd\":\"mute\"}", c));
}

void test_tone_hz_bounds() {
  DebugCommand c;
  TEST_ASSERT_EQUAL((int)ParseError::None, (int)parse("{\"cmd\":\"tone\",\"hz\":440}", c));
  TEST_ASSERT_EQUAL_UINT32(440, c.hz);
  TEST_ASSERT_EQUAL((int)ParseError::BadParam, (int)parse("{\"cmd\":\"tone\",\"hz\":20000}", c));
}

void test_deep_nesting_rejected() {
  DebugCommand c;
  TEST_ASSERT_EQUAL((int)ParseError::BadJson, (int)parse("{\"cmd\":\"ping\",\"x\":{\"y\":{\"z\":1}}}", c));
}

void test_framer_lines_crlf_and_empty() {
  LineFramer f;
  const char* in = "\r\n{\"a\":1}\r\n\n";
  int lines = 0;
  for (const char* p = in; *p; p++) {
    if (f.push(*p) == LineFramer::Event::Line) {
      lines++;
      TEST_ASSERT_EQUAL_STRING("{\"a\":1}", f.line());
    }
  }
  TEST_ASSERT_EQUAL(1, lines);
}

void test_framer_overflow_discards_until_newline_then_recovers() {
  LineFramer f;
  int overflows = 0, lines = 0;
  for (size_t i = 0; i < LineFramer::kMaxLine + 50; i++) {
    if (f.push('x') == LineFramer::Event::Overflow) overflows++;
  }
  TEST_ASSERT_EQUAL(1, overflows);
  TEST_ASSERT_EQUAL((int)LineFramer::Event::None, (int)f.push('\n'));  // tail of bad line swallowed
  const char* ok = "{\"cmd\":\"ping\"}\n";
  for (const char* p = ok; *p; p++) {
    if (f.push(*p) == LineFramer::Event::Line) lines++;
  }
  TEST_ASSERT_EQUAL(1, lines);
  TEST_ASSERT_EQUAL_STRING("{\"cmd\":\"ping\"}", f.line());
}

void test_utf8_truncate_never_splits() {
  char dst[4];
  // "aé€" = 61 C3A9 E282AC (6 bytes). Capacity 4 => 3 bytes max => "aé".
  TEST_ASSERT_TRUE(utf8CopyTruncate(dst, sizeof(dst), "a\xC3\xA9\xE2\x82\xAC"));
  TEST_ASSERT_EQUAL_STRING("a\xC3\xA9", dst);
  char dst2[3];  // 2 bytes max: "a" + half of é => "a"
  TEST_ASSERT_TRUE(utf8CopyTruncate(dst2, sizeof(dst2), "a\xC3\xA9"));
  TEST_ASSERT_EQUAL_STRING("a", dst2);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_valid_face_command);
  RUN_TEST(test_bad_json_and_non_object);
  RUN_TEST(test_missing_and_unknown_cmd);
  RUN_TEST(test_version_check);
  RUN_TEST(test_id_must_be_unsigned);
  RUN_TEST(test_state_command_and_alias);
  RUN_TEST(test_boot_expression_not_requestable);
  RUN_TEST(test_negative_or_string_ms_rejected_and_large_clamped);
  RUN_TEST(test_show_requires_kind_and_value);
  RUN_TEST(test_text_truncated_at_utf8_boundary);
  RUN_TEST(test_invalid_utf8_rejected);
  RUN_TEST(test_config_value_never_silently_truncated);
  RUN_TEST(test_mute_requires_bool);
  RUN_TEST(test_tone_hz_bounds);
  RUN_TEST(test_deep_nesting_rejected);
  RUN_TEST(test_framer_lines_crlf_and_empty);
  RUN_TEST(test_framer_overflow_discards_until_newline_then_recovers);
  RUN_TEST(test_utf8_truncate_never_splits);
  return UNITY_END();
}
