#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xmb::assets {

class QrcArchive {
public:
    bool load_from_file(const std::filesystem::path& path, std::string* out_error = nullptr);
    bool load_from_memory(std::span<const uint8_t> bytes, std::string* out_error = nullptr);

    [[nodiscard]] bool empty() const noexcept { return files_.empty(); }
    [[nodiscard]] std::size_t file_count() const noexcept { return files_.size(); }
    [[nodiscard]] uint32_t version() const noexcept { return version_; }
    [[nodiscard]] bool was_compressed() const noexcept { return was_compressed_; }

    [[nodiscard]] bool has(std::string_view rel_path) const;
    [[nodiscard]] std::span<const uint8_t> read(std::string_view rel_path) const;
    [[nodiscard]] std::string read_text(std::string_view rel_path) const;

    [[nodiscard]] std::vector<std::string> list_files(std::string_view prefix = "") const;
    [[nodiscard]] std::vector<std::string> list_overrides() const;

private:
    bool parse_qrcf(std::span<const uint8_t> raw, std::string* out_error);

    uint32_t version_ = 0;
    bool was_compressed_ = false;
    std::map<std::string, std::vector<uint8_t>, std::less<>> files_;
};

} // namespace xmb::assets
