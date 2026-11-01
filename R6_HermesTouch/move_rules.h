/*
 * move_rules.h — pure movement limits for R3_HermesMove (no hardware calls,
 * so the same rules are unit-tested on the Mac).
 *
 * Angles are in the M5StackChan library's units: tenths of a degree (10 = 1°).
 * Library ranges: yaw -1280..1280, pitch 0..900. M5Stack docs: keep pitch ~5°–85°.
 */
#pragma once
#include <stdlib.h>
#include <string.h>

namespace move_rules {

static const int YAW_MIN   = -300;  // -30°
static const int YAW_MAX   = 300;   // +30°
static const int PITCH_MIN = 100;   // 10° (well inside the 5° limit)
static const int PITCH_MAX = 700;   // 70° (well inside the 85° limit)
static const int MAX_STEP  = 300;   // at most 30° per axis in one move

/* Strict parse: optional '-', 1-4 digits, nothing else */
inline bool parse_angle(const char* s, int& out)
{
    if (!s) {
        return false;
    }
    size_t n = strlen(s);
    size_t i = (n > 0 && s[0] == '-') ? 1 : 0;
    if (n == i || n - i > 4) {
        return false;
    }
    for (size_t k = i; k < n; ++k) {
        if (s[k] < '0' || s[k] > '9') {
            return false;
        }
    }
    out = atoi(s);
    return true;
}

/* Returns nullptr if the move is allowed, otherwise the reason */
inline const char* check_move(int cur_yaw, int cur_pitch, int yaw, int pitch)
{
    if (yaw < YAW_MIN || yaw > YAW_MAX) {
        return "yaw out of range (-300..300)";
    }
    if (pitch < PITCH_MIN || pitch > PITCH_MAX) {
        return "pitch out of range (100..700)";
    }
    if (abs(yaw - cur_yaw) > MAX_STEP) {
        return "yaw step too large (max 300)";
    }
    if (abs(pitch - cur_pitch) > MAX_STEP) {
        return "pitch step too large (max 300)";
    }
    return nullptr;
}

}  // namespace move_rules
