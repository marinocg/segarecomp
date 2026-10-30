// SEG-108 (ADR 0252): test-only command line over the launcher's Sega CD save functions, so the generated-native test drives the launcher's own code
// (never shipped). Output is one `key=value` line per fact.
//   scd_save_tool prepare <game-dir> persist|scratch   -> status=<ok|created|wrong_size|unreadable|not_writable>, backup_ram=<path>, backup_ram_out=<path|->
//   scd_save_tool finish  <game-dir> <exit-code>       -> outcome=<stored|kept_run_failed|kept_commit_failed|not_requested>   (the options of the last `prepare persist`)
#include "sega_cd.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace launcher;

static const char *status_name(scd::BackupStatus s) {
  switch (s) {
    case scd::BackupStatus::ok: return "ok";
    case scd::BackupStatus::created: return "created";
    case scd::BackupStatus::wrong_size: return "wrong_size";
    case scd::BackupStatus::unreadable: return "unreadable";
    case scd::BackupStatus::not_writable: return "not_writable";
  }
  return "?";
}

int main(int argc, char **argv) {
  if (argc < 4) return 2;
  const std::string command = argv[1];
  const fs::path dir = fs::path(argv[2]);
  scd::RunOptions options;
  if (command == "prepare") {
    const bool persistent = std::string(argv[3]) == "persist";
    const scd::BackupStatus status = scd::prepare_backup_ram(options, dir, persistent);
    std::cout << "status=" << status_name(status) << "\nbackup_ram=" << options.backup_ram.string() << "\nbackup_ram_out="
              << (options.backup_ram_out.empty() ? std::string("-") : options.backup_ram_out.string()) << "\n";
    return 0;
  }
  if (command == "finish") {
    options.backup_ram = dir / "backup-ram.bin";
    options.backup_ram_out = dir / "backup-ram.next";
    switch (scd::finish_backup_ram(options, std::atoi(argv[3]))) {
      case scd::SaveOutcome::stored: std::cout << "outcome=stored\n"; break;
      case scd::SaveOutcome::kept_run_failed: std::cout << "outcome=kept_run_failed\n"; break;
      case scd::SaveOutcome::kept_commit_failed: std::cout << "outcome=kept_commit_failed\n"; break;
      case scd::SaveOutcome::not_requested: std::cout << "outcome=not_requested\n"; break;
    }
    return 0;
  }
  return 2;
}
