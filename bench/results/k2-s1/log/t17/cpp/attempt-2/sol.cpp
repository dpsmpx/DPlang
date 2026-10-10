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

// Строка абзаца до выравнивания: слова и признак последней строки абзаца.
struct RawLine {
    std::vector<std::string> words;
    bool last = false;
};

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
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

bool blank_line(const std::string& s) {
    for (char c : s) {
        if (c != ' ') return false;
    }
    return true;
}

void append_words(const std::string& s, std::vector<std::string>& words) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        while (i < n && s[i] == ' ') ++i;
        if (i >= n) break;
        size_t j = i;
        while (j < n && s[j] != ' ') ++j;
        words.push_back(s.substr(i, j - i));
        i = j;
    }
}

std::vector<RawLine> fill(const std::vector<std::string>& words, size_t width) {
    std::vector<RawLine> out;
    RawLine cur;
    size_t cur_len = 0;
    auto finish = [&]() {
        if (!cur.words.empty()) {
            out.push_back(cur);
            cur = RawLine{};
            cur_len = 0;
        }
    };
    for (const std::string& w : words) {
        if (w.size() > width) {
            finish();
            std::string rest = w;
            while (rest.size() > width) {
                RawLine piece;
                piece.words.push_back(rest.substr(0, width - 1) + "-");
                out.push_back(piece);
                rest = rest.substr(width - 1);
            }
            cur.words.push_back(rest);
            cur_len = rest.size();
            continue;
        }
        if (cur.words.empty()) {
            cur.words.push_back(w);
            cur_len = w.size();
        } else if (cur_len + 1 + w.size() <= width) {
            cur.words.push_back(w);
            cur_len += 1 + w.size();
        } else {
            finish();
            cur.words.push_back(w);
            cur_len = w.size();
        }
    }
    if (!cur.words.empty()) {
        cur.last = true;
        out.push_back(cur);
    }
    return out;
}

std::string join_left(const std::vector<std::string>& words) {
    std::string s;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) s += ' ';
        s += words[i];
    }
    return s;
}

std::string render(const RawLine& rl, size_t width, Align align) {
    std::string left = join_left(rl.words);
    switch (align) {
        case Align::Left:
            return left;
        case Align::Right:
            return std::string(width - left.size(), ' ') + left;
        case Align::Center:
            return std::string((width - left.size()) / 2, ' ') + left;
        case Align::Justify: {
            if (rl.last || rl.words.size() < 2) return left;
            size_t letters = 0;
            for (const std::string& w : rl.words) letters += w.size();
            size_t gaps = rl.words.size() - 1;
            size_t spaces = width - letters;
            size_t base = spaces / gaps;
            size_t extra = spaces % gaps;
            std::string s;
            for (size_t i = 0; i < rl.words.size(); ++i) {
                if (i) s += std::string(base + (i - 1 < extra ? 1 : 0), ' ');
                s += rl.words[i];
            }
            return s;
        }
    }
    return left;
}

}  // namespace

WrapResult wrap(const std::string& text, int width, Align align) {
    WrapResult res;
    if (width < 2 || width > 200) {
        res.error = WrapError::BadWidth;
        return res;
    }
    res.error = WrapError::None;
    const size_t w = static_cast<size_t>(width);

    std::vector<std::string> lines = split_lines(text);
    std::vector<std::vector<std::string>> paragraphs;
    bool in_para = false;
    for (const std::string& ln : lines) {
        if (blank_line(ln)) {
            in_para = false;
            continue;
        }
        if (!in_para) {
            paragraphs.emplace_back();
            in_para = true;
        }
        append_words(ln, paragraphs.back());
    }

    for (size_t p = 0; p < paragraphs.size(); ++p) {
        if (p) res.lines.push_back("");
        std::vector<RawLine> raw = fill(paragraphs[p], w);
        for (const RawLine& rl : raw) res.lines.push_back(render(rl, w, align));
    }
    return res;
}
