# ADR 0001: Vertical Slices and Explicit IR

- Status: Accepted
- Date: 2026-08-04

## Context

A global recompiler combines binary parsing, multiple ISAs, uncertain control flow, generated C,
and hardware runtime behavior. Building each subsystem to completion before integration delays
evidence and encourages incompatible assumptions.

## Decision

Develop thin end-to-end slices. Raw instructions are decoded into typed instructions, then lifted
to an explicit target-neutral IR before C emission. Source provenance and unresolved behavior are
first-class data. Generated C targets C11; implementation code targets C++20.

## Consequences

Each milestone must produce a visible command or executable artifact. Some interfaces will emerge
later than in a subsystem-first design. The extra IR boundary enables independent validation,
multiple CPU frontends, and emitters without coupling C syntax to opcode decoding.
