#pragma once

namespace SR {

class MonitorApp; // forward

class NodePanel
{
public:
    void init(MonitorApp* app);
    void render();
    bool visible = true;

private:
    void renderLocalNode();
    void renderPeerList();

    MonitorApp* m_app = nullptr;
};

} // namespace SR
