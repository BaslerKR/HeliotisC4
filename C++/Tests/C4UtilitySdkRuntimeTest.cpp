#include "Internal/C4UtilitySdkRuntime.h"

#include <iostream>

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "Usage: heliotis_c4_sdk_layout_test <C4Utility-root>\n";
        return 2;
    }

    const auto layout = heliotis::internal::C4UtilitySdkRuntime::fromRoot(argv[1]);
    std::cout << layout.diagnostic() << '\n';
    if (!layout.isComplete()) return 1;

    const std::string expectedRoot =
        heliotis::internal::C4UtilitySdkRuntime::processRootEnvironmentValue(argv[1]);
    if (expectedRoot.empty()
        || (expectedRoot.back() != '/' && expectedRoot.back() != '\\')) {
        std::cerr << "C4UTILITY_ROOT must retain a trailing directory separator.\n";
        return 1;
    }

    return 0;
}
