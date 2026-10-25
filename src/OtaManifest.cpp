#include "OtaManifest.h"

#include <stdio.h>
#include <string.h>

#include "OtaNoExcept.h"

namespace {

// --- A reader for one flat JSON object ---

struct Reader {
  const char* p;
  const char* end;

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  }

  bool eat(char c) {
    ws();
    if (p < end && *p == c) {
      p++;
      return true;
    }
    return false;
  }

  // A string, decoded into `out`. Escapes are honoured; \u is accepted only for
  // ASCII, which is all an offer contains. `out` may be null to skip.
  bool string(char* out, size_t capacity) {
    ws();
    if (p >= end || *p != '"') return false;
    p++;
    size_t n = 0;
    while (p < end && *p != '"') {
      char c = *p++;
      if ((unsigned char)c < 0x20) return false;
      if (c == '\\') {
        if (p >= end) return false;
        const char e = *p++;
        switch (e) {
          case '"': case '\\': case '/': c = e; break;
          case 'b': c = '\b'; break;
          case 'f': c = '\f'; break;
          case 'n': c = '\n'; break;
          case 'r': c = '\r'; break;
          case 't': c = '\t'; break;
          case 'u': {
            if (end - p < 4) return false;
            unsigned v = 0;
            for (int i = 0; i < 4; i++) {
              const char h = *p++;
              v <<= 4;
              if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
              else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
              else return false;
            }
            if (v == 0 || v > 0x7f) return false;
            c = (char)v;
            break;
          }
          default: return false;
        }
      }
      if (out) {
        if (n + 1 >= capacity) return false;
        out[n] = c;
      }
      n++;
    }
    if (p >= end) return false;
    p++;  // closing quote
    if (out) out[n] = '\0';
    return true;
  }

  bool number(uint32_t& out) {
    ws();
    if (p >= end || *p < '0' || *p > '9') return false;
    uint64_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
      v = v * 10u + (uint64_t)(*p++ - '0');
      if (v > 0xffffffffull) return false;
    }
    // A fraction or exponent is not a byte count or a job number.
    if (p < end && (*p == '.' || *p == 'e' || *p == 'E')) return false;
    out = (uint32_t)v;
    return true;
  }

  bool boolean(bool& out) {
    ws();
    if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
      p += 4;
      out = true;
      return true;
    }
    if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
      p += 5;
      out = false;
      return true;
    }
    return false;
  }

  // Any value, skipped. Nesting is bounded so a hostile payload cannot recurse
  // the stack away.
  bool skip(int depth = 0) {
    ws();
    if (p >= end || depth > 8) return false;
    if (*p == '"') return string(nullptr, 0);
    if (*p == '{' || *p == '[') {
      const char close = (*p == '{') ? '}' : ']';
      const bool object = (*p == '{');
      p++;
      if (eat(close)) return true;
      for (;;) {
        if (object) {
          if (!string(nullptr, 0) || !eat(':')) return false;
        }
        if (!skip(depth + 1)) return false;
        if (eat(',')) continue;
        return eat(close);
      }
    }
    // A literal or number: run to the next delimiter.
    const char* start = p;
    while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
           *p != '\n' && *p != '\r' && *p != '\t') {
      p++;
    }
    return p > start;
  }
};

bool equals(const char* a, const char* b) { return strcmp(a, b) == 0; }

// Escapes for a JSON string. Board and version come from the application or
// from an offer, so they are escaped rather than trusted. 0 when the result
// does not fit; `out` is still terminated, holding what did.
size_t escape(const char* in, char* out, size_t capacity) {
  size_t n = 0;
  for (; in && *in; in++) {
    const char c = *in;
    const char* rep = nullptr;
    char hex[7];
    if (c == '"') rep = "\\\"";
    else if (c == '\\') rep = "\\\\";
    else if ((unsigned char)c < 0x20) {
      snprintf(hex, sizeof(hex), "\\u%04x", (unsigned)(unsigned char)c);
      rep = hex;
    }
    const size_t len = rep ? strlen(rep) : 1;
    if (n + len >= capacity) {
      if (n < capacity) out[n] = '\0';
      return 0;
    }
    if (rep) memcpy(out + n, rep, len);
    else out[n] = c;
    n += len;
  }
  if (n >= capacity) return 0;
  out[n] = '\0';
  return n;
}

// The leading run of digits as a number, advancing `p`. -1 when there are none.
long digits(const char*& p) {
  if (*p < '0' || *p > '9') return -1;
  long v = 0;
  while (*p >= '0' && *p <= '9') {
    if (v < 100000000L) v = v * 10 + (*p - '0');
    p++;
  }
  return v;
}

}  // namespace

bool ota_parseOfferJson(const char* json, size_t length, OtaImage& image) {
  if (!json) return false;
  Reader r{json, json + length};
  if (!r.eat('{')) return false;

  OtaImage parsed;
  bool haveJob = false, haveSize = false, haveHash = false;
  bool haveVersion = false, haveBoard = false;
  char key[16];
  char hex[2 * MODBEE_OTA_SIGNATURE_MAX + 1];

  if (!r.eat('}')) {
    for (;;) {
      // A key longer than any we know is skipped, not refused.
      Reader save = r;
      if (!r.string(key, sizeof(key))) {
        r = save;
        if (!r.string(nullptr, 0)) return false;
        key[0] = '\0';
      }
      if (!r.eat(':')) return false;

      if (equals(key, "job")) {
        if (!r.number(parsed.job) || parsed.job == 0) return false;
        haveJob = true;
      } else if (equals(key, "size")) {
        if (!r.number(parsed.size) || parsed.size == 0) return false;
        haveSize = true;
      } else if (equals(key, "version")) {
        if (!r.string(parsed.version, sizeof(parsed.version))) return false;
        haveVersion = true;
      } else if (equals(key, "board")) {
        if (!r.string(parsed.board, sizeof(parsed.board))) return false;
        haveBoard = true;
      } else if (equals(key, "sha256")) {
        if (!r.string(hex, sizeof(hex))) return false;
        if (ota_fromHex(hex, strlen(hex), parsed.sha256, 32) != 32) return false;
        haveHash = true;
      } else if (equals(key, "signature")) {
        if (!r.string(hex, sizeof(hex))) return false;
        const size_t n = strlen(hex);
        if (n > 0) {
          const size_t got = ota_fromHex(hex, n, parsed.signature, sizeof(parsed.signature));
          if (got == 0) return false;
          parsed.signatureLength = (uint8_t)got;
        }
      } else if (equals(key, "apply")) {
        char mode[8];
        if (!r.string(mode, sizeof(mode))) return false;
        if (equals(mode, "manual")) parsed.flags |= MODBEE_OTA_FLAG_MANUAL_APPLY;
        else if (!equals(mode, "auto")) return false;
      } else if (equals(key, "force")) {
        bool force = false;
        if (!r.boolean(force)) return false;
        if (force) parsed.flags |= MODBEE_OTA_FLAG_FORCE;
      } else {
        if (!r.skip()) return false;
      }

      if (r.eat(',')) continue;
      if (r.eat('}')) break;
      return false;
    }
  }
  r.ws();
  if (r.p != r.end) return false;  // trailing junk
  if (!haveJob || !haveSize || !haveHash || !haveVersion || !haveBoard) return false;
  image = parsed;
  return true;
}

size_t ota_writeOfferJson(const OtaImage& image, char* out, size_t capacity) {
  char sha[65];
  char sig[2 * MODBEE_OTA_SIGNATURE_MAX + 1];
  char version[2 * MODBEE_OTA_VERSION_MAX];
  char board[2 * MODBEE_OTA_BOARD_MAX];
  if (!out || !ota_toHex(image.sha256, 32, sha, sizeof(sha))) return 0;
  if (!ota_toHex(image.signature, image.signatureLength, sig, sizeof(sig))) return 0;
  if (escape(image.version, version, sizeof(version)) == 0 && image.version[0]) return 0;
  if (escape(image.board, board, sizeof(board)) == 0 && image.board[0]) return 0;
  if (!image.version[0]) version[0] = '\0';
  if (!image.board[0]) board[0] = '\0';
  const int n = snprintf(out, capacity,
                         "{\"job\":%lu,\"version\":\"%s\",\"board\":\"%s\",\"size\":%lu,"
                         "\"sha256\":\"%s\",\"signature\":\"%s\",\"apply\":\"%s\",\"force\":%s}",
                         (unsigned long)image.job, version, board, (unsigned long)image.size,
                         sha, sig,
                         (image.flags & MODBEE_OTA_FLAG_MANUAL_APPLY) ? "manual" : "auto",
                         (image.flags & MODBEE_OTA_FLAG_FORCE) ? "true" : "false");
  return (n > 0 && (size_t)n < capacity) ? (size_t)n : 0;
}

size_t ota_writeStatusJson(const OtaStatus& status, char* out, size_t capacity) {
  char board[2 * MODBEE_OTA_BOARD_MAX];
  char running[2 * MODBEE_OTA_VERSION_MAX];
  char offered[2 * MODBEE_OTA_VERSION_MAX];
  board[0] = running[0] = offered[0] = '\0';
  escape(status.board, board, sizeof(board));
  escape(status.running, running, sizeof(running));
  escape(status.offered, offered, sizeof(offered));
  if (!out) return 0;
  const int n = snprintf(
      out, capacity,
      "{\"state\":\"%s\",\"outcome\":\"%s\",\"result\":\"%s\",\"job\":%lu,"
      "\"received\":%lu,\"size\":%lu,\"progress\":%u,\"board\":\"%s\","
      "\"running\":\"%s\",\"offered\":\"%s\"}",
      ota_stateName(status.state), ota_outcomeName(status.outcome),
      ota_resultName(status.result), (unsigned long)status.job,
      (unsigned long)status.received, (unsigned long)status.size,
      (unsigned)ota_percent(status.received, status.size), board, running, offered);
  return (n > 0 && (size_t)n < capacity) ? (size_t)n : 0;
}

int ota_compareVersions(const char* a, const char* b) {
  if (!a) a = "";
  if (!b) b = "";
  if (*a == 'v' || *a == 'V') a++;
  if (*b == 'v' || *b == 'V') b++;
  const char* pa = a;
  const char* pb = b;
  for (;;) {
    const long na = digits(pa);
    const long nb = digits(pb);
    if (na < 0 || nb < 0) break;  // not numeric here: fall back below
    if (na != nb) return na < nb ? -1 : 1;
    const bool moreA = (*pa == '.');
    const bool moreB = (*pb == '.');
    if (moreA && moreB) {
      pa++;
      pb++;
      continue;
    }
    if (moreA != moreB) {
      // "1.4" against "1.4.0": the longer one is newer unless the rest is zeros.
      const char* rest = moreA ? pa : pb;
      while (*rest == '.' || *rest == '0') rest++;
      if (*rest == '\0' || *rest == '-' || *rest == '+') {
        // Equal numerically; let the suffix rules below decide.
        if (moreA) { while (*pa == '.' || *pa == '0') pa++; }
        else { while (*pb == '.' || *pb == '0') pb++; }
      } else {
        return moreA ? 1 : -1;
      }
    }
    // Build metadata (+...) never orders. A pre-release (-...) sorts before
    // the release it leads up to.
    const bool endA = (*pa == '\0' || *pa == '+');
    const bool endB = (*pb == '\0' || *pb == '+');
    if (endA && endB) return 0;
    if (endA && *pb == '-') return 1;
    if (endB && *pa == '-') return -1;
    const int c = strcmp(pa, pb);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
  }
  const int c = strcmp(a, b);
  return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

size_t ota_signedMessage(const OtaImage& image, uint8_t* out, size_t capacity) {
  static const char domain[] = "modbee-ota-v1";  // 13 characters and the terminator
  if (!out || capacity < MODBEE_OTA_SIGNED_SIZE) return 0;
  memset(out, 0, MODBEE_OTA_SIGNED_SIZE);
  memcpy(out, domain, sizeof(domain));
  memcpy(out + 14, image.sha256, 32);
  out[46] = (uint8_t)image.size;
  out[47] = (uint8_t)(image.size >> 8);
  out[48] = (uint8_t)(image.size >> 16);
  out[49] = (uint8_t)(image.size >> 24);
  memcpy(out + 50, image.board, strnlen(image.board, 32));
  memcpy(out + 82, image.version, strnlen(image.version, 32));
  return MODBEE_OTA_SIGNED_SIZE;
}

size_t ota_fromHex(const char* hex, size_t length, uint8_t* out, size_t capacity) {
  if (!hex || !out || length == 0 || (length & 1) || length / 2 > capacity) return 0;
  for (size_t i = 0; i < length; i += 2) {
    uint8_t v = 0;
    for (int j = 0; j < 2; j++) {
      const char c = hex[i + j];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= (uint8_t)(c - '0');
      else if (c >= 'a' && c <= 'f') v |= (uint8_t)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= (uint8_t)(c - 'A' + 10);
      else return 0;
    }
    out[i / 2] = v;
  }
  return length / 2;
}

bool ota_toHex(const uint8_t* data, size_t length, char* out, size_t capacity) {
  static const char digitsHex[] = "0123456789abcdef";
  if (!out || capacity < length * 2 + 1) return false;
  for (size_t i = 0; i < length; i++) {
    out[i * 2] = digitsHex[data[i] >> 4];
    out[i * 2 + 1] = digitsHex[data[i] & 0x0f];
  }
  out[length * 2] = '\0';
  return true;
}
