#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace heliotis::internal {

struct C4UtilitySdkLayout {
    std::filesystem::path root;
    std::filesystem::path c4HdlConfig;
    std::filesystem::path c4HdlHeader;
    std::filesystem::path c4HdlLibrary;
    std::filesystem::path c4HdlRuntime;
    std::filesystem::path genApiRuntime;
    std::filesystem::path gcBaseRuntime;
    std::filesystem::path diaphusProducer;

    [[nodiscard]] bool isComplete() const;
    [[nodiscard]] std::vector<std::filesystem::path> missingPaths() const;
    [[nodiscard]] std::string diagnostic() const;
};

class C4UtilitySdkRuntime final {
public:
    [[nodiscard]] static C4UtilitySdkLayout fromRoot(const std::filesystem::path& root);
};

} // namespace heliotis::internal
