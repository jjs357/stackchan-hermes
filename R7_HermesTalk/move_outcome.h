#pragma once
/* Result of one head move; shared by /head/move and the tap gesture (R6_HermesTouch) */
#include <stdint.h>

enum class MoveFail { None, NotReady, ReadFailed, Rejected };

struct MoveOutcome {
    MoveFail fail       = MoveFail::None;
    const char* problem = nullptr;
    bool ok             = false;
    int target_yaw = 0, target_pitch = 0;
    int start_yaw = 0, start_pitch = 0;
    int end_yaw = 0, end_pitch = 0;
    bool end_known     = false;
    uint32_t move_ms   = 0;
    uint32_t duration  = 0;
    float peak_delta_ma = 0.0f;
    bool timed_out = false, stall_abort = false, cmd_failed = false;
};
