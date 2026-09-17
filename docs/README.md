# Findings

What the experiment actually found, including the dead ends.

The project asks how far an ORM can be derived from plain C++ structs using
C++26 static reflection. These notes record the answers that were not obvious
beforehand — and, more usefully, the ones that were obvious and wrong.

Each note gives the symptom, the cause, and the fix, with diagnostics quoted
verbatim from real compiles and real runs.

## The notes

| | Note | What it covers |
|---|---|---|
| 01 | [Carrying strings through compile time](findings-01-compile-time-strings.md) | Why `std::define_static_string` replaces a hand-rolled `fixed_string`, and the two independent rules a compile-time string must satisfy: its *type* must be structural, and its *value* must be a permitted constant. |
| 02 | [Reflection gotchas](findings-02-reflection-gotchas.md) | Seven surprises in GCC 16's reflection. Two fail silently; the rest produce diagnostics that do not name their own cause. |
| 03 | [What the second backend found](findings-03-second-backend.md) | The engine differences that inspection missed. A scorecard of predicted versus discovered, and why an abstraction with one implementation is a guess. |
| 04 | [Importing real data](findings-04-importing-real-data.md) | Seven things about CSV that only a file somebody else produced will tell you. |

## Two halves

**01 and 02 are about C++26.** They are the ones that would be different on
another compiler, or in another year, as reflection settles. Both are grounded in
a specific toolchain — GCC 16.1.0, `__cpp_impl_reflection 202603L` — and say so.

**03 and 04 are not about C++26 at all.** They are about building anything on top
of an abstraction, and they share one thesis stated at two levels:

> A dialect written against one engine is a guess about engines.
> A parser written against invented data is a guess about files.

Both were found the same way: something that passed a green test suite met
reality and failed immediately. If you read only one note, read 03 — it carries
the scorecard, and the scorecard is the argument.

## Where the answers landed

The short version of what the experiment established, for anyone who does not
want to read four notes:

- A plain aggregate struct **is** enough. No macros, no code generation, no
  registration step, no base class — the entity types in `tests/model.hxx` and
  `demo/openflights.hxx` are ordinary structs with optional annotations.
- The same descriptor serves **three unrelated consumers**: it emits the DDL,
  matches CSV header columns to members, and builds parameterised SQL. That is the
  strongest evidence that reflection describes the *type* rather than serving SQL
  specifically.
- The compile-time / run-time seam is real and visible: five of the twelve
  framework headers contain no reflection at all.
- The limits worth knowing are in 01 and 02, and none of them turned out to be
  fatal — they cost workarounds, not features.

## Reading them in order

01 first if you want the foundation; 02 first if you are already writing
reflection code and something is not compiling; 03 or 04 first if you are here
for the engineering rather than the language.
