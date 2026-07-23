#include "Internal/C4UtilitySdkRuntime.h"

#include <sstream>

namespace heliotis::internal {

namespace {

std::filesystem::path hdlRoot(const std::filesystem::path& root)
{
    return root / "c4hdl" / "win64-x64";
}

} // namespace

C4UtilitySdkLayout C4UtilitySdkRuntime::fromRoot(const std::filesystem::path& root)
{
    const auto c4HdlRoot = hdlRoot(root);
    C4UtilitySdkLayout layout;
    layout.root = root;
    layout.c4HdlConfig = c4HdlRoot / "c" / "cmake" / "C4HdlCConfig.cmake";
    layout.c4HdlHeader = c4HdlRoot / "c" / "include" / "C4HdlC.h";
    layout.c4HdlLibrary = c4HdlRoot / "c" / "lib" / "C4HdlC.lib";
    layout.c4HdlRuntime = c4HdlRoot / "c" / "bin" / "C4HdlC.dll";
    layout.genApiRuntime = c4HdlRoot / "genicam" / "bin" / "GenApi_MD_VC141_v3_2.dll";
    layout.gcBaseRuntime = c4HdlRoot / "genicam" / "bin" / "GCBase_MD_VC141_v3_2.dll";
    layout.diaphusProducer = root / "diaphus" / "win64-x64" / "diaphus.cti";
    return layout;
}

bool C4UtilitySdkLayout::isComplete() const
{
    return missingPaths().empty();
}

std::vector<std::filesystem::path> C4UtilitySdkLayout::missingPaths() const
{
    const std::vector<std::filesystem::path> requiredPaths {
        c4HdlConfig,
        c4HdlHeader,
        c4HdlLibrary,
        c4HdlRuntime,
        genApiRuntime,
        gcBaseRuntime,
        diaphusProducer,
    };

    std::vector<std::filesystem::path> missing;
    for (const auto& path : requiredPaths) {
        if (!std::filesystem::is_regular_file(path)) missing.push_back(path);
    }
    return missing;
}

std::string C4UtilitySdkLayout::diagnostic() const
{
    const auto missing = missingPaths();
    if (missing.empty()) return "C4Utility SDK layout is complete.";

    std::ostringstream stream;
    stream << "C4Utility SDK layout is incomplete:";
    for (const auto& path : missing) stream << "\n- " << path.string();
    return stream.str();
}

} // namespace heliotis::internal
