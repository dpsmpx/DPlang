// t15: эталон на C++20.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct Account {
    std::string name;
    int64_t balance;
    int64_t limit;
};

struct Ledger {
    std::vector<Account> accounts;              // по возрастанию имени
    std::vector<std::vector<Account>> saved;    // состояния на момент begin
};

static const int64_t BOUND = 1000000000000000;
static const int MAX_DEPTH = 8;

Ledger ledger_new() { return Ledger{}; }

static std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ' ') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool is_name(const std::string& s) {
    if (s.empty() || s.size() > 16 || s[0] < 'a' || s[0] > 'z') return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

static bool number(const std::string& s, int64_t& out) {
    if (s.empty() || s.size() > 18) return false;
    out = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + (c - '0');
    }
    return true;
}

static int find(const Ledger& l, const std::string& name) {
    for (size_t i = 0; i < l.accounts.size(); i++)
        if (l.accounts[i].name == name) return (int)i;
    return -1;
}

static int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

std::string ledger_exec(Ledger& l, const std::string& line) {
    std::vector<std::string> w = words(line);
    if (w.empty()) return "error syntax";
    const std::string& cmd = w[0];
    size_t argc = w.size() - 1;
    int64_t amount = 0;
    if (cmd == "begin" || cmd == "commit" || cmd == "rollback" || cmd == "report") {
        if (argc != 0) return "error syntax";
        if (cmd == "report") {
            if (l.accounts.empty()) return "(empty)";
            std::string out;
            for (const Account& a : l.accounts) {
                if (!out.empty()) out += " ";
                out += a.name + ":" + std::to_string(a.balance);
            }
            return out;
        }
        if (cmd == "begin") {
            if ((int)l.saved.size() == MAX_DEPTH) return "error depth";
            l.saved.push_back(l.accounts);
        } else {
            if (l.saved.empty()) return "error depth";
            if (cmd == "rollback") l.accounts = l.saved.back();
            l.saved.pop_back();
        }
        return "ok depth " + std::to_string(l.saved.size());
    }
    if (cmd == "balance" || cmd == "close") {
        if (argc != 1 || !is_name(w[1])) return "error syntax";
        int i = find(l, w[1]);
        if (i < 0) return "error unknown " + w[1];
        if (cmd == "balance") return std::to_string(l.accounts[i].balance);
        if (l.accounts[i].balance != 0) return "error nonzero";
        l.accounts.erase(l.accounts.begin() + i);
        return "ok";
    }
    if (cmd == "open" || cmd == "deposit" || cmd == "withdraw") {
        if (argc != 2 || !is_name(w[1]) || !number(w[2], amount)) return "error syntax";
        int i = find(l, w[1]);
        if (cmd == "open") {
            if (i >= 0) return "error exists " + w[1];
            Account a{w[1], 0, amount};
            auto at = std::lower_bound(l.accounts.begin(), l.accounts.end(), a,
                                       [](const Account& x, const Account& y) { return x.name < y.name; });
            l.accounts.insert(at, a);
            return "ok";
        }
        if (amount == 0) return "error amount";
        if (i < 0) return "error unknown " + w[1];
        Account& a = l.accounts[i];
        int64_t next = cmd == "deposit" ? a.balance + amount : a.balance - amount;
        if (next < -a.limit) return "error funds";
        if (abs64(next) > BOUND) return "error bound";
        a.balance = next;
        return "ok " + std::to_string(a.balance);
    }
    if (cmd == "transfer") {
        if (argc != 3 || !is_name(w[1]) || !is_name(w[2]) || !number(w[3], amount)) return "error syntax";
        if (amount == 0) return "error amount";
        int from = find(l, w[1]);
        if (from < 0) return "error unknown " + w[1];
        int to = find(l, w[2]);
        if (to < 0) return "error unknown " + w[2];
        if (from == to) return "error same";
        int64_t fee = std::max<int64_t>(1, amount / 100);
        int64_t nf = l.accounts[from].balance - amount - fee;
        int64_t nt = l.accounts[to].balance + amount;
        if (nf < -l.accounts[from].limit) return "error funds";
        if (abs64(nf) > BOUND || abs64(nt) > BOUND) return "error bound";
        l.accounts[from].balance = nf;
        l.accounts[to].balance = nt;
        return "ok " + std::to_string(nf) + " " + std::to_string(nt);
    }
    return "error syntax";
}

// --- driver ---
static const char* SCRIPT[] = {
    "report",
    "open alice 100",
    "open bob 0",
    "open alice 5",
    "  deposit   alice  500  ",
    "deposit bob 0",
    "deposit carol 5",
    "deposit bob +5",
    "deposit bob 5x",
    "deposit Bob 5",
    "deposit bob 1234567890123456789",
    "deposit bob 999999999999999999",
    "deposit bob 007",
    "withdraw bob 8",
    "withdraw alice 650",
    "withdraw alice 600",
    "transfer alice bob 50",
    "transfer bob alice 1",
    "transfer bob bob 1",
    "transfer bob alice 0",
    "transfer zed alice 1",
    "transfer alice zed 1",
    "transfer bob zed 0",
    "transfer bob bob 0",
    "transfer bob alice 3",
    "transfer alice bob 1000",
    "",
    "   ",
    "balance",
    "balance alice bob",
    "balance alice",
    "open carol_2 1000000000000000",
    "open a234567890123456 0",
    "open a2345678901234567 0",
    "open 9lives 0",
    "open dave\t1 0",
    "open b_ 0",
    "open b9 00",
    "report",
    "begin",
    "deposit carol_2 999999999999999",
    "deposit carol_2 2",
    "deposit carol_2 1",
    "transfer alice carol_2 1",
    "begin",
    "withdraw carol_2 1000000000000000",
    "close bob",
    "withdraw bob 1",
    "close bob",
    "close b_",
    "report",
    "rollback",
    "report",
    "begin",
    "close a234567890123456",
    "open eve 10",
    "commit",
    "report",
    "rollback",
    "report",
    "commit",
    "rollback",
    "begin x",
    "begin", "begin", "begin", "begin", "begin", "begin", "begin", "begin",
    "begin",
    "deposit alice 1",
    "commit", "commit", "commit", "commit", "commit", "commit", "commit",
    "rollback",
    "balance alice",
    "open big 2000000000000000",
    "withdraw big 999999999999999",
    "transfer big alice 1",
    "transfer big alice 2000000000000000",
    "deposit big 999999999999999",
    "transfer big b9 250",
    "transfer big b9 199",
    "transfer big b9 100",
    "close alice",
    "deposit alice 96",
    "close alice",
    "withdraw alice 1",
    "open alice 3",
    "withdraw alice 3",
    "withdraw alice 1",
    "transfer alice bob 1",
    "frobnicate",
    "REPORT",
    "report",
};

int main() {
    Ledger l = ledger_new();
    int n = 0;
    for (const char* line : SCRIPT) {
        n++;
        std::printf("%d: %s\n", n, ledger_exec(l, line).c_str());
    }
    return 0;
}
