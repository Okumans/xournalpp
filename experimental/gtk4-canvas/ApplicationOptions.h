#pragma once

#include <optional>
#include <string>

#include "BehaviorDefaults.h"

namespace prototype {

struct ApplicationOptions {
    std::string pdf;
    std::string nativeDocument;
    std::string worker;
    std::string rasterExecutable;
    std::string configDirectory;
    std::string pluginDirectory;
    std::string smokeDirectory;
    int autosaveSeconds = behavior::autosaveTimeoutSeconds;
    std::optional<int> autosaveSecondsOverride;
    bool dark = false;
    bool explicitDark = false;
    bool disableAudio = false;
    bool verifyPlugins = false;
    bool verifySearch = false;
    bool verifySearchScanAll = false;
    std::string verifySearchQuery;
    int verifySearchPage = 0;
    bool verifyScrollMotion = false;
    bool verifySmartSelectPerformance = false;
    bool verifySidebarPerformance = false;
    bool verifySidebarStartup = false;
    bool verifyPageLayout = false;
    bool verifyPresentationMode = false;
    bool verifyPenLineStyle = false;
    bool verifyBeamer = false;
    bool verifyLayerActions = false;
    bool verifyLatex = false;
    bool verifyTextClipboard = false;
    int width = 1100;
    int height = 820;
};

ApplicationOptions parseApplicationOptions(int argc, char** argv);

}  // namespace prototype
