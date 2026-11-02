const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const Module = require('node:module');

const commands = new Map();
const storedDiagnostics = new Map();
const event = () => ({ dispose() {} });
const vscode = {
  DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2 },
  CompletionItemKind: { Keyword: 14, Snippet: 27 },
  SymbolKind: { Function: 11 },
  StatusBarAlignment: { Left: 1 },
  ConfigurationTarget: { Workspace: 2 },
  Range: class Range { constructor(startLine, startCharacter, endLine, endCharacter) { Object.assign(this, { startLine, startCharacter, endLine, endCharacter }); } },
  Diagnostic: class Diagnostic { constructor(range, message, severity) { Object.assign(this, { range, message, severity }); } },
  CompletionItem: class CompletionItem { constructor(label, kind) { Object.assign(this, { label, kind }); } },
  SnippetString: class SnippetString { constructor(value) { this.value = value; } },
  DocumentSymbol: class DocumentSymbol { constructor(...args) { this.args = args; } },
  TextEdit: { replace: (...args) => ({ args }) },
  languages: {
    createDiagnosticCollection: () => ({ set: (uri, items) => storedDiagnostics.set(uri.toString(), items), delete: uri => storedDiagnostics.delete(uri.toString()), dispose() {} }),
    registerDocumentFormattingEditProvider: event,
    registerCompletionItemProvider: event,
    registerDocumentSymbolProvider: event,
    registerHoverProvider: event
  },
  window: {
    activeTextEditor: undefined,
    createOutputChannel: () => ({ clear() {}, show() {}, append() {}, appendLine() {}, dispose() {} }),
    createStatusBarItem: () => ({ show() {}, dispose() {} }),
    showErrorMessage() {}, showInformationMessage() {},
    showOpenDialog: async () => undefined,
    showQuickPick: async () => undefined
  },
  workspace: {
    workspaceFolders: [], textDocuments: [],
    getConfiguration: () => ({ get: (_key, fallback) => fallback, update: async () => {} }),
    onDidOpenTextDocument: event, onDidChangeTextDocument: event,
    onDidSaveTextDocument: event, onDidCloseTextDocument: event
  },
  commands: { registerCommand: (name, handler) => { commands.set(name, handler); return event(); }, executeCommand: async () => {} }
};

const originalLoad = Module._load;
Module._load = function (request, parent, isMain) {
  if (request === 'vscode') return vscode;
  return originalLoad.call(this, request, parent, isMain);
};

const extension = require('../extension.js');
const output = [
  "error[E0001]: Unsupported character '@'",
  '  --> /tmp/example.loom:3:4',
  '   |',
  '  3 |    @',
  '   |    ^ Unsupported character' 
].join('\n');
const parsed = extension.parseCompilerDiagnostics(output, 5);
assert.equal(parsed.length, 1);
assert.equal(parsed[0].code, 'E0001');
assert.equal(parsed[0].line, 2);
assert.equal(parsed[0].column, 3);
assert.equal(parsed[0].message, "Unsupported character '@'");

const context = { subscriptions: [], extensionPath: path.resolve(__dirname, '..') };
extension.activate(context);
for (const command of ['loom.checkFile', 'loom.build', 'loom.clean', 'loom.run', 'loom.showDependencyGraph'])
  assert.ok(commands.has(command), `missing command registration: ${command}`);

const packageJson = JSON.parse(fs.readFileSync(path.resolve(__dirname, '..', 'package.json'), 'utf8'));
assert.equal(packageJson.version, '1.0.0');
assert.ok(packageJson.contributes.languages[0].extensions.includes('.loom'));
assert.equal(packageJson.contributes.languages[0].icon.light, './icons/Loom_Logo.png');
assert.ok(packageJson.contributes.commands.some(command => command.command === 'loom.showDependencyGraph'));
console.log('VS Code extension smoke tests passed');

Module._load = originalLoad;
