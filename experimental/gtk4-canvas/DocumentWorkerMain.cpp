#include "DocumentWorker.h"

#include <clocale>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::setlocale(LC_NUMERIC, "C");

    std::vector<std::string> arguments;
    arguments.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    const auto result = prototype::bridge::runDocumentCommand(arguments);
    std::cout << result.standardOutput;
    std::cerr << result.standardError;
    return result.exitCode;
}
