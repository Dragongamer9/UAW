// A Lua 5.1 source compiler for LuaVM: some mods ship their scripts as plain source instead of
// the game's compiled bytecode. Source is parsed into a small syntax tree, then each function is
// turned into the same Lua 5.1 instructions luac would produce (not always the same choice of
// registers or jumps, but the same meaning), as a LuaProto the VM runs directly.
#include "alamo/luavm.hpp"

#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace alamo {
namespace {

struct SyntaxError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ------------------------------------------------------------------ lexer

enum class T {
    Eof, Name, Number, String, Symbol,
    And, Break, Do, Else, Elseif, End, False, For, Function, If, In, Local, Nil, Not, Or, Repeat, Return, Then,
    True, Until, While,
};

struct Tok {
    T t = T::Eof;
    std::string s; // name, string contents or symbol ("..", "==", "(", ...)
    double n = 0;
    int line = 1;
};

class Lexer {
public:
    explicit Lexer(std::string_view src) : s_(src) {
        if (s_.size() >= 3 && uint8_t(s_[0]) == 0xEF && uint8_t(s_[1]) == 0xBB && uint8_t(s_[2]) == 0xBF) p_ = 3; // UTF-8 BOM
        if (p_ < s_.size() && s_[p_] == '#')
            while (p_ < s_.size() && s_[p_] != '\n') ++p_;
    }

    Tok next() {
        skipSpace();
        Tok t;
        t.line = line_;
        if (p_ >= s_.size()) return t;
        const char c = s_[p_];
        if (std::isalpha(uint8_t(c)) || c == '_') {
            const size_t b = p_;
            while (p_ < s_.size() && (std::isalnum(uint8_t(s_[p_])) || s_[p_] == '_')) ++p_;
            t.s = std::string(s_.substr(b, p_ - b));
            static const std::unordered_map<std::string, T> kw = {
                {"and", T::And}, {"break", T::Break}, {"do", T::Do}, {"else", T::Else}, {"elseif", T::Elseif},
                {"end", T::End}, {"false", T::False}, {"for", T::For}, {"function", T::Function}, {"if", T::If},
                {"in", T::In}, {"local", T::Local}, {"nil", T::Nil}, {"not", T::Not}, {"or", T::Or},
                {"repeat", T::Repeat}, {"return", T::Return}, {"then", T::Then}, {"true", T::True},
                {"until", T::Until}, {"while", T::While}};
            auto it = kw.find(t.s);
            t.t = it == kw.end() ? T::Name : it->second;
            return t;
        }
        if (std::isdigit(uint8_t(c)) || (c == '.' && p_ + 1 < s_.size() && std::isdigit(uint8_t(s_[p_ + 1])))) {
            t.t = T::Number;
            const size_t b = p_;
            if (c == '0' && p_ + 1 < s_.size() && (s_[p_ + 1] == 'x' || s_[p_ + 1] == 'X')) {
                p_ += 2;
                while (p_ < s_.size() && std::isxdigit(uint8_t(s_[p_]))) ++p_;
                t.n = double(std::strtoull(std::string(s_.substr(b + 2, p_ - b - 2)).c_str(), nullptr, 16));
                return t;
            }
            while (p_ < s_.size()) {
                const char d = s_[p_];
                if (std::isdigit(uint8_t(d)) || d == '.') ++p_;
                else if ((d == 'e' || d == 'E')) {
                    ++p_;
                    if (p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-')) ++p_;
                } else break;
            }
            const std::string text(s_.substr(b, p_ - b));
            char* end = nullptr;
            t.n = std::strtod(text.c_str(), &end);
            if (!end || *end) fail("malformed number '" + text + "'");
            return t;
        }
        if (c == '"' || c == '\'') {
            t.t = T::String;
            t.s = quoted(c);
            return t;
        }
        if (c == '[') {
            const int level = longBracket();
            if (level >= 0) {
                t.t = T::String;
                t.s = longString(level);
                return t;
            }
        }
        t.t = T::Symbol;
        static const char* kLong[] = {"...", "..", "==", "~=", "<=", ">="};
        for (const char* sym : kLong)
            if (s_.compare(p_, std::strlen(sym), sym) == 0) {
                t.s = sym;
                p_ += std::strlen(sym);
                return t;
            }
        if (!std::strchr("+-*/%^#<>=(){}[];:,.", c)) fail(std::string("unexpected character '") + c + "'");
        t.s = std::string(1, c);
        ++p_;
        return t;
    }

    [[noreturn]] void fail(const std::string& msg) const { throw SyntaxError("line " + std::to_string(line_) + ": " + msg); }

private:
    std::string_view s_;
    size_t p_ = 0;
    int line_ = 1;

    void skipSpace() {
        while (p_ < s_.size()) {
            const char c = s_[p_];
            if (c == '\n') {
                ++line_;
                ++p_;
            } else if (std::isspace(uint8_t(c))) {
                ++p_;
            } else if (c == '-' && p_ + 1 < s_.size() && s_[p_ + 1] == '-') {
                p_ += 2;
                if (p_ < s_.size() && s_[p_] == '[') {
                    const int level = longBracket();
                    if (level >= 0) {
                        longString(level);
                        continue;
                    }
                }
                while (p_ < s_.size() && s_[p_] != '\n') ++p_;
            } else {
                break;
            }
        }
    }
    // At '[': the level of a long bracket "[==[" (consumed), or -1 (nothing consumed).
    int longBracket() {
        size_t q = p_ + 1;
        int level = 0;
        while (q < s_.size() && s_[q] == '=') ++q, ++level;
        if (q < s_.size() && s_[q] == '[') {
            p_ = q + 1;
            return level;
        }
        return -1;
    }
    std::string longString(int level) {
        if (p_ < s_.size() && s_[p_] == '\r') ++p_;
        if (p_ < s_.size() && s_[p_] == '\n') ++p_, ++line_;
        std::string close = "]" + std::string(size_t(level), '=') + "]";
        const size_t end = s_.find(close, p_);
        if (end == std::string_view::npos) fail("unfinished long string");
        std::string out(s_.substr(p_, end - p_));
        for (char ch : out)
            if (ch == '\n') ++line_;
        p_ = end + close.size();
        return out;
    }
    std::string quoted(char q) {
        ++p_;
        std::string out;
        while (true) {
            if (p_ >= s_.size() || s_[p_] == '\n') fail("unfinished string");
            char c = s_[p_++];
            if (c == q) break;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p_ >= s_.size()) fail("unfinished string");
            c = s_[p_++];
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'a': out += '\a'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'v': out += '\v'; break;
            case '\n': out += '\n'; ++line_; break;
            default:
                if (std::isdigit(uint8_t(c))) {
                    int v = c - '0';
                    for (int k = 0; k < 2 && p_ < s_.size() && std::isdigit(uint8_t(s_[p_])); ++k) v = v * 10 + (s_[p_++] - '0');
                    out += char(v);
                } else {
                    out += c; // \\ \" \' and anything else as itself
                }
            }
        }
        return out;
    }
};

// ------------------------------------------------------------------ syntax tree

struct Expr;
struct Stat;
using ExprP = std::unique_ptr<Expr>;
using Block = std::vector<std::unique_ptr<Stat>>;

struct FuncBody {
    std::vector<std::string> params;
    bool vararg = false;
    Block body;
};

struct Expr {
    enum Kind { Nil, True, False, Num, Str, Vararg, Func, Name, Index, Call, Method, Table, Bin, Un, Paren } k = Nil;
    double n = 0;
    std::string s;               // Str, Name, Method name, Bin/Un operator
    ExprP a, b;                  // Index (object, key), Bin (left, right), Un/Paren (operand), Call/Method (callee/object)
    std::vector<ExprP> args;     // Call/Method
    std::shared_ptr<FuncBody> fn;
    struct Field {
        ExprP key; // null: positional
        ExprP value;
    };
    std::vector<Field> fields;   // Table
    bool multi() const { return k == Call || k == Method || k == Vararg; }
};

struct Stat {
    enum Kind { Call, Local, Assign, Do, While, Repeat, If, NumFor, GenFor, LocalFunction, Return, Break } k = Do;
    std::vector<std::string> names;
    std::vector<ExprP> targets, exprs, conds; // If: conds[i] guards blocks[i]; one more block is the else
    std::vector<Block> blocks;
    ExprP call;
    std::shared_ptr<FuncBody> fn;
};

class Parser {
public:
    explicit Parser(std::string_view src) : lex_(src) { tok_ = lex_.next(); ahead_ = lex_.next(); }

    std::shared_ptr<FuncBody> chunk() {
        auto f = std::make_shared<FuncBody>();
        f->vararg = true;
        f->body = block();
        if (tok_.t != T::Eof) fail("'<eof>' expected");
        return f;
    }

private:
    Lexer lex_;
    Tok tok_, ahead_;

    [[noreturn]] void fail(const std::string& msg) const {
        throw SyntaxError("line " + std::to_string(tok_.line) + ": " + msg + " near '" + (tok_.t == T::Eof ? "<eof>" : tok_.s) + "'");
    }
    void advance() {
        tok_ = std::move(ahead_);
        ahead_ = lex_.next();
    }
    bool sym(const char* s) const { return tok_.t == T::Symbol && tok_.s == s; }
    bool accept(const char* s) {
        if (!sym(s)) return false;
        advance();
        return true;
    }
    bool accept(T t) {
        if (tok_.t != t) return false;
        advance();
        return true;
    }
    void expect(const char* s) {
        if (!accept(s)) fail(std::string("'") + s + "' expected");
    }
    void expect(T t, const char* what) {
        if (!accept(t)) fail(std::string("'") + what + "' expected");
    }
    std::string name() {
        if (tok_.t != T::Name) fail("name expected");
        std::string n = tok_.s;
        advance();
        return n;
    }
    bool blockFollows() const {
        return tok_.t == T::Eof || tok_.t == T::End || tok_.t == T::Else || tok_.t == T::Elseif || tok_.t == T::Until;
    }

    Block block() {
        Block b;
        while (!blockFollows()) {
            const bool last = tok_.t == T::Return || tok_.t == T::Break;
            b.push_back(statement());
            accept(";");
            if (last) break;
        }
        return b;
    }

    std::unique_ptr<Stat> statement() {
        auto s = std::make_unique<Stat>();
        switch (tok_.t) {
        case T::If: {
            advance();
            s->k = Stat::If;
            s->conds.push_back(expr());
            expect(T::Then, "then");
            s->blocks.push_back(block());
            while (tok_.t == T::Elseif) {
                advance();
                s->conds.push_back(expr());
                expect(T::Then, "then");
                s->blocks.push_back(block());
            }
            if (accept(T::Else)) s->blocks.push_back(block());
            expect(T::End, "end");
            return s;
        }
        case T::While:
            advance();
            s->k = Stat::While;
            s->conds.push_back(expr());
            expect(T::Do, "do");
            s->blocks.push_back(block());
            expect(T::End, "end");
            return s;
        case T::Do:
            advance();
            s->k = Stat::Do;
            s->blocks.push_back(block());
            expect(T::End, "end");
            return s;
        case T::For: {
            advance();
            std::string first = name();
            if (accept("=")) {
                s->k = Stat::NumFor;
                s->names.push_back(first);
                s->exprs.push_back(expr());
                expect(",");
                s->exprs.push_back(expr());
                if (accept(",")) s->exprs.push_back(expr());
            } else {
                s->k = Stat::GenFor;
                s->names.push_back(first);
                while (accept(",")) s->names.push_back(name());
                expect(T::In, "in");
                s->exprs = exprList();
            }
            expect(T::Do, "do");
            s->blocks.push_back(block());
            expect(T::End, "end");
            return s;
        }
        case T::Repeat:
            advance();
            s->k = Stat::Repeat;
            s->blocks.push_back(block());
            expect(T::Until, "until");
            s->conds.push_back(expr());
            return s;
        case T::Function: {
            advance();
            // function a.b.c:m(...) -> an assignment of the function to a.b.c.m
            auto target = std::make_unique<Expr>();
            target->k = Expr::Name;
            target->s = name();
            bool method = false;
            while (sym(".") || sym(":")) {
                method = sym(":");
                advance();
                auto key = std::make_unique<Expr>();
                key->k = Expr::Str;
                key->s = name();
                auto ix = std::make_unique<Expr>();
                ix->k = Expr::Index;
                ix->a = std::move(target);
                ix->b = std::move(key);
                target = std::move(ix);
                if (method) break;
            }
            auto f = std::make_unique<Expr>();
            f->k = Expr::Func;
            f->fn = body(method);
            s->k = Stat::Assign;
            s->targets.push_back(std::move(target));
            s->exprs.push_back(std::move(f));
            return s;
        }
        case T::Local:
            advance();
            if (accept(T::Function)) {
                s->k = Stat::LocalFunction;
                s->names.push_back(name());
                s->fn = body(false);
                return s;
            }
            s->k = Stat::Local;
            s->names.push_back(name());
            while (accept(",")) s->names.push_back(name());
            if (accept("=")) s->exprs = exprList();
            return s;
        case T::Return:
            advance();
            s->k = Stat::Return;
            if (!blockFollows() && !sym(";")) s->exprs = exprList();
            return s;
        case T::Break:
            advance();
            s->k = Stat::Break;
            return s;
        default: {
            ExprP e = suffixed();
            if (sym("=") || sym(",")) {
                s->k = Stat::Assign;
                s->targets.push_back(std::move(e));
                while (accept(",")) s->targets.push_back(suffixed());
                expect("=");
                s->exprs = exprList();
                for (const ExprP& t : s->targets)
                    if (t->k != Expr::Name && t->k != Expr::Index) fail("cannot assign to this expression");
                return s;
            }
            if (e->k != Expr::Call && e->k != Expr::Method) fail("syntax error");
            s->k = Stat::Call;
            s->call = std::move(e);
            return s;
        }
        }
    }

    std::shared_ptr<FuncBody> body(bool method) {
        auto f = std::make_shared<FuncBody>();
        if (method) f->params.push_back("self");
        expect("(");
        if (!sym(")")) {
            do {
                if (accept("...")) {
                    f->vararg = true;
                    break;
                }
                f->params.push_back(name());
            } while (accept(","));
        }
        expect(")");
        f->body = block();
        expect(T::End, "end");
        return f;
    }

    std::vector<ExprP> exprList() {
        std::vector<ExprP> v;
        v.push_back(expr());
        while (accept(",")) v.push_back(expr());
        return v;
    }

    ExprP primary() {
        if (tok_.t == T::Name) {
            auto e = std::make_unique<Expr>();
            e->k = Expr::Name;
            e->s = name();
            return e;
        }
        if (accept("(")) {
            auto e = std::make_unique<Expr>();
            e->k = Expr::Paren;
            e->a = expr();
            expect(")");
            return e;
        }
        fail("unexpected symbol");
    }

    std::vector<ExprP> callArgs() {
        std::vector<ExprP> args;
        if (tok_.t == T::String) {
            auto e = std::make_unique<Expr>();
            e->k = Expr::Str;
            e->s = tok_.s;
            advance();
            args.push_back(std::move(e));
        } else if (sym("{")) {
            args.push_back(table());
        } else {
            expect("(");
            if (!sym(")")) args = exprList();
            expect(")");
        }
        return args;
    }

    ExprP suffixed() {
        ExprP e = primary();
        while (true) {
            if (accept(".")) {
                auto key = std::make_unique<Expr>();
                key->k = Expr::Str;
                key->s = name();
                auto ix = std::make_unique<Expr>();
                ix->k = Expr::Index;
                ix->a = std::move(e);
                ix->b = std::move(key);
                e = std::move(ix);
            } else if (accept("[")) {
                auto ix = std::make_unique<Expr>();
                ix->k = Expr::Index;
                ix->a = std::move(e);
                ix->b = expr();
                expect("]");
                e = std::move(ix);
            } else if (accept(":")) {
                auto m = std::make_unique<Expr>();
                m->k = Expr::Method;
                m->s = name();
                m->a = std::move(e);
                m->args = callArgs();
                e = std::move(m);
            } else if (sym("(") || sym("{") || tok_.t == T::String) {
                auto c = std::make_unique<Expr>();
                c->k = Expr::Call;
                c->a = std::move(e);
                c->args = callArgs();
                e = std::move(c);
            } else {
                return e;
            }
        }
    }

    ExprP table() {
        expect("{");
        auto t = std::make_unique<Expr>();
        t->k = Expr::Table;
        while (!sym("}")) {
            Expr::Field f;
            if (accept("[")) {
                f.key = expr();
                expect("]");
                expect("=");
                f.value = expr();
            } else if (tok_.t == T::Name && ahead_.t == T::Symbol && ahead_.s == "=") {
                f.key = std::make_unique<Expr>();
                f.key->k = Expr::Str;
                f.key->s = name();
                advance(); // '='
                f.value = expr();
            } else {
                f.value = expr();
            }
            t->fields.push_back(std::move(f));
            if (!accept(",") && !accept(";")) break;
        }
        expect("}");
        return t;
    }

    ExprP simple() {
        auto e = std::make_unique<Expr>();
        switch (tok_.t) {
        case T::Number: e->k = Expr::Num; e->n = tok_.n; advance(); return e;
        case T::String: e->k = Expr::Str; e->s = tok_.s; advance(); return e;
        case T::Nil: e->k = Expr::Nil; advance(); return e;
        case T::True: e->k = Expr::True; advance(); return e;
        case T::False: e->k = Expr::False; advance(); return e;
        case T::Function: advance(); e->k = Expr::Func; e->fn = body(false); return e;
        default: break;
        }
        if (accept("...")) {
            e->k = Expr::Vararg;
            return e;
        }
        if (sym("{")) return table();
        return suffixed();
    }

    // Binary operator priorities (left, right), as in Lua 5.1's parser.
    bool binary(int& left, int& right, std::string& op) const {
        struct P { const char* op; int l, r; };
        static const P kOps[] = {{"+", 6, 6}, {"-", 6, 6}, {"*", 7, 7}, {"/", 7, 7}, {"%", 7, 7}, {"^", 10, 9},
                                 {"..", 5, 4}, {"==", 3, 3}, {"~=", 3, 3}, {"<", 3, 3}, {"<=", 3, 3}, {">", 3, 3}, {">=", 3, 3}};
        if (tok_.t == T::And) { op = "and"; left = right = 2; return true; }
        if (tok_.t == T::Or) { op = "or"; left = right = 1; return true; }
        if (tok_.t != T::Symbol) return false;
        for (const P& p : kOps)
            if (tok_.s == p.op) {
                op = p.op;
                left = p.l;
                right = p.r;
                return true;
            }
        return false;
    }

    ExprP expr(int limit = 0) {
        ExprP e;
        if (tok_.t == T::Not || sym("-") || sym("#")) {
            auto u = std::make_unique<Expr>();
            u->k = Expr::Un;
            u->s = tok_.t == T::Not ? "not" : tok_.s;
            advance();
            u->a = expr(8);
            e = std::move(u);
        } else {
            e = simple();
        }
        int l = 0, r = 0;
        std::string op;
        while (binary(l, r, op) && l > limit) {
            advance();
            auto b = std::make_unique<Expr>();
            b->k = Expr::Bin;
            b->s = op;
            b->a = std::move(e);
            b->b = expr(r);
            e = std::move(b);
        }
        return e;
    }
};

// ------------------------------------------------------------------ code generation

enum Op {
    MOVE, LOADK, LOADBOOL, LOADNIL, GETUPVAL, GETGLOBAL, GETTABLE, SETGLOBAL, SETUPVAL, SETTABLE, NEWTABLE, SELF,
    ADD, SUB, MUL, DIV, MOD, POW, UNM, NOT, LEN, CONCAT, JMP, EQ, LT, LE, TEST, TESTSET, CALL, TAILCALL, RETURN,
    FORLOOP, FORPREP, TFORLOOP, SETLIST, CLOSE, CLOSURE, VARARG,
};

constexpr int kMaxRegs = 250;

struct FuncState {
    FuncState* parent = nullptr;
    std::shared_ptr<LuaProto> f = std::make_shared<LuaProto>();
    struct Local {
        std::string name;
        size_t block; // index into blocks
    };
    std::vector<Local> locals; // active locals; local i lives in register i
    struct Scope {
        size_t nactive;
        bool loop = false, captured = false;
        std::vector<size_t> breaks;
    };
    std::vector<Scope> blocks;
    struct Up {
        std::string name;
        bool inStack;
        int index;
    };
    std::vector<Up> ups;
    int freereg = 0, maxStack = 2;
    std::map<std::pair<int, std::string>, int> constants;
};

class Compiler {
public:
    std::shared_ptr<LuaProto> compile(const FuncBody& main) { return function(main, nullptr); }

private:
    FuncState* fs_ = nullptr;

    [[noreturn]] static void fail(const std::string& msg) { throw SyntaxError(msg); }

    // ---- emitting
    size_t emit(uint32_t i) {
        fs_->f->code.push_back(i);
        return fs_->f->code.size() - 1;
    }
    size_t abc(Op op, int a, int b, int c) { return emit(uint32_t(op) | uint32_t(a) << 6 | uint32_t(c) << 14 | uint32_t(b) << 23); }
    size_t abx(Op op, int a, int bx) { return emit(uint32_t(op) | uint32_t(a) << 6 | uint32_t(bx) << 14); }
    size_t asbx(Op op, int a, int sbx) { return abx(op, a, sbx + 131071); }
    size_t here() const { return fs_->f->code.size(); }
    size_t jump() { return asbx(JMP, 0, 0); }
    // Points the jump (or FORPREP/FORLOOP) at `pc` to `target`.
    void patch(size_t pc, size_t target) {
        uint32_t& i = fs_->f->code[pc];
        const int sbx = int(int64_t(target) - int64_t(pc + 1));
        i = (i & 0x3FFFu) | uint32_t(sbx + 131071) << 14;
    }

    int reserve(int n = 1) {
        const int r = fs_->freereg;
        fs_->freereg += n;
        if (fs_->freereg > kMaxRegs) fail("function or expression too complex");
        fs_->maxStack = std::max(fs_->maxStack, fs_->freereg + 1);
        return r;
    }
    int nactive() const { return int(fs_->locals.size()); }

    int constant(const LuaValue& v) {
        std::pair<int, std::string> key{int(v.type), ""};
        if (v.type == LuaValue::Type::Number) key.second.assign(reinterpret_cast<const char*>(&v.n), sizeof v.n);
        else if (v.type == LuaValue::Type::String) key.second = v.s;
        else if (v.type == LuaValue::Type::Bool) key.second = v.b ? "1" : "0";
        auto it = fs_->constants.find(key);
        if (it != fs_->constants.end()) return it->second;
        const int k = int(fs_->f->constants.size());
        fs_->f->constants.push_back(v);
        fs_->constants.emplace(key, k);
        return k;
    }
    int stringConstant(const std::string& s) { return constant(LuaValue::string(s)); }

    // ---- scopes and names
    void enterBlock(bool loop) {
        FuncState::Scope s;
        s.nactive = fs_->locals.size();
        s.loop = loop;
        fs_->blocks.push_back(std::move(s));
    }
    // Closes the block: its locals go out of scope (CLOSE first if a closure captured one).
    // Returns its pending breaks.
    std::vector<size_t> leaveBlock() {
        FuncState::Scope s = std::move(fs_->blocks.back());
        fs_->blocks.pop_back();
        if (s.captured && fs_->locals.size() > s.nactive) abc(CLOSE, int(s.nactive), 0, 0);
        fs_->locals.resize(s.nactive);
        fs_->freereg = int(s.nactive);
        return std::move(s.breaks);
    }
    void addLocal(const std::string& name) {
        fs_->locals.push_back({name, fs_->blocks.size() - 1});
        fs_->freereg = std::max(fs_->freereg, nactive());
        fs_->maxStack = std::max(fs_->maxStack, fs_->freereg + 1);
    }
    static int findLocal(const FuncState& fs, const std::string& name) {
        for (size_t i = fs.locals.size(); i-- > 0;)
            if (fs.locals[i].name == name) return int(i);
        return -1;
    }
    static int findUp(FuncState& fs, const std::string& name) {
        for (size_t i = 0; i < fs.ups.size(); ++i)
            if (fs.ups[i].name == name) return int(i);
        if (!fs.parent) return -1;
        if (const int r = findLocal(*fs.parent, name); r >= 0) {
            fs.parent->blocks[fs.parent->locals[size_t(r)].block].captured = true;
            fs.ups.push_back({name, true, r});
        } else {
            const int u = findUp(*fs.parent, name);
            if (u < 0) return -1;
            fs.ups.push_back({name, false, u});
        }
        if (fs.ups.size() > 255) fail("too many upvalues");
        return int(fs.ups.size() - 1);
    }
    struct Var {
        enum Kind { Local, Up, Global } k;
        int index;
    };
    Var resolve(const std::string& name) {
        if (const int r = findLocal(*fs_, name); r >= 0) return {Var::Local, r};
        if (const int u = findUp(*fs_, name); u >= 0) return {Var::Up, u};
        return {Var::Global, stringConstant(name)};
    }

    // ---- functions
    std::shared_ptr<LuaProto> function(const FuncBody& fb, FuncState* parent) {
        FuncState fs;
        fs.parent = parent;
        FuncState* saved = fs_;
        fs_ = &fs;
        enterBlock(false);
        for (const std::string& p : fb.params) addLocal(p);
        fs.f->params = uint8_t(fb.params.size());
        fs.f->varargFlags = fb.vararg ? 2 : 0;
        statements(fb.body);
        abc(RETURN, 0, 1, 0);
        leaveBlock();
        fs.f->maxStack = uint8_t(std::min(fs.maxStack + 2, 255));
        fs.f->upvalues = uint8_t(fs.ups.size());
        lastUps_ = fs.ups; // for the parent's CLOSURE pseudo-instructions
        fs_ = saved;
        return fs.f;
    }

    // ---- expressions
    // The value of `e` in register `t` (already reserved).
    void toReg(const Expr& e, int t) {
        switch (e.k) {
        case Expr::Nil: abc(LOADNIL, t, t, 0); return;
        case Expr::True: abc(LOADBOOL, t, 1, 0); return;
        case Expr::False: abc(LOADBOOL, t, 0, 0); return;
        case Expr::Num: abx(LOADK, t, constant(LuaValue::number(e.n))); return;
        case Expr::Str: abx(LOADK, t, stringConstant(e.s)); return;
        case Expr::Vararg: abc(VARARG, t, 2, 0); return;
        case Expr::Paren: toReg(*e.a, t); return;
        case Expr::Func: {
            std::shared_ptr<LuaProto> p = function(*e.fn, fs_);
            fs_->f->protos.push_back(p);
            abx(CLOSURE, t, int(fs_->f->protos.size() - 1));
            for (const FuncState::Up& u : lastUps_) abc(u.inStack ? MOVE : GETUPVAL, 0, u.index, 0);
            return;
        }
        case Expr::Name: {
            const Var v = resolve(e.s);
            if (v.k == Var::Local) {
                if (v.index != t) abc(MOVE, t, v.index, 0);
            } else if (v.k == Var::Up) {
                abc(GETUPVAL, t, v.index, 0);
            } else {
                abx(GETGLOBAL, t, v.index);
            }
            return;
        }
        case Expr::Index: {
            const int save = fs_->freereg;
            const int o = anyReg(*e.a);
            const int k = rk(*e.b);
            abc(GETTABLE, t, o, k);
            fs_->freereg = save;
            return;
        }
        case Expr::Call:
        case Expr::Method:
        case Expr::Table: {
            const int save = fs_->freereg;
            int base = t;
            if (t == fs_->freereg - 1) fs_->freereg = t; // the target is the top: build it there
            else base = fs_->freereg;
            reserve(1);
            if (e.k == Expr::Table) tableAt(e, base);
            else callAt(e, base, 1);
            if (base != t) abc(MOVE, t, base, 0);
            fs_->freereg = save;
            return;
        }
        case Expr::Un: {
            const int save = fs_->freereg;
            const int r = anyReg(*e.a);
            abc(e.s == "not" ? NOT : e.s == "-" ? UNM : LEN, t, r, 0);
            fs_->freereg = save;
            return;
        }
        case Expr::Bin: binaryTo(e, t); return;
        }
    }

    // Any register holding the value: a local's own, or a new temporary.
    int anyReg(const Expr& e) {
        if (e.k == Expr::Name) {
            const Var v = resolve(e.s);
            if (v.k == Var::Local) return v.index;
        }
        const int r = reserve(1);
        toReg(e, r);
        return r;
    }
    // An RK operand: a constant (when it fits) or a register.
    int rk(const Expr& e) {
        int k = -1;
        if (e.k == Expr::Num) k = constant(LuaValue::number(e.n));
        else if (e.k == Expr::Str) k = stringConstant(e.s);
        else if (e.k == Expr::Nil) k = constant(LuaValue());
        else if (e.k == Expr::True || e.k == Expr::False) k = constant(LuaValue::boolean(e.k == Expr::True));
        if (k >= 0 && k < 256) return 0x100 | k;
        return anyReg(e);
    }

    void binaryTo(const Expr& e, int t) {
        const std::string& op = e.s;
        if (op == "and" || op == "or") {
            toReg(*e.a, t);
            abc(TEST, t, 0, op == "and" ? 0 : 1);
            const size_t j = jump();
            toReg(*e.b, t);
            patch(j, here());
            return;
        }
        const int save = fs_->freereg;
        if (op == "..") {
            const int r1 = reserve(1);
            toReg(*e.a, r1);
            const int r2 = reserve(1);
            toReg(*e.b, r2);
            abc(CONCAT, t, r1, r2);
            fs_->freereg = save;
            return;
        }
        static const std::pair<const char*, Op> kArith[] = {{"+", ADD}, {"-", SUB}, {"*", MUL}, {"/", DIV}, {"%", MOD}, {"^", POW}};
        for (const auto& [name, code] : kArith)
            if (op == name) {
                const int b = rk(*e.a);
                const int c = rk(*e.b);
                abc(code, t, b, c);
                fs_->freereg = save;
                return;
            }
        // Comparisons: a test that skips the jump when it holds, then the two booleans.
        int b = rk(*e.a);
        int c = rk(*e.b);
        Op code = EQ;
        int want = 1;
        if (op == "~=") want = 0;
        else if (op == "<") code = LT;
        else if (op == "<=") code = LE;
        else if (op == ">") code = LT, std::swap(b, c);
        else if (op == ">=") code = LE, std::swap(b, c);
        abc(code, want, b, c);
        asbx(JMP, 0, 1);
        abc(LOADBOOL, t, 0, 1);
        abc(LOADBOOL, t, 1, 0);
        fs_->freereg = save;
    }

    // A call whose function goes in `base` (reserved, the top) and its arguments above; leaves
    // `nresults` values from base (-1: all of them, setting the VM's top). Frees down to base + 1.
    void callAt(const Expr& e, int base, int nresults) {
        if (e.k == Expr::Method) {
            reserve(1); // self
            const int save = fs_->freereg;
            const int o = anyReg(*e.a);
            const int k = stringConstant(e.s);
            if (k < 256) {
                abc(SELF, base, o, 0x100 | k);
            } else {
                const int kr = reserve(1);
                abx(LOADK, kr, k);
                abc(SELF, base, o, kr);
            }
            fs_->freereg = save;
        } else {
            toReg(*e.a, base);
        }
        bool open = false;
        for (size_t i = 0; i < e.args.size(); ++i) {
            const Expr& a = *e.args[i];
            if (i + 1 == e.args.size() && a.multi()) {
                multi(a, -1);
                open = true;
            } else {
                toReg(a, reserve(1));
            }
        }
        const int nargs = fs_->freereg - base - 1;
        abc(CALL, base, open ? 0 : nargs + 1, nresults < 0 ? 0 : nresults + 1);
        fs_->freereg = base + 1;
    }

    void tableAt(const Expr& e, int t) {
        const size_t newTable = abc(NEWTABLE, t, 0, 0);
        (void)newTable;
        int pending = 0, batch = 1;
        auto flush = [&](int n) {
            if (batch <= 511) {
                abc(SETLIST, t, n, batch);
            } else {
                abc(SETLIST, t, n, 0);
                emit(uint32_t(batch));
            }
            ++batch;
            pending = 0;
            fs_->freereg = t + 1;
        };
        for (size_t i = 0; i < e.fields.size(); ++i) {
            const Expr::Field& f = e.fields[i];
            if (f.key) {
                const int save = fs_->freereg;
                const int k = rk(*f.key);
                const int v = rk(*f.value);
                abc(SETTABLE, t, k, v);
                fs_->freereg = save;
                continue;
            }
            if (i + 1 == e.fields.size() && f.value->multi()) {
                multi(*f.value, -1);
                flush(0);
                continue;
            }
            toReg(*f.value, reserve(1));
            if (++pending == 50) flush(50);
        }
        if (pending) flush(pending);
        fs_->freereg = t + 1;
    }

    // `want` values of a call or `...` (or a plain expression) at the top, from freereg on; -1: all.
    void multi(const Expr& e, int want) {
        if (e.k == Expr::Call || e.k == Expr::Method) {
            const int base = reserve(1);
            callAt(e, base, want);
            fs_->freereg = base;
            reserve(std::max(want, 0));
            return;
        }
        if (e.k == Expr::Vararg) {
            const int base = fs_->freereg;
            abc(VARARG, base, want < 0 ? 0 : want + 1, 0);
            fs_->freereg = base;
            reserve(std::max(want, 0));
            return;
        }
        const int r = reserve(1);
        toReg(e, r);
        if (want == 0) fs_->freereg = r;
        else if (want > 1) {
            const int extra = reserve(want - 1);
            abc(LOADNIL, extra, extra + want - 2, 0);
        }
    }

    // Exactly `want` values of the list at the top (missing ones nil, extra ones evaluated and dropped).
    void exprsTo(const std::vector<ExprP>& es, int want) {
        if (es.empty()) {
            if (want > 0) {
                const int r = reserve(want);
                abc(LOADNIL, r, r + want - 1, 0);
            }
            return;
        }
        for (size_t i = 0; i < es.size(); ++i) {
            const int left = want - int(i);
            if (i + 1 == es.size()) {
                if (es[i]->multi()) {
                    multi(*es[i], std::max(left, 0));
                } else {
                    multi(*es[i], left > 0 ? 1 : 0);
                    if (left > 1) {
                        const int r = reserve(left - 1);
                        abc(LOADNIL, r, r + left - 2, 0);
                    }
                }
            } else {
                const int r = reserve(1);
                toReg(*es[i], r);
                if (left <= 0) fs_->freereg = r;
            }
        }
    }

    // Jumps when `e` is false (the returned jump is to be patched to the false branch).
    size_t jumpIfFalse(const Expr& e) {
        const int save = fs_->freereg;
        const int r = anyReg(e);
        abc(TEST, r, 0, 0);
        fs_->freereg = save;
        return jump();
    }

    // ---- statements
    void statements(const Block& b) {
        for (const auto& s : b) {
            statement(*s);
            fs_->freereg = nactive();
        }
    }

    void block(const Block& b) {
        enterBlock(false);
        statements(b);
        leaveBlock();
    }

    void store(const Expr& target, int objReg, int keyRk, int src) {
        if (target.k == Expr::Index) {
            abc(SETTABLE, objReg, keyRk, src);
            return;
        }
        const Var v = resolve(target.s);
        if (v.k == Var::Local) abc(MOVE, v.index, src, 0);
        else if (v.k == Var::Up) abc(SETUPVAL, src, v.index, 0);
        else abx(SETGLOBAL, src, v.index);
    }

    void statement(const Stat& s) {
        switch (s.k) {
        case Stat::Call: {
            const int base = reserve(1);
            callAt(*s.call, base, 0);
            return;
        }
        case Stat::Local: {
            exprsTo(s.exprs, int(s.names.size()));
            for (const std::string& n : s.names) addLocal(n);
            return;
        }
        case Stat::LocalFunction: {
            addLocal(s.names[0]);
            Expr f;
            f.k = Expr::Func;
            f.fn = s.fn;
            toReg(f, nactive() - 1);
            return;
        }
        case Stat::Assign: {
            struct Target {
                int obj = 0, key = 0;
            };
            std::vector<Target> tg(s.targets.size());
            for (size_t i = 0; i < s.targets.size(); ++i)
                if (s.targets[i]->k == Expr::Index) {
                    tg[i].obj = anyReg(*s.targets[i]->a);
                    tg[i].key = rk(*s.targets[i]->b);
                }
            const int base = fs_->freereg;
            exprsTo(s.exprs, int(s.targets.size()));
            for (size_t i = s.targets.size(); i-- > 0;) store(*s.targets[i], tg[i].obj, tg[i].key, base + int(i));
            return;
        }
        case Stat::Do: block(s.blocks[0]); return;
        case Stat::While: {
            const size_t start = here();
            enterBlock(true);
            const size_t exit = jumpIfFalse(*s.conds[0]);
            block(s.blocks[0]);
            patch(jump(), start);
            for (size_t b : leaveBlock()) patch(b, here());
            patch(exit, here());
            return;
        }
        case Stat::Repeat: {
            const size_t start = here();
            enterBlock(true);
            enterBlock(false);
            statements(s.blocks[0]);
            // The condition sees the body's locals: true leaves, false goes round (closing them first).
            const int save = fs_->freereg;
            const int r = anyReg(*s.conds[0]);
            fs_->freereg = save;
            abc(TEST, r, 0, 1);
            const size_t leave = jump();
            const bool captured = fs_->blocks.back().captured && nactive() > int(fs_->blocks.back().nactive);
            if (captured) abc(CLOSE, int(fs_->blocks.back().nactive), 0, 0);
            patch(jump(), start);
            patch(leave, here());
            leaveBlock(); // closes again on the way out
            for (size_t b : leaveBlock()) patch(b, here());
            return;
        }
        case Stat::If: {
            std::vector<size_t> ends;
            for (size_t i = 0; i < s.conds.size(); ++i) {
                const size_t next = jumpIfFalse(*s.conds[i]);
                block(s.blocks[i]);
                if (i + 1 < s.blocks.size()) ends.push_back(jump());
                patch(next, here());
            }
            if (s.blocks.size() > s.conds.size()) block(s.blocks.back());
            for (size_t e : ends) patch(e, here());
            return;
        }
        case Stat::NumFor: {
            enterBlock(true);
            const int base = fs_->freereg;
            toReg(*s.exprs[0], reserve(1));
            toReg(*s.exprs[1], reserve(1));
            if (s.exprs.size() > 2) toReg(*s.exprs[2], reserve(1));
            else abx(LOADK, reserve(1), constant(LuaValue::number(1)));
            addLocal("(for index)");
            addLocal("(for limit)");
            addLocal("(for step)");
            const size_t prep = asbx(FORPREP, base, 0);
            const size_t body = here();
            enterBlock(false);
            addLocal(s.names[0]);
            statements(s.blocks[0]);
            leaveBlock();
            const size_t loop = asbx(FORLOOP, base, 0);
            patch(loop, body);
            patch(prep, loop);
            for (size_t b : leaveBlock()) patch(b, here());
            return;
        }
        case Stat::GenFor: {
            enterBlock(true);
            const int base = fs_->freereg;
            exprsTo(s.exprs, 3);
            addLocal("(for generator)");
            addLocal("(for state)");
            addLocal("(for control)");
            const size_t prep = jump();
            const size_t body = here();
            enterBlock(false);
            for (const std::string& n : s.names) addLocal(n);
            statements(s.blocks[0]);
            leaveBlock();
            patch(prep, here());
            abc(TFORLOOP, base, 0, int(s.names.size()));
            patch(jump(), body);
            for (size_t b : leaveBlock()) patch(b, here());
            return;
        }
        case Stat::Return: {
            if (s.exprs.empty()) {
                abc(RETURN, 0, 1, 0);
                return;
            }
            if (s.exprs.size() == 1 && !s.exprs[0]->multi()) {
                const int r = anyReg(*s.exprs[0]);
                abc(RETURN, r, 2, 0);
                return;
            }
            const int base = fs_->freereg;
            const bool open = s.exprs.back()->multi();
            for (size_t i = 0; i < s.exprs.size(); ++i) {
                if (i + 1 == s.exprs.size() && open) multi(*s.exprs[i], -1);
                else toReg(*s.exprs[i], reserve(1));
            }
            abc(RETURN, base, open ? 0 : int(s.exprs.size()) + 1, 0);
            return;
        }
        case Stat::Break: {
            size_t i = fs_->blocks.size();
            while (i-- > 0 && !fs_->blocks[i].loop) {}
            if (i == size_t(-1)) fail("no loop to break");
            FuncState::Scope& loop = fs_->blocks[i];
            if (nactive() > int(loop.nactive)) abc(CLOSE, int(loop.nactive), 0, 0);
            loop.breaks.push_back(jump());
            return;
        }
        }
    }

    std::vector<FuncState::Up> lastUps_; // the upvalues of the function compiled last
};

} // namespace

std::shared_ptr<LuaProto> compileLua(std::string_view source, std::string* error) {
    try {
        Parser parser(source);
        const std::shared_ptr<FuncBody> main = parser.chunk();
        return Compiler().compile(*main);
    } catch (const SyntaxError& e) {
        if (error) *error = e.what();
        return nullptr;
    }
}

bool LuaVM::runSource(std::string_view source, std::string* error) {
    std::string err;
    auto proto = compileLua(source, &err);
    if (!proto) {
        if (error) *error = err;
        return false;
    }
    auto cl = std::make_shared<LuaClosure>();
    cl->proto = proto;
    LuaValue fn;
    fn.type = LuaValue::Type::Function;
    fn.f = cl;
    failed_ = false;
    call(fn, {});
    if (failed_ && error) *error = error_;
    return !failed_;
}

} // namespace alamo
