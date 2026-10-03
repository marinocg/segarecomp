#!/usr/bin/env python3
"""SEG-030 (ADR 0079 decision 1): the build graph proves that no production target links or compiles the analysis.

The text scan in analysis_core_boundary_test.py is a fast pre-check only: CMake indirection (a variable holding a target name,
a generator expression that assembles one, a wrapper target defined in a tests/*.cmake file, target_sources() of an analysis
source, a relative #include) bypasses it. This test reads the configured build tree through the CMake file API (codemodel v2)
instead, so it sees what CMake actually generates.

Analysis targets: every target declared under libs/analysis/, libs/cpu/<cpu>/analysis/ or platforms/genesis/analysis_report/,
plus every target whose name is an analysis/driver name. Test targets: every target declared under tests/. Every other
non-utility target is a production target. For each production target:
  1. its transitive dependency closure (link and build dependencies, through any intermediate target, including test-defined
     wrapper targets) contains no analysis target;
  2. no link command fragment names an analysis/driver library file;
  3. no source file and no include directory lies in an analysis directory;
  4. no quoted or angle #include of any of its C/C++ sources resolves (against the source's own directory and the target's
     include directories) into an analysis directory.

usage:
  analysis_build_graph_test.py check --source-root <src> --build-dir <build> [--cmake <cmake> --generator <gen>]
      Reads <build>'s codemodel reply; when <build> has none (a CMake older than 3.27 cannot add the query during the same
      configure), configures a private temporary tree from <src> with a pre-written query and checks that instead.
  analysis_build_graph_test.py plant --source-root <src> --cmake <cmake> --generator <gen>
      Copies the source tree, configures an unmodified control (must pass) and one copy per planted violation (each must fail).
"""
import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

# Header-only INTERFACE libraries (the generic core) are not codemodel targets: their use shows up as an include directory.
ANALYSIS_DIRS = (re.compile(r"libs/analysis(/|$)"), re.compile(r"libs/cpu/[^/]+/analysis(/|$)"),
                 re.compile(r"platforms/genesis/analysis_report(/|$)"))
ANALYSIS_NAME = re.compile(r"^(segarecomp_analysis|segarecomp_cpu_[a-z0-9]+_analysis|segarecomp_genesis_analysis_report|"
                           r"segarecomp-genesis-analysis-report)$")
ANALYSIS_FILE = re.compile(r"segarecomp_(analysis|cpu_[a-z0-9]+_analysis|genesis_analysis_report)\b|segarecomp-genesis-analysis-report")
EXPECTED_ANALYSIS = {"segarecomp_cpu_m68k_analysis", "segarecomp_genesis_analysis_report",
                     "segarecomp-genesis-analysis-report"}
EXPECTED_PRODUCTION = {"segarecomp", "segarecomp_machine_genesis"}
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc"}
QUERY_CLIENT = "client-segarecomp-analysis-build-graph"


def in_analysis_dir(rel):
    return rel is not None and any(p.match(rel) for p in ANALYSIS_DIRS)


def relative(path, root):
    try:
        return pathlib.Path(path).resolve().relative_to(root).as_posix()
    except ValueError:
        return None


def load_codemodel(build):
    reply = build / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        return None
    index = json.loads(indexes[-1].read_text())
    entry = index.get("reply", {}).get("codemodel-v2")
    if entry is None:
        entry = index.get("reply", {}).get(QUERY_CLIENT, {}).get("codemodel-v2")
    if entry is None or "jsonFile" not in entry:
        return None
    codemodel = json.loads((reply / entry["jsonFile"]).read_text())
    return reply, codemodel


def analyse(build):
    """Returns (violations, summary) for the codemodel reply of `build`, or None when there is no reply."""
    loaded = load_codemodel(build)
    if loaded is None:
        return None
    reply, codemodel = loaded
    root = pathlib.Path(codemodel["paths"]["source"]).resolve()
    violations, summary = [], {"configurations": 0, "production": 0, "analysis": 0, "tests": 0}
    for configuration in codemodel["configurations"]:
        summary["configurations"] += 1
        targets = {}
        for ref in configuration["targets"]:
            targets[ref["id"]] = json.loads((reply / ref["jsonFile"]).read_text())
        kind = {}
        for tid, target in targets.items():
            directory = target["paths"]["source"]
            directory = "" if directory == "." else directory
            if in_analysis_dir(directory) or ANALYSIS_NAME.match(target["name"]):
                kind[tid] = "analysis"
            elif directory == "tests" or directory.startswith("tests/"):
                kind[tid] = "test"
            elif target["type"] == "UTILITY":
                kind[tid] = "utility"
            else:
                kind[tid] = "production"
        names = {t["name"] for t in targets.values()}
        for expected in sorted(EXPECTED_ANALYSIS - {t["name"] for tid, t in targets.items() if kind[tid] == "analysis"}):
            violations.append("expected analysis target missing from the graph: " + expected)
        for expected in sorted(EXPECTED_PRODUCTION - {t["name"] for tid, t in targets.items() if kind[tid] == "production"}):
            violations.append("expected production target missing from the graph: " + expected)
        summary["analysis"] += sum(1 for k in kind.values() if k == "analysis")
        summary["tests"] += sum(1 for k in kind.values() if k == "test")

        def closure(tid):
            seen, stack = set(), [tid]
            while stack:
                for dep in targets[stack.pop()].get("dependencies", []):
                    if dep["id"] in targets and dep["id"] not in seen:
                        seen.add(dep["id"])
                        stack.append(dep["id"])
            return seen

        for tid, target in sorted(targets.items(), key=lambda item: item[1]["name"]):
            if kind[tid] != "production":
                continue
            summary["production"] += 1
            name = target["name"]
            for dep in sorted(closure(tid), key=lambda d: targets[d]["name"]):
                if kind[dep] == "analysis":
                    violations.append("%s depends (transitively) on analysis target %s" % (name, targets[dep]["name"]))
            for fragment in target.get("link", {}).get("commandFragments", []):
                if ANALYSIS_FILE.search(fragment.get("fragment", "")):
                    violations.append("%s link fragment names an analysis library: %s" % (name, fragment["fragment"]))
            include_dirs = []
            for group in target.get("compileGroups", []):
                for include in group.get("includes", []):
                    include_dirs.append(pathlib.Path(include["path"]))
                    if in_analysis_dir(relative(include["path"], root)):
                        violations.append("%s has an analysis include directory: %s" % (name, include["path"]))
            for source in target.get("sources", []):
                path = pathlib.Path(source["path"])
                path = path if path.is_absolute() else root / path
                rel = relative(path, root)
                if in_analysis_dir(rel):
                    violations.append("%s compiles analysis source %s" % (name, rel))
                    continue
                if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                    continue
                # Follow project includes transitively (only files inside the source root).
                pending, visited = [path.resolve()], set()
                while pending:
                    current = pending.pop()
                    if current in visited:
                        continue
                    visited.add(current)
                    for bracket, spelled in INCLUDE.findall(current.read_text(errors="replace")):
                        bases = ([current.parent] if bracket == '"' else []) + include_dirs
                        for base in bases:
                            candidate = (base / spelled).resolve()
                            if not candidate.is_file():
                                continue
                            candidate_rel = relative(candidate, root)
                            if in_analysis_dir(candidate_rel):
                                violations.append("%s source %s includes analysis header %s (via %s)" %
                                                  (name, rel, spelled, relative(current, root)))
                            elif candidate_rel is not None:
                                pending.append(candidate)
                            break
        if summary["production"] == 0 or not names:
            violations.append("no production target found")
    return violations, summary


def configure(source, build, cmake, generator):
    query = build / ".cmake" / "api" / "v1" / "query" / QUERY_CLIENT
    query.mkdir(parents=True, exist_ok=True)
    (query / "codemodel-v2").write_text("")
    result = subprocess.run([cmake, "-S", str(source), "-B", str(build), "-G", generator, "-DBUILD_TESTING=ON"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        print(result.stdout[-4000:])
    return result.returncode == 0


def report(violations, summary, label):
    print("%s: %d production, %d analysis, %d test targets over %d configuration(s)" %
          (label, summary["production"], summary["analysis"], summary["tests"], summary["configurations"]))
    for violation in violations:
        print("FAIL  " + violation)


def check(args):
    source = pathlib.Path(args.source_root).resolve()
    build = pathlib.Path(args.build_dir).resolve()
    result = analyse(build)
    if result is None:
        if not args.cmake:
            print("FAIL  no codemodel reply in %s and no --cmake to configure a private tree" % build)
            return 1
        with tempfile.TemporaryDirectory(prefix="segarecomp-build-graph-") as temp:
            private = pathlib.Path(temp) / "build"
            if not configure(source, private, args.cmake, args.generator):
                print("FAIL  private configure failed")
                return 1
            result = analyse(private)
            label = "private configure of " + str(source)
    else:
        label = "build tree " + str(build)
    if result is None:
        print("FAIL  no codemodel reply")
        return 1
    violations, summary = result
    report(violations, summary, label)
    if violations:
        print("%d violation(s)" % len(violations))
        return 1
    print("analysis_build_graph_test: no production target links, compiles or includes the analysis")
    return 0


def append(path, text):
    with open(path, "a") as handle:
        handle.write(text)


# Planted bypasses of the text scan; each must be caught by the graph check. Paths are relative to the copied source root.
PLANTS = {
    "variable_indirection": [("apps/segarecomp/CMakeLists.txt",
                              '\nset(_planted_part "segarecomp::cpu_m68k")\nset(_planted "${_planted_part}_analysis")\n'
                              'target_link_libraries(segarecomp PRIVATE ${_planted})\n')],
    "tests_cmake_wrapper": [("tests/planted_wrapper.cmake",
                             "add_library(planted_wrapper STATIC ${CMAKE_CURRENT_LIST_DIR}/analysis_core_test.cpp)\n"
                             "target_link_libraries(planted_wrapper PUBLIC segarecomp::genesis_analysis_report)\n"),
                            ("tests/CMakeLists.txt", "\ninclude(${CMAKE_CURRENT_SOURCE_DIR}/planted_wrapper.cmake)\n"),
                            ("apps/segarecomp/CMakeLists.txt", "\ntarget_link_libraries(segarecomp PRIVATE planted_wrapper)\n")],
    "target_sources": [("apps/segarecomp/CMakeLists.txt",
                        "\ntarget_sources(segarecomp PRIVATE ${PROJECT_SOURCE_DIR}/platforms/genesis/analysis_report/src/report.cpp)\n")],
    "split_genex": [("apps/segarecomp/CMakeLists.txt",
                     "\ntarget_link_libraries(segarecomp PRIVATE \"segarecomp::cpu_m68k$<$<BOOL:1>:_analysis>\")\n")],
    "header_only_core": [("apps/segarecomp/CMakeLists.txt",
                          '\nset(_planted "segarecomp::analy")\ntarget_link_libraries(segarecomp_machine_genesis PUBLIC "${_planted}sis")\n')],
    "relative_include": [("apps/segarecomp/main.cpp",
                          '\n#include "../../libs/cpu/m68k/analysis/include/segarecomp/cpu/m68k/analysis/address_value.hpp"\n')],
}


def plant(args):
    source = pathlib.Path(args.source_root).resolve()
    failures = []
    with tempfile.TemporaryDirectory(prefix="segarecomp-build-graph-plant-") as temp:
        temp = pathlib.Path(temp)
        ignore = shutil.ignore_patterns("build", "build-*", "games", ".git", ".tools", "__pycache__")

        def copy(name):
            tree = temp / name
            shutil.copytree(source, tree, ignore=ignore, symlinks=True)
            return tree

        for name in ["control"] + sorted(PLANTS):
            tree = copy(name)
            for rel, text in PLANTS.get(name, []):
                append(tree / rel, text)
            build = temp / (name + "-build")
            if not configure(tree, build, args.cmake, args.generator):
                failures.append(name + ": configure failed")
                continue
            violations, summary = analyse(build)
            caught = bool(violations)
            expected = name != "control"
            print(("ok    " if caught == expected else "FAIL  ") + "%s: %s" %
                  (name, ("caught: " + violations[0]) if caught else "no violation"))
            if caught != expected:
                failures.append(name)
            shutil.rmtree(tree, ignore_errors=True)
            shutil.rmtree(build, ignore_errors=True)
    if failures:
        print("%d failure(s): %s" % (len(failures), ", ".join(failures)))
        return 1
    print("analysis_build_graph_plant_test: the control passes and every planted bypass is caught")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["check", "plant"])
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--build-dir")
    parser.add_argument("--cmake")
    parser.add_argument("--generator", default="Ninja")
    args = parser.parse_args()
    if args.mode == "check":
        if not args.build_dir:
            parser.error("check requires --build-dir")
        return check(args)
    if not args.cmake:
        parser.error("plant requires --cmake")
    return plant(args)


if __name__ == "__main__":
    sys.exit(main())
