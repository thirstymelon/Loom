# Compiler diagnostics

`loomc` prints source-located, human-readable diagnostics by default. Tools can request one JSON object on standard error per compiler error with:

```sh
loomc --diagnostic-format=json source.loom
```

The record has `severity`, `code`, `message`, `file`, and a zero-based UTF-16 `range` with `start` and `end` positions. This follows the position convention used by LSP. Standard output remains available for normal compiler output and optimizer reports.

Current diagnostic code families are:

| Code | Category |
|---|---|
| `E0001` | Lexical errors and invalid literals |
| `E0004` | Non-exhaustive pattern matching |
| `E0061` | Call argument count or shape |
| `E0277` | Invalid operator operands |
| `E0308` | Type mismatch or invalid conversion |
| `E0382` | Ownership, move, or borrow errors |
| `E0425` | Unresolved names |
| `E0428` | Duplicate declarations |
| `E0506` | Mutation through a read-only reference |
| `E0608` | Invalid indexing |
| `E1001` | Syntax errors |
| `E2001` | Other semantic/compiler errors |

The compiler currently assigns these codes through message categories. New diagnostics should move toward explicit diagnostic identifiers at their creation sites so wording changes cannot change a tool-facing code.
