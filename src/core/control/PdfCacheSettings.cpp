#include "PdfCache.h"

#include "control/settings/Settings.h"  // for Settings
#include "util/safe_casts.h"            // for as_unsigned

PdfCache::PdfCache(const XojPdfDocument& doc, Settings* settings):
        PdfCache(doc, settings ? as_unsigned(settings->getPdfPageCacheSize()) : 0,
                 settings ? settings->getPDFPageRerenderThreshold() : 5.0) {}

void PdfCache::updateSettings(Settings* settings) {
    if (settings) {
        setMaxSize(as_unsigned(settings->getPdfPageCacheSize()));
        setRefreshThreshold(settings->getPDFPageRerenderThreshold());
    }
}
