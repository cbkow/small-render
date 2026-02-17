#pragma once

#include "core/job_types.h"
#include "monitor/ui_data_cache.h"

#include <string>
#include <vector>

namespace SR {

class MonitorApp; // forward

class JobDetailPanel
{
public:
    void init(MonitorApp* app);
    void render();
    bool visible = true;

private:
    MonitorApp* m_app = nullptr;

    enum class Mode { Empty, Submission, Detail };
    Mode m_mode = Mode::Empty;

    // --- Submission state ---
    int m_selectedTemplateIdx = -1;
    char m_jobNameBuf[256] = {};
    char m_cmdBuf[512] = {};
    std::vector<std::vector<char>> m_flagBufs;

    // --- Output path state ---
    struct OutputBuf
    {
        int flagIndex = -1;
        std::vector<char> dirBuf;       // 512
        std::vector<char> filenameBuf;  // 256
        bool patternOverridden = false;
    };
    std::vector<OutputBuf> m_outputBufs;
    void resolveOutputPatterns(const JobTemplate& tmpl);

    int m_frameStart = 1, m_frameEnd = 250, m_chunkSize = 1;
    int m_priority = 50, m_maxRetries = 3, m_timeout = 0;
    bool m_hasTimeout = false;
    std::vector<std::string> m_errors;

    // --- Detail state ---
    std::string m_detailJobId;
    JobInfo m_cachedDetailJob;      // last-known-good for flicker prevention
    bool m_hasCachedDetailJob = false;
    UIDataCache::FrameStateSnapshot m_cachedFrameState; // last-known-good frame data
    bool m_pendingCancel = false;
    bool m_pendingRequeue = false;
    bool m_pendingDelete = false;

    void renderEmpty();
    void renderSubmission();
    void renderDetail();
    void onTemplateSelected(int idx);
    void doSubmit();
    std::string currentOS() const;

    // Phase 8: frame grid + progress (data from UIDataCache)
    void renderJobProgress(const JobManifest& manifest);
    void renderFrameGrid(const JobManifest& manifest);

    // Chunk table
    void renderChunkTable(const JobManifest& manifest);
    std::string hostnameForNodeId(const std::string& nodeId) const;
};

} // namespace SR
