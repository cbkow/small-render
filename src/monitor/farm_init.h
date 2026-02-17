#pragma once

#include <filesystem>
#include <string>

namespace SR {

class FarmInit
{
public:
    struct Result
    {
        bool success = false;
        std::filesystem::path farmPath;
        std::string error;
    };

    // Initialize farm directory structure at syncRoot/SmallRender-v1/.
    // Creates dirs + farm.json on first run; ensures own node dirs on every run.
    static Result init(const std::filesystem::path& syncRoot, const std::string& nodeId);
};

} // namespace SR
