// SPDX-License-Identifier: Apache-2.0
#include "midgard/pak.h"

#include <miniz.h>

namespace midgard {

struct Pak::Impl {
    mutable mz_zip_archive zip{};
    std::filesystem::path file;
};

Pak::Pak(const std::filesystem::path& file) : impl_(std::make_unique<Impl>()) {
    impl_->file = file;
    const std::u8string u8 = file.u8string();
    const std::string utf8(u8.begin(), u8.end());
    if (!mz_zip_reader_init_file(&impl_->zip, utf8.c_str(), 0))
        throw AssetError("cannot open " + file.string() + ": " +
                         mz_zip_get_error_string(mz_zip_get_last_error(&impl_->zip)));
}

Pak::~Pak() {
    mz_zip_reader_end(&impl_->zip);
}

bool Pak::contains(const std::string& path) const {
    return mz_zip_reader_locate_file(&impl_->zip, path.c_str(), nullptr, 0) >= 0;
}

std::vector<uint8_t> Pak::read(const std::string& path) const {
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&impl_->zip, path.c_str(), &size, 0);
    if (!data) throw AssetError(path + " is not in " + impl_->file.filename().string());
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> out(bytes, bytes + size);
    mz_free(data);
    return out;
}

std::vector<std::string> Pak::files() const {
    std::vector<std::string> names;
    const mz_uint count = mz_zip_reader_get_num_files(&impl_->zip);
    for (mz_uint i = 0; i < count; ++i) {
        char name[1024];
        mz_zip_reader_get_filename(&impl_->zip, i, name, sizeof(name));
        names.emplace_back(name);
    }
    return names;
}

}  // namespace midgard
