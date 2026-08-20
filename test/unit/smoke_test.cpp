// Trivial skeleton smoke test: the umbrella header must compile and link.
#include <mirrorcpp/mirrorcpp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_CASE("umbrella header compiles and links", "[smoke]") {
  REQUIRE(mirrorcpp_version() != nullptr);
  REQUIRE(std::string_view(mirrorcpp_version()) == MIRRORCPP_VERSION);
}
