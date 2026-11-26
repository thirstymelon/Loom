const vscode = require('vscode');
const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { promisify } = require('util');
const execFile = promisify(cp.execFile);
const { LoomLanguageClient } = require('./lsp-client');
const { parseCompilerDiagnostics } = require('./diagnostics');
const loom = require('./language');
let activeCleanup = () => {};

function registerLspProviders(context, client, vscode, semanticLegend) {
  const td = document => ({ uri: document.uri.toString() });
  const position = p => ({ line: p.line, character: p.character });
  const lspPosition = p => ({ line: p.line, character: p.character });
  const lspRange = r => new vscode.Range(lspPosition(r.start), lspPosition(r.end));
  const location = value => new vscode.Location(vscode.Uri.parse(value.uri || value.targetUri), lspRange(value.range || value.targetSelectionRange || value.targetRange));
  const request = (method, params, token) => client.request(method, params, token);
  context.subscriptions.push(
    vscode.languages.registerCompletionItemProvider('loom', {
      async provideCompletionItems(document, pos, token) {
        const text = document.getText();
        const items = [];
        const seen = new Set();

        const addItem = (label, kind, detail, docText, sortPriority = 2) => {
          if (!label || seen.has(label)) return;
          seen.add(label);
          const item = new vscode.CompletionItem(label, kind);
          if (detail) item.detail = detail;
          if (docText) item.documentation = typeof vscode.MarkdownString === 'function' ? new vscode.MarkdownString(docText) : docText;
          item.sortText = `${sortPriority}_${label}`;
          items.push(item);
        };

        // 1. LSP items have first priority
        try {
          const result = await request('textDocument/completion', { textDocument: td(document), position: position(pos) }, token);
          if (result && Array.isArray(result.items)) {
            for (const item of result.items) {
              if (item.documentation && typeof item.documentation === 'object' && item.documentation.value) {
                item.documentation = typeof vscode.MarkdownString === 'function' ? new vscode.MarkdownString(item.documentation.value) : item.documentation.value;
              }
              if (item.textEdit?.range) item.textEdit.range = lspRange(item.textEdit.range);
              addItem(item.label, item.kind || vscode.CompletionItemKind.Variable, item.detail, typeof item.documentation === 'string' ? item.documentation : undefined, 0);
            }
          }
        } catch (_) {}

        // 2. Instant local file analysis
        let localAnalysis = null;
        let activeBlock = '';
        try {
          localAnalysis = loom.analyze(text, document.uri.toString());
          const offset = document.offsetAt(pos);
          activeBlock = loom.currentBlock(localAnalysis, offset);
        } catch (_) {}

        if (localAnalysis) {
          for (const s of localAnalysis.symbols) {
            const isLocal = s.scope === activeBlock;
            const priority = isLocal ? 0 : 1;
            const kind = s.kind === 'function' ? vscode.CompletionItemKind.Function :
                         s.kind === 'record' ? vscode.CompletionItemKind.Struct :
                         s.kind === 'enum' ? vscode.CompletionItemKind.Enum :
                         s.kind === 'field' ? vscode.CompletionItemKind.Field :
                         s.kind === 'parameter' ? vscode.CompletionItemKind.Variable :
                         vscode.CompletionItemKind.Variable;
            const detail = s.type ? `${s.name} : ${s.type}` : `${s.name} (${s.kind})`;
            const doc = `**${s.name}**\n\n\`${s.type || s.kind}\` in ${s.scope}`;
            addItem(s.name, kind, detail, doc, priority);
          }
        }

        // 3. Built-in types
        for (const t of loom.BUILTIN_TYPES) {
          addItem(t, vscode.CompletionItemKind.Class, `Built-in type ${t}`, `Standard Loom primitive or collection type \`${t}\``, 3);
        }

        // 4. Keywords
        for (const k of loom.KEYWORDS) {
          addItem(k, vscode.CompletionItemKind.Keyword, `Keyword ${k}`, `Loom language keyword \`${k}\``, 4);
        }

        return items;
      }
    }),
    vscode.languages.registerDocumentSymbolProvider('loom', {
      async provideDocumentSymbols(document, token) {
        const symbols = await request('textDocument/documentSymbol', { textDocument: td(document) }, token);
        const convert = symbol => { const item = new vscode.DocumentSymbol(symbol.name, symbol.detail || '', symbol.kind, lspRange(symbol.range), lspRange(symbol.selectionRange || symbol.range)); item.children = (symbol.children || []).map(convert); return item; };
        return (symbols || []).map(convert);
      }
    }),
    vscode.languages.registerHoverProvider('loom', {
      async provideHover(document, pos, token) { const result = await request('textDocument/hover', { textDocument: td(document), position: position(pos) }, token); if (!result) return; const contents = result.contents?.value || result.contents; return new vscode.Hover(contents, result.range && lspRange(result.range)); }
    }),
    vscode.languages.registerDefinitionProvider('loom', {
      async provideDefinition(document, pos, token) { const result = await request('textDocument/definition', { textDocument: td(document), position: position(pos) }, token); return Array.isArray(result) ? result.map(location) : result && location(result); }
    }),
    vscode.languages.registerReferenceProvider('loom', {
      async provideReferences(document, pos, options, token) { const result = await request('textDocument/references', { textDocument: td(document), position: position(pos), context: { includeDeclaration: options.includeDeclaration } }, token); return (result || []).map(location); }
    }),
    vscode.languages.registerRenameProvider('loom', {
      async prepareRename(document, pos, token) { const result = await request('textDocument/prepareRename', { textDocument: td(document), position: position(pos) }, token); return result?.range ? { ...result, range: lspRange(result.range) } : result && lspRange(result); },
      async provideRenameEdits(document, pos, newName, token) {
        const result = await request('textDocument/rename', { textDocument: td(document), position: position(pos), newName }, token);
        if (!result) return;
        const edit = new vscode.WorkspaceEdit();
        for (const [uri, edits] of Object.entries(result.changes || {})) for (const item of edits) edit.replace(vscode.Uri.parse(uri), lspRange(item.range), item.newText);
        for (const item of result.documentChanges || []) for (const change of item.edits || []) edit.replace(vscode.Uri.parse(item.textDocument.uri), lspRange(change.range), change.newText);
        return edit;
      }
    }),
    vscode.languages.registerWorkspaceSymbolProvider('loom', {
      async provideWorkspaceSymbols(query, token) { const result = await request('workspace/symbol', { query }, token); return (result || []).map(s => new vscode.SymbolInformation(s.name, s.kind, s.containerName || '', location(s.location))); }
    }),
    vscode.languages.registerDocumentSemanticTokensProvider('loom', {
      async provideDocumentSemanticTokens(document, token) {
        const result = await request('textDocument/semanticTokens/full', { textDocument: td(document) }, token);
        const builder = new vscode.SemanticTokensBuilder(semanticLegend); let line = 0, character = 0;
        for (let i = 0; i < (result?.data || []).length; i += 5) {
          line += result.data[i]; character = result.data[i] ? result.data[i + 1] : character + result.data[i + 1];
          const modifiers = semanticLegend.tokenModifiers.filter((_, bit) => (result.data[i + 4] & (1 << bit)) !== 0);
          builder.push(line, character, result.data[i + 2], result.data[i + 3], modifiers);
        }
        return builder.build();
      }
    }, semanticLegend),
    vscode.languages.registerSignatureHelpProvider('loom', {
      async provideSignatureHelp(document, pos, token) { const result = await request('textDocument/signatureHelp', { textDocument: td(document), position: position(pos) }, token); if (!result) return; const markdown = value => value && typeof value === 'object' ? (value.value || '') : value; const help = new vscode.SignatureHelp(); help.signatures = (result.signatures || []).map(s => { const info = new vscode.SignatureInformation(s.label, markdown(s.documentation)); info.parameters = (s.parameters || []).map(p => new vscode.ParameterInformation(p.label, markdown(p.documentation))); return info; }); help.activeSignature = result.activeSignature || 0; help.activeParameter = result.activeParameter || 0; return help; }
    }, '(', ','),
    vscode.languages.registerCodeActionsProvider('loom', {
      provideCodeActions(document, range, context, token) { return request('textDocument/codeAction', { textDocument: td(document), range: { start: position(range.start), end: position(range.end) }, context: { diagnostics: context.diagnostics || [] } }, token); }
    }),
    vscode.languages.registerDocumentHighlightProvider('loom', {
      async provideDocumentHighlights(document, pos, token) { const items = await request('textDocument/documentHighlight', { textDocument: td(document), position: position(pos) }, token); return (items || []).map(item => new vscode.DocumentHighlight(lspRange(item.range), item.kind)); }
    })
  );
}

function activate(context) {
  const output = vscode.window.createOutputChannel('Loom');
  const lspDiagnostics = vscode.languages.createDiagnosticCollection('loom-language-server');
  const buildDiagnostics = vscode.languages.createDiagnosticCollection('loom-build');
  const rootPath = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath || '';
  const languageClient = new LoomLanguageClient(process.execPath, path.join(__dirname, 'server.js'));
  let lastLanguageServerState = 'stopped';
  let languageServerRestartTimer;
  let extensionDisposed = false;

  const status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
  status.text = '$(check) Loom';
  status.tooltip = 'Loom Language Server: Ready';
  status.command = 'loom.showOutput';

  languageClient.on('state', state => {
    output.appendLine(`Language server: ${state}`);
    if (state === 'running') {
      status.text = '$(check) Loom';
      status.tooltip = 'Loom Language Server: Ready';
    } else if (state === 'starting') {
      status.text = '$(sync~spin) Loom';
      status.tooltip = 'Loom Language Server: Starting...';
    } else {
      status.text = '$(circle-slash) Loom';
      status.tooltip = `Loom Language Server: ${state}`;
    }
    if (lastLanguageServerState === 'running' && state === 'stopped' && !extensionDisposed) {
      output.appendLine('Language server exited unexpectedly; restarting it.');
      clearTimeout(languageServerRestartTimer);
      languageServerRestartTimer = setTimeout(() => { if (!extensionDisposed) startLanguageServer(); }, 1000);
    }
    lastLanguageServerState = state;
  });
  languageClient.on('log', line => output.append(line));
  languageClient.on('error', error => vscode.window.showErrorMessage(`Loom language server failed: ${error.message}`));
  languageClient.on('notification', (method, params) => {
    if (method === 'textDocument/publishDiagnostics') {
      const uri = vscode.Uri.parse(params.uri);
      const diagnostics = (params.diagnostics || []).map(d => {
        const item = new vscode.Diagnostic(new vscode.Range(d.range.start.line, d.range.start.character, d.range.end.line, d.range.end.character), d.message,
          d.severity === 1 ? vscode.DiagnosticSeverity.Error : d.severity === 2 ? vscode.DiagnosticSeverity.Warning : d.severity === 3 ? vscode.DiagnosticSeverity.Information : vscode.DiagnosticSeverity.Hint);
        item.code = d.code; item.source = d.source || 'Loom'; return item;
      });
      lspDiagnostics.set(uri, diagnostics);
    } else if (method === 'loom/indexStatus') {
      if (params.status === 'indexing') {
        status.text = '$(sync~spin) Loom';
        status.tooltip = `Loom: Indexing ${params.fileCount || 0} workspace files...`;
      } else {
        status.text = '$(check) Loom';
        status.tooltip = `Loom: Language Server Ready (${params.fileCount || 0} files indexed)`;
      }
    }
  });

  let workspaceProcess;
  context.subscriptions.push(lspDiagnostics, buildDiagnostics, output, status, { dispose: () => { extensionDisposed = true; clearTimeout(languageServerRestartTimer); languageClient.stop(); } });

  function currentServerOptions() {
    const config = vscode.workspace.getConfiguration('loom');
    const configured = config.get('compilerPath', '').trim();
    const activeDocument = vscode.window.activeTextEditor?.document;
    return {
      compilerPath: configured || compilerPath(activeDocument),
      includePaths: findSdkPaths(activeDocument),
      lintRun: config.get('lint.run', 'onType'),
      lintDelay: config.get('lint.delay', 500),
      workspaceFolders: (vscode.workspace.workspaceFolders || []).map(folder => ({ uri: folder.uri.toString(), name: folder.name }))
    };
  }
  function synchronizeOpenDocuments() {
    for (const doc of vscode.workspace.textDocuments) if (doc.languageId === 'loom') languageClient.notify('textDocument/didOpen', { textDocument: { uri: doc.uri.toString(), languageId: 'loom', version: doc.version, text: doc.getText() } });
  }
  function startLanguageServer() {
    return languageClient.start(rootPath, currentServerOptions()).then(synchronizeOpenDocuments).catch(error => output.appendLine(`Language server initialization failed: ${error.message}`));
  }
  startLanguageServer();
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(doc => { if (doc.languageId === 'loom') languageClient.notify('textDocument/didOpen', { textDocument: { uri: doc.uri.toString(), languageId: 'loom', version: doc.version, text: doc.getText() } }); }),
    vscode.workspace.onDidChangeTextDocument(e => { if (e.document.languageId === 'loom') languageClient.notify('textDocument/didChange', { textDocument: { uri: e.document.uri.toString(), version: e.document.version }, contentChanges: [{ text: e.document.getText() }] }); }),
    vscode.workspace.onDidSaveTextDocument(doc => { if (doc.languageId === 'loom') languageClient.notify('textDocument/didSave', { textDocument: { uri: doc.uri.toString() }, text: doc.getText() }); }),
    vscode.workspace.onDidCloseTextDocument(doc => { if (doc.languageId === 'loom') languageClient.notify('textDocument/didClose', { textDocument: { uri: doc.uri.toString() } }); }),
    vscode.workspace.onDidChangeConfiguration(event => { if (event.affectsConfiguration('loom')) startLanguageServer(); })
  );
  if (typeof vscode.workspace.createFileSystemWatcher === 'function') {
    const watcher = vscode.workspace.createFileSystemWatcher('**/*.loom');
    context.subscriptions.push(watcher,
      watcher.onDidCreate(uri => { languageClient.notify('workspace/didChangeWatchedFiles', { changes: [{ uri: uri.toString(), type: 1 }] }); }),
      watcher.onDidChange(uri => { languageClient.notify('workspace/didChangeWatchedFiles', { changes: [{ uri: uri.toString(), type: 2 }] }); }),
      watcher.onDidDelete(uri => { languageClient.notify('workspace/didChangeWatchedFiles', { changes: [{ uri: uri.toString(), type: 3 }] }); }),
      vscode.workspace.onDidChangeWorkspaceFolders(event => languageClient.notify('workspace/didChangeWorkspaceFolders', { event: {
        added: event.added.map(folder => ({ uri: folder.uri.toString(), name: folder.name })),
        removed: event.removed.map(folder => ({ uri: folder.uri.toString(), name: folder.name }))
      } }))
    );
  }
  status.show();

  const semanticLegend = new vscode.SemanticTokensLegend(
    ['namespace', 'type', 'class', 'function', 'parameter', 'variable', 'property', 'keyword', 'number', 'string', 'comment'],
    ['declaration']
  );
  registerLspProviders(context, languageClient, vscode, semanticLegend);

  function defaultToolchainKind() {
    const candidates = [];
    if (process.env.HOME) candidates.push(path.join(process.env.HOME, '.loom', 'default_toolchain'));
    candidates.push('/usr/local/share/loom/default_toolchain', '/usr/share/loom/default_toolchain', '/opt/loom/default_toolchain');
    for (const file of candidates) {
      try {
        const value = fs.readFileSync(file, 'utf8').trim().toLowerCase();
        if (value === 'native' || value === 'rp2040') return value;
      } catch (_) {}
    }
    return 'native';
  }

  function projectToolchainKind(document) {
    const root = findWorkspaceRoot(document);
    if (root) {
      try {
        const text = fs.readFileSync(path.join(root, 'config.toml'), 'utf8');
        let section = '';
        let target = '';
        let cpu = '';
        let selected = '';
        for (const raw of text.split(/\r?\n/)) {
          const line = raw.replace(/#.*/, '').trim();
          const heading = /^\[([^\]]+)\]$/.exec(line);
          if (heading) { section = heading[1].toLowerCase(); continue; }
          const entry = /^([A-Za-z0-9_-]+)\s*=\s*"([^"]*)"/.exec(line);
          if (!entry) continue;
          const key = entry[1].toLowerCase();
          if (section === 'build' && key === 'target') target = entry[2].toLowerCase();
          if (section === 'build' && key === 'cpu') cpu = entry[2].toLowerCase();
          if (section === 'toolchain' && (key === 'name' || key === 'toolchain')) selected = entry[2].toLowerCase();
        }
        if (selected === 'native' || selected === 'rp2040') return selected;
        if (target.includes('thumbv6m') || cpu.includes('cortex-m0plus')) return 'rp2040';
      } catch (_) {}
    }
    return defaultToolchainKind();
  }

  function compilerPath(document) {
    const configured = vscode.workspace.getConfiguration('loom').get('compilerPath', '').trim();
    if (configured) return configured;
    const kind = projectToolchainKind(document);
    const candidates = [];
    // Prefer compiler binaries shipped with the opened project or its parent
    // Loom checkout. This keeps the editor on the same compiler build used by
    // `loom build` and avoids silently selecting a stale global toolchain.
    for (const folder of vscode.workspace.workspaceFolders || []) {
      let current = folder.uri.fsPath;
      for (let depth = 0; depth < 6; depth++) {
        candidates.push(path.join(current, 'bin', 'loomc'));
        candidates.push(path.join(current, 'loomc'));
        candidates.push(path.join(current, '.build', 'loomc'));
        const parent = path.dirname(current);
        if (parent === current) break;
        current = parent;
      }
    }
    candidates.push('/usr/local/bin/loomc', '/opt/homebrew/bin/loomc', '/usr/bin/loomc');
    if (process.env.HOME) candidates.push(path.join(process.env.HOME, '.loom', 'toolchains', kind, 'bin', 'loomc'));
    for (const prefix of ['/usr/local/share/loom/toolchains', '/usr/share/loom/toolchains', '/opt/loom/toolchains']) {
      candidates.push(path.join(prefix, kind, 'bin', 'loomc'));
    }
    if (context.extensionPath) candidates.push(path.resolve(context.extensionPath, '..', 'bin', 'loomc'));
    for (const candidate of candidates) {
      try { if (fs.statSync(candidate).isFile()) return candidate; } catch (_) {}
    }
    // An empty value lets the language server run its own discovery instead
    // of pinning it to an unresolvable literal command name.
    return '';
  }

  function findSdkPaths(document) {
    const candidates = [];
    if (process.env.LOOM_SDK_PATH) candidates.push(process.env.LOOM_SDK_PATH);
    if (process.env.LOOM_SDK) candidates.push(process.env.LOOM_SDK);
    if (process.env.HOME) candidates.push(path.join(process.env.HOME, '.loom', 'sdk'));
    candidates.push('/usr/local/share/loom/sdk');
    candidates.push('/opt/loom/sdk');
    const preferred = projectToolchainKind(document);
    const kinds = [preferred, preferred === 'native' ? 'rp2040' : 'native'];
    for (const kind of kinds) {
      if (process.env.HOME) candidates.push(path.join(process.env.HOME, '.loom', 'toolchains', kind, 'sdk'));
      for (const prefix of ['/usr/local/share/loom/toolchains', '/usr/share/loom/toolchains', '/opt/loom/toolchains']) {
        candidates.push(path.join(prefix, kind, 'sdk'));
      }
    }
    for (const folder of vscode.workspace.workspaceFolders || []) {
      candidates.push(path.join(folder.uri.fsPath, 'sdk'));
    }
    if (context.extensionPath) {
      candidates.push(path.resolve(context.extensionPath, '..', 'sdk'));
    }
    if (process.env.HOME) {
      const libDir = path.join(process.env.HOME, '.loom', 'libraries');
      try {
        if (fs.existsSync(libDir)) {
          for (const pkg of fs.readdirSync(libDir)) {
            const pkgPath = path.join(libDir, pkg);
            try {
              if (fs.statSync(pkgPath).isDirectory()) {
                candidates.push(pkgPath);
                candidates.push(path.join(pkgPath, 'src'));
              }
            } catch (_) {}
          }
        }
      } catch (_) {}
    }
    const resolved = [];
    const seen = new Set();
    for (const c of candidates) {
      try {
        if (fs.statSync(c).isDirectory()) {
          const subdirs = [
            path.join(c, 'targets', 'rp2040', 'bsp'),
            path.join(c, 'targets', 'rp2040'),
            path.join(c, 'std'),
            c
          ];
          for (const sub of subdirs) {
            try {
              if (fs.statSync(sub).isDirectory() && !seen.has(sub)) {
                seen.add(sub);
                resolved.push(sub);
              }
            } catch (_) {}
          }
        }
      } catch (_) {}
    }
    return resolved;
  }



  async function check(document) {
    if (document.languageId !== 'loom' || document.isUntitled) return;
    languageClient.notify('loom/checkDocument', { uri: document.uri.toString() });
  }

  function findWorkspaceRoot(document) {
    const folders = vscode.workspace.workspaceFolders || [];
    const file = document?.uri?.scheme === 'file' ? path.dirname(document.uri.fsPath) : undefined;
    const roots = folders.map(folder => folder.uri.fsPath);
    if (file) {
      let current = file;
      for (;;) {
        if (fs.existsSync(path.join(current, 'config.toml'))) return current;
        const parent = path.dirname(current);
        if (parent === current || !roots.some(root => current === root || current.startsWith(root + path.sep))) break;
        current = parent;
      }
    }
    for (const root of roots) if (fs.existsSync(path.join(root, 'config.toml'))) return root;
    return undefined;
  }

  function loomToolPath(root) {
    const configured = vscode.workspace.getConfiguration('loom').get('toolPath', '').trim();
    if (configured) return configured;
    const selectedCompiler = compilerPath(vscode.window.activeTextEditor?.document);
    const candidates = ['/usr/local/bin/loom', '/opt/homebrew/bin/loom', '/usr/bin/loom', path.join(root, 'bin', 'loom'), path.join(root, 'loom'), path.join(root, '.build', 'loom')];
    if (selectedCompiler) candidates.push(path.join(path.dirname(selectedCompiler), 'loom'));
    if (process.env.HOME) candidates.push(path.join(process.env.HOME, '.loom', 'bin', 'loom'));
    let current = root;
    for (let depth = 0; depth < 6; depth++) {
      candidates.push(path.join(current, 'bin', 'loom'));
      candidates.push(path.join(current, 'loom'));
      candidates.push(path.join(current, '.build', 'loom'));
      const parent = path.dirname(current);
      if (parent === current) break;
      current = parent;
    }
    if (context.extensionPath) candidates.push(path.resolve(context.extensionPath, '..', 'bin', 'loom'));
    for (const candidate of candidates) {
      try { if (fs.statSync(candidate).isFile()) return candidate; } catch (_) {}
    }
    return 'loom';
  }

  async function formatWithLoomFmt(document) {
    if (document.isUntitled || document.uri?.scheme !== 'file') return undefined;
    const root = findWorkspaceRoot(document) || path.dirname(document.uri.fsPath);
    const executable = loomToolPath(root);
    const temp = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'loom-fmt-'));
    const target = path.join(temp, path.basename(document.uri.fsPath) || 'document.loom');
    try {
      await fs.promises.writeFile(target, document.getText(), 'utf8');
      await execFile(executable, ['fmt', target], { cwd: root, windowsHide: true });
      const text = await fs.promises.readFile(target, 'utf8');
      if (text === document.getText()) return [];
      const lastLine = document.lineCount > 0 ? document.lineAt(document.lineCount - 1) : null;
      const fullRange = lastLine ? new vscode.Range(0, 0, document.lineCount - 1, lastLine.text.length) : new vscode.Range(0, 0, 0, 0);
      return [vscode.TextEdit.replace(fullRange, text)];
    } catch (error) {
      output.appendLine(`loom fmt unavailable (${error.message}); using built-in indentation.`);
      return undefined;
    } finally {
      await fs.promises.rm(temp, { recursive: true, force: true }).catch(() => {});
    }
  }

  function updateBuildDiagnostics(text, root) {
    buildDiagnostics.clear();
    const grouped = new Map();
    for (const item of parseCompilerDiagnostics(text, Number.MAX_SAFE_INTEGER)) {
      if (!item.file) continue;
      const file = path.isAbsolute(item.file) ? item.file : path.resolve(root, item.file);
      const uri = vscode.Uri.file(file);
      const severity = item.severity === 1 ? vscode.DiagnosticSeverity.Error : item.severity === 2 ? vscode.DiagnosticSeverity.Warning : item.severity === 3 ? vscode.DiagnosticSeverity.Information : vscode.DiagnosticSeverity.Hint;
      const diagnostic = new vscode.Diagnostic(new vscode.Range(item.line, item.column, item.line, item.column + Math.max(1, item.length || 1)), item.message, severity);
      diagnostic.code = item.code; diagnostic.source = 'Loom Build';
      const entries = grouped.get(uri.toString()) || { uri, items: [] };
      entries.items.push(diagnostic); grouped.set(uri.toString(), entries);
    }
    for (const entry of grouped.values()) buildDiagnostics.set(entry.uri, entry.items);
  }

  async function runWorkspaceCommand(command) {
    const root = findWorkspaceRoot(vscode.window.activeTextEditor?.document);
    if (!root) {
      vscode.window.showErrorMessage('Loom: open a workspace containing config.toml to build, clean, or run.');
      return;
    }
    const executable = loomToolPath(root);
    const config = path.join(root, 'config.toml');
    buildDiagnostics.clear();
    workspaceProcess?.kill();
    output.clear(); output.show(true);
    output.appendLine(`> ${executable} ${command} --config ${config}`);
    status.text = `$(sync~spin) Loom ${command}`;
    return new Promise(resolve => {
      const proc = cp.spawn(executable, [command, '--config', config], { cwd: root, windowsHide: true });
      workspaceProcess = proc;
      let stderr = '', stdout = '';
      let finished = false;
      const finish = () => {
        if (finished) return false;
        finished = true;
        if (workspaceProcess !== proc) return false;
        workspaceProcess = undefined;
        return true;
      };
      proc.stdout.setEncoding('utf8').on('data', chunk => { stdout += chunk; output.append(chunk); });
      proc.stderr.setEncoding('utf8').on('data', chunk => { stderr += chunk; output.append(chunk); });
      proc.on('error', err => {
        if (!finish()) return;
        status.text = '$(error) Loom';
        output.appendLine(`Could not run '${executable}': ${err.message}. Set loom.toolPath.`);
        vscode.window.showErrorMessage(`Loom ${command} failed to start. See the Loom output channel.`);
        resolve();
      });
      proc.on('close', code => {
        if (!finish()) return;
        if (code !== null) updateBuildDiagnostics(`${stderr}\n${stdout}`, root);
        status.text = code === 0 ? '$(check) Loom' : '$(error) Loom';
        output.appendLine(code === 0 ? `Loom ${command} completed.` : `Loom ${command} exited with code ${code}.`);
        if (code === 0) vscode.window.showInformationMessage(`Loom ${command} completed.`);
        else vscode.window.showErrorMessage(`Loom ${command} failed. See the Loom output channel.`);
        resolve();
      });
    });
  }

  activeCleanup = () => {
    workspaceProcess?.kill();
    workspaceProcess = undefined;
  };

  context.subscriptions.push(
    vscode.workspace.onDidCloseTextDocument(doc => { lspDiagnostics.delete(doc.uri); }),
    vscode.commands.registerCommand('loom.showOutput', () => output.show(true)),
    vscode.commands.registerCommand('loom.checkFile', () => { const ed = vscode.window.activeTextEditor; if (ed?.document.languageId === 'loom') return check(ed.document); }),
    vscode.commands.registerCommand('loom.setCompilerPath', async () => {
      const pick = await vscode.window.showOpenDialog({ canSelectFiles: true, canSelectFolders: false, canSelectMany: false, openLabel: 'Select loomc' });
      if (pick?.[0]) await vscode.workspace.getConfiguration('loom').update('compilerPath', pick[0].fsPath, vscode.ConfigurationTarget.Workspace);
    }),
    vscode.commands.registerCommand('loom.formatDocument', () => vscode.commands.executeCommand('editor.action.formatDocument')),
    vscode.commands.registerCommand('loom.build', () => runWorkspaceCommand('build')),
    vscode.commands.registerCommand('loom.clean', () => runWorkspaceCommand('clean')),
    vscode.commands.registerCommand('loom.run', () => runWorkspaceCommand('run')),
    vscode.commands.registerCommand('loom.test', () => runWorkspaceCommand('test')),
    vscode.commands.registerCommand('loom.fetch', () => runWorkspaceCommand('fetch')),
    vscode.commands.registerCommand('loom.flash', () => {
      const root = findWorkspaceRoot(vscode.window.activeTextEditor?.document);
      const executable = root ? loomToolPath(root) : 'loom';
      const term = vscode.window.createTerminal({ name: 'Loom Flash', cwd: root });
      term.show();
      term.sendText(`${executable} flash --monitor`);
    }),
    vscode.commands.registerCommand('loom.monitor', () => {
      const root = findWorkspaceRoot(vscode.window.activeTextEditor?.document);
      const executable = root ? loomToolPath(root) : 'loom';
      const term = vscode.window.createTerminal({ name: 'Loom Monitor', cwd: root });
      term.show();
      term.sendText(`${executable} monitor`);
    }),
    vscode.commands.registerCommand('loom.switchToolchain', async () => {
      const root = findWorkspaceRoot(vscode.window.activeTextEditor?.document);
      const executable = root ? loomToolPath(root) : 'loom';
      const pick = await vscode.window.showQuickPick(['rp2040', 'native'], {
        placeHolder: 'Select toolchain to activate with loom use'
      });
      if (pick) {
        const term = vscode.window.createTerminal({ name: 'Loom Toolchain', cwd: root });
        term.show();
        term.sendText(`${executable} use ${pick}`);
      }
    }),
    vscode.commands.registerCommand('loom.restartLanguageServer', async () => { await startLanguageServer(); }),
    vscode.commands.registerCommand('loom.refreshWorkspaceIndex', () => { languageClient.notify('loom/refreshIndex', {}); }),
    vscode.languages.registerDocumentFormattingEditProvider('loom', {
      async provideDocumentFormattingEdits(document, options) {
        // `loom fmt` is the canonical formatter (keyword casing, colon
        // alignment, declarations). The indenter below is only a fallback for
        // machines where the workspace tool is unavailable.
        const formatted = await formatWithLoomFmt(document);
        if (formatted !== undefined) return formatted;
        const size = vscode.workspace.getConfiguration('loom').get('format.indentSize', 4);
        const unit = options.insertSpaces ? ' '.repeat(size) : '\t';
        let inBlock = false;
        let inType = false;
        let blockHasInlineImplements = false;
        let blockHasInlineTakes = false;
        let currentSection = '';
        let controlStack = [];
        let ensureDepth = 0;
        const edits = [];

        for (let i = 0; i < document.lineCount; i++) {
          const line = document.lineAt(i);
          const raw = line.text.trim();
          if (!raw) {
            if (line.text.length > 0) edits.push(vscode.TextEdit.replace(line.range, ''));
            continue;
          }

          const isEndIf = /^end\s+if\b/i.test(raw);
          const isEndLoop = /^end\s+loop\b/i.test(raw);
          const isEndMatch = /^end\s+match\b/i.test(raw);
          const isEndEnsure = /^end\s+ensure\b/i.test(raw);
          const isElseIf = /^else\s+if\b/i.test(raw);
          const isElse = /^(else\b|else\s*:)/i.test(raw);
          const isWhen = /^when\b/i.test(raw);
          const isDefault = /^default\s*:/i.test(raw);
          const isEndGeneral = /^end\s+[a-z_][a-z0-9_]*\s*$/i.test(raw) && !isEndIf && !isEndLoop && !isEndMatch && !isEndEnsure;

          const closesEnsure = ensureDepth > 0 && /^\}/.test(raw);
          let lineDepth = 0;

          if (closesEnsure) {
            const base = blockHasInlineImplements ? 1 : 2;
            lineDepth = base + controlStack.length + ensureDepth - 1;
          } else if (isEndIf || isEndLoop || isEndMatch || isEndEnsure) {
            if (controlStack.length > 0 && controlStack[controlStack.length - 1].type === 'when') controlStack.pop();
            if (controlStack.length > 0) controlStack.pop();
            const base = blockHasInlineImplements ? 1 : 2;
            lineDepth = base + controlStack.length;
          } else if (isElseIf || isElse) {
            if (controlStack.length > 0 && controlStack[controlStack.length - 1].type === 'when') controlStack.pop();
            const base = blockHasInlineImplements ? 1 : 2;
            lineDepth = base + (controlStack.length > 0 ? controlStack.length - 1 : 0);
          } else if (isWhen || isDefault) {
            if (controlStack.length > 0 && controlStack[controlStack.length - 1].type === 'when') controlStack.pop();
            const base = blockHasInlineImplements ? 1 : 2;
            lineDepth = base + controlStack.length;
          } else if (isEndGeneral) {
            lineDepth = 0;
            inBlock = false;
            inType = false;
            currentSection = '';
            controlStack = [];
          } else if (/^block\b/i.test(raw)) {
            lineDepth = 0;
            inBlock = true;
            inType = false;
            controlStack = [];
            blockHasInlineImplements = /\bimplements\b/i.test(raw);
            blockHasInlineTakes = /\btakes\b/i.test(raw);
            currentSection = blockHasInlineTakes ? 'takes' : (blockHasInlineImplements ? 'implements' : '');
          } else if (/^(record|enum|extern\s+block)\b/i.test(raw)) {
            lineDepth = 0;
            inType = true;
            inBlock = false;
            currentSection = '';
          } else if (/^(declares|implements|takes|produces)\b/i.test(raw)) {
            lineDepth = 1;
            if (/^declares\b/i.test(raw)) currentSection = 'declares';
            else if (/^implements\b/i.test(raw)) currentSection = 'implements';
            else if (/^takes\b/i.test(raw)) currentSection = 'takes';
            else if (/^produces\b/i.test(raw)) currentSection = 'produces';
          } else if (inType) {
            lineDepth = 1;
          } else if (inBlock) {
            if (currentSection === 'takes' && blockHasInlineTakes) {
              lineDepth = 1;
            } else {
              const base = blockHasInlineImplements ? 1 : 2;
              lineDepth = base + controlStack.length + ensureDepth;
            }
          } else {
            lineDepth = 0;
          }

          const desired = unit.repeat(Math.max(0, lineDepth)) + raw;
          if (line.text !== desired) edits.push(vscode.TextEdit.replace(line.range, desired));

          if (closesEnsure) ensureDepth = Math.max(0, ensureDepth - 1);
          else if (/^ensure\b.*\{\s*$/i.test(raw)) ensureDepth++;
          if (/^if\b/i.test(raw)) controlStack.push({ type: 'if' });
          else if (/^loop\b/i.test(raw)) controlStack.push({ type: 'loop' });
          else if (/^match\b/i.test(raw)) controlStack.push({ type: 'match' });
          else if (isWhen || isDefault || /^else\s*:\s*$/i.test(raw)) controlStack.push({ type: 'when' });
        }
        return edits;
      }
    }),
  );

}

function deactivate() { activeCleanup(); }
module.exports = { activate, deactivate, parseCompilerDiagnostics };
