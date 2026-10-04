#include "support/source.h"

#include <cstdio>

namespace dp {

SourceFile::SourceFile(std::string path, std::string text) : path_(std::move(path)), text_(std::move(text)) {
    line_starts_.push_back(0);
    for (uint32_t i = 0; i < text_.size(); i++) {
        if (text_[i] == '\n') line_starts_.push_back(i + 1);
    }
}

LineCol SourceFile::line_col(uint32_t offset) const {
    if (offset > text_.size()) offset = static_cast<uint32_t>(text_.size());
    // Последняя строка, начало которой не дальше offset.
    uint32_t lo = 0, hi = static_cast<uint32_t>(line_starts_.size());
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (line_starts_[mid] <= offset) lo = mid;
        else hi = mid;
    }
    uint32_t col = 1;
    for (uint32_t i = line_starts_[lo]; i < offset; i++) {
        // Продолжающие байты UTF-8 (10xxxxxx) символа не начинают.
        if ((static_cast<unsigned char>(text_[i]) & 0xC0) != 0x80) col++;
    }
    return {lo + 1, col};
}

std::string_view SourceFile::line_text(uint32_t line) const {
    if (line == 0 || line > line_starts_.size()) return {};
    uint32_t begin = line_starts_[line - 1];
    uint32_t end = line < line_starts_.size() ? line_starts_[line] - 1 : static_cast<uint32_t>(text_.size());
    if (end > begin && text_[end - 1] == '\r') end--;
    return std::string_view(text_).substr(begin, end - begin);
}

FileId SourceMap::add(std::string path, std::string text) {
    files_.emplace_back(std::move(path), std::move(text));
    return static_cast<FileId>(files_.size() - 1);
}

bool read_file(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

}  // namespace dp
