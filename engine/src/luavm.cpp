// Lua 5.1 bytecode interpreter for the game's compiled scripts (see luavm.hpp).
#include "alamo/luavm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>

namespace alamo {

// ---------------------------------------------------------------- values

LuaValue LuaValue::newTable() { return table(std::make_shared<LuaTable>()); }

LuaValue LuaValue::get(const LuaValue& key) const { return isTable() ? t->get(key) : LuaValue(); }

std::string LuaValue::str() const {
    if (type == Type::String) return s;
    if (type == Type::Number) {
        char buf[64];
        if (n == std::floor(n) && std::fabs(n) < 1e15) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
        else std::snprintf(buf, sizeof buf, "%.14g", n);
        return buf;
    }
    if (type == Type::Bool) return b ? "true" : "false";
    if (type == Type::Nil) return "nil";
    return "";
}

bool operator<(const LuaValue& a, const LuaValue& b) {
    if (a.type != b.type) return a.type < b.type;
    switch (a.type) {
    case LuaValue::Type::Nil: return false;
    case LuaValue::Type::Bool: return a.b < b.b;
    case LuaValue::Type::Number: return a.n < b.n;
    case LuaValue::Type::String: return a.s < b.s;
    case LuaValue::Type::Table: return a.t.get() < b.t.get();
    case LuaValue::Type::Function: return a.f.get() < b.f.get();
    case LuaValue::Type::Native: return a.native.get() < b.native.get();
    }
    return false;
}

bool rawEquals(const LuaValue& a, const LuaValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
    case LuaValue::Type::Nil: return true;
    case LuaValue::Type::Bool: return a.b == b.b;
    case LuaValue::Type::Number: return a.n == b.n;
    case LuaValue::Type::String: return a.s == b.s;
    case LuaValue::Type::Table: return a.t == b.t;
    case LuaValue::Type::Function: return a.f == b.f;
    case LuaValue::Type::Native: return a.native == b.native;
    }
    return false;
}

LuaValue LuaTable::get(const LuaValue& key) const {
    auto it = fields.find(key);
    return it == fields.end() ? LuaValue() : it->second;
}

void LuaTable::set(const LuaValue& key, LuaValue value) {
    if (key.isNil()) return;
    if (value.isNil()) fields.erase(key);
    else fields[key] = std::move(value);
}

size_t LuaTable::length() const {
    size_t n = 0;
    while (fields.count(LuaValue::number(double(n + 1)))) ++n;
    return n;
}

// ---------------------------------------------------------------- loading

namespace {

struct Reader {
    const std::vector<uint8_t>& d;
    size_t p = 0;
    bool ok = true;
    int intSize = 4, sizeTSize = 4, numberSize = 8;
    bool integral = false, extraHeaderInt = false;

    uint64_t uint(int bytes) {
        if (p + size_t(bytes) > d.size()) { ok = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v |= uint64_t(d[p + size_t(i)]) << (8 * i);
        p += size_t(bytes);
        return v;
    }
    uint8_t byte() { return uint8_t(uint(1)); }
    int64_t integer() {
        const uint64_t v = uint(intSize);
        return intSize == 4 ? int64_t(int32_t(uint32_t(v))) : int64_t(v);
    }
    std::string string() {
        const uint64_t len = uint(sizeTSize);
        if (!ok || len == 0) return {};
        if (p + len > d.size()) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(d.data() + p), size_t(len - 1)); // drop the NUL
        p += size_t(len);
        return s;
    }
    double number() {
        if (integral) return double(int64_t(uint(numberSize)));
        if (numberSize == 4) {
            const uint32_t v = uint32_t(uint(4));
            float f;
            std::memcpy(&f, &v, 4);
            return f;
        }
        const uint64_t v = uint(8);
        double f;
        std::memcpy(&f, &v, 8);
        return f;
    }
    std::shared_ptr<LuaProto> function(int depth) {
        if (depth > 200) { ok = false; return nullptr; }
        auto f = std::make_shared<LuaProto>();
        string();           // source
        integer();          // linedefined
        integer();          // lastlinedefined
        if (extraHeaderInt) integer();
        f->upvalues = byte();
        f->params = byte();
        f->varargFlags = byte();
        f->maxStack = byte();
        const int64_t ncode = integer();
        if (!ok || ncode < 0 || ncode > 10'000'000) { ok = false; return nullptr; }
        f->code.reserve(size_t(ncode));
        for (int64_t i = 0; i < ncode && ok; ++i) f->code.push_back(uint32_t(uint(4)));
        const int64_t nk = integer();
        if (!ok || nk < 0 || nk > 10'000'000) { ok = false; return nullptr; }
        for (int64_t i = 0; i < nk && ok; ++i) {
            const uint8_t t = byte();
            switch (t) {
            case 0: f->constants.push_back(LuaValue()); break;
            case 1: f->constants.push_back(LuaValue::boolean(byte() != 0)); break;
            case 3: f->constants.push_back(LuaValue::number(number())); break;
            case 4: f->constants.push_back(LuaValue::string(string())); break;
            default: ok = false;
            }
        }
        const int64_t np = integer();
        if (!ok || np < 0 || np > 100'000) { ok = false; return nullptr; }
        for (int64_t i = 0; i < np && ok; ++i) f->protos.push_back(function(depth + 1));
        // Debug info: line numbers, locals, upvalue names.
        const int64_t nl = integer();
        for (int64_t i = 0; i < nl && ok; ++i) integer();
        const int64_t nloc = integer();
        for (int64_t i = 0; i < nloc && ok; ++i) { string(); integer(); integer(); }
        const int64_t nup = integer();
        for (int64_t i = 0; i < nup && ok; ++i) string();
        return ok ? f : nullptr;
    }
};

double toNumber(const LuaValue& v, bool* ok = nullptr) {
    if (ok) *ok = true;
    if (v.type == LuaValue::Type::Number) return v.n;
    if (v.type == LuaValue::Type::String) {
        char* end = nullptr;
        const double d = std::strtod(v.s.c_str(), &end);
        if (end && end != v.s.c_str()) return d;
    }
    if (ok) *ok = false;
    return 0;
}

} // namespace

std::shared_ptr<LuaProto> loadLuaChunk(const std::vector<uint8_t>& chunk, std::string* error) {
    auto fail = [&](const char* m) -> std::shared_ptr<LuaProto> {
        if (error) *error = m;
        return nullptr;
    };
    if (chunk.size() < 12 || std::memcmp(chunk.data(), "\x1bLua", 4) != 0) return fail("not a compiled Lua chunk");
    if (chunk[4] != 0x51) return fail("not Lua 5.1 bytecode");
    Reader r{chunk};
    const uint8_t format = chunk[5];
    if (chunk[6] != 1) return fail("big-endian chunk");
    r.intSize = chunk[7];
    r.sizeTSize = chunk[8];
    if (chunk[9] != 4) return fail("unexpected instruction size");
    r.numberSize = chunk[10];
    r.integral = chunk[11] != 0;
    r.extraHeaderInt = format != 0; // Petroglyph's format 'p'
    r.p = 12;
    auto f = r.function(0);
    if (!f || !r.ok) return fail("malformed chunk");
    return f;
}

// ---------------------------------------------------------------- VM

namespace {

std::vector<LuaValue> nextImpl(const std::vector<LuaValue>& a) {
    if (a.empty() || !a[0].isTable()) return {LuaValue()};
    const auto& f = a[0].t->fields;
    auto it = (a.size() < 2 || a[1].isNil()) ? f.begin() : f.upper_bound(a[1]);
    if (it == f.end()) return {LuaValue()};
    return {it->first, it->second};
}

} // namespace

LuaVM::LuaVM() : globals_(std::make_shared<LuaTable>()) {
    setGlobal("_G", LuaValue::table(globals_));
    setFunction("next", nextImpl);
    setFunction("pairs", [](const std::vector<LuaValue>& a) {
        return std::vector<LuaValue>{LuaValue::function(nextImpl), a.empty() ? LuaValue() : a[0], LuaValue()};
    });
    setFunction("ipairs", [](const std::vector<LuaValue>& a) {
        LuaNative step = [](const std::vector<LuaValue>& s) -> std::vector<LuaValue> {
            if (s.size() < 2 || !s[0].isTable()) return {LuaValue()};
            const double i = s[1].n + 1;
            LuaValue v = s[0].t->get(LuaValue::number(i));
            if (v.isNil()) return {LuaValue()};
            return {LuaValue::number(i), v};
        };
        return std::vector<LuaValue>{LuaValue::function(step), a.empty() ? LuaValue() : a[0], LuaValue::number(0)};
    });
    setFunction("setmetatable", [](const std::vector<LuaValue>& a) {
        if (a.empty() || !a[0].isTable()) return std::vector<LuaValue>{};
        a[0].t->meta = a.size() > 1 && a[1].isTable() ? a[1].t : nullptr;
        return std::vector<LuaValue>{a[0]};
    });
    setFunction("getmetatable", [](const std::vector<LuaValue>& a) {
        if (a.empty() || !a[0].isTable() || !a[0].t->meta) return std::vector<LuaValue>{LuaValue()};
        const LuaValue protect = a[0].t->meta->get(LuaValue::string("__metatable"));
        return std::vector<LuaValue>{protect.isNil() ? LuaValue::table(a[0].t->meta) : protect};
    });
    setFunction("rawget", [](const std::vector<LuaValue>& a) {
        return std::vector<LuaValue>{a.size() > 1 && a[0].isTable() ? a[0].t->get(a[1]) : LuaValue()};
    });
    setFunction("rawset", [](const std::vector<LuaValue>& a) {
        if (a.size() > 2 && a[0].isTable()) a[0].t->set(a[1], a[2]);
        return std::vector<LuaValue>{a.empty() ? LuaValue() : a[0]};
    });
    setFunction("rawequal", [](const std::vector<LuaValue>& a) {
        return std::vector<LuaValue>{LuaValue::boolean(a.size() > 1 && rawEquals(a[0], a[1]))};
    });
    setFunction("type", [](const std::vector<LuaValue>& a) {
        static const char* names[] = {"nil", "boolean", "number", "string", "table", "function", "function"};
        return std::vector<LuaValue>{LuaValue::string(names[int(a.empty() ? LuaValue::Type::Nil : a[0].type)])};
    });
    setFunction("tostring", [](const std::vector<LuaValue>& a) {
        return std::vector<LuaValue>{LuaValue::string(a.empty() ? "nil" : a[0].str())};
    });
    setFunction("tonumber", [](const std::vector<LuaValue>& a) {
        bool ok = false;
        const double d = a.empty() ? 0 : toNumber(a[0], &ok);
        return std::vector<LuaValue>{ok ? LuaValue::number(d) : LuaValue()};
    });
    setFunction("unpack", [](const std::vector<LuaValue>& a) {
        std::vector<LuaValue> out;
        if (!a.empty() && a[0].isTable())
            for (size_t i = 1, n = a[0].t->length(); i <= n; ++i) out.push_back(a[0].t->get(LuaValue::number(double(i))));
        return out;
    });
    LuaValue table = LuaValue::newTable();
    table.t->set(LuaValue::string("insert"), LuaValue::function([](const std::vector<LuaValue>& a) {
        if (a.empty() || !a[0].isTable()) return std::vector<LuaValue>{};
        LuaTable& t = *a[0].t;
        if (a.size() >= 3) {
            const size_t n = t.length(), pos = size_t(toNumber(a[1]));
            for (size_t i = n; i >= pos && i >= 1; --i) t.set(LuaValue::number(double(i + 1)), t.get(LuaValue::number(double(i))));
            t.set(LuaValue::number(double(pos)), a[2]);
        } else if (a.size() == 2) {
            t.set(LuaValue::number(double(t.length() + 1)), a[1]);
        }
        return std::vector<LuaValue>{};
    }));
    table.t->set(LuaValue::string("getn"), LuaValue::function([](const std::vector<LuaValue>& a) {
        return std::vector<LuaValue>{LuaValue::number(a.empty() || !a[0].isTable() ? 0 : double(a[0].t->length()))};
    }));
    setGlobal("table", table);
    LuaValue math = LuaValue::newTable();
    math.t->set(LuaValue::string("floor"), LuaValue::function([](const std::vector<LuaValue>& a) {
        return std::vector<LuaValue>{LuaValue::number(std::floor(a.empty() ? 0 : toNumber(a[0])))};
    }));
    math.t->set(LuaValue::string("max"), LuaValue::function([](const std::vector<LuaValue>& a) {
        double m = a.empty() ? 0 : toNumber(a[0]);
        for (const auto& v : a) m = std::max(m, toNumber(v));
        return std::vector<LuaValue>{LuaValue::number(m)};
    }));
    math.t->set(LuaValue::string("min"), LuaValue::function([](const std::vector<LuaValue>& a) {
        double m = a.empty() ? 0 : toNumber(a[0]);
        for (const auto& v : a) m = std::min(m, toNumber(v));
        return std::vector<LuaValue>{LuaValue::number(m)};
    }));
    setGlobal("math", math);
    setGlobal("string", LuaValue::newTable());
}

bool LuaVM::run(const std::vector<uint8_t>& chunk, std::string* error) {
    std::string err;
    auto proto = loadLuaChunk(chunk, &err);
    if (!proto) {
        if (error) *error = err;
        return false;
    }
    auto cl = std::make_shared<LuaClosure>();
    cl->proto = proto;
    failed_ = false;
    steps_ = 0;
    execute(cl, {}, 0);
    if (failed_ && error) *error = error_;
    return !failed_;
}

std::vector<LuaValue> LuaVM::call(const LuaValue& fn, const std::vector<LuaValue>& args) {
    steps_ = 0;
    if (fn.type == LuaValue::Type::Native && fn.native) return (*fn.native)(args);
    if (fn.type == LuaValue::Type::Function && fn.f) return execute(fn.f, args, 0);
    return {};
}

std::vector<LuaValue> LuaVM::execute(const std::shared_ptr<LuaClosure>& cl, std::vector<LuaValue> args, int depth) {
    if (failed_) return {};
    if (depth > 180) {
        failed_ = true;
        error_ = "call stack too deep";
        return {};
    }
    const LuaProto& P = *cl->proto;
    using Cell = std::shared_ptr<LuaValue>;
    std::vector<Cell> R(size_t(std::max<int>(P.maxStack, P.params) + 2));
    for (auto& c : R) c = std::make_shared<LuaValue>();
    auto reg = [&](size_t i) -> LuaValue& {
        if (i >= R.size()) {
            const size_t old = R.size();
            R.resize(i + 8);
            for (size_t k = old; k < R.size(); ++k) R[k] = std::make_shared<LuaValue>();
        }
        return *R[i];
    };
    std::vector<LuaValue> varargs;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i < P.params) reg(i) = args[i];
        else if (P.varargFlags) varargs.push_back(args[i]);
    }
    const auto& K = P.constants;
    auto RK = [&](uint32_t x) -> LuaValue { return (x & 0x100) ? (size_t(x & 0xFF) < K.size() ? K[x & 0xFF] : LuaValue()) : reg(x); };
    // Metamethods: a table's metatable entry (nil when it has none).
    auto metamethod = [](const LuaValue& v, const char* event) -> LuaValue {
        if (!v.isTable() || !v.t->meta) return LuaValue();
        return v.t->meta->get(LuaValue::string(event));
    };
    std::function<std::vector<LuaValue>(const LuaValue&, std::vector<LuaValue>)> invoke =
        [&](const LuaValue& f, std::vector<LuaValue> a) -> std::vector<LuaValue> {
        if (f.type == LuaValue::Type::Native && f.native) return (*f.native)(a);
        if (f.type == LuaValue::Type::Function && f.f) return execute(f.f, std::move(a), depth + 1);
        if (const LuaValue call = metamethod(f, "__call"); !call.isNil()) { // a callable table
            a.insert(a.begin(), f);
            return invoke(call, std::move(a));
        }
        return {}; // calling nil: forgiving
    };
    auto first = [](std::vector<LuaValue> r) { return r.empty() ? LuaValue() : std::move(r[0]); };
    std::function<LuaValue(const LuaValue&, const LuaValue&, int)> indexDepth = [&](const LuaValue& t, const LuaValue& k, int d) -> LuaValue {
        if (t.isTable()) {
            LuaValue v = t.t->get(k);
            if (!v.isNil() || !t.t->meta || d > 100) return v;
            const LuaValue h = t.t->meta->get(LuaValue::string("__index"));
            if (h.isNil()) return v;
            if (h.isTable()) return indexDepth(h, k, d + 1);
            return first(invoke(h, {t, k}));
        }
        return LuaValue();
    };
    auto index = [&](const LuaValue& t, const LuaValue& k) -> LuaValue { return indexDepth(t, k, 0); };
    std::function<void(const LuaValue&, const LuaValue&, const LuaValue&, int)> assign =
        [&](const LuaValue& t, const LuaValue& k, const LuaValue& v, int d) {
        if (!t.isTable()) return;
        if (t.t->meta && d < 100 && t.t->get(k).isNil()) {
            const LuaValue h = t.t->meta->get(LuaValue::string("__newindex"));
            if (h.isTable()) { assign(h, k, v, d + 1); return; }
            if (!h.isNil()) { invoke(h, {t, k, v}); return; }
        }
        t.t->set(k, v);
    };
    static const char* kArithEvents[] = {"__add", "__sub", "__mul", "__div", "__mod", "__pow"};
    auto arith = [&](int op, const LuaValue& a, const LuaValue& b) -> LuaValue {
        if (a.isTable() || b.isTable()) {
            const char* ev = kArithEvents[op - 12];
            LuaValue h = metamethod(a, ev);
            if (h.isNil()) h = metamethod(b, ev);
            if (!h.isNil()) return first(invoke(h, {a, b}));
        }
        const double x = toNumber(a), y = toNumber(b);
        switch (op) {
        case 12: return LuaValue::number(x + y);
        case 13: return LuaValue::number(x - y);
        case 14: return LuaValue::number(x * y);
        case 15: return LuaValue::number(x / y);
        case 16: return LuaValue::number(y == 0 ? NAN : x - std::floor(x / y) * y);
        case 17: return LuaValue::number(std::pow(x, y));
        }
        return LuaValue();
    };
    auto lessThan = [&](const LuaValue& a, const LuaValue& b) {
        if (a.isString() && b.isString()) return a.s < b.s;
        if (a.isTable() && b.isTable())
            if (const LuaValue h = metamethod(a, "__lt"); !h.isNil()) return first(invoke(h, {a, b})).truthy();
        return toNumber(a) < toNumber(b);
    };
    auto lessEqual = [&](const LuaValue& a, const LuaValue& b) {
        if (a.isTable() && b.isTable()) {
            if (const LuaValue h = metamethod(a, "__le"); !h.isNil()) return first(invoke(h, {a, b})).truthy();
            if (const LuaValue h = metamethod(a, "__lt"); !h.isNil()) return !first(invoke(h, {b, a})).truthy();
        }
        return !lessThan(b, a);
    };
    auto equal = [&](const LuaValue& a, const LuaValue& b) {
        if (rawEquals(a, b)) return true;
        if (a.isTable() && b.isTable())
            if (const LuaValue h = metamethod(a, "__eq"); !h.isNil() && rawEquals(h, metamethod(b, "__eq"))) return first(invoke(h, {a, b})).truthy();
        return false;
    };

    size_t pc = 0, top = 0;
    while (pc < P.code.size()) {
        if (++steps_ > stepBudget) {
            failed_ = true;
            error_ = "step budget exhausted";
            return {};
        }
        if (failed_) return {};
        const uint32_t i = P.code[pc++];
        const int op = int(i & 0x3F);
        const uint32_t A = (i >> 6) & 0xFF, C = (i >> 14) & 0x1FF, B = (i >> 23) & 0x1FF, Bx = i >> 14;
        const int sBx = int(Bx) - 131071;
        switch (op) {
        case 0: reg(A) = reg(B); break;                                             // MOVE
        case 1: reg(A) = Bx < K.size() ? K[Bx] : LuaValue(); break;                 // LOADK
        case 2: reg(A) = LuaValue::boolean(B != 0); if (C) ++pc; break;             // LOADBOOL
        case 3: for (uint32_t k = A; k <= B; ++k) reg(k) = LuaValue(); break;       // LOADNIL
        case 4: reg(A) = B < cl->upvalues.size() ? *cl->upvalues[B] : LuaValue(); break; // GETUPVAL
        case 5: reg(A) = Bx < K.size() ? globals_->get(K[Bx]) : LuaValue(); break;  // GETGLOBAL
        case 6: { LuaValue t = reg(B); reg(A) = index(t, RK(C)); break; }           // GETTABLE
        case 7: if (Bx < K.size()) globals_->set(K[Bx], reg(A)); break;             // SETGLOBAL
        case 8: if (B < cl->upvalues.size()) *cl->upvalues[B] = reg(A); break;      // SETUPVAL
        case 9: { LuaValue t = reg(A); assign(t, RK(B), RK(C), 0); break; }         // SETTABLE
        case 10: reg(A) = LuaValue::newTable(); break;                               // NEWTABLE
        case 11: { LuaValue o = reg(B); reg(A + 1) = o; reg(A) = index(o, RK(C)); break; } // SELF
        case 12: case 13: case 14: case 15: case 16: case 17: reg(A) = arith(op, RK(B), RK(C)); break;
        case 18: {                                                                   // UNM
            const LuaValue v = reg(B);
            if (const LuaValue h = metamethod(v, "__unm"); !h.isNil()) reg(A) = first(invoke(h, {v, v}));
            else reg(A) = LuaValue::number(-toNumber(v));
            break;
        }
        case 19: reg(A) = LuaValue::boolean(!reg(B).truthy()); break;               // NOT
        case 20: {                                                                   // LEN
            const LuaValue v = reg(B);
            if (const LuaValue h = metamethod(v, "__len"); !h.isNil()) reg(A) = first(invoke(h, {v}));
            else reg(A) = LuaValue::number(v.isString() ? double(v.s.size()) : v.isTable() ? double(v.t->length()) : 0);
            break;
        }
        case 21: {                                                                   // CONCAT
            // Right to left, as Lua does, so a table operand's __concat sees the rest joined.
            LuaValue acc = reg(C);
            for (uint32_t k = C; k-- > B;) {
                const LuaValue left = reg(k);
                LuaValue h = metamethod(left, "__concat");
                if (h.isNil()) h = metamethod(acc, "__concat");
                if (!h.isNil()) acc = first(invoke(h, {left, acc}));
                else acc = LuaValue::string(left.str() + acc.str());
            }
            reg(A) = acc.isString() || acc.isTable() ? acc : LuaValue::string(acc.str());
            break;
        }
        case 22: pc = size_t(int64_t(pc) + sBx); break;                             // JMP
        case 23: if (equal(RK(B), RK(C)) != (A != 0)) ++pc; break;                  // EQ
        case 24: if (lessThan(RK(B), RK(C)) != (A != 0)) ++pc; break;               // LT
        case 25: if (lessEqual(RK(B), RK(C)) != (A != 0)) ++pc; break;              // LE
        case 26: if (reg(A).truthy() != (C != 0)) ++pc; break;                      // TEST
        case 27: if (reg(B).truthy() == (C != 0)) reg(A) = reg(B); else ++pc; break; // TESTSET
        case 28: case 29: {                                                          // CALL / TAILCALL
            const size_t nargs = B == 0 ? (top > A + 1 ? top - A - 1 : 0) : B - 1;
            std::vector<LuaValue> a;
            a.reserve(nargs);
            for (size_t k = 0; k < nargs; ++k) a.push_back(reg(A + 1 + k));
            std::vector<LuaValue> res = invoke(reg(A), std::move(a));
            if (op == 29) return res;
            if (C == 0) {
                for (size_t k = 0; k < res.size(); ++k) reg(A + k) = res[k];
                top = A + res.size();
            } else {
                for (size_t k = 0; k + 1 < C; ++k) reg(A + k) = k < res.size() ? res[k] : LuaValue();
            }
            break;
        }
        case 30: {                                                                   // RETURN
            const size_t n = B == 0 ? (top > A ? top - A : 0) : B - 1;
            std::vector<LuaValue> res;
            for (size_t k = 0; k < n; ++k) res.push_back(reg(A + k));
            return res;
        }
        case 31: {                                                                   // FORLOOP
            const double step = toNumber(reg(A + 2));
            const double v = toNumber(reg(A)) + step;
            reg(A) = LuaValue::number(v);
            const double limit = toNumber(reg(A + 1));
            if (step > 0 ? v <= limit : v >= limit) {
                pc = size_t(int64_t(pc) + sBx);
                reg(A + 3) = LuaValue::number(v);
            }
            break;
        }
        case 32:                                                                     // FORPREP
            reg(A) = LuaValue::number(toNumber(reg(A)) - toNumber(reg(A + 2)));
            pc = size_t(int64_t(pc) + sBx);
            break;
        case 33: {                                                                   // TFORLOOP
            std::vector<LuaValue> res = invoke(reg(A), {reg(A + 1), reg(A + 2)});
            for (uint32_t k = 0; k < C; ++k) reg(A + 3 + k) = k < res.size() ? res[k] : LuaValue();
            if (!reg(A + 3).isNil()) reg(A + 2) = reg(A + 3);
            else ++pc;
            break;
        }
        case 34: {                                                                   // SETLIST
            size_t n = B == 0 ? (top > A + 1 ? top - A - 1 : 0) : B;
            size_t c = C;
            if (c == 0 && pc < P.code.size()) c = P.code[pc++];
            LuaValue t = reg(A);
            if (t.isTable())
                for (size_t k = 1; k <= n; ++k) t.t->set(LuaValue::number(double((c - 1) * 50 + k)), reg(A + k));
            break;
        }
        case 35:                                                                     // CLOSE
            for (size_t k = A; k < R.size(); ++k) R[k] = std::make_shared<LuaValue>(*R[k]);
            break;
        case 36: {                                                                   // CLOSURE
            if (Bx >= P.protos.size() || !P.protos[Bx]) { reg(A) = LuaValue(); break; }
            auto nc = std::make_shared<LuaClosure>();
            nc->proto = P.protos[Bx];
            for (uint8_t u = 0; u < nc->proto->upvalues && pc < P.code.size(); ++u) {
                const uint32_t pi = P.code[pc++];
                const uint32_t pb = (pi >> 23) & 0x1FF;
                if ((pi & 0x3F) == 0) { reg(pb); nc->upvalues.push_back(R[pb]); }               // MOVE: share our register
                else nc->upvalues.push_back(pb < cl->upvalues.size() ? cl->upvalues[pb] : std::make_shared<LuaValue>());
            }
            LuaValue v;
            v.type = LuaValue::Type::Function;
            v.f = nc;
            reg(A) = v;
            break;
        }
        case 37: {                                                                   // VARARG
            const size_t n = B == 0 ? varargs.size() : B - 1;
            for (size_t k = 0; k < n; ++k) reg(A + k) = k < varargs.size() ? varargs[k] : LuaValue();
            if (B == 0) top = A + n;
            break;
        }
        default:
            failed_ = true;
            error_ = "unknown opcode " + std::to_string(op);
            return {};
        }
    }
    return {};
}

// ---------------------------------------------------------------- source data

namespace {

struct Token {
    enum Kind { Name, String, Number, Symbol, End } kind = End;
    std::string text;
    double number = 0;
};

std::vector<Token> tokenize(std::string_view s) {
    std::vector<Token> out;
    size_t i = 0;
    auto longBracket = [&](size_t at, size_t& level) -> bool { // [[ or [==[
        if (at >= s.size() || s[at] != '[') return false;
        size_t j = at + 1;
        level = 0;
        while (j < s.size() && s[j] == '=') { ++level; ++j; }
        return j < s.size() && s[j] == '[';
    };
    auto skipLong = [&](size_t start, size_t level, std::string* text) {
        const std::string close = "]" + std::string(level, '=') + "]";
        const size_t body = start + level + 2;
        const size_t end = s.find(close, body);
        if (text) *text = std::string(s.substr(body, end == std::string_view::npos ? std::string_view::npos : end - body));
        return end == std::string_view::npos ? s.size() : end + close.size();
    };
    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            size_t level = 0;
            if (longBracket(i + 2, level)) { i = skipLong(i + 2, level, nullptr); continue; }
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        Token t;
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const size_t b = i;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
            t.kind = Token::Name;
            t.text = std::string(s.substr(b, i - b));
        } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            const size_t b = i;
            if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                i += 2;
                while (i < s.size() && std::isxdigit(static_cast<unsigned char>(s[i]))) ++i;
            } else {
                while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                        ((s[i] == '-' || s[i] == '+') && (s[i - 1] == 'e' || s[i - 1] == 'E'))))
                    ++i;
            }
            t.kind = Token::Number;
            t.text = std::string(s.substr(b, i - b));
            t.number = std::strtod(t.text.c_str(), nullptr);
        } else if (c == '"' || c == '\'') {
            ++i;
            t.kind = Token::String;
            while (i < s.size() && s[i] != c) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    const char e = s[++i];
                    t.text += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                } else {
                    t.text += s[i];
                }
                ++i;
            }
            ++i;
        } else if (size_t level = 0; longBracket(i, level)) {
            t.kind = Token::String;
            i = skipLong(i, level, &t.text);
        } else {
            t.kind = Token::Symbol;
            static const char* kMulti[] = {"...", "==", "~=", "<=", ">=", ".."};
            t.text = std::string(1, c);
            for (const char* m : kMulti)
                if (s.substr(i, std::strlen(m)) == m) { t.text = m; break; }
            i += t.text.size();
        }
        out.push_back(std::move(t));
    }
    out.push_back(Token{});
    return out;
}

struct DataParser {
    const std::vector<Token>& tk;
    LuaTable& globals;
    size_t p = 0;
    const Token& peek(size_t k = 0) const { return tk[std::min(p + k, tk.size() - 1)]; }
    bool sym(const char* v, size_t k = 0) const { return peek(k).kind == Token::Symbol && peek(k).text == v; }

    // Skips the rest of an expression up to a separator at this nesting level.
    void skipExpression() {
        int depth = 0;
        while (peek().kind != Token::End) {
            if (depth == 0 && (sym(",") || sym(";") || sym("}") || sym(")") || sym("]"))) return;
            if (sym("{") || sym("(") || sym("[")) ++depth;
            if (sym("}") || sym(")") || sym("]")) --depth;
            ++p;
        }
    }
    LuaValue expression() {
        LuaValue v = primary();
        const Token& n = peek();
        if (n.kind == Token::Symbol && n.text != "," && n.text != ";" && n.text != "}" && n.text != ")" && n.text != "]") {
            skipExpression(); // an operator follows: not a literal
            return LuaValue();
        }
        return v;
    }
    LuaValue primary() {
        const Token& t = peek();
        if (t.kind == Token::Number) { ++p; return LuaValue::number(t.number); }
        if (t.kind == Token::String) { ++p; return LuaValue::string(t.text); }
        if (sym("-") && peek(1).kind == Token::Number) { p += 2; return LuaValue::number(-tk[p - 1].number); }
        if (sym("{")) return table();
        if (t.kind == Token::Name) {
            if (t.text == "true") { ++p; return LuaValue::boolean(true); }
            if (t.text == "false") { ++p; return LuaValue::boolean(false); }
            if (t.text == "nil") { ++p; return LuaValue(); }
            const std::string name = t.text;
            ++p;
            if (sym("(")) { // a call: Find_Object_Type("X") -> "X", anything else nil
                ++p;
                LuaValue first;
                bool any = false;
                while (!sym(")") && peek().kind != Token::End) {
                    LuaValue a = expression();
                    if (!any) { first = a; any = true; }
                    if (sym(",")) ++p;
                    else if (!sym(")")) ++p;
                }
                if (sym(")")) ++p;
                return name == "Find_Object_Type" ? first : LuaValue();
            }
            if (sym(".") || sym("[") || sym(":")) { skipExpression(); return LuaValue(); }
            return globals.get(LuaValue::string(name));
        }
        skipExpression();
        return LuaValue();
    }
    LuaValue table() {
        LuaValue t = LuaValue::newTable();
        ++p; // {
        double next = 1;
        while (!sym("}") && peek().kind != Token::End) {
            if (sym("[")) {
                ++p;
                LuaValue k = expression();
                if (sym("]")) ++p;
                if (sym("=")) ++p;
                LuaValue v = expression();
                t.t->set(k, v);
            } else if (peek().kind == Token::Name && sym("=", 1)) {
                const std::string k = peek().text;
                p += 2;
                t.t->set(LuaValue::string(k), expression());
            } else {
                t.t->set(LuaValue::number(next++), expression());
            }
            if (sym(",") || sym(";")) ++p;
            else if (!sym("}")) ++p; // tolerate junk
        }
        if (sym("}")) ++p;
        return t;
    }
    void run() {
        while (peek().kind != Token::End) {
            if (peek().kind == Token::Name && sym("=", 1) && !(p > 0 && tk[p - 1].kind == Token::Symbol && tk[p - 1].text == ".")) {
                const std::string name = peek().text;
                p += 2;
                const bool literal = sym("{") || peek().kind == Token::String || peek().kind == Token::Number ||
                                     (peek().kind == Token::Name && (peek().text == "true" || peek().text == "false"));
                if (!literal) continue;
                LuaValue v = expression();
                globals.set(LuaValue::string(name), v);
                continue;
            }
            ++p;
        }
    }
};

} // namespace

void LuaVM::loadDataAssignments(std::string_view source) {
    const std::vector<Token> tokens = tokenize(source);
    DataParser parser{tokens, *globals_};
    parser.run();
}

} // namespace alamo
