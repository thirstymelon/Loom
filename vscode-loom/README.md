# Loom Language Support for Visual Studio Code

Official Visual Studio Code extension providing intelligent language support for the **Loom** systems programming language.

## Features

- **Rich Syntax Highlighting**:
  - Full TextMate grammar coverage for all Loom keywords, types, declarations, literals, operators, and comments.
  - Semantic token highlighting (`textDocument/semanticTokens/full`) for high-fidelity token distinction (modules, types, blocks, parameters, variables, fields).
- **Intelligent Autocompletion & Snippets**:
  - Context-aware completions for blocks, types, records, enums, keywords, and parameters.
  - Built-in snippets for `Module`, `Block`, `Record`, `Enum`, `If`, `Loop`, `Match`, and concurrency primitives.
- **Accurate Indentation & Formatting**:
  - Comprehensive language configuration rules (`language-configuration.json`) for automatic block indentation, bracket matching, and on-enter rules.
  - Document formatting delegates to `loom fmt` when available, with an intelligent built-in fallback indenter.
- **Editor Navigation & Intelligence**:
  - **Go to Definition**: Jump directly to declarations across files and modules.
  - **Find All References**: Find all usages of blocks, variables, and types across the workspace.
  - **Symbol Rename**: Workspace-wide symbol renaming with prepareRename validation.
  - **Hover Tooltips**: Inspect type information, block signatures, and documentation comments.
  - **Signature Help**: Parameter hints and active parameter tracking during block calls.
  - **Document & Workspace Symbols**: Outline view and workspace-wide symbol search (`Ctrl+T` / `Cmd+T`).
  - **Document Highlights**: Highlight all occurrences of the symbol under the cursor.
- **Real-Time Compiler Diagnostics**:
  - Fast, background diagnostics reporting syntax errors and type mismatches directly in the editor and Problems panel.
  - Non-polluting: only genuine compiler diagnostics from `loomc` are displayed.
- **Workspace Build & Tooling Integration**:
  - `Loom: Build Workspace` (`loom build`)
  - `Loom: Build and Run Workspace` (`loom run`)
  - `Loom: Run Tests` (`loom test`)
  - `Loom: Clean Workspace` (`loom clean`)
  - `Loom: Fetch Dependencies` (`loom fetch`)
  - `Loom: Check Current File` (`loomc --check`)
  - `Loom: Format Document` (`loom fmt`)
  - `Loom: Restart Language Server`
  - `Loom: Refresh Workspace Index`

## Installation

Install the `.vsix` package directly in Visual Studio Code:
1. Open the Extensions view (`Ctrl+Shift+X` / `Cmd+Shift+X`).
2. Click the `...` menu in the top-right corner.
3. Select **Install from VSIX...** and choose `Loom-1.4.0.vsix`.

Alternatively, via the command line:
```bash
code --install-extension Loom-1.4.0.vsix
```

## Configuration

| Setting | Default | Description |
|---|---|---|
| `loom.compilerPath` | `""` | Explicit path to `loomc`. If left empty, automatically detects the toolchain from `config.toml`, `~/.loom/toolchains/`, workspace `bin/`, or `PATH`. |
| `loom.toolPath` | `""` | Explicit path to the `loom` workspace CLI tool. |
| `loom.lint.run` | `"onType"` | When to run compiler diagnostics: `"onType"` (debounced) or `"onSave"`. |
| `loom.lint.delay` | `500` | Debounce delay in milliseconds before triggering background diagnostics on edit. |
| `loom.format.indentSize` | `4` | Number of spaces per indentation level for the fallback indenter. |

## Development & Testing

Run all language service and integration tests:
```bash
npm test
```

Package the extension into a `.vsix` bundle:
```bash
npm run package
```
