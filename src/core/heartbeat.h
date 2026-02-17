#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace SR {

// On-disk heartbeat JSON schema — written atomically to {nodes}/{node_id}/heartbeat.json
struct Heartbeat
{
    int         _version = 1;
    std::string node_id;
    std::string hostname;
    std::string os;
    std::string app_version;
    uint32_t    protocol_version = 1;
    uint64_t    seq = 0;
    int64_t     timestamp_ms = 0;
    std::string node_state = "active";    // active | stopped | draining
    std::string render_state = "idle";    // idle | rendering
    std::string active_job;               // empty = null
    std::string active_frames;            // empty = null
    std::string gpu_name;
    int         cpu_cores = 0;
    uint64_t    ram_gb = 0;
    std::vector<std::string> tags;
    bool        is_coordinator = false;
    int64_t     last_cmd_timestamp_ms = 0;
};

inline void to_json(nlohmann::json& j, const Heartbeat& h)
{
    j = nlohmann::json{
        {"_version", h._version},
        {"node_id", h.node_id},
        {"hostname", h.hostname},
        {"os", h.os},
        {"app_version", h.app_version},
        {"protocol_version", h.protocol_version},
        {"seq", h.seq},
        {"timestamp_ms", h.timestamp_ms},
        {"node_state", h.node_state},
        {"render_state", h.render_state},
        {"active_job", h.active_job.empty() ? nlohmann::json(nullptr) : nlohmann::json(h.active_job)},
        {"active_frames", h.active_frames.empty() ? nlohmann::json(nullptr) : nlohmann::json(h.active_frames)},
        {"gpu_name", h.gpu_name},
        {"cpu_cores", h.cpu_cores},
        {"ram_gb", h.ram_gb},
        {"tags", h.tags},
        {"is_coordinator", h.is_coordinator},
        {"last_cmd_timestamp_ms", h.last_cmd_timestamp_ms},
    };
}

inline void from_json(const nlohmann::json& j, Heartbeat& h)
{
    if (j.contains("_version"))           j.at("_version").get_to(h._version);
    if (j.contains("node_id"))            j.at("node_id").get_to(h.node_id);
    if (j.contains("hostname"))           j.at("hostname").get_to(h.hostname);
    if (j.contains("os"))                 j.at("os").get_to(h.os);
    if (j.contains("app_version"))        j.at("app_version").get_to(h.app_version);
    if (j.contains("protocol_version"))   j.at("protocol_version").get_to(h.protocol_version);
    if (j.contains("seq"))                j.at("seq").get_to(h.seq);
    if (j.contains("timestamp_ms"))       j.at("timestamp_ms").get_to(h.timestamp_ms);
    if (j.contains("node_state"))         j.at("node_state").get_to(h.node_state);
    if (j.contains("render_state"))       j.at("render_state").get_to(h.render_state);
    if (j.contains("active_job") && !j.at("active_job").is_null())
        j.at("active_job").get_to(h.active_job);
    if (j.contains("active_frames") && !j.at("active_frames").is_null())
        j.at("active_frames").get_to(h.active_frames);
    if (j.contains("gpu_name"))           j.at("gpu_name").get_to(h.gpu_name);
    if (j.contains("cpu_cores"))          j.at("cpu_cores").get_to(h.cpu_cores);
    if (j.contains("ram_gb"))             j.at("ram_gb").get_to(h.ram_gb);
    if (j.contains("tags"))               j.at("tags").get_to(h.tags);
    if (j.contains("is_coordinator"))   j.at("is_coordinator").get_to(h.is_coordinator);
    if (j.contains("last_cmd_timestamp_ms")) j.at("last_cmd_timestamp_ms").get_to(h.last_cmd_timestamp_ms);
}

// In-memory node info: heartbeat + derived staleness state (used by UI)
struct NodeInfo
{
    Heartbeat heartbeat;
    bool      isLocal = false;
    bool      isDead = true;            // assume dead until seq advances
    uint32_t  staleCount = 0;           // consecutive scans with unchanged seq
    uint64_t  lastSeenSeq = 0;
    bool      clockSkewWarning = false;
    int64_t   skewAmountMs = 0;
    bool      reclaimEligible = true;   // dead nodes are reclaimable

    // UDP fast path tracking (runtime only, not serialized)
    bool    hasUdpContact = false;
    int64_t lastUdpContactMs = 0;
};

} // namespace SR
