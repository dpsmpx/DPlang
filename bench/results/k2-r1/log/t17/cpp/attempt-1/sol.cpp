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
using RawLine = std::vector<std::string>;

bool is_blank_line(const std::string& s) {
    for (char c : s) {
        if (c != ' ') return false;
    }
    return true;
}

void split_words(const std::string& line, std::vector<std::string>& out) {
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        while (i < n && line[i] == ' ') ++i;
        size_t start = i;
        while (i < n && line[i] != ' ') ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
}

size_t raw_len(const RawLine& rl) {
    size_t len = 0;
    for (const auto& w : rl) len += w.size();
    if (!rl.empty()) len += rl.size() - 1;
    return len;
}

std::string join_left(const RawLine& rl) {
    std::string s;
    for (size_t i = 0; i < rl.size(); ++i) {
        if (i > 0) s.push_back(' ');
        s += rl[i];
    }
    return s;
}

std::vector<RawLine> fill_paragraph(const std::vector<std::string>& words, size_t width) {
    std::vector<RawLine> lines;
    RawLine cur;
    size_t cur_len = 0;
    for (const auto& w : words) {
        if (w.size() > width) {
            if (!cur.empty()) {
                lines.push_back(cur);
                cur.clear();
                cur_len = 0;
            }
            std::string rem = w;
            while (rem.size() > width) {
                lines.push_back(RawLine{rem.substr(0, width - 1) + "-"});
                rem = rem.substr(width - 1);
            }
            cur.push_back(rem);
            cur_len = rem.size();
            continue;
        }
        if (cur.empty()) {
            cur.push_back(w);
            cur_len = w.size();
        } else if (cur_len + 1 + w.size() <= width) {
            cur.push_back(w);
            cur_len += 1 + w.size();
        } else {
            lines.push_back(cur);
            cur.clear();
            cur.push_back(w);
            cur_len = w.size();
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

std::string align_line(const RawLine& rl, size_t width, Align align, bool last) {
    const size_t len = raw_len(rl);
    switch (align) {
        case Align::Left:
            return join_left(rl);
        case Align::Right:
            return std::string(width - len, ' ') + join_left(rl);
        case Align::Center:
            return std::string((width - len) / 2, ' ') + join_left(rl);
        case Align::Justify: {
            if (last || rl.size() < 2) return join_left(rl);
            const size_t gaps = rl.size() - 1;
            const size_t extra = width - len;
            const size_t each = extra / gaps;
            const size_t rest = extra % gaps;
            std::string s;
            for (size_t i = 0; i < rl.size(); ++i) {
                if (i > 0) {
                    size_t sp = 1 + each + ((i - 1) < rest ? 1 : 0);
                    s.append(sp, ' ');
                }
                s += rl[i];
            }
            return s;
        }
    }
    return join_left(rl);
}

}  // namespace

WrapResult wrap(const std::string& text, int width, Align align) {
    WrapResult res{WrapError::None, {}};
    if (width < 2 || width > 200) {
        res.error = WrapError::BadWidth;
        return res;
    }
    const size_t w = static_cast<size_t>(width);

    // Разбить на строки текста по '\n'.
    std::vector<std::string> src_lines;
    {
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                src_lines.push_back(text.substr(start, i - start));
                start = i + 1;
            }
        }
    }

    // Собрать абзацы как списки слов.
    std::vector<std::vector<std::string>> paragraphs;
    std::vector<std::string> cur;
    bool in_par = false;
    for (const auto& line : src_lines) {
        if (is_blank_line(line)) {
            if (in_par) {
                paragraphs.push_back(cur);
                cur.clear();
                in_par = false;
            }
        } else {
            in_par = true;
            split_words(line, cur);
        }
    }
    if (in_par) paragraphs.push_back(cur);

    bool first = true;
    for (const auto& words : paragraphs) {
        if (words.empty()) continue;
        std::vector<RawLine> raw = fill_paragraph(words, w);
        if (raw.empty()) continue;
        if (!first) res.lines.push_back("");
        first = false;
        for (size_t i = 0; i < raw.size(); ++i) {
            res.lines.push_back(align_line(raw[i], w, align, i + 1 == raw.size()));
        }
    }
    return res;
}
