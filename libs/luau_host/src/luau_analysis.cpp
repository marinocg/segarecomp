// SEG-137-T005 (ADR 0295): the static check `mod check`/`pack`/`build` run on Luau sources, against the API list (luau_api.hpp). It uses the pinned parser (Ast) only: the Luau type-inference
// Analysis module is not vendored (size; see the ADR), so this is deliberately a narrow, deterministic check of the host surface, not a general type checker.
#include "segarecomp/luau_api.hpp"

#include "Luau/Allocator.h"
#include "Luau/Ast.h"
#include "Luau/Parser.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace segarecomp::luau {

namespace {

struct Checker : Luau::AstVisitor {
  const std::string &file;
  const std::set<std::string> &declared;
  std::vector<AnalysisIssue> &issues;
  std::set<const Luau::AstExpr *> handled;   // call targets already checked as calls

  Checker(const std::string &f, const std::set<std::string> &d, std::vector<AnalysisIssue> &i) : file(f), declared(d), issues(i) {}

  void report(const Luau::Location &at, const char *cls, std::string message) {
    AnalysisIssue issue;
    issue.cls = cls;
    issue.where.file = file;
    issue.where.line = at.begin.line + 1;
    issue.where.message = std::move(message);
    issues.push_back(std::move(issue));
  }

  // "sega", "sega.overlay.rect": the dotted path of a chain rooted at the global `sega`; empty when the expression is anything else (an alias, a computed key).
  static std::string chain(const Luau::AstExpr *e) {
    if (const auto *g = e->as<Luau::AstExprGlobal>()) return g->name == "sega" ? "sega" : "";
    if (const auto *i = e->as<Luau::AstExprIndexName>()) {
      const std::string base = chain(i->expr);
      return base.empty() ? "" : base + "." + i->index.value;
    }
    if (const auto *i = e->as<Luau::AstExprIndexExpr>()) {
      const auto *key = i->index->as<Luau::AstExprConstantString>();
      const std::string base = chain(i->expr);
      return base.empty() || key == nullptr ? "" : base + "." + std::string(key->value.data, key->value.size);
    }
    return "";
  }

  static bool is_namespace(const std::string &path) {
    const std::string prefix = path + ".";
    for (const ApiEntry &e : api_entries())
      if (e.name.substr(0, prefix.size()) == prefix) return true;
    return false;
  }

  // Returns the entry when `path` is a known, declared API name; reports otherwise.
  const ApiEntry *resolve(const Luau::Location &at, const std::string &path, bool as_call) {
    if (path == "sega") return nullptr;
    const ApiEntry *e = find_api(path);
    if (e == nullptr || e->kind == "callback") {
      if (is_namespace(path)) {
        if (as_call) report(at, "E_LUAU_TYPE", path + " is a table, not a function");
        return nullptr;
      }
      report(at, "E_LUAU_API", path + " is not part of the Luau API");
      return nullptr;
    }
    if (!e->capability.empty() && declared.count(std::string(e->capability)) == 0) {
      report(at, "E_LUAU_CAPABILITY", path + " needs the capability " + std::string(e->capability) + ", which the manifest does not declare");
      return nullptr;
    }
    return e;
  }

  // The literal type of an argument: "number", "string", "boolean", "nil", or "" when not a literal.
  static std::string literal_type(const Luau::AstExpr *a, double &number) {
    if (const auto *n = a->as<Luau::AstExprConstantNumber>()) {
      number = n->value;
      return "number";
    }
    if (const auto *u = a->as<Luau::AstExprUnary>()) {
      if (u->op == Luau::AstExprUnary::Op::Minus)
        if (const auto *n = u->expr->as<Luau::AstExprConstantNumber>()) {
          number = -n->value;
          return "number";
        }
    }
    if (a->is<Luau::AstExprConstantString>()) return "string";
    if (a->is<Luau::AstExprConstantBool>()) return "boolean";
    if (a->is<Luau::AstExprConstantNil>()) return "nil";
    return "";
  }

  bool visit(Luau::AstExprCall *call) override {
    const std::string path = chain(call->func);
    if (path.empty()) return true;
    handled.insert(call->func);
    const ApiEntry *e = resolve(call->location, path, true);
    if (e == nullptr) return true;
    if (e->kind != "function") {
      report(call->location, "E_LUAU_TYPE", path + " is a value, not a function");
      return true;
    }
    if (call->self) {
      report(call->location, "E_LUAU_TYPE", path + " is called with ':' but takes no self; use '.'");
      return true;
    }
    std::vector<std::pair<std::string, std::string>> params;
    std::string_view rest = e->params;
    while (!rest.empty()) {
      const std::size_t comma = rest.find(',');
      const std::string_view one = rest.substr(0, comma);
      const std::size_t colon = one.find(':');
      params.emplace_back(std::string(one.substr(0, colon)), std::string(one.substr(colon + 1)));
      rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    const std::size_t count = call->args.size;
    const bool open_ended = count > 0 && (call->args.data[count - 1]->is<Luau::AstExprCall>() || call->args.data[count - 1]->is<Luau::AstExprVarargs>());
    if (count > params.size() || (count < params.size() && !open_ended)) {
      report(call->location, "E_LUAU_TYPE", path + " takes " + std::to_string(params.size()) + " argument(s), not " + std::to_string(count));
      return true;
    }
    for (std::size_t i = 0; i < count; ++i) {
      double number = 0;
      const std::string got = literal_type(call->args.data[i], number);
      if (got.empty()) continue;
      const std::string &want = params[i].second;
      const std::string where = "argument " + std::to_string(i + 1) + " (" + params[i].first + ") of " + path;
      if (got != want) report(call->args.data[i]->location, "E_LUAU_TYPE", where + " must be a " + want + ", not a " + got);
      else if (want == "number" && (!std::isfinite(number) || number != std::floor(number))) report(call->args.data[i]->location, "E_LUAU_TYPE", where + " must be an integer");
    }
    return true;
  }

  bool visit(Luau::AstExprIndexName *n) override {
    if (handled.count(n) != 0) return false;
    const std::string path = chain(n);
    if (path.empty()) return true;
    (void)resolve(n->location, path, false);
    return false;
  }

  bool visit(Luau::AstExprIndexExpr *n) override {
    if (handled.count(n) != 0) return false;
    const std::string path = chain(n);
    if (path.empty()) return true;
    (void)resolve(n->location, path, false);
    return false;
  }

  bool visit(Luau::AstStatFunction *f) override {   // `function render(a, b)` defines a lifecycle callback: it may take at most the parameters the host passes
    if (const auto *g = f->name->as<Luau::AstExprGlobal>()) {
      const ApiEntry *e = find_api(g->name.value);
      if (e != nullptr && e->kind == "callback") {
        std::size_t allowed = e->params.empty() ? 0 : 1;
        for (const char c : e->params) allowed += c == ',' ? 1U : 0U;
        if (f->func->args.size > allowed)
          report(f->location, "E_LUAU_TYPE", std::string(e->name) + " is called with " + std::to_string(allowed) + " argument(s), the function declares " + std::to_string(f->func->args.size));
      }
    }
    return true;
  }
};

}  // namespace

std::vector<AnalysisIssue> analyze_mod(const mod::Package &package) {
  std::set<std::string> declared(package.manifest.capabilities.begin(), package.manifest.capabilities.end());
  declared.insert(package.manifest.capabilities_optional.begin(), package.manifest.capabilities_optional.end());
  std::vector<AnalysisIssue> all;
  for (const mod::Payload &p : package.manifest.payloads) {
    if (p.kind != "luau-source") continue;
    const auto it = package.files.find(p.path);
    if (it == package.files.end()) continue;
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    const Luau::ParseResult parsed = Luau::Parser::parse(reinterpret_cast<const char *>(it->second.data()), it->second.size(), names, allocator);
    if (!parsed.errors.empty() || parsed.root == nullptr) continue;   // the trusted compile step reports a syntax error
    std::vector<AnalysisIssue> issues;
    Checker checker(p.path, declared, issues);
    parsed.root->visit(&checker);
    std::stable_sort(issues.begin(), issues.end(), [](const AnalysisIssue &a, const AnalysisIssue &b) { return a.where.line < b.where.line; });
    all.insert(all.end(), issues.begin(), issues.end());
  }
  return all;
}

}  // namespace segarecomp::luau
