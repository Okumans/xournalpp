#pragma once

#include <string>
#include <vector>

namespace prototype::bridge {

struct DocumentCommandResult {
    int exitCode = 0;
    std::string standardOutput;
    std::string standardError;
};

/** Run the upstream document load/save/render command in the current process. */
DocumentCommandResult runDocumentCommand(const std::vector<std::string>& arguments);

}  // namespace prototype::bridge
