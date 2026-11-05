/*
 * R7_HermesTalk v0.17.0: the five settings the robot needs, kept in NVS namespace "hermes"
 * (written over USB by tools/stackchan-setup.py) or, if nothing is saved, taken from secrets.h.
 * In a header so the Arduino builder's generated prototypes can use these types.
 */
#pragma once
#include <stddef.h>

struct RobotConfig {
    char ssid[33];
    char pass[65];
    char token[129];
    char hook_host[64];
    char hook_secret[129];
};

struct CfgField {
    const char* key;
    size_t offset;
    size_t size;
    bool secret;  // never printed
};

static const CfgField CFG_FIELDS[] = {
    {"ssid", offsetof(RobotConfig, ssid), sizeof(RobotConfig::ssid), false},
    {"pass", offsetof(RobotConfig, pass), sizeof(RobotConfig::pass), true},
    {"token", offsetof(RobotConfig, token), sizeof(RobotConfig::token), true},
    {"hook_host", offsetof(RobotConfig, hook_host), sizeof(RobotConfig::hook_host), false},
    {"hook_secret", offsetof(RobotConfig, hook_secret), sizeof(RobotConfig::hook_secret), true},
};
static const size_t CFG_COUNT = sizeof(CFG_FIELDS) / sizeof(CFG_FIELDS[0]);
