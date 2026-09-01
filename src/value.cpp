// mirrorcpp/value.cpp — Value variant + ITF codec implementation (design §3.4, §5.1).
#include <mirrorcpp/value.hpp>

#include <cmath>
#include <string>

namespace mirrorcpp {

Value::Kind Value::kind() const noexcept {
  // Storage alternative order is 1:1 with Kind (see value.hpp); index() is the
  // discriminant.
  return static_cast<Kind>(storage_.index());
}

std::optional<Value::Int> Value::as_int() const noexcept {
  if (auto* p = std::get_if<Int>(&storage_)) return *p;
  return std::nullopt;
}

const std::string* Value::as_str() const noexcept {
  return std::get_if<std::string>(&storage_);
}

const Value::Record* Value::as_record() const noexcept {
  const Box<Record>* p = std::get_if<Box<Record>>(&storage_);
  return p ? &**p : nullptr;
}

bool Value::operator==(const Value& o) const {
  if (storage_.index() != o.storage_.index()) return false;
  return std::visit(
      [&](const auto& self) -> bool {
        using Self = std::decay_t<decltype(self)>;
        const auto& other = std::get<Self>(o.storage_);
        if constexpr (std::is_same_v<Self, Box<Set>>) {
          // Set equality: same length + mutual membership (unordered, §3.4).
          const auto& xs = self->elems;
          const auto& ys = other->elems;
          if (xs.size() != ys.size()) return false;
          for (const auto& x : xs) {
            bool found = false;
            for (const auto& y : ys) {
              if (x == y) {
                found = true;
                break;
              }
            }
            if (!found) return false;
          }
          return true;
        } else {
          return self == other;  // structural (recurses into Value for nested values)
        }
      },
      storage_);
}

const Value* get_param(const State& state, std::string_view name) noexcept {
  auto it = state.find(std::string(name));
  return it == state.end() ? nullptr : &it->second;
}

std::optional<Value::Int> get_param_int(const State& state, std::string_view name) noexcept {
  const Value* v = get_param(state, name);
  if (!v) return std::nullopt;
  return v->as_int();
}

nlohmann::json encode_value(const Value& v) {
  switch (v.kind()) {
    case Value::Kind::null:
      return nullptr;
    case Value::Kind::integer: {
      const auto i = v.as_int();
      return nlohmann::json{{"#bigint", i->str()}};  // ALWAYS #bigint, never a JSON number
    }
    case Value::Kind::boolean:
      return v.get<bool>();
    case Value::Kind::string:
      return *v.as_str();
    case Value::Kind::set: {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& e : v.get<Value::Set>().elems) arr.push_back(encode_value(e));
      return nlohmann::json{{"#set", std::move(arr)}};
    }
    case Value::Kind::seq: {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& e : v.get<Value::Seq>().elems) arr.push_back(encode_value(e));
      return arr;
    }
    case Value::Kind::tuple: {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& e : v.get<Value::Tuple>().elems) arr.push_back(encode_value(e));
      return nlohmann::json{{"#tup", std::move(arr)}};
    }
    case Value::Kind::record: {
      nlohmann::json obj = nlohmann::json::object();
      for (const auto& [k, val] : v.get<Value::Record>().fields) obj[k] = encode_value(val);
      return obj;
    }
    case Value::Kind::map: {
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& [k, val] : v.get<Value::Map>().entries)
        arr.push_back(nlohmann::json::array({encode_value(k), encode_value(val)}));
      return nlohmann::json{{"#map", std::move(arr)}};
    }
    case Value::Kind::variant: {
      const auto& var = v.get<Value::Variant>();
      return nlohmann::json{{"tag", var.tag}, {"value", encode_value(*var.value)}};
    }
    case Value::Kind::unserializable:
      return nlohmann::json{{"#unserializable", v.get<Value::Unserializable>().text}};
  }
  return nullptr;  // unreachable
}

namespace {

Value decode_bigint(const nlohmann::json& j) {
  if (!j.is_string()) throw JsonError("malformed #bigint: value must be a string");
  std::string s = j.get<std::string>();
  if (s.empty()) return Value();  // {"#bigint": ""} -> Null (§3.4)
  // optional leading '-', then at least one digit, all digits (§5.1)
  size_t pos = 0;
  if (s[0] == '-') pos = 1;
  if (pos >= s.size())
    throw JsonError(std::string("malformed #bigint: '") + s + "'");
  for (; pos < s.size(); ++pos) {
    if (s[pos] < '0' || s[pos] > '9')
      throw JsonError(std::string("malformed #bigint: '") + s + "'");
  }
  return Value(Value::Int(s));
}

Value decode_variant(const nlohmann::json& j) {
  if (!j["tag"].is_string()) throw JsonError("malformed variant: tag must be a string");
  return Value(Value::Variant{j["tag"].get<std::string>(),
                              Box<Value>(decode_value(j["value"]))});
}

}  // namespace

Value decode_value(const nlohmann::json& j) {
  if (j.is_null()) return Value();
  if (j.is_boolean()) return Value(j.get<bool>());
  if (j.is_string()) return Value(j.get<std::string>());
  if (j.is_number_unsigned()) return Value(Value::Int(j.get<std::uint64_t>()));
  if (j.is_number_integer()) return Value(Value::Int(j.get<std::int64_t>()));
  if (j.is_number_float()) {
    // Haskell parity (pinned by the golden corpus, decode_only.jsonl): bare
    // *integral* JSON numbers are accepted on decode (2.0, 3e2, 1.23e5) even
    // though a conforming client always emits #bigint (guide C11). Only
    // non-integral numbers are rejected. cpp_int construction from a double
    // truncates; d is integral here.
    const double d = j.get<double>();
    if (!std::isfinite(d) || std::trunc(d) != d)
      throw JsonError("bare JSON number not in ITF grammar");
    return Value(Value::Int(d));
  }

  if (j.is_array()) {
    Value::Seq seq;
    for (const auto& e : j) seq.elems.push_back(decode_value(e));
    return Value(std::move(seq));
  }

  // Objects (in priority order): tagged forms, variant, then record.
  if (auto it = j.find("#bigint"); it != j.end()) return decode_bigint(*it);
  if (auto it = j.find("#tup"); it != j.end()) {
    if (!it->is_array()) throw JsonError("malformed #tup: value must be an array");
    Value::Tuple t;
    for (const auto& e : *it) t.elems.push_back(decode_value(e));
    return Value(std::move(t));
  }
  if (auto it = j.find("#set"); it != j.end()) {
    if (!it->is_array()) throw JsonError("malformed #set: value must be an array");
    Value::Set s;
    for (const auto& e : *it) s.elems.push_back(decode_value(e));
    return Value(std::move(s));
  }
  if (auto it = j.find("#map"); it != j.end()) {
    if (!it->is_array()) throw JsonError("malformed #map: value must be an array");
    Value::Map m;
    for (const auto& entry : *it) {
      if (!entry.is_array() || entry.size() != 2)
        throw JsonError("malformed #map: entries must be [key, value] pairs");
      // Keys decode as full Values: plain string -> Str, {"#bigint": …} -> Int (§3.4).
      m.entries.emplace_back(decode_value(entry[0]), decode_value(entry[1]));
    }
    return Value(std::move(m));
  }
  if (auto it = j.find("#unserializable"); it != j.end()) {
    if (!it->is_string()) throw JsonError("malformed #unserializable: value must be a string");
    return Value(Value::Unserializable{it->get<std::string>()});
  }
  if (j.size() == 2 && j.contains("tag") && j.contains("value")) return decode_variant(j);

  Value::Record rec;  // all other objects -> Record
  for (auto it = j.begin(); it != j.end(); ++it)
    rec.fields.emplace(it.key(), decode_value(it.value()));
  return Value(std::move(rec));
}

nlohmann::json encode_state(const State& s) {
  nlohmann::json obj = nlohmann::json::object();
  for (const auto& [k, v] : s) obj[k] = encode_value(v);
  return obj;
}

State decode_state(const nlohmann::json& j) {
  if (!j.is_object()) throw JsonError("state must be a JSON object");
  State s;
  for (auto it = j.begin(); it != j.end(); ++it)
    s.emplace(it.key(), decode_value(it.value()));
  return s;
}

std::string to_string(const Value& v) { return encode_value(v).dump(); }

}  // namespace mirrorcpp
