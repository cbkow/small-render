#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>
#include <chrono>

namespace SR {

class MessageDedup
{
public:
    // Returns true if already seen. Records the ID if new.
    bool isDuplicate(const std::string& msgId)
    {
        auto now = nowMs();
        auto it = m_seen.find(msgId);
        if (it != m_seen.end())
            return true;
        m_seen[msgId] = now;
        return false;
    }

    // Drop entries older than 60 seconds. Call every ~30s.
    void purge()
    {
        auto cutoff = nowMs() - 60000;
        for (auto it = m_seen.begin(); it != m_seen.end(); )
        {
            if (it->second < cutoff)
                it = m_seen.erase(it);
            else
                ++it;
        }
    }

private:
    static int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::unordered_map<std::string, int64_t> m_seen;
};

} // namespace SR
