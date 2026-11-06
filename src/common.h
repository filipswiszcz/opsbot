#ifndef HOR_COMMON_H
#define HOR_COMMON_H

#include <cstdint>

#define DISCORD_API_URL "https://discord.com/api/v10/"
#define DISCORD_MAX_CHANNELS 64

#define PERFORCE_MAX_DEPOTS 16

typedef float f32;
typedef double f64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

static_assert(sizeof(f32) == 4, "Float must be exactly 4 bytes");
static_assert(sizeof(f64) == 8, "Double must be exactly 8 bytes");

static const char *find_json_object_end(const char *pos) noexcept {
    u8 has_quotes = 0, is_escaped = 0;
    i32 depth = 1;

    for (const char *p = pos; *p != '\0'; p++) {
        if (has_quotes) {
            if (is_escaped) {
                is_escaped = 0;
            } else if (*p == '\\') {
                is_escaped = 1;
            } else if (*p == '"') {
                has_quotes = 0;
            }
        } else {
            if (*p == '"') {
                has_quotes = 1;
            } else if (*p == '{') {
                depth++;
            } else if (*p == '}') {
                depth--;
                if (depth == 0) {
                    return p;
                }
            }
        }
    }

    return nullptr;
}

#endif //!HOR_COMMON_H