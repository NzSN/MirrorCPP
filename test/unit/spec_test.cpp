// mirrorcpp/unit/spec_test.cpp — EXTENDS/INSTANCE closure resolution (design §5.5/§8).
#include <mirrorcpp/error.hpp>
#include <mirrorcpp/spec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

#ifndef MIRRORCPP_TEST_FIXTURES_DIR
#error "MIRRORCPP_TEST_FIXTURES_DIR must be defined by the build (test/fixtures)"
#endif
#ifndef MIRRORCPP_SPECS_DIR
#error "MIRRORCPP_SPECS_DIR must be defined by the build (specs/)"
#endif

namespace fs = std::filesystem;
using mirrorcpp::ApalacheSpec;
using mirrorcpp::ErrorKind;

namespace {

fs::path fixtures() { return MIRRORCPP_TEST_FIXTURES_DIR; }
fs::path tla(const char* rel) { return fixtures() / "tla" / rel; }
fs::path specs() { return MIRRORCPP_SPECS_DIR; }

}  // namespace

TEST_CASE("diamond imports are deduplicated by canonical path", "[spec]") {
  auto spec = mirrorcpp::spec_from_files(tla("diamond/DiamondRoot.tla"));
  REQUIRE(spec.has_value());
  // root + DiamondLeft + DiamondRight + DiamondShared (once) = 4
  REQUIRE(spec->sources.size() == 4);
  CHECK(spec->sources[0].starts_with("---- MODULE DiamondRoot ----"));
  CHECK(spec->sources[1].starts_with("---- MODULE DiamondLeft ----"));
  CHECK(spec->sources[2].starts_with("---- MODULE DiamondRight ----"));
  CHECK(spec->sources[3].starts_with("---- MODULE DiamondShared ----"));
  // DiamondShared appears exactly once (dedup).
  int shared = 0;
  for (const auto& s : spec->sources) if (s.starts_with("---- MODULE DiamondShared ----")) ++shared;
  CHECK(shared == 1);
}

TEST_CASE("INSTANCE ... WITH resolves the first token as module name", "[spec]") {
  auto spec = mirrorcpp::spec_from_files(tla("instance/InstanceMain.tla"));
  REQUIRE(spec.has_value());
  REQUIRE(spec->sources.size() == 2);
  CHECK(spec->sources[0].starts_with("---- MODULE InstanceMain ----"));
  CHECK(spec->sources[1].starts_with("---- MODULE InstDep ----"));
}

TEST_CASE("continued EXTENDS and embedded INSTANCE forms resolve outside comments and strings", "[spec]") {
  auto spec = mirrorcpp::spec_from_files(tla("advanced/AdvancedRoot.tla"));
  REQUIRE(spec.has_value());
  REQUIRE(spec->sources.size() == 4);
  CHECK(spec->sources[0].starts_with("---- MODULE AdvancedRoot ----"));
  CHECK(spec->sources[1].starts_with("---- MODULE AdvA ----"));
  CHECK(spec->sources[2].starts_with("---- MODULE AdvB ----"));
  CHECK(spec->sources[3].starts_with("---- MODULE AdvC ----"));
}

TEST_CASE("missing module is a spec_source error naming module and importer", "[spec]") {
  auto spec = mirrorcpp::spec_from_files(tla("missing/MissingMain.tla"));
  REQUIRE_FALSE(spec.has_value());
  CHECK(spec.error().kind == ErrorKind::spec_source);
  CHECK(spec.error().message.find("NoSuchModule") != std::string::npos);
  CHECK(spec.error().message.find("MissingMain") != std::string::npos);
}

TEST_CASE("same module name in two search dirs is ambiguous", "[spec]") {
  auto base = tla("ambiguity");
  auto spec = mirrorcpp::spec_from_files(base / "main/AmbMain.tla",
                                        {base / "libA", base / "libB"});
  REQUIRE_FALSE(spec.has_value());
  CHECK(spec.error().kind == ErrorKind::spec_source);
  CHECK(spec.error().message.find("SharedName") != std::string::npos);
  CHECK(spec.error().message.find("libA") != std::string::npos);
  CHECK(spec.error().message.find("libB") != std::string::npos);
}

TEST_CASE("builtin EXTENDS modules are skipped", "[spec]") {
  auto spec = mirrorcpp::spec_from_files(tla("builtin/BuiltinOnly.tla"));
  REQUIRE(spec.has_value());
  REQUIRE(spec->sources.size() == 1);
  CHECK(spec->sources[0].starts_with("---- MODULE BuiltinOnly ----"));
}

TEST_CASE("dependency is resolved from search_dirs when not next to the importer", "[spec]") {
  auto base = tla("search");
  auto spec = mirrorcpp::spec_from_files(base / "root/Importer.tla", {base / "lib"});
  REQUIRE(spec.has_value());
  REQUIRE(spec->sources.size() == 2);
  CHECK(spec->sources[0].starts_with("---- MODULE Importer ----"));
  CHECK(spec->sources[1].starts_with("---- MODULE SearchDep ----"));
}

TEST_CASE("vendored ExtMain resolves with root first (sources[0])", "[spec]") {
  auto spec = mirrorcpp::spec_from_files(specs() / "ExtMain.tla");
  REQUIRE(spec.has_value());
  REQUIRE(spec->sources.size() == 2);
  CHECK(spec->sources[0].starts_with("---- MODULE ExtMain ----"));
  CHECK(spec->sources[1].starts_with("---- MODULE ExtDep ----"));
}

TEST_CASE("missing root file is a spec_source error", "[spec]") {
  auto spec = mirrorcpp::spec_from_files(fixtures() / "tla" / "no-such-module.tla");
  REQUIRE_FALSE(spec.has_value());
  CHECK(spec.error().kind == ErrorKind::spec_source);
}

TEST_CASE("default_search_dirs splits TLA_LIBRARY_PATH on ':'", "[spec]") {
  const char* saved = std::getenv("TLA_LIBRARY_PATH");

  // Unset -> empty list.
  ::unsetenv("TLA_LIBRARY_PATH");
  CHECK(mirrorcpp::default_search_dirs().empty());

  // Empty value -> empty list.
  ::setenv("TLA_LIBRARY_PATH", "", 1);
  CHECK(mirrorcpp::default_search_dirs().empty());

  // Two entries, trailing colon yields an empty segment that is dropped.
  ::setenv("TLA_LIBRARY_PATH", "/alpha:/beta/gamma:", 1);
  auto dirs = mirrorcpp::default_search_dirs();
  REQUIRE(dirs.size() == 2);
  CHECK(dirs[0] == fs::path("/alpha"));
  CHECK(dirs[1] == fs::path("/beta/gamma"));

  if (saved) ::setenv("TLA_LIBRARY_PATH", saved, 1);
  else ::unsetenv("TLA_LIBRARY_PATH");
}
