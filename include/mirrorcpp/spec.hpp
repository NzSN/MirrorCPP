// mirrorcpp/spec.hpp — EXTENDS/INSTANCE closure resolution (design §5.5).
//
// Direct port of MirrorECMA's spec.ts. spec_from_files reads a root TLA+
// module, parses its EXTENDS / INSTANCE clauses (first whitespace-token of
// each comma-separated part, so "INSTANCE X WITH y <- z" yields X), skips
// the builtin modules, and recursively resolves <Name>.tla in the importing
// file's directory first and then each search dir. Missing modules and
// ambiguous (same name in >1 directory) modules are spec_source errors.
// Diamonds are deduplicated by canonical path; sources[0] is always the root.
#ifndef MIRRORCPP_SPEC_HPP
#define MIRRORCPP_SPEC_HPP

#include <mirrorcpp/error.hpp>
#include <mirrorcpp/protocol.hpp>   // ApalacheSpec

#include <filesystem>
#include <string>
#include <vector>

namespace mirrorcpp {

// Search directories consulted after the importing module's own directory.
std::vector<std::filesystem::path> default_search_dirs();

// Resolve a root module and its full EXTENDS/INSTANCE dependency closure.
// The returned ApalacheSpec has sources[0] = root module source, followed by
// each dependency exactly once (dedup by canonical path). On failure returns
// Error{ErrorKind::spec_source, message} naming the module and importer.
Result<ApalacheSpec> spec_from_files(
    const std::filesystem::path& root,
    std::vector<std::filesystem::path> search_dirs = default_search_dirs());

}  // namespace mirrorcpp

#endif  // MIRRORCPP_SPEC_HPP
