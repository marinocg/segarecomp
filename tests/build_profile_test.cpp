// SEG-125 (ADR 0273): the build profiler with a fake clock and synthetic compiler jobs: nested-span accounting without double counting, parallel batch wall vs aggregate
// job time, initial compilation, reuse, supersession by a preparation round, final/pass-only classification, unavailable resource measurements, failed/incomplete
// reports and the shareable (label-free) rendering. No compiler, no ROM.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "build_profile.hpp"

namespace {
int failures = 0;
std::uint64_t g_ns = 0;
std::uint64_t fake_now() { return g_ns; }
void advance_ms(double ms) { g_ns += static_cast<std::uint64_t>(ms * 1.0e6); }
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; ++failures; } } while (0)
bool has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

using Out = segarecomp_cli::BuildProfile::JobResult::Outcome;
constexpr Out C = Out::compiled, F = Out::failed, N = Out::not_started, R = Out::reused;

segarecomp_cli::BuildProfile::JobResult job(const char *object, const char *label, Out outcome, double wall, bool cpu = true) {
  segarecomp_cli::BuildProfile::JobResult j;
  j.object = object;
  j.label = label;
  j.outcome = outcome;
  if (outcome == Out::not_started || outcome == Out::reused) cpu = false;
  j.wall_ms = wall;
  j.source_bytes = 1000;
  j.object_bytes = 400;
  if (cpu) { j.usage.cpu_available = j.usage.rss_available = true; j.usage.cpu_ms = static_cast<std::uint64_t>(wall); j.usage.peak_rss_kib = 50000; }
  return j;
}
}  // namespace

int main() {
  using segarecomp_cli::BuildProfile;
  using segarecomp_cli::ProfileSpan;
  {   // disabled: nothing happens, nothing is written
    BuildProfile off(fake_now);
    off.begin("x", true);
    off.record_job(job("/o/m68k-aa.o", "u.c", C, 5));
    CHECK(!off.enabled());
    CHECK(off.finish("complete", ""));
  }
  g_ns = 1000;
  BuildProfile p(fake_now);
  p.enable("unused.json");
  p.set_config({"genesis", "2", "", "compatibility", "cc", "cc 13.2", 4, false});
  {   // stage "compile" contains stage "z80-materialize" which contains an operation; time not covered by children stays exclusive to the parent
    ProfileSpan compile(&p, "compile", true);
    advance_ms(10);                                 // compile exclusive
    {
      ProfileSpan materialize(&p, "z80-materialize", true);
      advance_ms(5);                                // materialize exclusive
      {
        ProfileSpan batch(&p, "compile_batch");
        advance_ms(40);                             // wall time of a 4-job parallel batch...
        p.record_batch_wall(40, 4, 4);
        // ...whose jobs took 30+35+38+40 = 143 ms of their own: more than the wall time, because they ran concurrently
        p.record_job(job("/o/m68k-aaa.o", "main.c", C, 30));
        p.record_job(job("/o/m68k-bbb.o", "u1.c", C, 35));
        p.record_job(job("/o/z80-ccc.o", "z80_0.c", C, 38));
        p.record_job(job("/o/rt-runtime.o", "runtime.c", C, 40));
      }
      advance_ms(2);
    }
    advance_ms(3);
  }
  p.next_round();   // alias preparation round 1: u1 re-emitted differently (bbb superseded by ddd), main.c reused
  {
    ProfileSpan round(&p, "alias_round");
    { ProfileSpan emit(&p, "m68k_emit"); advance_ms(7); p.count("m68k_emissions"); }
    ProfileSpan batch(&p, "compile_batch");
    advance_ms(20);
    p.record_batch_wall(20, 4, 1);
    p.record_job(job("/o/m68k-aaa.o", "main.c", R, 0));
    p.record_job(job("/o/m68k-ddd.o", "u1.c", C, 20));
  }
  p.mark_final({"/o/m68k-aaa.o", "/o/m68k-ddd.o", "/o/rt-runtime.o", "/o/z80-ccc.o"}, {});
  p.set_size("executable", 123456);
  g_ns += 1000000;   // 1 ms of unattributed time at the top level
  const std::string json = p.render("complete", "", false);
  CHECK(has(json, "\"kind\":\"build\""));
  CHECK(has(json, "\"status\":\"complete\""));
  // accounting: stage compile inclusive = 10+5+40+2+3 = 60, exclusive = 60 - 47 = 13; materialize inclusive 47 exclusive 7 (5+2); compile_batch 40 + 20 inclusive
  CHECK(has(json, "{\"name\":\"compile\",\"count\":1,\"inclusive_ms\":60.000,\"exclusive_ms\":13.000}"));
  CHECK(has(json, "{\"name\":\"z80-materialize\",\"count\":1,\"inclusive_ms\":47.000,\"exclusive_ms\":7.000}"));
  CHECK(has(json, "{\"name\":\"compile_batch\",\"count\":2,\"inclusive_ms\":60.000,\"exclusive_ms\":60.000}"));
  CHECK(has(json, "{\"name\":\"alias_round\",\"count\":1,\"inclusive_ms\":27.000,\"exclusive_ms\":0.000}"));
  // exclusive times add up: 13 + 7 + 60 + 0 + 7 (m68k_emit) = 87 = 60 (compile) + 27 (round); 1 ms of the top level stays unattributed
  CHECK(has(json, "\"exclusive_sum_ms\":87.000"));
  CHECK(has(json, "\"unattributed_ms\":1.000"));
  CHECK(has(json, "\"m68k_emissions\":1"));
  CHECK(has(json, "\"executable_bytes\":123456"));
  // compile: 5 jobs recorded, 4 compiled initially + 1 compiled in the round, 1 reuse; bbb (u1 v1) superseded by ddd
  CHECK(has(json, "\"units_total\":6"));
  CHECK(has(json, "\"attempted\":5,\"compiled\":5,\"failed\":0,\"not_started\":0,\"reused\":1"));
  CHECK(has(json, "\"final\":4,\"superseded\":1,\"pass_only\":0"));
  CHECK(has(json, "\"aggregate_job_ms\":163.000"));      // 143 + 20 job time, vs batch_wall 60 (parallel)
  CHECK(has(json, "\"batch_wall_ms\":60.000"));
  CHECK(has(json, "\"wasted_ms\":35.000"));              // only the superseded object's compile
  CHECK(has(json, "\"reuse_saved_ms\":30.000"));         // the reused main.c would have cost its original 30 ms
  CHECK(has(json, "\"phases\":{\"initial\":{\"compiled\":4,\"job_ms\":143.000},\"preparation\":{\"compiled\":1,\"job_ms\":20.000}}"));
  CHECK(has(json, "\"compiler_cpu_ms\":163.000"));
  CHECK(has(json, "\"compiler_peak_rss_kib\":50000"));
  CHECK(has(json, "\"link_cpu_ms\":null"));
  CHECK(has(json, "\"label\":\"runtime.c\""));            // private rendering names units
  const std::string shareable = p.render("complete", "", true);
  CHECK(!has(shareable, "runtime.c") && !has(shareable, "slowest_units") && has(shareable, "\"privacy\":\"shareable\""));
  CHECK(has(shareable, "\"category\":\"m68k_generated\""));  // categories, never names

  {   // unavailable resource measurements are null + listed, never zero; failures are marked and close open spans
    BuildProfile q(fake_now);
    q.enable("unused.json");
    q.set_config({"genesis", "1", "", "optimized", "cc", "", 1, true});
    q.begin("compile", true);
    q.begin("compile_batch", false);
    advance_ms(9);
    q.record_job(job("/o/z80-x.o", "z.c", C, 9, false));
    q.record_batch_wall(9, 1, 1);
    const std::string failed = [&] { while (!q.innermost_stage().empty() && false) {} return q.render("failed", q.innermost_stage(), false); }();
    CHECK(has(failed, "\"status\":\"failed\"") && has(failed, "\"failure\":{\"stage\":\"compile\"}"));
    CHECK(has(failed, "\"compiler_cpu_ms\":null") && has(failed, "\"metric\":\"compiler_cpu_ms\""));
    CHECK(has(failed, "\"unit_disposition\""));                 // the final set was never known
    CHECK(has(failed, "\"disposition\":\"unknown\""));
    CHECK(q.finish("incomplete", "compile"));                  // writes the file; spans still open are closed first
    std::remove("unused.json");
  }
  {   // a failed job, a job never started after it, a serial and a parallel success, and an unknown disposition: attempted work stays visible, successes are separate
    BuildProfile q(fake_now);
    q.enable("unused.json");
    q.set_config({"genesis", "1", "", "compatibility", "cc", "", 4, false});
    q.begin("compile", true);
    q.begin("compile_batch", false);
    advance_ms(30);
    q.record_job(job("/o/m68k-1.o", "ok1.c", C, 12));       // succeeded
    q.record_job(job("/o/m68k-2.o", "bad.c", F, 25));         // ran for 25 ms and failed
    q.record_job(job("/o/m68k-3.o", "skipped1.c", N, 0));     // never started
    q.record_job(job("/o/m68k-4.o", "skipped2.c", N, 0));
    q.record_batch_wall(30, 4, 4);
    const std::string failed_json = q.render("failed", "compile", false);
    CHECK(has(failed_json, "\"units_total\":4"));
    CHECK(has(failed_json, "\"attempted\":2,\"compiled\":1,\"failed\":1,\"not_started\":2,\"reused\":0"));
    CHECK(has(failed_json, "\"aggregate_job_ms\":37.000,\"failed_ms\":25.000,\"wasted_ms\":0.000"));   // the failed 25 ms is real work, reported as failed, not as a produced or superseded object
    CHECK(has(failed_json, "\"object_bytes\":400}"));                                                     // only the successful job produced an object
    CHECK(has(failed_json, "\"compiler_cpu_ms\":37.000"));                                                 // the failed job's CPU still counts as consumed
    CHECK(has(failed_json, "\"disposition\":\"failed\"") && has(failed_json, "\"disposition\":\"unknown\""));   // failed is not a disposition of a produced object; the success is unknown (no final link)
    CHECK(has(failed_json, "\"unit_disposition\""));
    CHECK(!has(failed_json, "skipped1.c"));                                                                  // skipped jobs are counted, never listed as slow units
  }
  {   // serial vs parallel success: wall equals the sum for one worker, is smaller for several
    BuildProfile serial(fake_now), parallel(fake_now);
    serial.enable("unused.json"); parallel.enable("unused.json");
    serial.record_job(job("/o/rt-a.o", "a.c", C, 10)); serial.record_job(job("/o/rt-b.o", "b.c", C, 10)); serial.record_batch_wall(20, 1, 2);
    parallel.record_job(job("/o/rt-a.o", "a.c", C, 10)); parallel.record_job(job("/o/rt-b.o", "b.c", C, 10)); parallel.record_batch_wall(10, 2, 2);
    CHECK(has(serial.render("complete", "", false), "\"max_workers\":1") && has(serial.render("complete", "", false), "\"batch_wall_ms\":20.000,\"aggregate_job_ms\":20.000"));
    CHECK(has(parallel.render("complete", "", false), "\"max_workers\":2") && has(parallel.render("complete", "", false), "\"batch_wall_ms\":10.000,\"aggregate_job_ms\":20.000"));
  }
  std::remove("unused.json");
  if (failures == 0) std::cout << "build_profile_test: ok\n";
  return failures == 0 ? 0 : 1;
}
