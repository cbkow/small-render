#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace SR {

// --- Timing Presets ---

enum class TimingPreset
{
    LocalNAS,   // Low latency local network
    CloudFS,    // Higher latency cloud storage (Dropbox, OneDrive, etc.)
    Custom      // User-defined values
};

inline const char* timingPresetName(TimingPreset p)
{
    switch (p)
    {
        case TimingPreset::LocalNAS: return "Local / NAS";
        case TimingPreset::CloudFS:  return "Cloud FS";
        case TimingPreset::Custom:   return "Custom";
    }
    return "Unknown";
}

struct TimingConfig
{
    uint32_t heartbeat_interval_ms = 5000;
    uint32_t scan_interval_ms      = 3000;
    uint32_t claim_settle_ms       = 3000;
    uint32_t dead_threshold_scans  = 3;       // consecutive stale scans before declaring dead
};

inline TimingConfig timingForPreset(TimingPreset p)
{
    switch (p)
    {
        case TimingPreset::LocalNAS:
            return { 5000, 3000, 3000, 3 };    // death at ~9s
        case TimingPreset::CloudFS:
            return { 10000, 5000, 5000, 4 };   // death at ~20s
        case TimingPreset::Custom:
        default:
            return {}; // defaults
    }
}

// --- Main Config ---

struct Config
{
    // Sync root path (shared filesystem mount point)
    std::string sync_root;

    // Timing
    TimingPreset timing_preset = TimingPreset::LocalNAS;
    TimingConfig timing;

    // Node tags (for job targeting)
    std::vector<std::string> tags;

    // Coordinator
    bool is_coordinator = false;

    // Agent settings
    bool auto_start_agent = true;

    // UDP multicast fast path
    bool udp_enabled = true;
    uint16_t udp_port = 4242;

    // UI preferences
    bool show_notifications = true;
    float font_scale = 1.0f;
};

// JSON serialization
inline void to_json(nlohmann::json& j, const Config& c)
{
    j = nlohmann::json{
        {"sync_root", c.sync_root},
        {"timing_preset", static_cast<int>(c.timing_preset)},
        {"timing", {
            {"heartbeat_interval_ms", c.timing.heartbeat_interval_ms},
            {"scan_interval_ms", c.timing.scan_interval_ms},
            {"claim_settle_ms", c.timing.claim_settle_ms},
            {"dead_threshold_scans", c.timing.dead_threshold_scans},
        }},
        {"tags", c.tags},
        {"is_coordinator", c.is_coordinator},
        {"auto_start_agent", c.auto_start_agent},
        {"udp_enabled", c.udp_enabled},
        {"udp_port", c.udp_port},
        {"show_notifications", c.show_notifications},
        {"font_scale", c.font_scale},
    };
}

inline void from_json(const nlohmann::json& j, Config& c)
{
    if (j.contains("sync_root"))       j.at("sync_root").get_to(c.sync_root);
    if (j.contains("timing_preset"))   c.timing_preset = static_cast<TimingPreset>(j.at("timing_preset").get<int>());
    if (j.contains("timing"))
    {
        auto& t = j.at("timing");
        if (t.contains("heartbeat_interval_ms")) t.at("heartbeat_interval_ms").get_to(c.timing.heartbeat_interval_ms);
        if (t.contains("scan_interval_ms"))       t.at("scan_interval_ms").get_to(c.timing.scan_interval_ms);
        if (t.contains("claim_settle_ms"))         t.at("claim_settle_ms").get_to(c.timing.claim_settle_ms);
        if (t.contains("dead_threshold_scans"))   t.at("dead_threshold_scans").get_to(c.timing.dead_threshold_scans);
        // Ignore legacy "stale_threshold_ms" field from older configs
    }
    if (j.contains("tags"))              j.at("tags").get_to(c.tags);
    if (j.contains("is_coordinator"))   j.at("is_coordinator").get_to(c.is_coordinator);
    if (j.contains("auto_start_agent")) j.at("auto_start_agent").get_to(c.auto_start_agent);
    if (j.contains("udp_enabled"))       j.at("udp_enabled").get_to(c.udp_enabled);
    if (j.contains("udp_port"))          c.udp_port = j.at("udp_port").get<uint16_t>();
    if (j.contains("show_notifications")) j.at("show_notifications").get_to(c.show_notifications);
    if (j.contains("font_scale"))         j.at("font_scale").get_to(c.font_scale);
}

// --- Constants ---
constexpr uint32_t CLOCK_SKEW_WARN_MS    = 30000;
constexpr uint32_t PROTOCOL_VERSION      = 1;

#ifndef APP_VERSION
#define APP_VERSION "0.1.0"
#endif

} // namespace SR
