// mirrorcpp/unit/error_test.cpp — Error model tests (design §4.3).
#include <mirrorcpp/error.hpp>
#include <mirrorcpp/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using mirrorcpp::Error;
using mirrorcpp::ErrorKind;
using mirrorcpp::Result;
using mirrorcpp::make_error;
using mirrorcpp::make_step_mismatch;
using mirrorcpp::error_kind_name;
using mirrorcpp::State;

TEST_CASE("ErrorKind has the §4.3 enumerators", "[error]") {
  REQUIRE(error_kind_name(ErrorKind::io) == std::string("io"));
  REQUIRE(error_kind_name(ErrorKind::spawn) == std::string("spawn"));
  REQUIRE(error_kind_name(ErrorKind::json) == std::string("json"));
  REQUIRE(error_kind_name(ErrorKind::protocol) == std::string("protocol"));
  REQUIRE(error_kind_name(ErrorKind::registration) == std::string("registration"));
  REQUIRE(error_kind_name(ErrorKind::spec_invalid) == std::string("spec_invalid"));
  REQUIRE(error_kind_name(ErrorKind::step_mismatch) == std::string("step_mismatch"));
  REQUIRE(error_kind_name(ErrorKind::tls) == std::string("tls"));
  REQUIRE(error_kind_name(ErrorKind::registry) == std::string("registry"));
  REQUIRE(error_kind_name(ErrorKind::spec_source) == std::string("spec_source"));
  REQUIRE(error_kind_name(ErrorKind::model_interface) ==
          std::string("model_interface"));
}

TEST_CASE("Error default-constructs and takes kind+message", "[error]") {
  Error e;
  REQUIRE(e.kind == ErrorKind::io);
  REQUIRE(e.message.empty());

  Error f(ErrorKind::json, "bad json");
  REQUIRE(f.kind == ErrorKind::json);
  REQUIRE(f.message == "bad json");
}

TEST_CASE("make_error builds an Error", "[error]") {
  auto e = make_error(ErrorKind::protocol, "unexpected message");
  REQUIRE(e.kind == ErrorKind::protocol);
  REQUIRE(e.message == "unexpected message");
}

TEST_CASE("step_mismatch errors carry expected/actual", "[error]") {
  State expected;
  expected["x"] = mirrorcpp::Value(1);
  State actual;
  actual["x"] = mirrorcpp::Value(2);

  auto e = make_step_mismatch("state mismatch at variable x", expected, actual);
  REQUIRE(e.kind == ErrorKind::step_mismatch);
  REQUIRE(e.is_step_mismatch());
  REQUIRE(e.expected.has_value());
  REQUIRE(e.actual.has_value());
  REQUIRE(*e.expected == expected);
  REQUIRE(*e.actual == actual);
  REQUIRE(e.message == "state mismatch at variable x");
}

TEST_CASE("non-step-mismatch errors have no payload", "[error]") {
  auto e = make_error(ErrorKind::tls, "handshake failed");
  REQUIRE_FALSE(e.is_step_mismatch());
  REQUIRE_FALSE(e.expected.has_value());
  REQUIRE_FALSE(e.actual.has_value());
  REQUIRE(e.hints == nullptr);
}

TEST_CASE("Result<T> success carries the value", "[error]") {
  Result<int> r = 42;
  REQUIRE(r.has_value());
  REQUIRE(*r == 42);
  REQUIRE(r.value() == 42);
  REQUIRE(static_cast<bool>(r));
}

TEST_CASE("Result<T> failure carries the Error", "[error]") {
  Result<int> r = mirrorcpp::Result<int>(std::unexpected(make_error(ErrorKind::json, "boom")));
  REQUIRE_FALSE(r.has_value());
  REQUIRE_FALSE(static_cast<bool>(r));
  REQUIRE(r.error().kind == ErrorKind::json);
  REQUIRE(r.error().message == "boom");
}

TEST_CASE("Result<void> success and failure", "[error]") {
  Result<void> ok;
  REQUIRE(ok.has_value());

  Result<void> bad = std::unexpected(make_error(ErrorKind::io, "eof"));
  REQUIRE_FALSE(bad.has_value());
  REQUIRE(bad.error().kind == ErrorKind::io);
}

TEST_CASE("Error is copyable and movable", "[error]") {
  Error e(ErrorKind::step_mismatch, "m");
  State s; s["k"] = mirrorcpp::Value(9);
  e.expected = s;

  Error copy = e;
  REQUIRE(copy.kind == e.kind);
  REQUIRE(copy.expected == e.expected);

  Error moved = std::move(copy);
  REQUIRE(moved.message == "m");
  REQUIRE(moved.expected.has_value());
}
