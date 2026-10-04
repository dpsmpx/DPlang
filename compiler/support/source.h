// Исходные файлы и позиции в них.
//
// Позиция в файле — смещение в байтах. Для сообщений смещение
// переводится в строку и столбец (с 1); столбец считается в символах
// UTF-8, а не в байтах.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dp {

using FileId = uint32_t;

// Участок исходника: [begin, end) в байтах файла file.
struct Span {
    FileId file = 0;
    uint32_t begin = 0;
    uint32_t end = 0;
};

struct LineCol {
    uint32_t line = 1;  // с 1
    uint32_t col = 1;   // с 1, в символах UTF-8
};

class SourceFile {
public:
    SourceFile(std::string path, std::string text);

    const std::string& path() const { return path_; }
    const std::string& text() const { return text_; }
    LineCol line_col(uint32_t offset) const;
    // Текст строки line (с 1) без перевода строки.
    std::string_view line_text(uint32_t line) const;
    uint32_t line_count() const { return static_cast<uint32_t>(line_starts_.size()); }

private:
    std::string path_;
    std::string text_;
    std::vector<uint32_t> line_starts_;
};

// Все файлы одного запуска компилятора; FileId — индекс в нём.
class SourceMap {
public:
    FileId add(std::string path, std::string text);
    const SourceFile& file(FileId id) const { return files_[id]; }
    size_t size() const { return files_.size(); }

private:
    std::vector<SourceFile> files_;
};

// Читает файл целиком; false — файла нет или он не читается.
bool read_file(const std::string& path, std::string& out);

}  // namespace dp
