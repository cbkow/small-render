#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>

namespace SR {

// ─── Shared sub-structs (used by both template and manifest) ────────────────

struct ProgressPattern
{
    std::string regex;
    std::string type;               // "fraction" or "percentage"
    int numerator_group = 1;        // for "fraction"
    int denominator_group = 2;      // for "fraction"
    int group = 1;                  // for "percentage"
    std::string info;
};

struct CompletionPattern
{
    std::string regex;
    std::string info;
};

struct ErrorPattern
{
    std::string regex;
    std::string info;
};

struct ProgressConfig
{
    std::vector<ProgressPattern> patterns;
    std::optional<CompletionPattern> completion_pattern;
    std::vector<ErrorPattern> error_patterns;
};

struct OutputDetection
{
    std::optional<std::string> stdout_regex;     // nullopt = no detection
    int path_group = 1;
    std::string validation = "exit_code_only";   // or "exists_nonzero"
    std::string info;
};

struct ProcessConfig
{
    std::string kill_method = "terminate";
    std::optional<std::string> working_dir;
};

// ─── Template-specific structs ──────────────────────────────────────────────

struct TemplateCmd
{
    std::string os_windows;         // JSON key "windows"
    std::string os_linux;           // JSON key "linux"
    std::string os_macos;           // JSON key "macos"
    std::string label;
    bool editable = true;
};

struct TemplateFlag
{
    std::string flag;               // "-b", "-o", "" (positional)
    std::optional<std::string> value;   // nullopt=standalone, ""=user fills, "{frame}"=runtime
    std::string info;               // UI label
    bool editable = false;
    bool required = false;
    std::string type;               // "file" = file picker, "output" = output path, "" = plain text
    std::string filter;             // file extensions for NFD, e.g. "blend" or "aep"
    std::string id;                 // cross-reference identifier for {flag:id} tokens
    std::optional<std::string> default_pattern;  // auto-resolve pattern for output paths
};

struct JobDefaults
{
    int frame_start = 1;
    int frame_end = 250;
    int chunk_size = 1;
    int priority = 50;
    int max_retries = 3;
    std::optional<int> timeout_seconds;
};

struct JobTemplate
{
    int _version = 1;
    std::string template_id;
    std::string name;
    TemplateCmd cmd;
    std::vector<TemplateFlag> flags;
    std::string frame_padding;              // e.g. "####" (Blender), "[####]" (AE)
    JobDefaults job_defaults;
    ProgressConfig progress;
    OutputDetection output_detection;
    ProcessConfig process;
    std::map<std::string, std::string> environment;
    std::vector<std::string> tags_required;

    // Runtime (not serialized)
    bool valid = false;
    std::string validation_error;
    bool isExample = false;
};

// ─── Manifest-specific structs ──────────────────────────────────────────────

struct ManifestFlag
{
    std::string flag;
    std::optional<std::string> value;
};

struct JobManifest
{
    int _version = 1;
    std::string job_id;             // the slug
    std::string template_id;       // provenance
    std::string submitted_by;      // node_id
    std::string submitted_os;      // "windows" | "linux" | "macos"
    int64_t submitted_at_ms = 0;

    std::map<std::string, std::string> cmd;   // "windows"->path, "linux"->path, "macos"->path
    std::vector<ManifestFlag> flags;

    int frame_start = 1;
    int frame_end = 250;
    int chunk_size = 1;
    int max_retries = 3;
    std::optional<int> timeout_seconds;

    std::optional<std::string> output_dir;  // parent directory of output path — auto-created before render

    ProgressConfig progress;
    OutputDetection output_detection;
    ProcessConfig process;
    std::map<std::string, std::string> environment;
    std::vector<std::string> tags_required;
};

// ─── Job state structs ──────────────────────────────────────────────────────

struct JobStateEntry
{
    std::string state;      // "active" | "paused" | "cancelled" | "completed"
    int priority = 50;
    std::string node_id;    // who wrote this entry
    int64_t timestamp_ms = 0;
};

struct JobInfo
{
    JobManifest manifest;
    std::string current_state = "active";
    int current_priority = 50;
};

// ─── Claim structs ──────────────────────────────────────────────────────────

struct ChunkRange
{
    int frame_start = 0;
    int frame_end = 0;

    std::string rangeStr() const
    {
        std::ostringstream ss;
        ss << std::setfill('0') << std::setw(6) << frame_start
           << "-"
           << std::setfill('0') << std::setw(6) << frame_end;
        return ss.str();
    }

    bool operator==(const ChunkRange& o) const
    {
        return frame_start == o.frame_start && frame_end == o.frame_end;
    }
};

enum class ChunkState { Unclaimed, Rendering, Completed, Failed, Abandoned };

inline std::vector<ChunkRange> computeChunks(int frame_start, int frame_end, int chunk_size)
{
    std::vector<ChunkRange> chunks;
    if (chunk_size <= 0 || frame_start > frame_end)
        return chunks;

    for (int f = frame_start; f <= frame_end; f += chunk_size)
    {
        ChunkRange c;
        c.frame_start = f;
        c.frame_end = (std::min)(f + chunk_size - 1, frame_end);
        chunks.push_back(c);
    }
    return chunks;
}

// ─── Dispatch structs (coordinator-based dispatch) ───────────────────────────

struct DispatchChunk
{
    int frame_start = 0;
    int frame_end = 0;
    std::string state = "pending";      // pending | assigned | completed | failed
    std::string assigned_to;
    int64_t assigned_at_ms = 0;
    int64_t completed_at_ms = 0;
    int retry_count = 0;
};

struct DispatchTable
{
    int _version = 1;
    std::string coordinator_id;
    int64_t updated_at_ms = 0;
    std::vector<DispatchChunk> chunks;
};

// ─── Helper ─────────────────────────────────────────────────────────────────

inline std::string getCmdForOS(const TemplateCmd& cmd, const std::string& os)
{
    if (os == "windows") return cmd.os_windows;
    if (os == "macos")   return cmd.os_macos;
    return cmd.os_linux;
}

// ─── JSON serialization: ProgressPattern ────────────────────────────────────

inline void to_json(nlohmann::json& j, const ProgressPattern& p)
{
    j = nlohmann::json{
        {"regex", p.regex},
        {"type", p.type},
        {"info", p.info},
    };
    if (p.type == "fraction")
    {
        j["numerator_group"] = p.numerator_group;
        j["denominator_group"] = p.denominator_group;
    }
    else
    {
        j["group"] = p.group;
    }
}

inline void from_json(const nlohmann::json& j, ProgressPattern& p)
{
    if (j.contains("regex"))             j.at("regex").get_to(p.regex);
    if (j.contains("type"))              j.at("type").get_to(p.type);
    if (j.contains("numerator_group"))   j.at("numerator_group").get_to(p.numerator_group);
    if (j.contains("denominator_group")) j.at("denominator_group").get_to(p.denominator_group);
    if (j.contains("group"))             j.at("group").get_to(p.group);
    if (j.contains("info"))              j.at("info").get_to(p.info);
}

// ─── JSON serialization: CompletionPattern ──────────────────────────────────

inline void to_json(nlohmann::json& j, const CompletionPattern& p)
{
    j = nlohmann::json{{"regex", p.regex}, {"info", p.info}};
}

inline void from_json(const nlohmann::json& j, CompletionPattern& p)
{
    if (j.contains("regex")) j.at("regex").get_to(p.regex);
    if (j.contains("info"))  j.at("info").get_to(p.info);
}

// ─── JSON serialization: ErrorPattern ───────────────────────────────────────

inline void to_json(nlohmann::json& j, const ErrorPattern& p)
{
    j = nlohmann::json{{"regex", p.regex}, {"info", p.info}};
}

inline void from_json(const nlohmann::json& j, ErrorPattern& p)
{
    if (j.contains("regex")) j.at("regex").get_to(p.regex);
    if (j.contains("info"))  j.at("info").get_to(p.info);
}

// ─── JSON serialization: ProgressConfig ─────────────────────────────────────

inline void to_json(nlohmann::json& j, const ProgressConfig& c)
{
    j = nlohmann::json{
        {"patterns", c.patterns},
        {"error_patterns", c.error_patterns},
    };
    if (c.completion_pattern.has_value())
        j["completion_pattern"] = c.completion_pattern.value();
    else
        j["completion_pattern"] = nullptr;
}

inline void from_json(const nlohmann::json& j, ProgressConfig& c)
{
    if (j.contains("patterns"))    j.at("patterns").get_to(c.patterns);
    if (j.contains("completion_pattern") && !j.at("completion_pattern").is_null())
        c.completion_pattern = j.at("completion_pattern").get<CompletionPattern>();
    if (j.contains("error_patterns")) j.at("error_patterns").get_to(c.error_patterns);
}

// ─── JSON serialization: OutputDetection ────────────────────────────────────

inline void to_json(nlohmann::json& j, const OutputDetection& o)
{
    j = nlohmann::json{
        {"stdout_regex", o.stdout_regex.has_value() ? nlohmann::json(o.stdout_regex.value()) : nlohmann::json(nullptr)},
        {"path_group", o.path_group},
        {"validation", o.validation},
        {"info", o.info},
    };
}

inline void from_json(const nlohmann::json& j, OutputDetection& o)
{
    if (j.contains("stdout_regex") && !j.at("stdout_regex").is_null())
        o.stdout_regex = j.at("stdout_regex").get<std::string>();
    if (j.contains("path_group"))  j.at("path_group").get_to(o.path_group);
    if (j.contains("validation"))  j.at("validation").get_to(o.validation);
    if (j.contains("info"))        j.at("info").get_to(o.info);
}

// ─── JSON serialization: ProcessConfig ──────────────────────────────────────

inline void to_json(nlohmann::json& j, const ProcessConfig& p)
{
    j = nlohmann::json{
        {"kill_method", p.kill_method},
        {"working_dir", p.working_dir.has_value() ? nlohmann::json(p.working_dir.value()) : nlohmann::json(nullptr)},
    };
}

inline void from_json(const nlohmann::json& j, ProcessConfig& p)
{
    if (j.contains("kill_method"))  j.at("kill_method").get_to(p.kill_method);
    if (j.contains("working_dir") && !j.at("working_dir").is_null())
        p.working_dir = j.at("working_dir").get<std::string>();
}

// ─── JSON serialization: TemplateCmd ────────────────────────────────────────

inline void to_json(nlohmann::json& j, const TemplateCmd& c)
{
    j = nlohmann::json{
        {"windows", c.os_windows},
        {"linux", c.os_linux},
        {"macos", c.os_macos},
        {"label", c.label},
        {"editable", c.editable},
    };
}

inline void from_json(const nlohmann::json& j, TemplateCmd& c)
{
    if (j.contains("windows"))  j.at("windows").get_to(c.os_windows);
    if (j.contains("linux"))    j.at("linux").get_to(c.os_linux);
    if (j.contains("macos"))    j.at("macos").get_to(c.os_macos);
    if (j.contains("label"))    j.at("label").get_to(c.label);
    if (j.contains("editable")) j.at("editable").get_to(c.editable);
}

// ─── JSON serialization: TemplateFlag ───────────────────────────────────────

inline void to_json(nlohmann::json& j, const TemplateFlag& f)
{
    j = nlohmann::json{
        {"flag", f.flag},
        {"value", f.value.has_value() ? nlohmann::json(f.value.value()) : nlohmann::json(nullptr)},
        {"info", f.info},
        {"editable", f.editable},
        {"required", f.required},
    };
    if (!f.type.empty())   j["type"] = f.type;
    if (!f.filter.empty()) j["filter"] = f.filter;
    if (!f.id.empty())     j["id"] = f.id;
    if (f.default_pattern.has_value()) j["default_pattern"] = f.default_pattern.value();
}

inline void from_json(const nlohmann::json& j, TemplateFlag& f)
{
    if (j.contains("flag"))     j.at("flag").get_to(f.flag);
    if (j.contains("value") && !j.at("value").is_null())
        f.value = j.at("value").get<std::string>();
    else if (j.contains("value") && j.at("value").is_null())
        f.value = std::nullopt;
    if (j.contains("info"))     j.at("info").get_to(f.info);
    if (j.contains("editable")) j.at("editable").get_to(f.editable);
    if (j.contains("required")) j.at("required").get_to(f.required);
    if (j.contains("type"))     j.at("type").get_to(f.type);
    if (j.contains("filter"))   j.at("filter").get_to(f.filter);
    if (j.contains("id"))       j.at("id").get_to(f.id);
    if (j.contains("default_pattern") && !j.at("default_pattern").is_null())
        f.default_pattern = j.at("default_pattern").get<std::string>();
}

// ─── JSON serialization: JobDefaults ────────────────────────────────────────

inline void to_json(nlohmann::json& j, const JobDefaults& d)
{
    j = nlohmann::json{
        {"frame_start", d.frame_start},
        {"frame_end", d.frame_end},
        {"chunk_size", d.chunk_size},
        {"priority", d.priority},
        {"max_retries", d.max_retries},
        {"timeout_seconds", d.timeout_seconds.has_value() ? nlohmann::json(d.timeout_seconds.value()) : nlohmann::json(nullptr)},
    };
}

inline void from_json(const nlohmann::json& j, JobDefaults& d)
{
    if (j.contains("frame_start"))  j.at("frame_start").get_to(d.frame_start);
    if (j.contains("frame_end"))    j.at("frame_end").get_to(d.frame_end);
    if (j.contains("chunk_size"))   j.at("chunk_size").get_to(d.chunk_size);
    if (j.contains("priority"))     j.at("priority").get_to(d.priority);
    if (j.contains("max_retries"))  j.at("max_retries").get_to(d.max_retries);
    if (j.contains("timeout_seconds") && !j.at("timeout_seconds").is_null())
        d.timeout_seconds = j.at("timeout_seconds").get<int>();
}

// ─── JSON serialization: JobTemplate ────────────────────────────────────────

inline void to_json(nlohmann::json& j, const JobTemplate& t)
{
    j = nlohmann::json{
        {"_version", t._version},
        {"template_id", t.template_id},
        {"name", t.name},
        {"cmd", t.cmd},
        {"flags", t.flags},
        {"job_defaults", t.job_defaults},
        {"progress", t.progress},
        {"output_detection", t.output_detection},
        {"process", t.process},
        {"environment", t.environment},
        {"tags_required", t.tags_required},
    };
    if (!t.frame_padding.empty()) j["frame_padding"] = t.frame_padding;
}

inline void from_json(const nlohmann::json& j, JobTemplate& t)
{
    if (j.contains("_version"))          j.at("_version").get_to(t._version);
    if (j.contains("template_id"))       j.at("template_id").get_to(t.template_id);
    if (j.contains("name"))              j.at("name").get_to(t.name);
    if (j.contains("cmd"))               j.at("cmd").get_to(t.cmd);
    if (j.contains("flags"))             j.at("flags").get_to(t.flags);
    if (j.contains("frame_padding"))     j.at("frame_padding").get_to(t.frame_padding);
    if (j.contains("job_defaults"))      j.at("job_defaults").get_to(t.job_defaults);
    if (j.contains("progress"))          j.at("progress").get_to(t.progress);
    if (j.contains("output_detection"))  j.at("output_detection").get_to(t.output_detection);
    if (j.contains("process"))           j.at("process").get_to(t.process);
    if (j.contains("environment"))       j.at("environment").get_to(t.environment);
    if (j.contains("tags_required"))     j.at("tags_required").get_to(t.tags_required);
}

// ─── JSON serialization: ManifestFlag ───────────────────────────────────────

inline void to_json(nlohmann::json& j, const ManifestFlag& f)
{
    j = nlohmann::json{
        {"flag", f.flag},
        {"value", f.value.has_value() ? nlohmann::json(f.value.value()) : nlohmann::json(nullptr)},
    };
}

inline void from_json(const nlohmann::json& j, ManifestFlag& f)
{
    if (j.contains("flag")) j.at("flag").get_to(f.flag);
    if (j.contains("value") && !j.at("value").is_null())
        f.value = j.at("value").get<std::string>();
    else if (j.contains("value") && j.at("value").is_null())
        f.value = std::nullopt;
}

// ─── JSON serialization: JobManifest ────────────────────────────────────────

inline void to_json(nlohmann::json& j, const JobManifest& m)
{
    j = nlohmann::json{
        {"_version", m._version},
        {"job_id", m.job_id},
        {"template_id", m.template_id},
        {"submitted_by", m.submitted_by},
        {"submitted_os", m.submitted_os},
        {"submitted_at_ms", m.submitted_at_ms},
        {"cmd", m.cmd},
        {"flags", m.flags},
        {"frame_start", m.frame_start},
        {"frame_end", m.frame_end},
        {"chunk_size", m.chunk_size},
        {"max_retries", m.max_retries},
        {"timeout_seconds", m.timeout_seconds.has_value() ? nlohmann::json(m.timeout_seconds.value()) : nlohmann::json(nullptr)},
        {"output_dir", m.output_dir.has_value() ? nlohmann::json(m.output_dir.value()) : nlohmann::json(nullptr)},
        {"progress", m.progress},
        {"output_detection", m.output_detection},
        {"process", m.process},
        {"environment", m.environment},
        {"tags_required", m.tags_required},
    };
}

inline void from_json(const nlohmann::json& j, JobManifest& m)
{
    if (j.contains("_version"))          j.at("_version").get_to(m._version);
    if (j.contains("job_id"))            j.at("job_id").get_to(m.job_id);
    if (j.contains("template_id"))       j.at("template_id").get_to(m.template_id);
    if (j.contains("submitted_by"))      j.at("submitted_by").get_to(m.submitted_by);
    if (j.contains("submitted_os"))      j.at("submitted_os").get_to(m.submitted_os);
    if (j.contains("submitted_at_ms"))   j.at("submitted_at_ms").get_to(m.submitted_at_ms);
    if (j.contains("cmd"))               j.at("cmd").get_to(m.cmd);
    if (j.contains("flags"))             j.at("flags").get_to(m.flags);
    if (j.contains("frame_start"))       j.at("frame_start").get_to(m.frame_start);
    if (j.contains("frame_end"))         j.at("frame_end").get_to(m.frame_end);
    if (j.contains("chunk_size"))        j.at("chunk_size").get_to(m.chunk_size);
    if (j.contains("max_retries"))       j.at("max_retries").get_to(m.max_retries);
    if (j.contains("timeout_seconds") && !j.at("timeout_seconds").is_null())
        m.timeout_seconds = j.at("timeout_seconds").get<int>();
    if (j.contains("output_dir") && !j.at("output_dir").is_null())
        m.output_dir = j.at("output_dir").get<std::string>();
    if (j.contains("progress"))          j.at("progress").get_to(m.progress);
    if (j.contains("output_detection"))  j.at("output_detection").get_to(m.output_detection);
    if (j.contains("process"))           j.at("process").get_to(m.process);
    if (j.contains("environment"))       j.at("environment").get_to(m.environment);
    if (j.contains("tags_required"))     j.at("tags_required").get_to(m.tags_required);
}

// ─── JSON serialization: DispatchChunk ──────────────────────────────────────

inline void to_json(nlohmann::json& j, const DispatchChunk& d)
{
    j = nlohmann::json{
        {"frame_start", d.frame_start},
        {"frame_end", d.frame_end},
        {"state", d.state},
        {"assigned_to", d.assigned_to},
        {"assigned_at_ms", d.assigned_at_ms},
        {"completed_at_ms", d.completed_at_ms},
        {"retry_count", d.retry_count},
    };
}

inline void from_json(const nlohmann::json& j, DispatchChunk& d)
{
    if (j.contains("frame_start"))     j.at("frame_start").get_to(d.frame_start);
    if (j.contains("frame_end"))       j.at("frame_end").get_to(d.frame_end);
    if (j.contains("state"))           j.at("state").get_to(d.state);
    if (j.contains("assigned_to"))     j.at("assigned_to").get_to(d.assigned_to);
    if (j.contains("assigned_at_ms"))  j.at("assigned_at_ms").get_to(d.assigned_at_ms);
    if (j.contains("completed_at_ms")) j.at("completed_at_ms").get_to(d.completed_at_ms);
    if (j.contains("retry_count"))     j.at("retry_count").get_to(d.retry_count);
}

// ─── JSON serialization: DispatchTable ──────────────────────────────────────

inline void to_json(nlohmann::json& j, const DispatchTable& dt)
{
    j = nlohmann::json{
        {"_version", dt._version},
        {"coordinator_id", dt.coordinator_id},
        {"updated_at_ms", dt.updated_at_ms},
        {"chunks", dt.chunks},
    };
}

inline void from_json(const nlohmann::json& j, DispatchTable& dt)
{
    if (j.contains("_version"))        j.at("_version").get_to(dt._version);
    if (j.contains("coordinator_id"))  j.at("coordinator_id").get_to(dt.coordinator_id);
    if (j.contains("updated_at_ms"))   j.at("updated_at_ms").get_to(dt.updated_at_ms);
    if (j.contains("chunks"))          j.at("chunks").get_to(dt.chunks);
}

// ─── JSON serialization: JobStateEntry ──────────────────────────────────────

inline void to_json(nlohmann::json& j, const JobStateEntry& s)
{
    j = nlohmann::json{
        {"_version", 1},
        {"state", s.state},
        {"priority", s.priority},
        {"node_id", s.node_id},
        {"timestamp_ms", s.timestamp_ms},
    };
}

inline void from_json(const nlohmann::json& j, JobStateEntry& s)
{
    if (j.contains("state"))        j.at("state").get_to(s.state);
    if (j.contains("priority"))     j.at("priority").get_to(s.priority);
    if (j.contains("node_id"))      j.at("node_id").get_to(s.node_id);
    if (j.contains("timestamp_ms")) j.at("timestamp_ms").get_to(s.timestamp_ms);
}

} // namespace SR
