// t17: эталон на C++20.
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

static std::vector<std::vector<std::string>> paragraphs(const std::string& text) {
    std::vector<std::vector<std::string>> out;
    std::vector<std::string> cur;
    size_t i = 0;
    while (i <= text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string::npos) j = text.size();
        std::string line = text.substr(i, j - i);
        std::vector<std::string> words;
        std::string w;
        for (char c : line) {
            if (c == ' ') {
                if (!w.empty()) words.push_back(w);
                w.clear();
            } else {
                w += c;
            }
        }
        if (!w.empty()) words.push_back(w);
        if (words.empty()) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.insert(cur.end(), words.begin(), words.end());
        }
        i = j + 1;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static std::string join(const std::vector<std::string>& words) {
    std::string s;
    for (size_t i = 0; i < words.size(); i++) {
        if (i > 0) s += ' ';
        s += words[i];
    }
    return s;
}

static std::vector<std::vector<std::string>> fill(const std::vector<std::string>& words, int width) {
    std::vector<std::vector<std::string>> lines;
    std::vector<std::string> cur;
    int cur_len = 0;
    for (const std::string& w : words) {
        int n = (int)w.size();
        if (n <= width) {
            if (cur.empty()) {
                cur.push_back(w);
                cur_len = n;
            } else if (cur_len + 1 + n <= width) {
                cur.push_back(w);
                cur_len += 1 + n;
            } else {
                lines.push_back(cur);
                cur = {w};
                cur_len = n;
            }
            continue;
        }
        if (!cur.empty()) lines.push_back(cur);
        std::string rest = w;
        while ((int)rest.size() > width) {
            lines.push_back({rest.substr(0, width - 1) + "-"});
            rest = rest.substr(width - 1);
        }
        cur = {rest};
        cur_len = (int)rest.size();
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

static std::string render(const std::vector<std::string>& words, int width, Align align, bool last) {
    std::string s = join(words);
    int extra = width - (int)s.size();
    switch (align) {
        case Align::Left: return s;
        case Align::Right: return std::string(extra, ' ') + s;
        case Align::Center: return std::string(extra / 2, ' ') + s;
        case Align::Justify: {
            if (last || words.size() < 2) return s;
            int gaps = (int)words.size() - 1;
            std::string out = words[0];
            for (int g = 0; g < gaps; g++) {
                out += std::string(1 + extra / gaps + (g < extra % gaps ? 1 : 0), ' ');
                out += words[g + 1];
            }
            return out;
        }
    }
    return s;
}

WrapResult wrap(const std::string& text, int width, Align align) {
    if (width < 2 || width > 200) return {WrapError::BadWidth, {}};
    WrapResult r{WrapError::None, {}};
    for (const auto& para : paragraphs(text)) {
        if (!r.lines.empty()) r.lines.push_back("");
        auto lines = fill(para, width);
        for (size_t k = 0; k < lines.size(); k++) r.lines.push_back(render(lines[k], width, align, k + 1 == lines.size()));
    }
    return r;
}

// --- driver ---
static const char* align_name(Align a) {
    switch (a) {
        case Align::Left: return "Left";
        case Align::Right: return "Right";
        case Align::Center: return "Center";
        case Align::Justify: return "Justify";
    }
    return "?";
}

static void show(const char* title, const std::string& text, int width, Align align) {
    std::printf("== %s, width %d, %s\n", title, width, align_name(align));
    WrapResult r = wrap(text, width, align);
    if (r.error == WrapError::BadWidth) {
        std::printf("error BadWidth\n");
        return;
    }
    if (r.lines.empty()) std::printf("(no lines)\n");
    for (const std::string& line : r.lines) std::printf("|%s|\n", line.c_str());
}

int main() {
    const std::string fox = "The quick brown fox jumps over the lazy dog and keeps running far away.";
    const Align all[] = {Align::Left, Align::Right, Align::Center, Align::Justify};
    for (int w : {10, 16, 21})
        for (Align a : all) show("fox", fox, w, a);
    const std::string paras = "\n   \nFirst paragraph here,\n  on two lines.\n\n\n    \nSecond   one\twith a tab.\n  \nx\n";
    for (Align a : all) show("paragraphs", paras, 12, a);
    show("justify gaps", "a b c d", 10, Align::Justify);
    show("justify gaps", "aa b c\nlast line", 9, Align::Justify);
    show("justify gaps", "aa b c dd ee", 9, Align::Justify);
    show("long words", "abcdef", 5, Align::Left);
    show("long words", "abcdefghi", 5, Align::Left);
    show("long words", "abcdefghij", 5, Align::Right);
    show("long words", "xx abcdefghij yy zz", 5, Align::Justify);
    show("long words", "xx abcdefghij yy zz", 5, Align::Center);
    show("long words", "a supercalifragilisticexpialidocious word", 8, Align::Justify);
    show("width 2", "abc de f", 2, Align::Left);
    show("width 2", "abc de f", 2, Align::Justify);
    show("exact", "four five sixx", 9, Align::Justify);
    show("exact", "four five sixx", 14, Align::Right);
    show("empty", "", 10, Align::Left);
    show("empty", "   \n  \n", 10, Align::Justify);
    show("bad width", fox, 1, Align::Left);
    show("bad width", fox, 0, Align::Left);
    show("bad width", fox, -5, Align::Center);
    show("bad width", fox, 201, Align::Right);
    show("wide", fox, 200, Align::Center);
    show("wide", fox, 200, Align::Justify);
    return 0;
}
