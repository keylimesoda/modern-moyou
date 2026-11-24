#include "assets/qrc_loader.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>

namespace xmb::assets {

namespace {

constexpr uint32_t kInvalidOffset = 0xFFFFFFFFu;

bool read_be32(std::span<const uint8_t> buf, std::size_t offset, uint32_t& out) {
    if (offset + 4 > buf.size()) {
        return false;
    }
    out = (static_cast<uint32_t>(buf[offset + 0]) << 24) |
          (static_cast<uint32_t>(buf[offset + 1]) << 16) |
          (static_cast<uint32_t>(buf[offset + 2]) << 8) |
          static_cast<uint32_t>(buf[offset + 3]);
    return true;
}

bool read_cstr(std::span<const uint8_t> buf, std::size_t offset, std::string& out) {
    if (offset >= buf.size()) {
        return false;
    }
    std::size_t end = offset;
    while (end < buf.size() && buf[end] != 0) {
        ++end;
    }
    if (end >= buf.size()) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(buf.data() + offset), end - offset);
    return true;
}

void set_error(std::string* out_error, std::string msg) {
    if (out_error) {
        *out_error = std::move(msg);
    }
}

} // namespace

bool QrcArchive::load_from_file(const std::filesystem::path& path, std::string* out_error) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        set_error(out_error, "Failed to open QRC file: " + path.string());
        return false;
    }
    const std::streamsize size = ifs.tellg();
    if (size < 8) {
        set_error(out_error, "QRC file too small: " + path.string());
        return false;
    }
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> raw(static_cast<std::size_t>(size));
    if (!ifs.read(reinterpret_cast<char*>(raw.data()), size)) {
        set_error(out_error, "Failed to read QRC file: " + path.string());
        return false;
    }
    return load_from_memory(raw, out_error);
}

bool QrcArchive::load_from_memory(std::span<const uint8_t> bytes, std::string* out_error) {
    files_.clear();
    version_ = 0;
    was_compressed_ = false;

    if (bytes.size() < 8) {
        set_error(out_error, "QRC buffer too small");
        return false;
    }

    if (std::memcmp(bytes.data(), "QRCC", 4) == 0) {
        uint32_t uncompressed_size = 0;
        if (!read_be32(bytes, 4, uncompressed_size)) {
            set_error(out_error, "Truncated QRCC header");
            return false;
        }
        std::vector<uint8_t> decompressed(uncompressed_size);
        uLongf dest_len = uncompressed_size;
        const int zret = uncompress(
            decompressed.data(),
            &dest_len,
            bytes.data() + 8,
            static_cast<uLong>(bytes.size() - 8));
        if (zret != Z_OK) {
            set_error(out_error, "zlib uncompress failed with code " + std::to_string(zret));
            return false;
        }
        if (dest_len != uncompressed_size) {
            set_error(out_error, "Decompressed size mismatch");
            return false;
        }
        was_compressed_ = true;
        return parse_qrcf(decompressed, out_error);
    }

    if (std::memcmp(bytes.data(), "QRCF", 4) == 0) {
        was_compressed_ = false;
        return parse_qrcf(bytes, out_error);
    }

    set_error(out_error, "Unsupported QRC magic (expected QRCC or QRCF)");
    return false;
}

bool QrcArchive::parse_qrcf(std::span<const uint8_t> raw, std::string* out_error) {
    if (raw.size() < 56 || std::memcmp(raw.data(), "QRCF", 4) != 0) {
        set_error(out_error, "Invalid QRCF payload header");
        return false;
    }

    uint32_t header[13]{};
    for (std::size_t i = 0; i < 13; ++i) {
        if (!read_be32(raw, 4 + i * 4, header[i])) {
            set_error(out_error, "Truncated QRCF header");
            return false;
        }
    }

    version_ = header[0];
    const uint32_t tbl_off  = header[1], tbl_sz  = header[2];
    const uint32_t id_off   = header[3], id_sz   = header[4];
    const uint32_t str_off  = header[5], str_sz  = header[6];
    const uint32_t data_off = header[11], data_sz = header[12];

    auto slice = [&](uint32_t off, uint32_t sz, std::span<const uint8_t>& out) -> bool {
        if (static_cast<uint64_t>(off) + sz > raw.size()) {
            return false;
        }
        out = raw.subspan(off, sz);
        return true;
    };

    std::span<const uint8_t> tbl, id_tbl, str_tbl, data_blob;
    if (!slice(tbl_off, tbl_sz, tbl) ||
        !slice(id_off, id_sz, id_tbl) ||
        !slice(str_off, str_sz, str_tbl) ||
        !slice(data_off, data_sz, data_blob)) {
        set_error(out_error, "QRCF section offsets exceed buffer bounds");
        return false;
    }

    // Iterative DFS over the QRCF node tree to avoid recursion depth issues
    std::vector<uint32_t> stack;
    if (!tbl.empty()) {
        stack.push_back(0);
    }

    while (!stack.empty()) {
        uint32_t node_off = stack.back();
        stack.pop_back();

        while (node_off != kInvalidOffset) {
            uint32_t name_off = 0, num_attrs = 0, parent = 0, prev_sib = 0;
            uint32_t next_sib = 0, first_child = 0, last_child = 0;
            if (!read_be32(tbl, node_off + 0, name_off) ||
                !read_be32(tbl, node_off + 4, num_attrs) ||
                !read_be32(tbl, node_off + 8, parent) ||
                !read_be32(tbl, node_off + 12, prev_sib) ||
                !read_be32(tbl, node_off + 16, next_sib) ||
                !read_be32(tbl, node_off + 20, first_child) ||
                !read_be32(tbl, node_off + 24, last_child)) {
                set_error(out_error, "Truncated QRCF node entry");
                return false;
            }
            (void)parent;
            (void)prev_sib;
            (void)last_child;

            std::string tag;
            if (!read_cstr(str_tbl, name_off, tag)) {
                set_error(out_error, "Invalid QRCF tag string offset");
                return false;
            }

            bool has_src = false;
            bool has_id = false;
            uint32_t blob_off = 0, blob_sz = 0, id_entry_off = 0;

            std::size_t attr_off = static_cast<std::size_t>(node_off) + 28;
            for (uint32_t a = 0; a < num_attrs; ++a) {
                uint32_t a_name_off = 0, a_type = 0, a_val1 = 0, a_val2 = 0;
                if (!read_be32(tbl, attr_off + 0, a_name_off) ||
                    !read_be32(tbl, attr_off + 4, a_type) ||
                    !read_be32(tbl, attr_off + 8, a_val1) ||
                    !read_be32(tbl, attr_off + 12, a_val2)) {
                    set_error(out_error, "Truncated QRCF attribute entry");
                    return false;
                }
                (void)a_type;
                std::string a_name;
                if (!read_cstr(str_tbl, a_name_off, a_name)) {
                    set_error(out_error, "Invalid QRCF attribute name offset");
                    return false;
                }
                if (a_name == "src") {
                    has_src = true;
                    blob_off = a_val1;
                    blob_sz = a_val2;
                } else if (a_name == "id") {
                    has_id = true;
                    id_entry_off = a_val1;
                }
                attr_off += 16;
            }

            if (tag == "file" && has_src && has_id) {
                std::string rel_path;
                if (!read_cstr(id_tbl, static_cast<std::size_t>(id_entry_off) + 4, rel_path)) {
                    set_error(out_error, "Invalid QRCF id_tbl path offset");
                    return false;
                }
                if (static_cast<uint64_t>(blob_off) + blob_sz > data_blob.size()) {
                    set_error(out_error, "QRCF file payload out of bounds: " + rel_path);
                    return false;
                }
                std::vector<uint8_t> payload(
                    data_blob.begin() + blob_off,
                    data_blob.begin() + blob_off + blob_sz);
                files_.insert_or_assign(std::move(rel_path), std::move(payload));
            }

            if (first_child != kInvalidOffset) {
                stack.push_back(first_child);
            }
            node_off = next_sib;
        }
    }

    return true;
}

bool QrcArchive::has(std::string_view rel_path) const {
    return files_.find(rel_path) != files_.end();
}

std::span<const uint8_t> QrcArchive::read(std::string_view rel_path) const {
    auto it = files_.find(rel_path);
    if (it == files_.end()) {
        return {};
    }
    return it->second;
}

std::string QrcArchive::read_text(std::string_view rel_path) const {
    auto bytes = read(rel_path);
    if (bytes.empty()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::vector<std::string> QrcArchive::list_files(std::string_view prefix) const {
    std::vector<std::string> result;
    for (const auto& [path, _] : files_) {
        if (prefix.empty() || path.starts_with(prefix)) {
            result.push_back(path);
        }
    }
    return result;
}

std::vector<std::string> QrcArchive::list_overrides() const {
    std::set<std::string> names;
    constexpr std::string_view kPrefix = "override/";
    for (const auto& [path, _] : files_) {
        if (path.starts_with(kPrefix)) {
            const auto rest = std::string_view(path).substr(kPrefix.size());
            const auto slash = rest.find('/');
            if (slash != std::string_view::npos && slash > 0) {
                names.emplace(rest.substr(0, slash));
            }
        }
    }
    return {names.begin(), names.end()};
}

} // namespace xmb::assets
