// A small interpreter for the game's compiled Lua scripts.
//
// Universe at War ships its scripts as Lua 5.1 bytecode with two twists: lua_Number is a
// 4-byte float (header "\x1bLuaQ" format 'p', number size 4) and every function header has
// one extra int after lastlinedefined. This VM runs that bytecode well enough to evaluate
// data scripts (research trees, starting forces): all 38 opcodes, closures and upvalues,
// varargs, numeric and generic for loops, metatables (__index, __newindex, __call, arithmetic,
// __concat, __unm, __len, __eq, __lt, __le; setmetatable, getmetatable, rawget, rawset,
// rawequal). There are no coroutines, and it is
// forgiving where real Lua would raise errors - indexing or calling nil yields nil - so a
// script that calls engine functions we don't provide still runs to the end.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace alamo {

struct LuaTable;
struct LuaClosure;
struct LuaValue;
using LuaNative = std::function<std::vector<LuaValue>(const std::vector<LuaValue>& args)>;

struct LuaValue {
    enum class Type : uint8_t { Nil, Bool, Number, String, Table, Function, Native };
    Type type = Type::Nil;
    bool b = false;
    double n = 0;
    std::string s;
    std::shared_ptr<LuaTable> t;
    std::shared_ptr<LuaClosure> f;
    std::shared_ptr<LuaNative> native;

    LuaValue() = default;
    static LuaValue boolean(bool v) { LuaValue r; r.type = Type::Bool; r.b = v; return r; }
    static LuaValue number(double v) { LuaValue r; r.type = Type::Number; r.n = v; return r; }
    static LuaValue string(std::string v) { LuaValue r; r.type = Type::String; r.s = std::move(v); return r; }
    static LuaValue table(std::shared_ptr<LuaTable> v) { LuaValue r; r.type = Type::Table; r.t = std::move(v); return r; }
    static LuaValue function(LuaNative fn) { LuaValue r; r.type = Type::Native; r.native = std::make_shared<LuaNative>(std::move(fn)); return r; }
    static LuaValue newTable();

    bool isNil() const { return type == Type::Nil; }
    bool truthy() const { return !(type == Type::Nil || (type == Type::Bool && !b)); }
    bool isString() const { return type == Type::String; }
    bool isTable() const { return type == Type::Table && t; }
    // Field access for tables (nil otherwise).
    LuaValue get(const LuaValue& key) const;
    LuaValue get(std::string_view key) const { return get(string(std::string(key))); }
    LuaValue get(double index) const { return get(number(index)); }
    std::string str() const; // strings and numbers as text, "" otherwise
};

bool operator<(const LuaValue& a, const LuaValue& b);  // table key order
bool rawEquals(const LuaValue& a, const LuaValue& b);

struct LuaTable {
    std::map<LuaValue, LuaValue> fields; // nil values are erased
    std::shared_ptr<LuaTable> meta;      // metatable (setmetatable), or null
    LuaValue get(const LuaValue& key) const;
    void set(const LuaValue& key, LuaValue value);
    size_t length() const;               // border: largest n with 1..n present
};

struct LuaProto {
    std::vector<uint32_t> code;
    std::vector<LuaValue> constants;
    std::vector<std::shared_ptr<LuaProto>> protos;
    uint8_t upvalues = 0, params = 0, varargFlags = 0, maxStack = 0;
};

struct LuaClosure {
    std::shared_ptr<LuaProto> proto;
    std::vector<std::shared_ptr<LuaValue>> upvalues;
};

class LuaVM {
public:
    LuaVM();
    // Loads a compiled chunk and runs its main function. False (with a message) on a
    // malformed chunk or when the step budget runs out.
    bool run(const std::vector<uint8_t>& chunk, std::string* error = nullptr);
    std::vector<LuaValue> call(const LuaValue& fn, const std::vector<LuaValue>& args);
    // Plain Lua source (mods ship some scripts uncompiled): sets every `Name = value` assignment
    // whose value is a literal - numbers, strings, booleans, nested table constructors and
    // Find_Object_Type("X") (which yields "X") - wherever it appears, later ones winning.
    // Anything else (operators, other calls) reads as nil. Enough for data scripts.
    void loadDataAssignments(std::string_view source);

    LuaValue global(std::string_view name) const { return globals_->get(LuaValue::string(std::string(name))); }
    void setGlobal(std::string_view name, LuaValue v) { globals_->set(LuaValue::string(std::string(name)), std::move(v)); }
    void setFunction(std::string_view name, LuaNative fn) { setGlobal(name, LuaValue::function(std::move(fn))); }
    LuaTable& globals() { return *globals_; }

    uint64_t stepBudget = 20'000'000;    // instructions per run/call before giving up
    bool failed() const { return failed_; }
    const std::string& lastError() const { return error_; }

private:
    std::vector<LuaValue> execute(const std::shared_ptr<LuaClosure>& cl, std::vector<LuaValue> args, int depth);
    std::shared_ptr<LuaTable> globals_;
    uint64_t steps_ = 0;
    bool failed_ = false;
    std::string error_;
};

// Parses a chunk into its main function prototype (null on error).
std::shared_ptr<LuaProto> loadLuaChunk(const std::vector<uint8_t>& chunk, std::string* error = nullptr);

} // namespace alamo
