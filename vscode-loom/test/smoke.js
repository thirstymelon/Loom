const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const Module = require('node:module');

const commands = new Map();
const storedDiagnostics = new Map();
const providers = new Map();
const treeProviders = new Map();
const lspCalls = [];
const lspNotifications = [];
const clientEvents = new Map();
let openedFinding;
class FakeLanguageClient {
  constructor() { this.state = 'running'; }
  on(name, callback) { const handlers = clientEvents.get(name) || []; handlers.push(callback); clientEvents.set(name, handlers); return this; }
  start() { return Promise.resolve({}); }
  stop() {}
  notify(method, params) { lspNotifications.push({ method, params }); }
  request(method, params) {
    lspCalls.push({ method, params });
    const range = { start: { line: 1, character: 2 }, end: { line: 1, character: 6 } };
    const location = { uri: 'file:///tmp/provider.loom', range };
    if (method === 'textDocument/completion') return Promise.resolve({ items: [{ label: 'FromLsp', kind: 6 }] });
    if (method === 'textDocument/documentSymbol') return Promise.resolve([{ name: 'FromLsp', kind: 12, range, selectionRange: range }]);
    if (method === 'textDocument/hover') return Promise.resolve({ contents: { kind: 'markdown', value: 'LSP hover' }, range });
    if (method === 'textDocument/definition') return Promise.resolve(location);
    if (method === 'textDocument/references') return Promise.resolve([location, location]);
    if (method === 'textDocument/prepareRename') return Promise.resolve({ range, placeholder: 'Before' });
    if (method === 'textDocument/rename') return Promise.resolve({ changes: { 'file:///tmp/provider.loom': [{ range, newText: 'After' }] } });
    if (method === 'workspace/symbol') return Promise.resolve([{ name: 'FromLsp', kind: 12, location }]);
    if (method === 'textDocument/semanticTokens/full') return Promise.resolve({ data: [1, 2, 4, 3, 1] });
    if (method === 'textDocument/signatureHelp') return Promise.resolve({ signatures: [{ label: 'FromLsp(A : Signed_32)', parameters: [{ label: 'A : Signed_32' }] }], activeSignature: 0, activeParameter: 0 });
    if (method === 'textDocument/codeAction') return Promise.resolve([{ title: 'LSP action', kind: 'quickfix' }]);
    if (method === 'textDocument/documentHighlight') return Promise.resolve([{ range, kind: 1 }]);
    return Promise.resolve(null);
  }
}
const event = () => ({ dispose() {} });
const vscode = {
  DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2 },
  CompletionItemKind: { Keyword: 14, Snippet: 27, Variable: 6 },
  SymbolKind: { Function: 11, Struct: 23, Enum: 10, Field: 8, Variable: 12 },
  CodeActionKind: { QuickFix: 'quickfix' },
  StatusBarAlignment: { Left: 1 },
  ConfigurationTarget: { Workspace: 2 },
  Range: class Range { constructor(a, b, c, d) { if (typeof a === 'object') Object.assign(this, { start: a, end: b }); else Object.assign(this, { start: { line: a, character: b }, end: { line: c, character: d } }); } isEqual(other) { return JSON.stringify(this) === JSON.stringify(other); } },
  TreeItemCollapsibleState: { None: 0, Collapsed: 1, Expanded: 2 },
  TreeItem: class TreeItem { constructor(label, collapsibleState) { Object.assign(this, { label, collapsibleState }); } },
  Location: class Location { constructor(uri, range) { Object.assign(this, { uri, range }); } },
  Hover: class Hover { constructor(contents, range) { Object.assign(this, { contents, range }); } },
  SignatureInformation: class SignatureInformation { constructor(label) { this.label = label; this.parameters = []; } },
  ParameterInformation: class ParameterInformation { constructor(label) { this.label = label; } },
  SignatureHelp: class SignatureHelp {},
  SymbolInformation: class SymbolInformation { constructor(name, kind, containerName, location) { Object.assign(this, { name, kind, containerName, location }); } },
  CodeAction: class CodeAction { constructor(title, kind) { Object.assign(this, { title, kind }); } },
  WorkspaceEdit: class WorkspaceEdit { constructor() { this.edits = []; } replace(uri, range, newText) { this.edits.push({ uri, range, newText }); } },
  SemanticTokensLegend: class SemanticTokensLegend { constructor(tokenTypes, tokenModifiers) { Object.assign(this, { tokenTypes, tokenModifiers }); } },
  SemanticTokensBuilder: class SemanticTokensBuilder { constructor(legend) { this.tokens = []; } push(...token) { this.tokens.push(token); } build() { return this.tokens; } },
  Diagnostic: class Diagnostic { constructor(range, message, severity) { Object.assign(this, { range, message, severity }); } },
  CompletionItem: class CompletionItem { constructor(label, kind) { Object.assign(this, { label, kind }); } },
  SnippetString: class SnippetString { constructor(value) { this.value = value; } },
  DocumentSymbol: class DocumentSymbol { constructor(...args) { this.args = args; } },
  DocumentHighlight: class DocumentHighlight { constructor(range, kind) { Object.assign(this, { range, kind }); } },
  Uri: { parse: value => ({ toString: () => value, fsPath: value.replace('file://', '') }) },
  TextEdit: { replace: (...args) => ({ args }) },
  languages: {
    createDiagnosticCollection: () => ({ set: (uri, items) => storedDiagnostics.set(uri.toString(), items), get: uri => storedDiagnostics.get(uri.toString()), delete: uri => storedDiagnostics.delete(uri.toString()), clear: () => storedDiagnostics.clear(), dispose() {} }),
    registerDocumentFormattingEditProvider: (lang, p) => { providers.set('format', p); return event(); },
    registerCompletionItemProvider: (lang, p, ...triggers) => { providers.set(triggers.includes(' ') ? 'snippetCompletion' : 'completion', p); return event(); },
    registerDocumentSymbolProvider: (lang, p) => { providers.set('documentSymbols', p); return event(); },
    registerHoverProvider: (lang, p) => { providers.set('hover', p); return event(); },
    registerDefinitionProvider: (lang, p) => { providers.set('definition', p); return event(); },
    registerReferenceProvider: (lang, p) => { providers.set('references', p); return event(); },
    registerRenameProvider: (lang, p) => { providers.set('rename', p); return event(); },
    registerWorkspaceSymbolProvider: (lang, p) => { providers.set('workspaceSymbols', p); return event(); },
    registerSignatureHelpProvider: (lang, p) => { providers.set('signature', p); return event(); },
    registerCodeActionsProvider: (lang, p) => { providers.set('codeActions', p); return event(); },
    registerDocumentSemanticTokensProvider: (lang, p) => { providers.set('semanticTokens', p); return event(); },
    registerDocumentHighlightProvider: (lang, p) => { providers.set('highlights', p); return event(); }
  },
  window: {
    activeTextEditor: undefined,
    createOutputChannel: () => ({ clear() {}, show() {}, append() {}, appendLine() {}, dispose() {} }),
    createStatusBarItem: () => ({ show() {}, dispose() {} }),
    showErrorMessage() {}, showInformationMessage() {},
    showOpenDialog: async () => undefined,
    showQuickPick: async () => undefined,
    registerTreeDataProvider: (id, provider) => { treeProviders.set(id, provider); return event(); },
    openTextDocument: async uri => ({ uri }),
    showTextDocument: async document => { openedFinding = { document, editor: { revealRange() {}, selection: undefined } }; return openedFinding.editor; }
  },
  workspace: {
    workspaceFolders: [], textDocuments: [],
    openTextDocument: async uri => ({ uri }),
    getConfiguration: () => ({ get: (_key, fallback) => fallback, update: async () => {} }),
    onDidOpenTextDocument: event, onDidChangeTextDocument: event,
    onDidSaveTextDocument: event, onDidCloseTextDocument: event, onDidChangeConfiguration: event
  },
  Selection: class Selection { constructor(start, end) { Object.assign(this, { start, end }); } },
  TextEditorRevealType: { InCenterIfOutsideViewport: 1 },
  commands: { registerCommand: (name, handler) => { commands.set(name, handler); return event(); }, executeCommand: async () => {} }
};

const originalLoad = Module._load;
Module._load = function (request, parent, isMain) {
  if (request === 'vscode') return vscode;
  if (request === './lsp-client' && parent?.filename.endsWith('/extension.js')) return { LoomLanguageClient: FakeLanguageClient };
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
const typeParsed = extension.parseCompilerDiagnostics([
  'error[E1001]: Expected Signed_32, found Unsigned_32',
  '  --> /tmp/type.loom:8:1', '   |', '  8 |   A + B', '   | ^ Expected Signed_32, found Unsigned_32'
].join('\n'), 10);
assert.equal(typeParsed[0].code, 'E1001');

const context = { subscriptions: [], extensionPath: path.resolve(__dirname, '..') };
extension.activate(context);
for (const command of ['loom.checkFile', 'loom.build', 'loom.clean', 'loom.run', 'loom.test', 'loom.fetch', 'loom.showOutput', 'loom.restartLanguageServer', 'loom.refreshWorkspaceIndex'])
  assert.ok(commands.has(command), `missing command registration: ${command}`);
for (const provider of ['completion','documentSymbols','hover','definition','references','rename','workspaceSymbols','signature','codeActions','semanticTokens','highlights']) assert.ok(providers.has(provider), `missing provider: ${provider}`);

const packageJson = JSON.parse(fs.readFileSync(path.resolve(__dirname, '..', 'package.json'), 'utf8'));
assert.equal(packageJson.version, '1.5.0');
assert.equal(packageJson.contributes.configurationDefaults['[loom]']['editor.semanticHighlighting.enabled'], true);
assert.ok(packageJson.contributes.semanticTokenScopes.some(entry => entry.language === 'loom' && entry.scopes.keyword[0] === 'keyword.control.loom'));
assert.ok(packageJson.contributes.languages[0].extensions.includes('.loom'));
assert.equal(packageJson.contributes.languages[0].icon.light, './icons/Loom_Logo.png');
async function testProvidersAndAuditLifecycle() {
const text = 'Block Twice Takes\n A : Signed_32\nProduces Signed_32\nDeclares\n P : Signed_32 Pointer\nImplements\n P = Address(A)\n Give A + A + Deref(P)\nEnd Twice\nBlock Run Implements\n X : Signed_32 = Twice(1)\nEnd Run\n';
const uri = { fsPath: '/tmp/provider.loom', toString: () => 'file:///tmp/provider.loom' };
const document = {
  uri, fileName: uri.fsPath, languageId: 'loom', version: 1, isUntitled: false,
  lineCount: text.split('\n').length,
  getText: () => text,
  positionAt(offset) { const before = text.slice(0, offset).split('\n'); return { line: before.length - 1, character: before.at(-1).length }; },
  offsetAt(position) { return text.split('\n').slice(0, position.line).reduce((n, line) => n + line.length + 1, 0) + position.character; },
  lineAt(line) { const value = text.split('\n')[line] || ''; return { text: value, isEmpty: !value, range: { start: { line, character: 0 }, end: { line, character: value.length } } }; }
};
vscode.workspace.textDocuments.push(document);
const callOffset = text.lastIndexOf('Twice(1');
const callPosition = document.positionAt(callOffset + 1);
assert.equal((await providers.get('completion').provideCompletionItems(document, callPosition))[0].label, 'FromLsp');
assert.equal((await providers.get('hover').provideHover(document, callPosition)).contents, 'LSP hover');
assert.ok(await providers.get('definition').provideDefinition(document, callPosition));
assert.equal((await providers.get('signature').provideSignatureHelp(document, document.positionAt(callOffset + 'Twice('.length))).signatures[0].label, 'FromLsp(A : Signed_32)');
assert.equal((await providers.get('references').provideReferences(document, callPosition, { includeDeclaration: true })).length, 2);
assert.equal((await providers.get('rename').provideRenameEdits(document, callPosition, 'Double')).edits.length, 1);
assert.equal((await providers.get('documentSymbols').provideDocumentSymbols(document)).length, 1);
assert.ok((await providers.get('semanticTokens').provideDocumentSemanticTokens(document)).length > 0);
assert.equal((await providers.get('workspaceSymbols').provideWorkspaceSymbols('From'))[0].name, 'FromLsp');
assert.equal((await providers.get('highlights').provideDocumentHighlights(document, callPosition)).length, 1);
assert.equal(lspCalls.length, 10, 'language features must query the LSP');

// The built-in fallback formatter must keep Ensure bodies nested even when
// the workspace tool is unavailable (the test URI is not a file URI).
const ensureText = 'Block Run\nImplements\n    Ensure {\n    Write("x")\n    }\nEnd Run\n';
const ensureDoc = {
  uri: { scheme: 'untitled', fsPath: '/tmp/ensure.loom', toString: () => 'untitled:ensure.loom' },
  fileName: '/tmp/ensure.loom', languageId: 'loom', version: 1, isUntitled: true,
  lineCount: ensureText.split('\n').length,
  getText: () => ensureText,
  lineAt(line) { const value = ensureText.split('\n')[line] || ''; return { text: value, isEmpty: !value, range: { start: { line, character: 0 }, end: { line, character: value.length } } }; }
};
const formatEdits = await providers.get('format').provideDocumentFormattingEdits(ensureDoc, { insertSpaces: true });
const formattedLines = formatEdits.map(edit => edit.args[1]);
assert.ok(formattedLines.includes('            Write("x")'), 'Ensure body must be indented');
assert.ok(formattedLines.includes('        }'), 'closing brace must align with Ensure');
const findingRange = { start: { line: 4, character: 2 }, end: { line: 4, character: 7 } };
for (const handler of clientEvents.get('notification') || []) handler('textDocument/publishDiagnostics', { uri: 'file:///tmp/provider.loom', diagnostics: [{ range: findingRange, severity: 1, code: 'E1001', source: 'Loom', message: 'Syntax error' }] });
assert.ok(storedDiagnostics.get('file:///tmp/provider.loom')?.length > 0, 'compiler diagnostics should be published to editor');

// The built-in fallback formatter must align Default: with When in Match statements
const matchText = 'Block Test\nImplements\nMatch X\nWhen 1\nPrint(1)\nDefault:\nPrint(0)\nEnd Match\nEnd Test\n';
const matchDoc = {
  uri: { scheme: 'untitled', fsPath: '/tmp/match.loom', toString: () => 'untitled:match.loom' },
  fileName: '/tmp/match.loom', languageId: 'loom', version: 1, isUntitled: true,
  lineCount: matchText.split('\n').length,
  getText: () => matchText,
  lineAt(line) { const value = matchText.split('\n')[line] || ''; return { text: value, isEmpty: !value, range: { start: { line, character: 0 }, end: { line, character: value.length } } }; }
};
const matchEdits = await providers.get('format').provideDocumentFormattingEdits(matchDoc, { insertSpaces: true });
const formattedMatch = matchEdits.map(edit => edit.args[1]);
assert.ok(formattedMatch.includes('            When 1'), 'When must be indented inside Match');
assert.ok(formattedMatch.includes('            Default:'), 'Default: must align with When in Match statement');
console.log('VS Code extension smoke tests passed');
}
testProvidersAndAuditLifecycle().catch(error => { console.error(error); process.exitCode = 1; });

Module._load = originalLoad;
