// mirrorcpp/value.hpp — Value variant + ITF codec (design §5.1).
//
// Value models a TLA+/ITF value: arbitrary-precision integers
// (boost::multiprecision::cpp_int), booleans, strings, and the recursive
// TLA+ structures (sets, sequences, tuples, records, maps/functions,
// variants) plus Null and Unserializable. Recursive alternatives are held
// behind Box<T> (a deep-copying unique_ptr wrapper) so the std::variant is
// well-formed with recursive payloads; Value stays a normal copyable value
// type with value semantics.
//
// Equality follows the mirror's Eq Value: structural for everything except
// Set, which is compared by length + mutual membership (unordered).
//
// The ITF codec (encode_value/decode_value/encode_state/decode_state) uses
// nlohmann::json as the DOM. decode_* throw JsonError on malformed input
// (design §5.1: "throws JsonError internally"); protocol.hpp catches and
// converts to Error{kind=json}.
#ifndef MIRRORCPP_VALUE_HPP
#define MIRRORCPP_VALUE_HPP

#include <boost/multiprecision/cpp_int.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mirrorcpp {

// Thrown by decode_value / decode_state on malformed ITF/JSON input. Internal
// exception type: callers that cross the API boundary (protocol.hpp) catch it
// and convert to Error{ErrorKind::json}.
class JsonError : public std::runtime_error {
 public:
  explicit JsonError(const std::string& what) : std::runtime_error(what) {}
};

// Box<T>: deep-copying unique_ptr wrapper (design §5.1). Copy ctor/assign
// clone the pointee, so Box (and therefore Value) has value semantics despite
// holding recursive structures behind pointers.
template <class T>
class Box {
 public:
  Box() : p_(std::make_unique<T>()) {}
  Box(T v) : p_(std::make_unique<T>(std::move(v))) {}
  Box(const Box& o) : p_(o.p_ ? std::make_unique<T>(*o.p_) : std::make_unique<T>()) {}
  Box(Box&&) noexcept = default;
  Box& operator=(const Box& o) {
    if (this != &o) p_ = o.p_ ? std::make_unique<T>(*o.p_) : std::make_unique<T>();
    return *this;
  }
  Box& operator=(Box&&) noexcept = default;

  T& operator*() noexcept { return *p_; }
  const T& operator*() const noexcept { return *p_; }
  T* operator->() noexcept { return p_.get(); }
  const T* operator->() const noexcept { return p_.get(); }

  friend bool operator==(const Box& a, const Box& b) { return *a.p_ == *b.p_; }

 private:
  std::unique_ptr<T> p_;
};

class Value {
 public:
  using Int = boost::multiprecision::cpp_int;

  struct Null {
    bool operator==(const Null&) const = default;
  };
  struct Set {   // unordered equality (length + mutual membership)
    std::vector<Value> elems;
    bool operator==(const Set& o) const = default;
  };
  struct Seq {
    std::vector<Value> elems;
    bool operator==(const Seq& o) const = default;
  };
  struct Tuple {
    std::vector<Value> elems;
    bool operator==(const Tuple& o) const = default;
  };
  struct Record {
    std::map<std::string, Value> fields;
    bool operator==(const Record& o) const = default;
  };
  struct Map {
    std::vector<std::pair<Value, Value>> entries;
    bool operator==(const Map& o) const = default;
  };
  struct Variant {
    std::string tag;
    Box<Value> value;
    bool operator==(const Variant& o) const = default;
  };
  struct Unserializable {
    std::string text;
    bool operator==(const Unserializable& o) const = default;
  };

  // Discriminant; maps 1:1 onto Storage alternatives (index order).
  enum class Kind {
    null, integer, boolean, string,
    set, seq, tuple, record, map, variant, unserializable,
  };

  using Storage = std::variant<Null, Int, bool, std::string,
                               Box<Set>, Box<Seq>, Box<Tuple>,
                               Box<Record>, Box<Map>, Box<Variant>, Unserializable>;

  // ---- constructors ----
  Value() : storage_(Null{}) {}
  Value(Null) : storage_(Null{}) {}
  Value(Int i) : storage_(std::move(i)) {}
  Value(bool b) : storage_(b) {}
  Value(std::string s) : storage_(std::move(s)) {}
  Value(const char* s) : storage_(std::string(s)) {}
  Value(Set s) : storage_(Box<Set>(std::move(s))) {}
  Value(Seq s) : storage_(Box<Seq>(std::move(s))) {}
  Value(Tuple t) : storage_(Box<Tuple>(std::move(t))) {}
  Value(Record r) : storage_(Box<Record>(std::move(r))) {}
  Value(Map m) : storage_(Box<Map>(std::move(m))) {}
  Value(Variant v) : storage_(Box<Variant>(std::move(v))) {}
  Value(Unserializable u) : storage_(std::move(u)) {}
  Value(std::nullptr_t) : storage_(Null{}) {}

  // Integral literals -> Int (excludes bool so Value(true) stays a Bool).
  template <std::integral T>
    requires(!std::is_same_v<T, bool>)
  Value(T i) : storage_(Int(i)) {}

  // ---- discriminators ----
  Kind kind() const noexcept;
  bool is_null() const noexcept { return std::holds_alternative<Null>(storage_); }
  bool is_int() const noexcept { return std::holds_alternative<Int>(storage_); }
  bool is_bool() const noexcept { return std::holds_alternative<bool>(storage_); }
  bool is_str() const noexcept { return std::holds_alternative<std::string>(storage_); }
  bool is_set() const noexcept { return std::holds_alternative<Box<Set>>(storage_); }
  bool is_seq() const noexcept { return std::holds_alternative<Box<Seq>>(storage_); }
  bool is_tuple() const noexcept { return std::holds_alternative<Box<Tuple>>(storage_); }
  bool is_record() const noexcept { return std::holds_alternative<Box<Record>>(storage_); }
  bool is_map() const noexcept { return std::holds_alternative<Box<Map>>(storage_); }
  bool is_variant() const noexcept { return std::holds_alternative<Box<Variant>>(storage_); }
  bool is_unserializable() const noexcept { return std::holds_alternative<Unserializable>(storage_); }

  // ---- typed accessors (is<T> / get<T>, design §5.1) ----
  template <class T>
  bool is() const noexcept {
    if constexpr (std::is_same_v<T, Set>) return is_set();
    else if constexpr (std::is_same_v<T, Seq>) return is_seq();
    else if constexpr (std::is_same_v<T, Tuple>) return is_tuple();
    else if constexpr (std::is_same_v<T, Record>) return is_record();
    else if constexpr (std::is_same_v<T, Map>) return is_map();
    else if constexpr (std::is_same_v<T, Variant>) return is_variant();
    else if constexpr (std::is_same_v<T, Unserializable>) return is_unserializable();
    else return std::holds_alternative<T>(storage_);
  }

  template <class T>
  T& get() {
    if constexpr (std::is_same_v<T, Set>) return *std::get<Box<Set>>(storage_);
    else if constexpr (std::is_same_v<T, Seq>) return *std::get<Box<Seq>>(storage_);
    else if constexpr (std::is_same_v<T, Tuple>) return *std::get<Box<Tuple>>(storage_);
    else if constexpr (std::is_same_v<T, Record>) return *std::get<Box<Record>>(storage_);
    else if constexpr (std::is_same_v<T, Map>) return *std::get<Box<Map>>(storage_);
    else if constexpr (std::is_same_v<T, Variant>) return *std::get<Box<Variant>>(storage_);
    else if constexpr (std::is_same_v<T, Unserializable>) return std::get<Unserializable>(storage_);
    else return std::get<T>(storage_);
  }

  template <class T>
  const T& get() const {
    if constexpr (std::is_same_v<T, Set>) return *std::get<Box<Set>>(storage_);
    else if constexpr (std::is_same_v<T, Seq>) return *std::get<Box<Seq>>(storage_);
    else if constexpr (std::is_same_v<T, Tuple>) return *std::get<Box<Tuple>>(storage_);
    else if constexpr (std::is_same_v<T, Record>) return *std::get<Box<Record>>(storage_);
    else if constexpr (std::is_same_v<T, Map>) return *std::get<Box<Map>>(storage_);
    else if constexpr (std::is_same_v<T, Variant>) return *std::get<Box<Variant>>(storage_);
    else if constexpr (std::is_same_v<T, Unserializable>) return std::get<Unserializable>(storage_);
    else return std::get<T>(storage_);
  }

  // ---- convenience accessors (design §5.1) ----
  std::optional<Int> as_int() const noexcept;
  const std::string* as_str() const noexcept;
  const Record* as_record() const noexcept;

  // ---- equality (set = unordered; everything else structural) ----
  bool operator==(const Value& o) const;
  bool operator!=(const Value& o) const { return !(*this == o); }

 private:
  Storage storage_;
};

// State = a map of variable name -> value (design §5.1).
using State = std::map<std::string, Value>;

// Convenience accessors for paramVars extraction from a State (§5.1).
const Value* get_param(const State& state, std::string_view name) noexcept;
std::optional<Value::Int> get_param_int(const State& state, std::string_view name) noexcept;

// ---- ITF codec (design §3.4 / §5.1) ----
// encode_* never throw. decode_* throw JsonError on malformed input.
nlohmann::json encode_value(const Value& v);
Value decode_value(const nlohmann::json& j);
nlohmann::json encode_state(const State& s);
State decode_state(const nlohmann::json& j);

// Debug/error-message rendering: canonical ITF JSON text of a value.
std::string to_string(const Value& v);

}  // namespace mirrorcpp

#endif  // MIRRORCPP_VALUE_HPP
