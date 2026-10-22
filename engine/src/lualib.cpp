// Lua 5.1's string library (with its patterns), the rest of table and math, and print: the parts
// of the standard library the game's scripts use that the core VM doesn't define itself.
#include "alamo/luavm.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

namespace alamo {
namespace {

using Values = std::vector<LuaValue>;

double num(const Values& a, size_t i, double fallback = 0) {
    if (i >= a.size()) return fallback;
    if (a[i].type == LuaValue::Type::Number) return a[i].n;
    if (a[i].isString()) {
        char* end = nullptr;
        const double d = std::strtod(a[i].s.c_str(), &end);
        if (end && end != a[i].s.c_str()) return d;
    }
    return fallback;
}
std::string str(const Values& a, size_t i) { return i < a.size() ? a[i].str() : std::string(); }
Values one(LuaValue v) { return Values{std::move(v)}; }

// ---- Lua patterns (a port of Lua 5.1's lstrlib matcher)

constexpr int kMaxCaptures = 32;
constexpr long kCapUnfinished = -1, kCapPosition = -2;

struct MatchState {
    MatchState(const char* s, const char* e, const char* pe) : srcInit(s), srcEnd(e), patEnd(pe) {}
    const char* srcInit;
    const char* srcEnd;
    const char* patEnd;
    int level = 0;
    struct {
        const char* init = nullptr;
        long len = 0;
    } capture[kMaxCaptures];
    int depth = 0;
};

bool classEnd(const char*& p, const MatchState& ms) {
    if (p >= ms.patEnd) return false;
    const char c = *p++;
    if (c == '%') {
        if (p >= ms.patEnd) return false;
        ++p;
        return true;
    }
    if (c == '[') {
        if (p < ms.patEnd && *p == '^') ++p;
        do {
            if (p >= ms.patEnd) return false;
            const char k = *p++;
            if (k == '%' && p < ms.patEnd) ++p;
        } while (p < ms.patEnd && *p != ']');
        if (p < ms.patEnd) ++p;
    }
    return true;
}

bool singleClass(int c, int cl) {
    bool res;
    switch (std::tolower(cl)) {
    case 'a': res = std::isalpha(c); break;
    case 'c': res = std::iscntrl(c); break;
    case 'd': res = std::isdigit(c); break;
    case 'l': res = std::islower(c); break;
    case 'p': res = std::ispunct(c); break;
    case 's': res = std::isspace(c); break;
    case 'u': res = std::isupper(c); break;
    case 'w': res = std::isalnum(c); break;
    case 'x': res = std::isxdigit(c); break;
    case 'z': res = c == 0; break;
    default: return cl == c;
    }
    return std::isupper(cl) ? !res : res;
}

bool matchSet(int c, const char* p, const char* ec) {
    bool sig = true;
    if (*(p + 1) == '^') {
        sig = false;
        ++p;
    }
    while (++p < ec) {
        if (*p == '%') {
            ++p;
            if (singleClass(c, uint8_t(*p))) return sig;
        } else if (*(p + 1) == '-' && p + 2 < ec) {
            p += 2;
            if (uint8_t(*(p - 2)) <= c && c <= uint8_t(*p)) return sig;
        } else if (uint8_t(*p) == c) {
            return sig;
        }
    }
    return !sig;
}

bool singleMatch(int c, const char* p, const char* ep) {
    switch (*p) {
    case '.': return true;
    case '%': return singleClass(c, uint8_t(*(p + 1)));
    case '[': return matchSet(c, p, ep - 1);
    default: return uint8_t(*p) == c;
    }
}

const char* doMatch(MatchState& ms, const char* s, const char* p);

const char* maxExpand(MatchState& ms, const char* s, const char* p, const char* ep) {
    long i = 0;
    while (s + i < ms.srcEnd && singleMatch(uint8_t(s[i]), p, ep)) ++i;
    while (i >= 0) {
        if (const char* r = doMatch(ms, s + i, ep + 1)) return r;
        --i;
    }
    return nullptr;
}

const char* minExpand(MatchState& ms, const char* s, const char* p, const char* ep) {
    for (;;) {
        if (const char* r = doMatch(ms, s, ep + 1)) return r;
        if (s < ms.srcEnd && singleMatch(uint8_t(*s), p, ep)) ++s;
        else return nullptr;
    }
}

const char* startCapture(MatchState& ms, const char* s, const char* p, long what) {
    if (ms.level >= kMaxCaptures) return nullptr;
    ms.capture[ms.level].init = s;
    ms.capture[ms.level].len = what;
    ++ms.level;
    const char* r = doMatch(ms, s, p);
    if (!r) --ms.level;
    return r;
}

const char* endCapture(MatchState& ms, const char* s, const char* p) {
    int l = -1;
    for (int k = ms.level - 1; k >= 0; --k)
        if (ms.capture[k].len == kCapUnfinished) {
            l = k;
            break;
        }
    if (l < 0) return nullptr;
    ms.capture[l].len = long(s - ms.capture[l].init);
    const char* r = doMatch(ms, s, p);
    if (!r) ms.capture[l].len = kCapUnfinished;
    return r;
}

const char* matchBalance(MatchState& ms, const char* s, const char* p) {
    if (p + 1 >= ms.patEnd || s >= ms.srcEnd || *s != *p) return nullptr;
    const char b = *p, e = *(p + 1);
    int cont = 1;
    while (++s < ms.srcEnd) {
        if (*s == e) {
            if (--cont == 0) return s + 1;
        } else if (*s == b) {
            ++cont;
        }
    }
    return nullptr;
}

const char* doMatch(MatchState& ms, const char* s, const char* p) {
    if (++ms.depth > 200) return nullptr;
    struct Leave {
        MatchState& m;
        ~Leave() { --m.depth; }
    } leave{ms};
    while (true) {
        if (p >= ms.patEnd) return s;
        switch (*p) {
        case '(':
            if (p + 1 < ms.patEnd && *(p + 1) == ')') return startCapture(ms, s, p + 2, kCapPosition);
            return startCapture(ms, s, p + 1, kCapUnfinished);
        case ')': return endCapture(ms, s, p + 1);
        case '%':
            if (p + 1 < ms.patEnd && *(p + 1) == 'b') {
                s = matchBalance(ms, s, p + 2);
                if (!s) return nullptr;
                p += 4;
                continue;
            }
            if (p + 1 < ms.patEnd && *(p + 1) == 'f') {
                p += 2;
                if (p >= ms.patEnd || *p != '[') return nullptr;
                const char* ep = p;
                classEnd(ep, ms);
                const int prev = s == ms.srcInit ? 0 : uint8_t(*(s - 1));
                const int cur = s < ms.srcEnd ? uint8_t(*s) : 0;
                if (matchSet(prev, p, ep - 1) || !matchSet(cur, p, ep - 1)) return nullptr;
                p = ep;
                continue;
            }
            if (p + 1 < ms.patEnd && std::isdigit(uint8_t(*(p + 1)))) { // back reference %1
                const int l = *(p + 1) - '1';
                if (l < 0 || l >= ms.level || ms.capture[l].len < 0) return nullptr;
                const size_t len = size_t(ms.capture[l].len);
                if (size_t(ms.srcEnd - s) >= len && std::memcmp(ms.capture[l].init, s, len) == 0) {
                    s += len;
                    p += 2;
                    continue;
                }
                return nullptr;
            }
            break;
        case '$':
            if (p + 1 == ms.patEnd) return s == ms.srcEnd ? s : nullptr;
            break;
        default: break;
        }
        const char* ep = p;
        if (!classEnd(ep, ms)) return nullptr;
        const bool m = s < ms.srcEnd && singleMatch(uint8_t(*s), p, ep);
        const char q = ep < ms.patEnd ? *ep : '\0';
        switch (q) {
        case '?': {
            if (m)
                if (const char* r = doMatch(ms, s + 1, ep + 1)) return r;
            p = ep + 1;
            continue;
        }
        case '*': return maxExpand(ms, s, p, ep);
        case '+': return m ? maxExpand(ms, s + 1, p, ep) : nullptr;
        case '-': return minExpand(ms, s, p, ep);
        default:
            if (!m) return nullptr;
            ++s;
            p = ep;
        }
    }
}

LuaValue capture(const MatchState& ms, int i, const char* s, const char* e) {
    if (i >= ms.level) {
        if (i == 0) return LuaValue::string(std::string(s, size_t(e - s))); // whole match
        return LuaValue();
    }
    if (ms.capture[i].len == kCapPosition) return LuaValue::number(double(ms.capture[i].init - ms.srcInit + 1));
    return LuaValue::string(std::string(ms.capture[i].init, size_t(std::max(ms.capture[i].len, 0L))));
}

Values captures(const MatchState& ms, const char* s, const char* e, bool wholeIfNone) {
    Values out;
    const int n = ms.level == 0 && wholeIfNone ? 1 : ms.level;
    for (int i = 0; i < n; ++i) out.push_back(capture(ms, i, s, e));
    return out;
}

bool hasSpecials(const std::string& p) { return p.find_first_of("^$*+?.([%-") != std::string::npos; }

// string.find / string.match.
Values findAux(const Values& a, bool find) {
    const std::string s = str(a, 0), p = str(a, 1);
    long init = long(num(a, 2, 1));
    if (init < 0) init = std::max(1L, long(s.size()) + init + 1);
    if (init < 1) init = 1;
    if (size_t(init) > s.size() + 1) return one(LuaValue());
    const bool plain = a.size() > 3 && a[3].truthy();
    if (find && (plain || !hasSpecials(p))) {
        const size_t at = s.find(p, size_t(init - 1));
        if (at == std::string::npos) return one(LuaValue());
        return {LuaValue::number(double(at + 1)), LuaValue::number(double(at + p.size()))};
    }
    MatchState ms{s.data(), s.data() + s.size(), p.data() + p.size()};
    const char* pat = p.data();
    const bool anchor = !p.empty() && p[0] == '^';
    if (anchor) ++pat;
    const char* src = s.data() + init - 1;
    do {
        ms.level = 0;
        ms.depth = 0;
        if (const char* e = doMatch(ms, src, pat)) {
            if (find) {
                Values out{LuaValue::number(double(src - s.data() + 1)), LuaValue::number(double(e - s.data()))};
                for (LuaValue& c : captures(ms, nullptr, nullptr, false)) out.push_back(std::move(c));
                return out;
            }
            return captures(ms, src, e, true);
        }
    } while (src++ < ms.srcEnd && !anchor);
    return one(LuaValue());
}

// string.format: %d %i %u %c %x %X %o %e %E %f %g %G %q %s %% with flags, width and precision.
std::string format(const Values& a) {
    const std::string f = str(a, 0);
    std::string out;
    size_t arg = 1;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') {
            out += f[i];
            continue;
        }
        if (++i >= f.size()) break;
        if (f[i] == '%') {
            out += '%';
            continue;
        }
        std::string spec = "%";
        while (i < f.size() && std::strchr("-+ #0", f[i])) spec += f[i++];
        while (i < f.size() && std::isdigit(uint8_t(f[i]))) spec += f[i++];
        if (i < f.size() && f[i] == '.') {
            spec += f[i++];
            while (i < f.size() && std::isdigit(uint8_t(f[i]))) spec += f[i++];
        }
        if (i >= f.size()) break;
        const char conv = f[i];
        char buf[512];
        switch (conv) {
        case 'd': case 'i': spec += "lld"; std::snprintf(buf, sizeof buf, spec.c_str(), (long long)num(a, arg++)); out += buf; break;
        case 'u': spec += "llu"; std::snprintf(buf, sizeof buf, spec.c_str(), (unsigned long long)(long long)num(a, arg++)); out += buf; break;
        case 'x': case 'X': case 'o':
            spec += "ll";
            spec += conv;
            std::snprintf(buf, sizeof buf, spec.c_str(), (unsigned long long)(long long)num(a, arg++));
            out += buf;
            break;
        case 'c': out += char(int(num(a, arg++))); break;
        case 'e': case 'E': case 'f': case 'g': case 'G':
            spec += conv;
            std::snprintf(buf, sizeof buf, spec.c_str(), num(a, arg++));
            out += buf;
            break;
        case 'q': {
            const std::string s = str(a, arg++);
            out += '"';
            for (char c : s) {
                if (c == '"' || c == '\\') out += '\\', out += c;
                else if (c == '\n') out += "\\n";
                else if (c == '\r') out += "\\r";
                else if (c == '\0') out += "\\000";
                else out += c;
            }
            out += '"';
            break;
        }
        case 's': {
            const std::string s = str(a, arg++);
            if (spec == "%") out += s;
            else {
                spec += 's';
                std::snprintf(buf, sizeof buf, spec.c_str(), s.c_str());
                out += buf;
            }
            break;
        }
        default: out += conv; break;
        }
    }
    return out;
}

} // namespace

void installLuaLibrary(LuaVM& vm) {
    LuaVM* self = &vm;
    auto lib = [&](const char* name) {
        LuaValue t = vm.global(name);
        if (!t.isTable()) {
            t = LuaValue::newTable();
            vm.setGlobal(name, t);
        }
        return t.t;
    };
    auto def = [](const std::shared_ptr<LuaTable>& t, const char* name, LuaNative fn) {
        t->set(LuaValue::string(name), LuaValue::function(std::move(fn)));
    };

    // string
    const auto S = lib("string");
    def(S, "len", [](const Values& a) { return one(LuaValue::number(double(str(a, 0).size()))); });
    def(S, "sub", [](const Values& a) {
        const std::string s = str(a, 0);
        const long n = long(s.size());
        long i = long(num(a, 1, 1)), j = long(num(a, 2, -1));
        if (i < 0) i = std::max(n + i + 1, 1L);
        else if (i == 0) i = 1;
        if (j < 0) j = n + j + 1;
        else if (j > n) j = n;
        return one(LuaValue::string(i > j ? std::string() : s.substr(size_t(i - 1), size_t(j - i + 1))));
    });
    def(S, "upper", [](const Values& a) {
        std::string s = str(a, 0);
        for (char& c : s) c = char(std::toupper(uint8_t(c)));
        return one(LuaValue::string(s));
    });
    def(S, "lower", [](const Values& a) {
        std::string s = str(a, 0);
        for (char& c : s) c = char(std::tolower(uint8_t(c)));
        return one(LuaValue::string(s));
    });
    def(S, "rep", [](const Values& a) {
        std::string out;
        const std::string s = str(a, 0);
        for (long k = 0, n = long(num(a, 1)); k < n && out.size() < (1u << 24); ++k) out += s;
        return one(LuaValue::string(out));
    });
    def(S, "reverse", [](const Values& a) {
        std::string s = str(a, 0);
        std::reverse(s.begin(), s.end());
        return one(LuaValue::string(s));
    });
    def(S, "byte", [](const Values& a) {
        const std::string s = str(a, 0);
        long i = long(num(a, 1, 1));
        long j = long(num(a, 2, double(i)));
        const long n = long(s.size());
        if (i < 0) i = n + i + 1;
        if (j < 0) j = n + j + 1;
        Values out;
        for (long k = std::max(i, 1L); k <= std::min(j, n); ++k) out.push_back(LuaValue::number(double(uint8_t(s[size_t(k - 1)]))));
        return out;
    });
    def(S, "char", [](const Values& a) {
        std::string s;
        for (size_t i = 0; i < a.size(); ++i) s += char(int(num(a, i)));
        return one(LuaValue::string(s));
    });
    def(S, "format", [](const Values& a) { return one(LuaValue::string(format(a))); });
    def(S, "find", [](const Values& a) { return findAux(a, true); });
    def(S, "match", [](const Values& a) { return findAux(a, false); });
    def(S, "gmatch", [](const Values& a) {
        auto state = std::make_shared<std::pair<std::string, size_t>>(str(a, 0), 0);
        const std::string p = str(a, 1);
        return one(LuaValue::function([state, p](const Values&) -> Values {
            const std::string& s = state->first;
            MatchState ms{s.data(), s.data() + s.size(), p.data() + p.size()};
            for (const char* src = s.data() + state->second; src <= ms.srcEnd; ++src) {
                ms.level = 0;
                ms.depth = 0;
                if (const char* e = doMatch(ms, src, p.data())) {
                    size_t next = size_t(e - s.data());
                    if (e == src) ++next; // empty match: move on
                    state->second = next;
                    return captures(ms, src, e, true);
                }
            }
            state->second = s.size() + 1;
            return one(LuaValue());
        }));
    });
    def(S, "gsub", [self](const Values& a) {
        const std::string s = str(a, 0), p = str(a, 1);
        const LuaValue repl = a.size() > 2 ? a[2] : LuaValue();
        const long maxN = a.size() > 3 && !a[3].isNil() ? long(num(a, 3)) : -1;
        const bool anchor = !p.empty() && p[0] == '^';
        const char* pat = p.data() + (anchor ? 1 : 0);
        MatchState ms{s.data(), s.data() + s.size(), p.data() + p.size()};
        std::string out;
        long n = 0;
        const char* src = s.data();
        while (maxN < 0 || n < maxN) {
            ms.level = 0;
            ms.depth = 0;
            const char* e = doMatch(ms, src, pat);
            if (e) {
                ++n;
                const Values caps = captures(ms, src, e, true);
                LuaValue value;
                if (repl.isString() || repl.type == LuaValue::Type::Number) {
                    const std::string r = repl.str();
                    std::string v;
                    for (size_t i = 0; i < r.size(); ++i) {
                        if (r[i] != '%' || i + 1 >= r.size()) {
                            v += r[i];
                            continue;
                        }
                        const char d = r[++i];
                        if (d == '0') v += std::string(src, size_t(e - src));
                        else if (std::isdigit(uint8_t(d))) v += capture(ms, d - '1', src, e).str();
                        else v += d;
                    }
                    value = LuaValue::string(v);
                } else if (repl.isTable()) {
                    value = repl.t->get(caps.empty() ? LuaValue() : caps[0]);
                } else if (!repl.isNil()) {
                    const Values r = self->call(repl, caps);
                    value = r.empty() ? LuaValue() : r[0];
                }
                if (!value.truthy()) out.append(src, size_t(e - src)); // false/nil keeps the original
                else out += value.str();
            }
            if (e && e > src) src = e;
            else if (src < ms.srcEnd) out += *src++;
            else break;
            if (anchor) break;
        }
        if (src < ms.srcEnd) out.append(src, size_t(ms.srcEnd - src));
        return Values{LuaValue::string(out), LuaValue::number(double(n))};
    });

    // table
    const auto Tb = lib("table");
    def(Tb, "remove", [](const Values& a) {
        if (a.empty() || !a[0].isTable()) return Values{};
        LuaTable& t = *a[0].t;
        const size_t n = t.length();
        if (n == 0) return one(LuaValue());
        size_t pos = a.size() > 1 ? size_t(std::max(1.0, num(a, 1))) : n;
        if (pos > n) return one(LuaValue());
        LuaValue removed = t.get(LuaValue::number(double(pos)));
        for (; pos < n; ++pos) t.set(LuaValue::number(double(pos)), t.get(LuaValue::number(double(pos + 1))));
        t.set(LuaValue::number(double(n)), LuaValue());
        return one(removed);
    });
    def(Tb, "concat", [](const Values& a) {
        if (a.empty() || !a[0].isTable()) return one(LuaValue::string(""));
        const std::string sep = str(a, 1);
        const size_t n = a[0].t->length();
        const size_t i = size_t(num(a, 2, 1)), j = size_t(num(a, 3, double(n)));
        std::string out;
        for (size_t k = i; k <= j; ++k) {
            if (k > i) out += sep;
            out += a[0].t->get(LuaValue::number(double(k))).str();
        }
        return one(LuaValue::string(out));
    });
    def(Tb, "sort", [self](const Values& a) {
        if (a.empty() || !a[0].isTable()) return Values{};
        LuaTable& t = *a[0].t;
        const size_t n = t.length();
        std::vector<LuaValue> v;
        for (size_t k = 1; k <= n; ++k) v.push_back(t.get(LuaValue::number(double(k))));
        const LuaValue cmp = a.size() > 1 ? a[1] : LuaValue();
        auto less = [&](const LuaValue& x, const LuaValue& y) {
            if (!cmp.isNil()) {
                const Values r = self->call(cmp, {x, y});
                return !r.empty() && r[0].truthy();
            }
            if (x.isString() && y.isString()) return x.s < y.s;
            return x.n < y.n;
        };
        std::stable_sort(v.begin(), v.end(), less);
        for (size_t k = 0; k < v.size(); ++k) t.set(LuaValue::number(double(k + 1)), v[k]);
        return Values{};
    });
    def(Tb, "maxn", [](const Values& a) {
        double m = 0;
        if (!a.empty() && a[0].isTable())
            for (const auto& [k, v] : a[0].t->fields)
                if (k.type == LuaValue::Type::Number) m = std::max(m, k.n);
        return one(LuaValue::number(m));
    });
    def(Tb, "foreachi", [self](const Values& a) {
        if (a.size() < 2 || !a[0].isTable()) return Values{};
        for (size_t k = 1, n = a[0].t->length(); k <= n; ++k) {
            Values r = self->call(a[1], {LuaValue::number(double(k)), a[0].t->get(LuaValue::number(double(k)))});
            if (!r.empty() && !r[0].isNil()) return r;
        }
        return Values{};
    });
    def(Tb, "foreach", [self](const Values& a) {
        if (a.size() < 2 || !a[0].isTable()) return Values{};
        const auto fields = a[0].t->fields; // copy: the function may change the table
        for (const auto& [k, v] : fields) {
            Values r = self->call(a[1], {k, v});
            if (!r.empty() && !r[0].isNil()) return r;
        }
        return Values{};
    });

    // math
    const auto M = lib("math");
    auto unary = [&](const char* name, double (*f)(double)) {
        def(M, name, [f](const Values& a) { return one(LuaValue::number(f(num(a, 0)))); });
    };
    unary("abs", [](double x) { return std::fabs(x); });
    unary("ceil", [](double x) { return std::ceil(x); });
    unary("sqrt", [](double x) { return std::sqrt(x); });
    unary("sin", [](double x) { return std::sin(x); });
    unary("cos", [](double x) { return std::cos(x); });
    unary("tan", [](double x) { return std::tan(x); });
    unary("exp", [](double x) { return std::exp(x); });
    unary("log10", [](double x) { return std::log10(x); });
    unary("deg", [](double x) { return x * 180.0 / 3.14159265358979323846; });
    unary("rad", [](double x) { return x * 3.14159265358979323846 / 180.0; });
    def(M, "log", [](const Values& a) { return one(LuaValue::number(std::log(num(a, 0)))); });
    def(M, "pow", [](const Values& a) { return one(LuaValue::number(std::pow(num(a, 0), num(a, 1)))); });
    def(M, "fmod", [](const Values& a) { return one(LuaValue::number(std::fmod(num(a, 0), num(a, 1)))); });
    def(M, "modf", [](const Values& a) {
        double ip = 0;
        const double fp = std::modf(num(a, 0), &ip);
        return Values{LuaValue::number(ip), LuaValue::number(fp)};
    });
    M->set(LuaValue::string("pi"), LuaValue::number(3.14159265358979323846));
    M->set(LuaValue::string("huge"), LuaValue::number(HUGE_VAL));
    // Deterministic: a fixed-seed generator per VM (scripts must not make lockstep games differ).
    auto seed = std::make_shared<uint64_t>(0x9E3779B97F4A7C15ull);
    def(M, "randomseed", [seed](const Values& a) {
        *seed = uint64_t(int64_t(num(a, 0))) * 0x9E3779B97F4A7C15ull + 1;
        return Values{};
    });
    def(M, "random", [seed](const Values& a) {
        *seed = *seed * 6364136223846793005ull + 1442695040888963407ull;
        const double r = double(*seed >> 11) / double(1ull << 53);
        if (a.empty()) return one(LuaValue::number(r));
        const double lo = a.size() > 1 ? num(a, 0) : 1, hi = a.size() > 1 ? num(a, 1) : num(a, 0);
        return one(LuaValue::number(std::floor(lo + r * (hi - lo + 1))));
    });

    vm.setFunction("print", [](const Values& a) {
        std::string line;
        for (size_t i = 0; i < a.size(); ++i) line += (i ? "\t" : "") + (a[i].type == LuaValue::Type::Nil ? std::string("nil") : a[i].str());
        std::fprintf(stderr, "lua: %s\n", line.c_str());
        return Values{};
    });
}

} // namespace alamo
