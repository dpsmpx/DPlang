// t17: заготовка — напишите тела функций по task.md.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

enum class Align { Left, Right, Center, Justify };
enum class WrapError { None, BadWidth };

struct WrapResult {
    WrapError error;
    std::vector<std::string> lines;
};

namespace {

// Строка абзаца до выравнивания: слова по порядку.
struct RawLine {
    std::vector<std::string> words;
    bool last_of_paragraph = false;
};

std::string join_left(const RawLine& l) {
    std::string s;
    for (size_t i = 0; i < l.words.size(); ++i) {
        if (i > 0) s += ' ';
        s += l.words[i];
    }
    return s;
}

std::string render(const RawLine& l, size_t width, Align align) {
    std::string left = join_left(l);
    switch (align) {
        case Align::Left:
            return left;
        case Align::Right:
            return std::string(width - left.size(), ' ') + left;
        case Align::Center:
            return std::string((width - left.size()) / 2, ' ') + left;
        case Align::Justify: {
            if (l.last_of_paragraph || l.words.size() < 2) return left;
            size_t total = 0;
            for (const auto& w : l.words) total += w.size();
            size_t gaps = l.words.size() - 1;
            size_t spaces = width - total;
            size_t each = spaces / gaps;
            size_t extra = spaces % gaps;
            std::string s;
            for (size_t i = 0; i < l.words.size(); ++i) {
                if (i > 0) {
                    size_t k = each + (i - 1 < extra ? 1 : 0);
                    s.append(k, ' ');
                }
                s += l.words[i];
            }
            return s;
        }
    }
    return left;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

bool is_blank(const std::string& line) {
    for (char c : line) {
        if (c != ' ') return false;
    }
    return true;
}

void split_words(const std::string& line, std::vector<std::string>& out) {
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        while (i < n && line[i] == ' ') ++i;
        if (i >= n) break;
        size_t j = i;
        while (j < n && line[j] != ' ') ++j;
        out.push_back(line.substr(i, j - i));
        i = j;
    }
}

std::vector<RawLine> fill_paragraph(const std::vector<std::string>& words, size_t width) {
    std::vector<RawLine> lines;
    RawLine cur;
    size_t cur_len = 0;
    auto flush = [&]() {
        if (!cur.words.empty()) {
            lines.push_back(cur);
            cur = RawLine{};
            cur_len = 0;
        }
    };
    for (const std::string& w : words) {
        if (w.size() <= width) {
            if (cur.words.empty()) {
                cur.words.push_back(w);
                cur_len = w.size();
            } else if (cur_len + 1 + w.size() <= width) {
                cur.words.push_back(w);
                cur_len += 1 + w.size();
            } else {
                flush();
                cur.words.push_back(w);
                cur_len = w.size();
            }
            continue;
        }
        flush();
        std::string rest = w;
        while (rest.size() > width) {
            RawLine piece;
            piece.words.push_back(rest.substr(0, width - 1) + "-");
            lines.push_back(piece);
            rest = rest.substr(width - 1);
        }
        cur.words.push_back(rest);
        cur_len = rest.size();
    }
    flush();
    if (!lines.empty()) lines.back().last_of_paragraph = true;
    return lines;
}

}  // namespace

WrapResult wrap(const std::string& text, int width, Align align) {
    WrapResult res{WrapError::None, {}};
    if (width < 2 || width > 200) {
        res.error = WrapError::BadWidth;
        return res;
    }
    const size_t w = static_cast<size_t>(width);
    std::vector<std::string> src = split_lines(text);
    std::vector<std::vector<std::string>> paragraphs;
    bool in_par = false;
    for (const std::string& line : src) {
        if (is_blank(line)) {
            in_par = false;
            continue;
        }
        if (!in_par) {
            paragraphs.emplace_back();
            in_par = true;
        }
        split_words(line, paragraphs.back());
    }
    bool first = true;
    for (const auto& words : paragraphs) {
        std::vector<RawLine> lines = fill_paragraph(words, w);
        if (lines.empty()) continue;
        if (!first) res.lines.push_back("");
        first = false;
        for (const RawLine& l : lines) {
            res.lines.push_back(render(l, w, align));
        }
    }
    return res;
}
