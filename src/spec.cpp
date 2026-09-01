// src/spec.cpp — EXTENDS/INSTANCE closure resolution (design §5.5).
//
// Port of MirrorECMA's spec.ts. No exceptions escape this module: every
// failure path returns Error{ErrorKind::spec_source, message}.

#include <mirrorcpp/spec.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <cctype>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace mirrorcpp {
namespace {

// -----------------------------------------------------------------------
// Builtin modules that are always resolvable by Apalache (never vendored)
// -----------------------------------------------------------------------
bool is_builtin(std::string_view name) {
  static const std::string kBuiltins[] = {
      "Naturals", "Integers", "Reals", "Sequences",
      "FiniteSets", "TLC", "Bags", "Apalache",
  };
  for (const auto& b : kBuiltins) if (name == b) return true;
  return false;
}

// -----------------------------------------------------------------------
// EXTENDS / INSTANCE clause parsing (guide C13)
// -----------------------------------------------------------------------
// Tokenize only identifiers and commas outside strings, line comments, and
// nested block comments. This covers continued EXTENDS lists plus INSTANCE in
// top-level, LOCAL, and operator-expression forms.
std::vector<std::string> dependency_tokens(const std::string& src) {
  std::vector<std::string> out;
  std::size_t i = 0;
  std::size_t block_depth = 0;
  while (i < src.size()) {
    if (block_depth > 0) {
      if (i + 1 < src.size() && src[i] == '(' && src[i + 1] == '*') {
        ++block_depth; i += 2; continue;
      }
      if (i + 1 < src.size() && src[i] == '*' && src[i + 1] == ')') {
        --block_depth; i += 2; continue;
      }
      ++i;
      continue;
    }
    if (i + 1 < src.size() && src[i] == '\\' && src[i + 1] == '*') {
      while (i < src.size() && src[i] != '\n') ++i;
      continue;
    }
    if (i + 1 < src.size() && src[i] == '(' && src[i + 1] == '*') {
      block_depth = 1; i += 2; continue;
    }
    if (src[i] == '"') {
      ++i;
      while (i < src.size()) {
        if (src[i] == '\\' && i + 1 < src.size()) { i += 2; continue; }
        if (src[i] == '"') { ++i; break; }
        ++i;
      }
      continue;
    }
    if (src[i] == ',') { out.emplace_back(","); ++i; continue; }
    const auto c = static_cast<unsigned char>(src[i]);
    if (std::isalpha(c) || src[i] == '_') {
      const std::size_t start = i++;
      while (i < src.size()) {
        const auto d = static_cast<unsigned char>(src[i]);
        if (!std::isalnum(d) && src[i] != '_') break;
        ++i;
      }
      out.push_back(src.substr(start, i - start));
      continue;
    }
    ++i;
  }
  return out;
}

std::vector<std::string> import_names(const std::string& src) {
  std::vector<std::string> names;
  const auto tokens = dependency_tokens(src);
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i] == "EXTENDS") {
      std::size_t j = i + 1;
      if (j >= tokens.size() || tokens[j] == ",") continue;
      names.push_back(tokens[j++]);
      while (j + 1 < tokens.size() && tokens[j] == "," && tokens[j + 1] != ",") {
        names.push_back(tokens[j + 1]);
        j += 2;
      }
      i = j == 0 ? 0 : j - 1;
    } else if (tokens[i] == "INSTANCE" && i + 1 < tokens.size()) {
      names.push_back(tokens[++i]);
    }
  }
  return names;
}

// -----------------------------------------------------------------------
// File helpers
// -----------------------------------------------------------------------
bool read_file(const std::filesystem::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return static_cast<bool>(f) || f.eof();
}

std::optional<std::filesystem::path> canonical_of(const std::filesystem::path& p) {
  std::error_code ec;
  auto c = std::filesystem::canonical(p, ec);
  if (ec) return std::nullopt;
  return c;
}

}  // namespace

// -----------------------------------------------------------------------
// default_search_dirs
// -----------------------------------------------------------------------
std::vector<std::filesystem::path> default_search_dirs() {
  std::vector<std::filesystem::path> dirs;
  const char* raw = std::getenv("TLA_LIBRARY_PATH");
  if (raw == nullptr) return dirs;
  std::string_view sv(raw);
  std::size_t start = 0;
  while (start <= sv.size()) {
    const std::size_t colon = sv.find(':', start);
    const std::string_view part = (colon == std::string_view::npos)
                                      ? sv.substr(start)
                                      : sv.substr(start, colon - start);
    if (!part.empty()) dirs.emplace_back(part);
    if (colon == std::string_view::npos) break;
    start = colon + 1;
  }
  return dirs;
}

// -----------------------------------------------------------------------
// spec_from_files
// -----------------------------------------------------------------------
Result<ApalacheSpec> spec_from_files(const std::filesystem::path& root,
                                     std::vector<std::filesystem::path> search_dirs) {
  // Resolve the root first; every module lives in `sources` with its
  // canonical path in `pending` at the same index (BFS dependency order).
  std::vector<std::string> sources;
  std::vector<std::filesystem::path> pending;
  std::unordered_set<std::string> visited;

  auto push_file = [&](const std::filesystem::path& file) -> Result<void> {
    auto canonical = canonical_of(file);
    if (!canonical) {
      return std::unexpected(Error(ErrorKind::spec_source,
                                   "cannot resolve spec file '" + file.string() + "'"));
    }
    const std::string key = canonical->string();
    if (!visited.insert(key).second) return {};  // diamond dedup
    std::string src;
    if (!read_file(*canonical, src)) {
      return std::unexpected(Error(ErrorKind::spec_source,
                                   "cannot read spec file '" + canonical->string() + "'"));
    }
    sources.push_back(src);
    pending.push_back(*canonical);
    return {};
  };

  if (auto r = push_file(root); !r) return std::unexpected(r.error());

  std::size_t idx = 0;
  while (idx < pending.size()) {
    // Copy out of the vectors: push_file() below push_back()s into them, which
    // may reallocate and invalidate any references/iterators held across calls.
    const std::filesystem::path importer = pending[idx];
    const std::string src = sources[idx];
    ++idx;

    // Candidate directories: the importer's own dir first, then each search dir.
    std::vector<std::filesystem::path> cand_dirs;
    auto importer_dir = importer.parent_path();
    cand_dirs.push_back(importer_dir.empty() ? std::filesystem::path(".") : importer_dir);
    cand_dirs.insert(cand_dirs.end(), search_dirs.begin(), search_dirs.end());

    for (const std::string& name : import_names(src)) {
      if (is_builtin(name)) continue;

      // Collect all candidate files for <Name>.tla, dedup by canonical path.
      std::vector<std::filesystem::path> candidates;
      std::set<std::string> seen;
      for (const auto& d : cand_dirs) {
        const auto cand = d / (name + ".tla");
        std::error_code ec;
        if (!std::filesystem::exists(cand, ec)) continue;
        auto cc = canonical_of(cand);
        if (cc && seen.insert(cc->string()).second) candidates.push_back(*cc);
      }

      const std::string importer_name = importer.filename().string();
      if (candidates.empty()) {
        std::string dirs_str;
        for (std::size_t i = 0; i < cand_dirs.size(); ++i) {
          if (i) dirs_str += ", ";
          dirs_str += cand_dirs[i].string();
        }
        return std::unexpected(Error(ErrorKind::spec_source,
            "module '" + name + "' imported by '" + importer_name +
            "' not found (searched: " + dirs_str + ")"));
      }
      if (candidates.size() > 1) {
        std::string cands_str;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
          if (i) cands_str += ", ";
          cands_str += candidates[i].string();
        }
        return std::unexpected(Error(ErrorKind::spec_source,
            "module '" + name + "' imported by '" + importer_name +
            "' is ambiguous; found in multiple directories: " + cands_str));
      }

      if (auto r = push_file(candidates[0]); !r) return std::unexpected(r.error());
    }
  }

  ApalacheSpec spec;
  spec.sources = std::move(sources);
  return spec;
}

}  // namespace mirrorcpp
