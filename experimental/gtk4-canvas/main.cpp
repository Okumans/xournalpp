#include <exception>
#include <iostream>
#include <string_view>
#include <utility>

#include "PageRasterRenderer.h"
#include "Prototype.h"

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--gtk4-page-raster-worker") {
            return prototype::runPageRasterWorker(argc, argv);
        }
        auto options = prototype::parseApplicationOptions(argc, argv);
        prototype::gtk4::Prototype app(std::move(options));
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
