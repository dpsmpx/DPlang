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

namespace {

const int64_t kBound = 1000000000000000LL;
const size_t kMaxDepth = 8;

std::vector<std::string> split_words(const std::string& line) {
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

bool valid_name(const std::string& s) {
    if (s.empty() || s.size() > 16) return false;
    if (!(s[0] >= 'a' && s[0] <= 'z')) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool parse_number(const std::string& s, int64_t& out) {
    if (s.empty() || s.size() > 18) return false;
    int64_t v = 0;
    for (char c : s) {
        if (!(c >= '0' && c <= '9')) return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

std::string num(int64_t v) { return std::to_string(v); }

}  // namespace

Ledger ledger_new() {
    return Ledger{};
}

std::string ledger_exec(Ledger& l, const std::string& line) {
    const std::string syntax = "error syntax";
    std::vector<std::string> w = split_words(line);
    if (w.empty()) return syntax;
    const std::string& cmd = w[0];
    const size_t argc = w.size() - 1;

    if (cmd == "open") {
        if (argc != 2) return syntax;
        int64_t limit = 0;
        if (!valid_name(w[1]) || !parse_number(w[2], limit)) return syntax;
        if (l.accounts.count(w[1])) return "error exists " + w[1];
        Account a;
        a.balance = 0;
        a.limit = limit;
        l.accounts[w[1]] = a;
        return "ok";
    }
    if (cmd == "deposit" || cmd == "withdraw") {
        if (argc != 2) return syntax;
        int64_t amount = 0;
        if (!valid_name(w[1]) || !parse_number(w[2], amount)) return syntax;
        if (amount == 0) return "error amount";
        auto it = l.accounts.find(w[1]);
        if (it == l.accounts.end()) return "error unknown " + w[1];
        Account& a = it->second;
        int64_t nb;
        if (cmd == "deposit") {
            nb = a.balance + amount;
        } else {
            nb = a.balance - amount;
            if (nb < -a.limit) return "error funds";
        }
        if (nb > kBound || nb < -kBound) return "error bound";
        a.balance = nb;
        return "ok " + num(nb);
    }
    if (cmd == "transfer") {
        if (argc != 3) return syntax;
        int64_t amount = 0;
        if (!valid_name(w[1]) || !valid_name(w[2]) || !parse_number(w[3], amount)) return syntax;
        if (amount == 0) return "error amount";
        auto fi = l.accounts.find(w[1]);
        if (fi == l.accounts.end()) return "error unknown " + w[1];
        auto ti = l.accounts.find(w[2]);
        if (ti == l.accounts.end()) return "error unknown " + w[2];
        if (w[1] == w[2]) return "error same";
        int64_t fee = amount / 100;
        if (fee < 1) fee = 1;
        Account& from = fi->second;
        Account& to = ti->second;
        int64_t nf = from.balance - (amount + fee);
        if (nf < -from.limit) return "error funds";
        if (nf > kBound || nf < -kBound) return "error bound";
        int64_t nt = to.balance + amount;
        if (nt > kBound || nt < -kBound) return "error bound";
        from.balance = nf;
        to.balance = nt;
        return "ok " + num(nf) + " " + num(nt);
    }
    if (cmd == "balance") {
        if (argc != 1) return syntax;
        if (!valid_name(w[1])) return syntax;
        auto it = l.accounts.find(w[1]);
        if (it == l.accounts.end()) return "error unknown " + w[1];
        return num(it->second.balance);
    }
    if (cmd == "close") {
        if (argc != 1) return syntax;
        if (!valid_name(w[1])) return syntax;
        auto it = l.accounts.find(w[1]);
        if (it == l.accounts.end()) return "error unknown " + w[1];
        if (it->second.balance != 0) return "error nonzero";
        l.accounts.erase(it);
        return "ok";
    }
    if (cmd == "begin") {
        if (argc != 0) return syntax;
        if (l.snapshots.size() >= kMaxDepth) return "error depth";
        l.snapshots.push_back(l.accounts);
        return "ok depth " + num(static_cast<int64_t>(l.snapshots.size()));
    }
    if (cmd == "commit") {
        if (argc != 0) return syntax;
        if (l.snapshots.empty()) return "error depth";
        l.snapshots.pop_back();
        return "ok depth " + num(static_cast<int64_t>(l.snapshots.size()));
    }
    if (cmd == "rollback") {
        if (argc != 0) return syntax;
        if (l.snapshots.empty()) return "error depth";
        l.accounts = l.snapshots.back();
        l.snapshots.pop_back();
        return "ok depth " + num(static_cast<int64_t>(l.snapshots.size()));
    }
    if (cmd == "report") {
        if (argc != 0) return syntax;
        if (l.accounts.empty()) return "(empty)";
        std::string out;
        bool first = true;
        for (const auto& kv : l.accounts) {
            if (!first) out += ' ';
            first = false;
            out += kv.first;
            out += ':';
            out += num(kv.second.balance);
        }
        return out;
    }
    return syntax;
}
