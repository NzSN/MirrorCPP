// mirrorcpp/unit/value_test.cpp — Value variant + ITF codec tests (design §8, §3.4, §5.1).
#include <mirrorcpp/value.hpp>
#include <mirrorcpp/error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using mirrorcpp::Value;
using mirrorcpp::State;
using mirrorcpp::Box;
using mirrorcpp::JsonError;
using mirrorcpp::encode_value;
using mirrorcpp::decode_value;
using mirrorcpp::encode_state;
using mirrorcpp::decode_state;
using mirrorcpp::get_param;
using mirrorcpp::get_param_int;
using mirrorcpp::to_string;
using nlohmann::json;

namespace {

// A value held behind a Box must deep-copy; mutate the returned copy and the
// original must be unchanged.
template <class T>
bool is_boxed(const Value& v) {
  return v.is<T>();
}

}  // namespace

// ---- constructor round-trips (§8: round-trip every constructor) ----

TEST_CASE("Null round-trips as JSON null", "[value][codec]") {
  Value v;                      // default ctor -> Null
  REQUIRE(v.is_null());
  Value v2 = Value(Value::Null{});
  REQUIRE(v2.is_null());
  Value v3 = nullptr;
  REQUIRE(v3.is_null());
  REQUIRE(encode_value(v).is_null());
  REQUIRE(decode_value(nullptr).is_null());
  REQUIRE(decode_value(encode_value(v)) == v);
}

TEST_CASE("Int round-trips as #bigint, never a JSON number", "[value][codec]") {
  Value zero(0);
  REQUIRE(zero.is_int());
  REQUIRE(encode_value(zero) == json{{"#bigint", "0"}});
  REQUIRE(decode_value(json{{"#bigint", "0"}}) == zero);

  Value fortytwo(42);
  REQUIRE(encode_value(fortytwo) == json{{"#bigint", "42"}});
  // must not be a bare JSON number
  REQUIRE(encode_value(fortytwo).is_object());
  REQUIRE(encode_value(fortytwo)["#bigint"] == "42");

  Value neg(-5);
  REQUIRE(encode_value(neg) == json{{"#bigint", "-5"}});
  REQUIRE(decode_value(json{{"#bigint", "-5"}}) == neg);

  // round-trip
  REQUIRE(decode_value(encode_value(zero)) == zero);
  REQUIRE(decode_value(encode_value(fortytwo)) == fortytwo);
  REQUIRE(decode_value(encode_value(neg)) == neg);
}

TEST_CASE("Bool round-trips as JSON booleans", "[value][codec]") {
  Value t(true);
  Value f(false);
  REQUIRE(t.is_bool());
  REQUIRE(encode_value(t) == json(true));
  REQUIRE(encode_value(f) == json(false));
  REQUIRE(decode_value(json(true)) == t);
  REQUIRE(decode_value(json(false)) == f);
  REQUIRE(decode_value(encode_value(t)) == t);
}

TEST_CASE("Str round-trips as JSON string", "[value][codec]") {
  Value s(std::string("hello"));
  REQUIRE(s.is_str());
  REQUIRE(encode_value(s) == json("hello"));
  REQUIRE(decode_value(json("hello")) == s);
  REQUIRE(decode_value(encode_value(s)) == s);

  Value s2("c-string");
  REQUIRE(s2.is_str());
  REQUIRE(decode_value(encode_value(s2)) == s2);
}

TEST_CASE("Seq round-trips as bare JSON array", "[value][codec]") {
  Value seq = Value(Value::Seq{{Value(1), Value("two"), Value(true)}});
  REQUIRE(seq.is_seq());
  REQUIRE(encode_value(seq) == json::array({json{{"#bigint", "1"}}, "two", true}));
  auto back = decode_value(json::array({json{{"#bigint", "1"}}, "two", true}));
  REQUIRE(back.is_seq());
  REQUIRE(back == seq);
  REQUIRE(decode_value(encode_value(seq)) == seq);
}

TEST_CASE("Tuple round-trips as #tup", "[value][codec]") {
  Value tup = Value(Value::Tuple{{Value(1), Value("x")}});
  REQUIRE(tup.is_tuple());
  REQUIRE(encode_value(tup) == json{{"#tup", json::array({json{{"#bigint", "1"}}, "x"})}});
  REQUIRE(decode_value(encode_value(tup)) == tup);
}

TEST_CASE("Set round-trips as #set", "[value][codec]") {
  Value set = Value(Value::Set{{Value(1), Value(2), Value(3)}});
  REQUIRE(set.is_set());
  REQUIRE(encode_value(set) == json{{"#set", json::array({json{{"#bigint", "1"}}, json{{"#bigint", "2"}}, json{{"#bigint", "3"}}})}});
  auto back = decode_value(json{{"#set", json::array({json{{"#bigint", "1"}}, json{{"#bigint", "2"}}, json{{"#bigint", "3"}}})}});
  REQUIRE(back.is_set());
  REQUIRE(back == set);
  REQUIRE(decode_value(encode_value(set)) == set);
}

TEST_CASE("Record round-trips as JSON object", "[value][codec]") {
  Value rec = Value(Value::Record{{{"a", Value(1)}, {"b", Value("x")}}});
  REQUIRE(rec.is_record());
  REQUIRE(encode_value(rec) == json{{"a", json{{"#bigint", "1"}}}, {"b", "x"}});
  auto back = decode_value(json{{"a", json{{"#bigint", "1"}}}, {"b", "x"}});
  REQUIRE(back.is_record());
  REQUIRE(back == rec);
  REQUIRE(decode_value(encode_value(rec)) == rec);
}

TEST_CASE("Map round-trips as #map", "[value][codec]") {
  Value map = Value(Value::Map{{{Value("k"), Value(1)}}});
  REQUIRE(map.is_map());
  REQUIRE(encode_value(map) == json{{"#map", json::array({json::array({"k", json{{"#bigint", "1"}}})})}});
  auto back = decode_value(json{{"#map", json::array({json::array({"k", json{{"#bigint", "1"}}})})}});
  REQUIRE(back.is_map());
  REQUIRE(back == map);
  REQUIRE(decode_value(encode_value(map)) == map);
}

TEST_CASE("Variant round-trips as {tag,value}", "[value][codec]") {
  Value var = Value(Value::Variant{"some", Box<Value>(Value(42))});
  REQUIRE(var.is_variant());
  REQUIRE(encode_value(var) == json{{"tag", "some"}, {"value", json{{"#bigint", "42"}}}});
  auto back = decode_value(json{{"tag", "some"}, {"value", json{{"#bigint", "42"}}}});
  REQUIRE(back.is_variant());
  REQUIRE(back == var);
  REQUIRE(decode_value(encode_value(var)) == var);
}

TEST_CASE("Unserializable round-trips as #unserializable", "[value][codec]") {
  Value u = Value(Value::Unserializable{"boom"});
  REQUIRE(u.is_unserializable());
  REQUIRE(encode_value(u) == json{{"#unserializable", "boom"}});
  auto back = decode_value(json{{"#unserializable", "boom"}});
  REQUIRE(back.is_unserializable());
  REQUIRE(back == u);
  REQUIRE(decode_value(encode_value(u)) == u);
}

// ---- #bigint edge cases (§8) ----

TEST_CASE("#bigint empty string decodes as Null", "[value][codec][bigint]") {
  REQUIRE(decode_value(json{{"#bigint", ""}}).is_null());
}

TEST_CASE("#bigint negatives and zero", "[value][codec][bigint]") {
  REQUIRE(decode_value(json{{"#bigint", "-5"}}) == Value(-5));
  REQUIRE(decode_value(json{{"#bigint", "0"}}) == Value(0));
  REQUIRE(decode_value(json{{"#bigint", "-0"}}) == Value(0));
  REQUIRE(decode_value(json{{"#bigint", "007"}}) == Value(7));  // leading zeros tolerated
}

TEST_CASE("#bigint values larger than 2^64", "[value][codec][bigint]") {
  Value::Int huge("18446744073709551616");  // 2^64
  Value v(huge);
  REQUIRE(encode_value(v) == json{{"#bigint", "18446744073709551616"}});
  REQUIRE(decode_value(json{{"#bigint", "18446744073709551616"}}) == v);

  Value::Int bigger("12345678901234567890123456789012345678901234567890");
  Value b(bigger);
  REQUIRE(decode_value(json{{"#bigint", "12345678901234567890123456789012345678901234567890"}}) == b);
  REQUIRE(decode_value(encode_value(b)) == b);

  // negative huge
  Value::Int hneg("-18446744073709551617");
  REQUIRE(decode_value(json{{"#bigint", "-18446744073709551617"}}) == Value(hneg));
}

TEST_CASE("#bigint malformed digit strings throw JsonError", "[value][codec][bigint]") {
  REQUIRE_THROWS_AS(decode_value(json{{"#bigint", "abc"}}), JsonError);
  REQUIRE_THROWS_AS(decode_value(json{{"#bigint", "12a"}}), JsonError);
  REQUIRE_THROWS_AS(decode_value(json{{"#bigint", "+5"}}), JsonError);
  REQUIRE_THROWS_AS(decode_value(json{{"#bigint", "-"}}), JsonError);
  REQUIRE_THROWS_AS(decode_value(json{{"#bigint", "1.5"}}), JsonError);
  REQUIRE_THROWS_AS(decode_value(json{{"#bigint", " 5"}}), JsonError);
  REQUIRE_THROWS_AS(decode_value(json{{"#bigint", "--5"}}), JsonError);
  REQUIRE_THROWS_AS(decode_value(json{{"#bigint", 42}}), JsonError);   // non-string value
  REQUIRE_THROWS_AS(decode_value(json{{"#bigint", true}}), JsonError);
}

TEST_CASE("bare JSON numbers decode as Int", "[value][codec]") {
  REQUIRE(decode_value(json(42)) == Value(42));
  REQUIRE(decode_value(json(0)) == Value(0));
  REQUIRE(decode_value(json(-7)) == Value(-7));
  REQUIRE(decode_value(json(18446744073709551615ULL)) ==
          Value(Value::Int("18446744073709551615")));
}

// ---- #map key forms (§8, §3.4) ----

TEST_CASE("#map decodes both string and #bigint key forms", "[value][codec][map]") {
  auto back = decode_value(json{{"#map", json::array({
        json::array({"k1", json{{"#bigint", "1"}}}),
        json::array({json{{"#bigint", "5"}}, "x"})})}});
  REQUIRE(back.is_map());
  const auto& entries = back.get<Value::Map>().entries;
  REQUIRE(entries.size() == 2);
  REQUIRE(entries[0].first == Value("k1"));      // plain string key -> Str
  REQUIRE(entries[0].second == Value(1));
  REQUIRE(entries[1].first == Value(5));         // #bigint key -> Int
  REQUIRE(entries[1].second == Value("x"));
}

TEST_CASE("#map encodes keys as encoded values", "[value][codec][map]") {
  Value map = Value(Value::Map{{{Value("k"), Value(1)}, {Value(5), Value("x")}}});
  auto enc = encode_value(map);
  REQUIRE(enc == json{{"#map", json::array({
        json::array({"k", json{{"#bigint", "1"}}}),
        json::array({json{{"#bigint", "5"}}, "x"})})}});
  // round-trip both key forms
  REQUIRE(decode_value(enc) == map);
}

// ---- variant vs record disambiguation (§8, §3.4) ----

TEST_CASE("two-key {tag,value} object decodes as Variant", "[value][codec][variant]") {
  auto v = decode_value(json{{"tag", "t1"}, {"value", json{{"#bigint", "5"}}}});
  REQUIRE(v.is_variant());
  REQUIRE(v.get<Value::Variant>().tag == "t1");
  REQUIRE(*v.get<Value::Variant>().value == Value(5));
}

TEST_CASE("objects with more keys than {tag,value} decode as Record", "[value][codec][variant]") {
  auto v = decode_value(json{{"tag", "t1"}, {"value", json{{"#bigint", "5"}}}, {"extra", 1}});
  REQUIRE(v.is_record());
  REQUIRE(v.as_record()->fields.size() == 3);
  REQUIRE(v.get<Value::Record>().fields.at("tag") == Value("t1"));
  REQUIRE(v.get<Value::Record>().fields.at("value") == Value(5));
}

TEST_CASE("single-key {tag} or {value} objects decode as Record", "[value][codec][variant]") {
  REQUIRE(decode_value(json{{"tag", "t1"}}).is_record());
  REQUIRE(decode_value(json{{"value", 1}}).is_record());
}

TEST_CASE("record field named tag/value but extra fields is a Record", "[value][codec][variant]") {
  auto v = decode_value(json{{"tag", "t1"}, {"value", 5}, {"other", true}});
  REQUIRE(v.is_record());
}

// ---- set equality (unordered) ----

TEST_CASE("set equality is unordered by length + membership", "[value][equality]") {
  Value a = Value(Value::Set{{Value(1), Value(2), Value(3)}});
  Value b = Value(Value::Set{{Value(3), Value(2), Value(1)}});
  REQUIRE(a == b);
  REQUIRE(b == a);

  Value c = Value(Value::Set{{Value(1), Value(2), Value(4)}});
  REQUIRE(a != c);   // different member
  REQUIRE(c != a);

  Value d = Value(Value::Set{{Value(1), Value(2)}});
  REQUIRE(a != d);   // different length
  REQUIRE(d != a);

  // nested sets
  Value n1 = Value(Value::Set{{Value(1), Value(Value::Set{{Value(2), Value(3)}})}});
  Value n2 = Value(Value::Set{{Value(Value::Set{{Value(3), Value(2)}}), Value(1)}});
  REQUIRE(n1 == n2);
}

TEST_CASE("sequence equality is ordered (structural)", "[value][equality]") {
  Value a = Value(Value::Seq{{Value(1), Value(2)}});
  Value b = Value(Value::Seq{{Value(2), Value(1)}});
  REQUIRE(a != b);
  REQUIRE(a == a);
  REQUIRE(a != Value(1));
}

// ---- value semantics: deep copy behind Box ----

TEST_CASE("Value deep-copies recursive payloads", "[value][semantics]") {
  Value original = Value(Value::Seq{{Value(1), Value(Value::Set{{Value(2)}})}});
  Value copy = original;
  REQUIRE(copy == original);

  // mutate the copy's nested seq
  copy.get<Value::Seq>().elems.push_back(Value(99));
  REQUIRE(copy != original);
  REQUIRE(original.get<Value::Seq>().elems.size() == 2);

  // assignment also deep-copies
  Value assigned;
  assigned = original;
  REQUIRE(assigned == original);
  assigned.get<Value::Seq>().elems.clear();
  REQUIRE(assigned != original);
  REQUIRE(original.get<Value::Seq>().elems.size() == 2);
}

TEST_CASE("Box is deep-copying", "[value][semantics]") {
  using mirrorcpp::Box;
  Box<Value::Set> a(Value::Set{{Value(1)}});
  Box<Value::Set> b = a;
  REQUIRE(*a == *b);
  a->elems.push_back(Value(2));
  REQUIRE(*a != *b);
  REQUIRE(b->elems.size() == 1);
}

// ---- accessors (§5.1) ----

TEST_CASE("as_int returns optional<Int> or nullopt", "[value][accessors]") {
  REQUIRE(Value(7).as_int().has_value());
  REQUIRE(Value(7).as_int() == Value::Int(7));
  REQUIRE_FALSE(Value("x").as_int().has_value());
  REQUIRE_FALSE(Value(true).as_int().has_value());
}

TEST_CASE("as_str returns pointer or nullptr", "[value][accessors]") {
  Value s(std::string("hi"));
  REQUIRE(s.as_str() != nullptr);
  REQUIRE(*s.as_str() == "hi");
  REQUIRE(Value(1).as_str() == nullptr);
}

TEST_CASE("as_record returns pointer or nullptr", "[value][accessors]") {
  Value r = Value(Value::Record{{{"a", Value(1)}}});
  REQUIRE(r.as_record() != nullptr);
  REQUIRE(r.as_record()->fields.at("a") == Value(1));
  REQUIRE(Value(1).as_record() == nullptr);
}

TEST_CASE("get_param / get_param_int extract params from a State", "[value][accessors]") {
  State s;
  s["x"] = Value(42);
  s["y"] = Value("s");
  REQUIRE(get_param(s, "x") != nullptr);
  REQUIRE(*get_param(s, "x") == Value(42));
  REQUIRE(get_param(s, "z") == nullptr);
  REQUIRE(get_param_int(s, "x").has_value());
  REQUIRE(*get_param_int(s, "x") == Value::Int(42));
  REQUIRE_FALSE(get_param_int(s, "y").has_value());  // not an int
  REQUIRE_FALSE(get_param_int(s, "z").has_value());  // absent
}

// ---- state codec (§5.1) ----

TEST_CASE("encode_state / decode_state round-trip", "[value][codec][state]") {
  State s;
  s["hr"] = Value(3);
  s["running"] = Value(true);
  s["tag"] = Value("hello");
  s["nested"] = Value(Value::Record{{{"k", Value(Value::Set{{Value(1), Value(2)}})}}});
  s["big"] = Value(Value::Int("123456789012345678901234567890"));

  auto enc = encode_state(s);
  REQUIRE(enc.is_object());
  REQUIRE(enc["hr"] == json{{"#bigint", "3"}});
  REQUIRE(enc["running"] == json(true));
  REQUIRE(enc["big"] == json{{"#bigint", "123456789012345678901234567890"}});

  State back = decode_state(enc);
  REQUIRE(back == s);
  REQUIRE(back["nested"] == s["nested"]);
}

TEST_CASE("decode_state rejects non-object input", "[value][codec][state]") {
  REQUIRE_THROWS_AS(decode_state(json::array({1, 2})), JsonError);
  REQUIRE_THROWS_AS(decode_state(json("nope")), JsonError);
}

// ---- decode shapes (arrays -> Seq, objects -> Record, null -> Null) ----

TEST_CASE("decode arrays always yield Seq (not Tuple/Set)", "[value][codec]") {
  auto v = decode_value(json::array({json{{"#bigint", "1"}}, "x"}));
  REQUIRE(v.is_seq());
  REQUIRE(v.get<Value::Seq>().elems.size() == 2);
}

TEST_CASE("decode_record fields recurse", "[value][codec]") {
  auto v = decode_value(json{{"a", json{{"#set", json::array({json{{"#bigint", "1"}}})}}}});
  REQUIRE(v.is_record());
  REQUIRE(v.get<Value::Record>().fields.at("a").is_set());
}

// ---- to_string ----

TEST_CASE("to_string renders canonical ITF JSON", "[value]") {
  REQUIRE(to_string(Value(42)) == "{\"#bigint\":\"42\"}");
  REQUIRE(to_string(Value(true)) == "true");
  REQUIRE(to_string(Value("hi")) == "\"hi\"");
}
