#include "gateway_reply.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

#include "utf8.h"

namespace robot {

bool isSafeAudioPath(const char* p) {
  if (!p || strncmp(p, "/v1/", 4) != 0 || strlen(p) > kMaxAudioPathLen || strstr(p, "..")) return false;
  for (const char* c = p; *c; c++) {
    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '/' ||
          *c == '_' || *c == '-' || *c == '.'))
      return false;
  }
  return true;
}

// base64url only: the token goes into an HTTP header, so no spaces, CR/LF or ':' can get in.
bool isSafeContextToken(const char* t, size_t cap) {
  if (!t || !*t) return false;
  size_t n = 0;
  for (const char* c = t; *c; c++, n++) {
    if (n + 1 >= cap || n >= kMaxContextLen) return false;
    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' ||
          *c == '-'))
      return false;
  }
  return true;
}

static bool allowedEmotion(Expression e) {
  return e == Expression::Happy || e == Expression::Sad || e == Expression::Surprised || e == Expression::Idle ||
         e == Expression::Thinking;
}

bool parseGatewayReply(const char* body, size_t len, GatewayReply& out, char* audioPath, size_t pathCap,
                       uint32_t& audioRate, char* ctxOut, size_t ctxCap, ContextUpdate* ctxUpdate) {
  if (ctxUpdate) *ctxUpdate = ContextUpdate::Keep;
  JsonDocument doc;
  if (deserializeJson(doc, body, len, DeserializationOption::NestingLimit(4))) return false;
  if (!doc.is<JsonObjectConst>()) return false;
  GatewayReply r;
  r.ignore = doc["ignore"] | false;
  utf8CopyTruncate(r.lang, sizeof(r.lang), doc["lang"] | "");
  JsonObjectConst disp = doc["display"];
  const char* oled = disp["oled_text"] | "";
  if (!utf8IsValid(oled)) return false;
  utf8CopyTruncate(r.oledText, sizeof(r.oledText), oled);
  r.rtl = disp["rtl"] | false;
  const char* longText = disp["long_text"] | "";
  if (!utf8IsValid(longText)) return false;
  utf8CopyTruncate(r.longText, sizeof(r.longText), longText);
  r.longRtl = disp["long_rtl"] | false;
  JsonObjectConst content = disp["content"];
  if (!content.isNull()) {
    ContentKind k;
    const char* v = content["value"] | "";
    const char* cap = content["caption"] | "";
    if (parseContentKind(content["kind"] | "", k) && *v && utf8IsValid(v) && utf8IsValid(cap)) {
      r.contentKind = k;
      utf8CopyTruncate(r.contentValue, sizeof(r.contentValue), v);
      utf8CopyTruncate(r.contentCaption, sizeof(r.contentCaption), cap);
    }
  }
  Expression e;
  r.emotion = (parseExpression(doc["emotion"] | "happy", e) && allowedEmotion(e)) ? e : Expression::Happy;

  if (audioPath && pathCap) audioPath[0] = 0;
  audioRate = 0;
  JsonObjectConst audio = doc["audio"];
  if (!audio.isNull() && !r.ignore) {
    const char* fmt = audio["format"] | "";
    const char* path = audio["path"] | "";
    uint32_t rate = audio["sample_rate"] | 0;
    bool rateOk = rate == 8000 || rate == 16000 || rate == 22050 || rate == 24000;
    if (strcmp(fmt, "pcm_s16le") == 0 && rateOk && isSafeAudioPath(path) && strlen(path) < pathCap) {
      snprintf(audioPath, pathCap, "%s", path);
      audioRate = rate;
      r.hasAudio = true;
    }
  }
  out = r;
  if (ctxUpdate && ctxOut && ctxCap) {
    JsonVariantConst ctx = doc["ctx"];
    if (!ctx.isNull()) {
      const char* tok = ctx.is<const char*>() ? ctx.as<const char*>() : "";
      if (isSafeContextToken(tok, ctxCap)) {
        snprintf(ctxOut, ctxCap, "%s", tok);
        *ctxUpdate = ContextUpdate::Set;
      } else {
        *ctxUpdate = ContextUpdate::Clear;
      }
    }
  }
  return true;
}

}  // namespace robot
