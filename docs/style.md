# Loom Style Guide

This guide describes a consistent style for Loom source. It uses syntax supported by the current compiler and formatter.

## A complete module

```loom
Module Main

Block Sum
Takes
    Values : Signed_32 Slice
Produces Signed_32
Declares
    Total : Signed_32 = 0
Implements
    Loop Value in Values
        Total = Total + Value
    End Loop
    Give Total
End Sum

Block Main
Declares
    Values : Signed_32 Array[4]
    Result : Signed_32
Implements
    Values[1] = 10
    Values[2] = 20
    Values[3] = 30
    Values[4] = 40
    Result = Sum(Slice(Address(Values[1]), Values.Length))
    Print(Result)
End Main
```

## Layout and declarations

- Put one `Module` declaration at the top of each source file.
- Separate top-level blocks with one blank line.
- Keep block sections in the order `Takes`, `Produces`, `Declares`, `Implements`; omit empty sections when the grammar allows it.
- Put each parameter, local declaration, and statement on its own line. Indent block contents by four spaces.
- Use one statement per line. Do not use semicolons to combine statements.
- Keep comments on their own line when they explain a multi-line operation.
- Run `loom fmt` before committing. The formatter is the canonical source for keyword casing, indentation, and declaration alignment.

## Names and keywords

- Keywords are case-sensitive. Use the documented spelling (`Block`, `Takes`, `Loop`, `End`).
- Use `Pascal_Case` for block, type, and module names; use descriptive `Pascal_Case` names for variables to match the existing standard library and examples.
- Keep names specific (`Input_Length`, `Result`) and avoid abbreviations unless they are established domain terms.

## Expressions and control flow

- Put spaces around binary operators: `Left + Right`, `Index <= Limit`, `Flag And Ready`.
- Put a space after commas. Keep call and indexing delimiters adjacent: `Clamp(Value, 0, Limit)`, `Values[Index]`.
- Use parentheses when they make precedence or intent clearer.
- Prefer collection iteration (`Loop Value in Values`) when an index is not needed. Use `Loop Index, Value in Values` when both are needed; use explicit indexing when the algorithm genuinely needs non-sequential access.
- Keep loop bounds and loop body effects visible. Use `Exit`/`Skip` only when they make the control flow clearer than a condition.
- Integer arithmetic is checked and traps on overflow. Use `Wrapping_Add`, `Wrapping_Sub`, or `Wrapping_Mul` only when modulo-width arithmetic is intentional, such as counters, hashes, and low-level bit operations.

## Types and memory

- State integer widths explicitly at interfaces and for stored data.
- Prefer fixed arrays for fixed-size data, `Slice` for borrowed contiguous data, and `Vector` for growable collections.
- Keep pointer operations near the boundary that needs them; use named helpers for repeated unsafe address arithmetic.
- Make ownership transfer explicit with the language's `Move`/`Borrow` operations where supported. Do not treat `String` as an owning value until its ownership rules are documented and enforced by the compiler.
- Use `Ensure` for cleanup that must happen on normal and early-exit paths.

## Performance-oriented source

- Start with clear safe code and profile release builds before rewriting loops.
- Prefer slice iteration over repeated dynamic indexing when visiting every element once.
- Use wrapping arithmetic only when its behavior is correct for every input; it gives the optimizer more freedom but removes overflow traps for that operation.
- Separate independent work from shared mutation before requesting parallel execution. Avoid creating tasks for tiny units of work.
- Keep a simple scalar implementation as the reference when adding optimized or parallel variants.

## Formatting and checking

```sh
loom fmt
loom fmt --check
loom test
```

If the installed Loom version does not support a listed formatter flag, run `loom fmt` and use the project's build/test commands to validate the result.
