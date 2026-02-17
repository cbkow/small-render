#pragma once

#include <string>

namespace SR {

class MonitorApp; // forward

class SettingsPanel
{
public:
    void init(MonitorApp* app);
    void render();

private:
    MonitorApp* m_app = nullptr;
    bool m_needsReload = true;

    // Editable copies of config values (applied on Save)
    char m_syncRootBuf[512] = {};
    int  m_timingPreset = 0;
    char m_tagsBuf[256] = {};
    bool m_isCoordinator = false;
    bool m_autoStartAgent = true;
    bool m_udpEnabled = true;
    int  m_udpPort = 4242;
    bool m_showNotifications = true;
    float m_fontScale = 1.0f;

    // Custom timing (editable when preset is Custom)
    int m_heartbeatMs = 5000;
    int m_scanMs = 3000;
    int m_claimSettleMs = 3000;
    int m_deadThresholdScans = 3;

    // Track sync root to detect changes on save
    std::string m_savedSyncRoot;

    // Font scale presets
    static constexpr float FONT_SCALE_SMALL  = 0.75f;
    static constexpr float FONT_SCALE_MEDIUM = 1.0f;
    static constexpr float FONT_SCALE_LARGE  = 1.25f;
    static constexpr float FONT_SCALE_XLARGE = 1.5f;

    void loadFromConfig();
    void applyToConfig();
    void drawFontSizeSection();
    void drawFontPreview();
};

} // namespace SR
