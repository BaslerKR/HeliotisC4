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
    return layout.isComplete() ? 0 : 1;
}
