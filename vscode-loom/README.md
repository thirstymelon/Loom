# Loom Language Support for Visual Studio Code

Editor support for the implemented Loom syntax. The extension registers only the `.loom` language and associates the Loom logo with that language; it does not install or select a file icon theme.

## Features

- TextMate syntax colors, comments, indentation, folding, brackets, snippets, and document formatting.
- Compiler-backed current-file diagnostics with error code, line, column, and caret span mapping.
- Keyword/type completions, hover documentation, and Block document symbols.
- Commands for checking the current file, selecting `loomc`, formatting, building, cleaning, and running a configured workspace.
- Local call/data graph explorer consuming JSON produced by `loom graph`; it uses an inline SVG renderer and no remote service or dependency.

## Compiler discovery

`loom.compilerPath` selects the compiler. If unset, the extension checks `/usr/bin/loomc`, then workspace and parent `bin/loomc` locations, then `PATH`. `loom.toolPath` optionally selects the workspace tool; by default the extension checks `/usr/bin/loom`, project `bin/loom`, and `PATH`.

The extension lint command compiles a temporary copy of the current file. Cross-file calls and globals therefore need whole-project validation with **Loom: Build Workspace**.

## Development and tests

Open this folder in VS Code and press `F5` to start an Extension Development Host. Smoke tests require only Node.js:

```sh
npm test
```

The smoke suite verifies activation, command registration, `.loom` language/icon metadata, and source diagnostic parsing. A full Extension Development Host suite is not included yet.

## Package a new VSIX

With `@vscode/vsce` available, run:

```sh
npm test
npm run package
```

This packages version 1.0.0 as `Loom-1.0.0.vsix` in this folder. The root `Loom-0.1.3.vsix` is retained as a previous release artifact and is not overwritten.
