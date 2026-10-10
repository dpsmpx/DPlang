// t15: заготовка — напишите тела функций по task.md.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

struct Account {
    int64_t balance = 0;
    int64_t limit = 0;
};

struct UndoEntry {
    std::string name;
    bool existed = false;
    Account old;
};

struct Ledger {
    // поля — на ваш выбор
    std::map<std::string, Account> accounts;
    std::vector<UndoEntry> undo;
    std::vector<size_t> marks;
};

static const int64_t kBound = 1000000000000000LL;
static const size_t kMaxDepth = 8;

static std::vector<std::string> split_words(const std::string& line) {
    std::vector<std::string> words;
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
    return words;
}

static bool valid_name(const std::string& s) {
    if (s.empty() || s.size() > 16) return false;
    if (!(s[0] >= 'a' && s[0] <= 'z')) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

static bool parse_number(const std::string& s, int64_t& out) {
    if (s.empty() || s.size() > 18) return false;
    int64_t v = 0;
    for (char c : s) {
        if (!(c >= '0' && c <= '9')) return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

static int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

static void record(Ledger& l, const std::string& name) {
    if (l.marks.empty()) return;
    UndoEntry e;
    e.name = name;
    auto it = l.accounts.find(name);
    if (it != l.accounts.end()) {
        e.existed = true;
        e.old = it->second;
    }
    l.undo.push_back(e);
}

static void set_balance(Ledger& l, const std::string& name, int64_t bal) {
    record(l, name);
    l.accounts[name].balance = bal;
}

static const std::string kSyntax = "error syntax";

Ledger ledger_new() {
    return Ledger{};
}

std::string ledger_exec(Ledger& l, const std::string& line) {
    std::vector<std::string> w = split_words(line);
    if (w.empty()) return kSyntax;
    const std::string& cmd = w[0];

    if (cmd == "open") {
        if (w.size() != 3) return kSyntax;
        int64_t limit = 0;
        if (!valid_name(w[1]) || !parse_number(w[2], limit)) return kSyntax;
        if (l.accounts.count(w[1])) return "error exists " + w[1];
        record(l, w[1]);
        Account a;
        a.balance = 0;
        a.limit = limit;
        l.accounts[w[1]] = a;
        return "ok";
    }
    if (cmd == "deposit" || cmd == "withdraw") {
        if (w.size() != 3) return kSyntax;
        int64_t amount = 0;
        if (!valid_name(w[1]) || !parse_number(w[2], amount)) return kSyntax;
        if (amount == 0) return "error amount";
        auto it = l.accounts.find(w[1]);
        if (it == l.accounts.end()) return "error unknown " + w[1];
        const Account& a = it->second;
        int64_t nb = (cmd == "deposit") ? a.balance + amount : a.balance - amount;
        if (nb < -a.limit) return "error funds";
        if (abs64(nb) > kBound) return "error bound";
        set_balance(l, w[1], nb);
        return "ok " + std::to_string(nb);
    }
    if (cmd == "transfer") {
        if (w.size() != 4) return kSyntax;
        int64_t amount = 0;
        if (!valid_name(w[1]) || !valid_name(w[2]) || !parse_number(w[3], amount)) return kSyntax;
        if (amount == 0) return "error amount";
        auto fi = l.accounts.find(w[1]);
        if (fi == l.accounts.end()) return "error unknown " + w[1];
        auto ti = l.accounts.find(w[2]);
        if (ti == l.accounts.end()) return "error unknown " + w[2];
        if (w[1] == w[2]) return "error same";
        int64_t fee = amount / 100;
        if (fee < 1) fee = 1;
        const Account& fa = fi->second;
        const Account& ta = ti->second;
        int64_t nf = fa.balance - (amount + fee);
        int64_t nt = ta.balance + amount;
        if (nf < -fa.limit) return "error funds";
        if (nt < -ta.limit) return "error funds";
        if (abs64(nf) > kBound) return "error bound";
        if (abs64(nt) > kBound) return "error bound";
        std::string from = w[1];
        std::string to = w[2];
        set_balance(l, from, nf);
        set_balance(l, to, nt);
        return "ok " + std::to_string(nf) + " " + std::to_string(nt);
    }
    if (cmd == "balance") {
        if (w.size() != 2) return kSyntax;
        if (!valid_name(w[1])) return kSyntax;
        auto it = l.accounts.find(w[1]);
        if (it == l.accounts.end()) return "error unknown " + w[1];
        return std::to_string(it->second.balance);
    }
    if (cmd == "close") {
        if (w.size() != 2) return kSyntax;
        if (!valid_name(w[1])) return kSyntax;
        auto it = l.accounts.find(w[1]);
        if (it == l.accounts.end()) return "error unknown " + w[1];
        if (it->second.balance != 0) return "error nonzero";
        record(l, w[1]);
        l.accounts.erase(w[1]);
        return "ok";
    }
    if (cmd == "begin") {
        if (w.size() != 1) return kSyntax;
        if (l.marks.size() >= kMaxDepth) return "error depth";
        l.marks.push_back(l.undo.size());
        return "ok depth " + std::to_string(l.marks.size());
    }
    if (cmd == "commit") {
        if (w.size() != 1) return kSyntax;
        if (l.marks.empty()) return "error depth";
        l.marks.pop_back();
        if (l.marks.empty()) l.undo.clear();
        return "ok depth " + std::to_string(l.marks.size());
    }
    if (cmd == "rollback") {
        if (w.size() != 1) return kSyntax;
        if (l.marks.empty()) return "error depth";
        size_t mark = l.marks.back();
        l.marks.pop_back();
        while (l.undo.size() > mark) {
            UndoEntry& e = l.undo.back();
            if (e.existed) {
                l.accounts[e.name] = e.old;
            } else {
                l.accounts.erase(e.name);
            }
            l.undo.pop_back();
        }
        return "ok depth " + std::to_string(l.marks.size());
    }
    if (cmd == "report") {
        if (w.size() != 1) return kSyntax;
        if (l.accounts.empty()) return "(empty)";
        std::string out;
        bool first = true;
        for (const auto& kv : l.accounts) {
            if (!first) out += ' ';
            first = false;
            out += kv.first;
            out += ':';
            out += std::to_string(kv.second.balance);
        }
        return out;
    }
    return kSyntax;
}
