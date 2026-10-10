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

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = text.find('\n', start);
        if (p == std::string::npos) {
            out.push_back(text.substr(start));
            break;
        }
        out.push_back(text.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

void split_words(const std::string& line, std::vector<std::string>& words) {
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        while (i < n && line[i] == ' ') ++i;
        if (i >= n) break;
        size_t j = i;
        while (j < n && line[j] != ' ') ++j;
        words.push_back(line.substr(i, j - i));
        i = j;
    }
}

bool blank(const std::string& line) {
    for (char c : line) {
        if (c != ' ') return false;
    }
    return true;
}

std::string join(const std::vector<std::string>& ws) {
    std::string s;
    for (size_t i = 0; i < ws.size(); ++i) {
        if (i > 0) s += ' ';
        s += ws[i];
    }
    return s;
}

std::string render(const std::vector<std::string>& ws, size_t width, Align align, bool last) {
    std::string s = join(ws);
    switch (align) {
        case Align::Left:
            return s;
        case Align::Right:
            return std::string(width - s.size(), ' ') + s;
        case Align::Center:
            return std::string((width - s.size()) / 2, ' ') + s;
        case Align::Justify: {
            if (last || ws.size() < 2) return s;
            size_t gaps = ws.size() - 1;
            size_t extra = width - s.size();
            size_t each = extra / gaps;
            size_t rem = extra % gaps;
            std::string r;
            for (size_t i = 0; i < ws.size(); ++i) {
                if (i > 0) {
                    size_t sp = 1 + each + ((i - 1) < rem ? 1 : 0);
                    r += std::string(sp, ' ');
                }
                r += ws[i];
            }
            return r;
        }
    }
    return s;
}

void fill_paragraph(const std::vector<std::string>& words, size_t width, Align align,
                    std::vector<std::string>& out) {
    std::vector<std::vector<std::string>> lines;
    std::vector<std::string> cur;
    size_t curlen = 0;
    for (const std::string& w : words) {
        if (w.size() > width) {
            if (!cur.empty()) {
                lines.push_back(cur);
                cur.clear();
                curlen = 0;
            }
            std::string rest = w;
            while (rest.size() > width) {
                lines.push_back({rest.substr(0, width - 1) + "-"});
                rest = rest.substr(width - 1);
            }
            cur.push_back(rest);
            curlen = rest.size();
        } else if (cur.empty()) {
            cur.push_back(w);
            curlen = w.size();
        } else if (curlen + 1 + w.size() <= width) {
            cur.push_back(w);
            curlen += 1 + w.size();
        } else {
            lines.push_back(cur);
            cur.clear();
            cur.push_back(w);
            curlen = w.size();
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    for (size_t i = 0; i < lines.size(); ++i) {
        out.push_back(render(lines[i], width, align, i + 1 == lines.size()));
    }
}

}  // namespace

WrapResult wrap(const std::string& text, int width, Align align) {
    if (width < 2 || width > 200) return WrapResult{WrapError::BadWidth, {}};
    const size_t w = static_cast<size_t>(width);
    std::vector<std::string> out;
    std::vector<std::string> lines = split_lines(text);
    std::vector<std::string> words;
    bool any_para = false;
    for (size_t i = 0; i <= lines.size(); ++i) {
        bool end_para = (i == lines.size()) || blank(lines[i]);
        if (end_para) {
            if (!words.empty()) {
                if (any_para) out.push_back("");
                fill_paragraph(words, w, align, out);
                any_para = true;
                words.clear();
            }
        } else {
            split_words(lines[i], words);
        }
    }
    return WrapResult{WrapError::None, out};
}
