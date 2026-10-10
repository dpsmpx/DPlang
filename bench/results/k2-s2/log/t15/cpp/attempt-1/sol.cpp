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

struct Ledger {
    // поля — на ваш выбор
    std::map<std::string, Account> accounts;
    std::vector<std::map<std::string, Account>> snapshots;
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
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

static int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

static std::string depth_reply(const Ledger& l) {
    return "ok depth " + std::to_string(l.snapshots.size());
}

Ledger ledger_new() {
    return Ledger{};
}

std::string ledger_exec(Ledger& l, const std::string& line) {
    const std::string kSyntax = "error syntax";
    std::vector<std::string> w = split_words(line);
    if (w.empty()) return kSyntax;
    const std::string& cmd = w[0];
    const size_t argc = w.size() - 1;

    if (cmd == "open") {
        if (argc != 2) return kSyntax;
        int64_t limit = 0;
        if (!valid_name(w[1]) || !parse_number(w[2], limit)) return kSyntax;
        if (l.accounts.count(w[1])) return "error exists " + w[1];
        Account a;
        a.balance = 0;
        a.limit = limit;
        l.accounts[w[1]] = a;
        return "ok";
    }
    if (cmd == "deposit" || cmd == "withdraw") {
        if (argc != 2) return kSyntax;
        int64_t amount = 0;
        if (!valid_name(w[1]) || !parse_number(w[2], amount)) return kSyntax;
        if (amount == 0) return "error amount";
        auto it = l.accounts.find(w[1]);
        if (it == l.accounts.end()) return "error unknown " + w[1];
        Account& a = it->second;
        int64_t nb = cmd == "deposit" ? a.balance + amount : a.balance - amount;
        if (nb < -a.limit) return "error funds";
        if (abs64(nb) > kBound) return "error bound";
        a.balance = nb;
        return "ok " + std::to_string(nb);
    }
    if (cmd == "transfer") {
        if (argc != 3) return kSyntax;
        int64_t amount = 0;
        if (!valid_name(w[1]) || !valid_name(w[2]) || !parse_number(w[3], amount)) return kSyntax;
        if (amount == 0) return "error amount";
        auto fit = l.accounts.find(w[1]);
        if (fit == l.accounts.end()) return "error unknown " + w[1];
        auto tit = l.accounts.find(w[2]);
        if (tit == l.accounts.end()) return "error unknown " + w[2];
        if (w[1] == w[2]) return "error same";
        int64_t fee = amount / 100;
        if (fee < 1) fee = 1;
        Account& from = fit->second;
        Account& to = tit->second;
        int64_t nf = from.balance - (amount + fee);
        int64_t nt = to.balance + amount;
        if (nf < -from.limit) return "error funds";
        if (nt < -to.limit) return "error funds";
        if (abs64(nf) > kBound) return "error bound";
        if (abs64(nt) > kBound) return "error bound";
        from.balance = nf;
        to.balance = nt;
        return "ok " + std::to_string(nf) + " " + std::to_string(nt);
    }
    if (cmd == "balance") {
        if (argc != 1) return kSyntax;
        if (!valid_name(w[1])) return kSyntax;
        auto it = l.accounts.find(w[1]);
        if (it == l.accounts.end()) return "error unknown " + w[1];
        return std::to_string(it->second.balance);
    }
    if (cmd == "close") {
        if (argc != 1) return kSyntax;
        if (!valid_name(w[1])) return kSyntax;
        auto it = l.accounts.find(w[1]);
        if (it == l.accounts.end()) return "error unknown " + w[1];
        if (it->second.balance != 0) return "error nonzero";
        l.accounts.erase(it);
        return "ok";
    }
    if (cmd == "begin") {
        if (argc != 0) return kSyntax;
        if (l.snapshots.size() >= kMaxDepth) return "error depth";
        l.snapshots.push_back(l.accounts);
        return depth_reply(l);
    }
    if (cmd == "commit") {
        if (argc != 0) return kSyntax;
        if (l.snapshots.empty()) return "error depth";
        l.snapshots.pop_back();
        return depth_reply(l);
    }
    if (cmd == "rollback") {
        if (argc != 0) return kSyntax;
        if (l.snapshots.empty()) return "error depth";
        l.accounts = std::move(l.snapshots.back());
        l.snapshots.pop_back();
        return depth_reply(l);
    }
    if (cmd == "report") {
        if (argc != 0) return kSyntax;
        if (l.accounts.empty()) return "(empty)";
        std::string out;
        for (const auto& kv : l.accounts) {
            if (!out.empty()) out += ' ';
            out += kv.first;
            out += ':';
            out += std::to_string(kv.second.balance);
        }
        return out;
    }
    return kSyntax;
}
