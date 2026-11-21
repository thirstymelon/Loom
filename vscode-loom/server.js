'use strict';

// Standalone JSON-RPC/LSP process. Compiler-produced semantic JSON is the source
// of truth for symbols and expression types; language.js supplies only lexical
// information needed for incomplete-buffer completion and token coloring.
const cp = require('node:child_process');
const fs = require('node:fs');
const fsp = fs.promises;
const os = require('node:os');
const path = require('node:path');
const { pathToFileURL, fileURLToPath } = require('node:url');
const loom = require('./language');
const { parseCompilerDiagnostics } = require('./diagnostics');

const TOKEN_TYPES = ['namespace','type','class','function','parameter','variable','property','keyword','number','string','comment'];
const documents = new Map();
const requestChildren = new Map();
const cancelled = new Set();
const compilerDiagnostics = new Map();
const compilerDiagnosticTargets = new Map();
const openVersions = new Map();
const outputBuffer = Buffer.alloc(0);
let inputBuffer = outputBuffer;
let workspaces = [];
let compilerPath = '';
let includePaths = [];
let lintRun = 'onType';
let lintDelay = 500;
let semantic = { schemaVersion: 1, sources: [], symbols: [], expressions: [] };
let semanticByPath = new Map();
let semanticSymbols = new Map();
let semanticReferences = new Map();
let generation = 0;
let indexTimer;
let analysisTimers = new Map();
let shuttingDown = false;

function send(message) {
  const body = Buffer.from(JSON.stringify(message), 'utf8');
  process.stdout.write(`Content-Length: ${body.length}\r\n\r\n`);
  process.stdout.write(body);
}
function response(id, result) { send({ jsonrpc: '2.0', id, result }); }
function errorResponse(id, code, message) { send({ jsonrpc: '2.0', id, error: { code, message } }); }
function notify(method, params) { send({ jsonrpc: '2.0', method, params }); }

function parseInput() {
  while (true) {
    const headerEnd = inputBuffer.indexOf('\r\n\r\n');
    if (headerEnd < 0) return;
    const header = inputBuffer.subarray(0, headerEnd).toString('ascii');
    const match = /(?:^|\r\n)Content-Length:\s*(\d+)/i.exec(header);
    if (!match) { inputBuffer = inputBuffer.subarray(headerEnd + 4); continue; }
    const size = Number(match[1]);
    if (!Number.isSafeInteger(size) || size < 0 || size > 64 * 1024 * 1024) { inputBuffer = Buffer.alloc(0); return; }
    const bodyStart = headerEnd + 4;
    if (inputBuffer.length < bodyStart + size) return;
    const body = inputBuffer.subarray(bodyStart, bodyStart + size).toString('utf8');
    inputBuffer = inputBuffer.subarray(bodyStart + size);
    try { dispatch(JSON.parse(body)); }
    catch (error) { notify('window/logMessage', { type: 1, message: `Invalid LSP message: ${error.message}` }); }
  }
}
process.stdin.on('data', chunk => { inputBuffer = Buffer.concat([inputBuffer, chunk]); parseInput(); });
process.stdin.on('end', () => { shuttingDown = true; process.exit(0); });

function pathForUri(uri) {
  try { return uri.startsWith('file:') ? path.resolve(fileURLToPath(uri)) : ''; } catch (_) { return ''; }
}
function uriForPath(filePath) { return pathToFileURL(path.resolve(filePath)).toString(); }
function rangeContains(range, pos) {
  const afterStart = pos.line > range.start.line || (pos.line === range.start.line && pos.character >= range.start.character);
  const beforeEnd = pos.line < range.end.line || (pos.line === range.end.line && pos.character <= range.end.character);
  return afterStart && beforeEnd;
}
function documentText(uri) { return documents.get(uri)?.text; }

// Only one compiler run should own a document or workspace index at a time.
// Starting a newer run kills the older child instead of letting it finish in
// the background (which is what made typing outpace the debounce).
const activeChildren = new Map();
function killActiveChild(key) {
  const child = activeChildren.get(key);
  if (!child) return;
  activeChildren.delete(key);
  try { child.kill(); } catch (_) {}
}

function run(command, args, options = {}, requestId, ownerKey) {
  return new Promise((resolve, reject) => {
    if (requestId !== undefined && cancelled.has(requestId)) { reject(Object.assign(new Error('Request cancelled'), { code: -32800 })); return; }
    if (ownerKey) killActiveChild(ownerKey);
    let child;
    try { child = cp.spawn(command, args, { ...options, windowsHide: true, stdio: ['ignore','pipe','pipe'] }); }
    catch (error) { reject(error); return; }
    if (requestId !== undefined) requestChildren.set(requestId, child);
    if (ownerKey) activeChildren.set(ownerKey, child);
    const release = () => {
      if (requestId !== undefined) requestChildren.delete(requestId);
      if (ownerKey && activeChildren.get(ownerKey) === child) activeChildren.delete(ownerKey);
    };
    let stdout = '', stderr = '';
    child.stdout.setEncoding('utf8').on('data', chunk => { stdout += chunk; });
    child.stderr.setEncoding('utf8').on('data', chunk => { stderr += chunk; });
    child.on('error', error => { release(); reject(error); });
    child.on('close', code => {
      release();
      if (cancelled.has(requestId)) reject(Object.assign(new Error('Request cancelled'), { code: -32800 }));
      else resolve({ code, stdout, stderr });
    });
  });
}

function findCompiler(root) {
  // A bare command name is a valid explicit choice; a path that no longer
  // exists must fall through to discovery rather than blocking it.
  if (compilerPath && (compilerPath.includes('/') || compilerPath.includes('\\')) && !fs.existsSync(compilerPath)) compilerPath = '';
  if (compilerPath) return compilerPath;
  if (process.env.LOOMC) return process.env.LOOMC;
  const kinds = [];
  try {
    const marker = process.env.HOME ? path.join(process.env.HOME, '.loom', 'default_toolchain') : '';
    const selected = marker ? fs.readFileSync(marker, 'utf8').trim().toLowerCase() : '';
    if (selected === 'native' || selected === 'rp2040') kinds.push(selected);
  } catch (_) {}
  for (const kind of ['native', 'rp2040']) if (!kinds.includes(kind)) kinds.push(kind);
  const candidates = [];
  if (root) candidates.push(path.join(root, 'bin', 'loomc'), path.join(root, 'loomc'), path.join(root, '.build', 'loomc'));
  if (process.env.HOME) for (const kind of kinds) candidates.push(path.join(process.env.HOME, '.loom', 'toolchains', kind, 'bin', 'loomc'));
  for (const prefix of ['/usr/local/share/loom/toolchains', '/usr/share/loom/toolchains', '/opt/loom/toolchains']) {
    for (const kind of kinds) candidates.push(path.join(prefix, kind, 'bin', 'loomc'));
  }
  candidates.push('/usr/local/bin/loomc', '/usr/bin/loomc');
  for (const candidate of candidates) { try { if (fs.existsSync(candidate)) return candidate; } catch (_) {} }
  return 'loomc';
}

const EXCLUDED_DIRS = new Set(['.git','.build','build','dist','target','node_modules','vendor','deps','.loom','audit','out','coverage']);
async function collectSourceFiles(root) {
  const files = [];
  const stack = [root];
  while (stack.length) {
    const dir = stack.pop();
    let entries;
    try { entries = await fsp.readdir(dir, { withFileTypes: true }); } catch (_) { continue; }
    for (const entry of entries) {
      if (entry.name.startsWith('.') && EXCLUDED_DIRS.has(entry.name)) continue;
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) { if (!EXCLUDED_DIRS.has(entry.name)) stack.push(full); }
      else if (entry.isFile() && entry.name.endsWith('.loom')) files.push(path.resolve(full));
    }
  }
  return files.sort();
}

function allWorkspaceFiles() {
  const indexedPaths = new Set((semantic.sources || []).map(source => path.resolve(source.path)));
  for (const uri of documents.keys()) {
    const file = pathForUri(uri);
    if (file && file.endsWith('.loom')) indexedPaths.add(file);
  }
  return [...indexedPaths].sort();
}

async function createCompilerInputs(root, files, overlayDirty) {
  const temp = await fsp.mkdtemp(path.join(os.tmpdir(), 'loom-lsp-'));
  const inputs = [];
  const sourceOverrides = new Map();
  for (const file of files) {
    const uri = uriForPath(file), opened = documents.get(uri);
    if (overlayDirty && opened && opened.text !== undefined) {
      // Resolve the owner folder per file so a multi-root workspace does not
      // flatten files from additional roots onto the first root's paths.
      const owner = workspaces.find(folder => file === folder || file.startsWith(folder + path.sep)) || root;
      const ownerIndex = workspaces.indexOf(owner);
      const relative = path.relative(owner, file);
      const safeRelative = relative && !relative.startsWith(`..${path.sep}`) && !path.isAbsolute(relative) ? relative : path.basename(file);
      const target = path.join(temp, 'overlay', ownerIndex >= 0 ? String(ownerIndex) : 'external', safeRelative);
      await fsp.mkdir(path.dirname(target), { recursive: true });
      await fsp.writeFile(target, opened.text, 'utf8');
      inputs.push(target);
      sourceOverrides.set(path.resolve(target), file);
    } else inputs.push(file);
  }
  return { temp, inputs, sourceOverrides };
}

function compilerArgs(mode, outputPath, inputs, extraDirectories = []) {
  const args = [mode, '--output', outputPath, ...inputs];
  const directories = new Set([...includePaths, ...workspaces.flatMap(root => [root, path.join(root, 'src')]), ...(semantic.sources || []).map(source => path.dirname(source.path)), ...extraDirectories]);
  for (const include of directories) args.push('-I', include);
  return args;
}

function resetSemantic(next) {
  semantic = next;
  semanticByPath = new Map();
  semanticSymbols = new Map();
  semanticReferences = new Map();
  const sourceFiles = next.sources || [];
  for (const source of sourceFiles) semanticByPath.set(path.resolve(source.path), source);
  for (const symbol of next.symbols || []) semanticSymbols.set(symbol.id, symbol);
  for (const expression of next.expressions || []) {
    if (!expression.resolvedSymbol) continue;
    const source = sourceFiles[expression.sourceIndex];
    if (!source) continue;
    const uri = uriForPath(source.path);
    const rows = semanticReferences.get(expression.resolvedSymbol) || [];
    rows.push({ uri, range: expression.range, kind: expression.kind, owner: expression.owner });
    semanticReferences.set(expression.resolvedSymbol, rows);
  }
}



async function reindex(requestId) {
  const thisGeneration = ++generation;
  const files = [];
  for (const root of workspaces) files.push(...await collectSourceFiles(root));
  for (const [uri, doc] of documents) {
    const file = pathForUri(uri);
    if (file && file.endsWith('.loom') && !files.includes(file)) files.push(file);
  }
  files.sort();
  notify('loom/indexStatus', { status: 'indexing', fileCount: files.length });
  if (!files.length) {
    resetSemantic({ schemaVersion: 1, sources: [], symbols: [], expressions: [] });
    notify('loom/indexStatus', { status: 'ready', fileCount: 0 });
    return;
  }

  const sources = [];
  const allSymbols = [];
  const allExpressions = [];

  for (let i = 0; i < files.length; i++) {
    const file = files[i];
    const uri = uriForPath(file);
    const doc = documents.get(uri);
    let text = doc?.text;
    if (text === undefined) {
      try { text = await fsp.readFile(file, 'utf8'); } catch (_) { continue; }
    }
    const sourceIndex = sources.length;
    sources.push({ path: file, index: sourceIndex });

    const modMatch = /^\s*Module\s+([A-Za-z_][A-Za-z0-9_]*)/m.exec(text);
    const moduleName = modMatch ? modMatch[1] : path.basename(file, '.loom');

    const analysis = loom.analyze(text, uri);
    const toRange = (token) => ({
      start: { line: token.line, character: token.character },
      end: { line: token.line, character: token.character + (token.end - token.start) }
    });

    for (const s of analysis.symbols) {
      const range = toRange(s.token);
      let containerRange = range;
      if (s.kind === 'function') {
        const blk = analysis.blocks.find(b => b.name === s.name);
        if (blk) {
          const startToken = analysis.tokens.find(t => t.start === blk.start) || s.token;
          const endToken = analysis.tokens.filter(t => t.end <= blk.end).pop() || s.token;
          containerRange = {
            start: { line: startToken.line, character: startToken.character },
            end: { line: endToken.line, character: endToken.character + (endToken.end - endToken.start) }
          };
        }
      }
      const sym = {
        id: `${sourceIndex}:${s.name}:${s.kind}:${s.scope}`,
        name: s.name,
        kind: s.kind,
        type: { name: s.type || s.kind, kind: s.kind },
        scope: s.scope === 'workspace' ? 'module' : s.scope,
        module: moduleName,
        range: range,
        containerRange: containerRange,
        sourceIndex: sourceIndex
      };
      if (s.kind === 'function') {
        const blk = analysis.blocks.find(b => b.name === s.name);
        if (blk) {
          sym.type = {
            name: blk.result ? `(${blk.params.map(p => p.type).join(', ')}) -> ${blk.result}` : 'procedure',
            kind: 'blockPointer',
            element: { name: blk.result, kind: blk.result ? 'type' : 'null' }
          };
        }
      }
      allSymbols.push(sym);
    }

    const tokens = analysis.tokens;
    for (let j = 0; j < tokens.length - 1; j++) {
      const t = tokens[j];
      const next = tokens[j + 1];
      if (t.kind === 'identifier' && next?.value === '(') {
        let targetName = t.value;
        allExpressions.push({
          kind: 'call',
          text: targetName,
          range: toRange(t),
          sourceIndex: sourceIndex,
          owner: loom.currentBlock(analysis, t.start),
          resolvedSymbol: null
        });
      }
    }
  }

  for (const expr of allExpressions) {
    const sym = allSymbols.find(s => s.name === expr.text && (s.kind === 'function' || s.kind === 'externFunction'));
    if (sym) {
      expr.resolvedSymbol = sym.id;
    }
  }

  if (thisGeneration !== generation) return;
  resetSemantic({ schemaVersion: 1, positionEncoding: 'utf-16', sources, symbols: allSymbols, expressions: allExpressions });
  notify('loom/indexStatus', { status: 'ready', fileCount: sources.length, symbolCount: allSymbols.length, failedFiles: 0 });
}

function scheduleIndex(delay = 650) {
  clearTimeout(indexTimer);
  generation++;
  indexTimer = setTimeout(() => reindex().catch(error => notify('window/logMessage', { type: 1, message: error.message })), delay);
}

function setCompilerDiagnostics(ownerUri, items, workspace) {
  const previousTargets = compilerDiagnosticTargets.get(ownerUri) || new Set();
  for (const targetUri of previousTargets) {
    const owners = compilerDiagnostics.get(targetUri);
    owners?.delete(ownerUri);
    if (owners && !owners.size) compilerDiagnostics.delete(targetUri);
  }
  const grouped = new Map();
  for (const item of items) {
    let targetUri = ownerUri;
    if (item.file) {
      const outputPath = path.resolve(item.file);
      const sourcePath = workspace.sourceOverrides.get(outputPath) || outputPath;
      targetUri = uriForPath(sourcePath);
    }
    if (!grouped.has(targetUri)) grouped.set(targetUri, []);
    const { file, ...diagnostic } = item;
    grouped.get(targetUri).push(diagnostic);
  }
  const targets = new Set([...previousTargets, ...grouped.keys()]);
  for (const [targetUri, diagnostics] of grouped) {
    if (!compilerDiagnostics.has(targetUri)) compilerDiagnostics.set(targetUri, new Map());
    compilerDiagnostics.get(targetUri).set(ownerUri, diagnostics);
  }
  compilerDiagnosticTargets.set(ownerUri, new Set(grouped.keys()));
  for (const targetUri of targets) publishDiagnostics(targetUri);
}

function removeCompilerDiagnostics(ownerUri) {
  const targets = compilerDiagnosticTargets.get(ownerUri) || new Set();
  for (const targetUri of targets) {
    const owners = compilerDiagnostics.get(targetUri);
    owners?.delete(ownerUri);
    if (owners && !owners.size) compilerDiagnostics.delete(targetUri);
    publishDiagnostics(targetUri);
  }
  compilerDiagnosticTargets.delete(ownerUri);
}

const checkGenerations = new Map();
async function checkDocument(uri, requestId) {
  const doc = documents.get(uri), file = pathForUri(uri);
  if (!doc || !file || !workspaces.length) return;
  const generation = (checkGenerations.get(uri) || 0) + 1;
  checkGenerations.set(uri, generation);
  const root = workspaces.find(folder => file === folder || file.startsWith(folder + path.sep)) || path.dirname(file);
  let workspace;
  try { workspace = await createCompilerInputs(root, [file], true); }
  catch (_) { return; }
  try {
    const result = await run(findCompiler(root), ['--check', workspace.inputs[0], ...compilerArgs('', '', [], [path.dirname(workspace.inputs[0])]).slice(2)], { cwd: root }, requestId, `check:${uri}`);
    if (checkGenerations.get(uri) !== generation) return;
    if (result.code === null) return;
    if (!documents.has(uri) || documents.get(uri).version !== doc.version) return;
    const items = parseCompilerDiagnostics(`${result.stderr}\n${result.stdout}`, doc.text.split(/\r?\n/).length).map(item => ({
      severity: item.severity,
      code: item.code || 'LOOM-COMPILER',
      message: item.message,
      file: item.file,
      range: { start: { line: item.line, character: item.column }, end: { line: item.line, character: item.column + item.length } }
    }));
    setCompilerDiagnostics(uri, items, workspace);
  } catch (error) {
    if (error.code !== -32800) notify('window/logMessage', { type: 1, message: `Loom compiler check failed: ${error.message}` });
  } finally { await fsp.rm(workspace.temp, { recursive: true, force: true }).catch(() => {}); }
}

function removeDocumentState(uri) {
  removeCompilerDiagnostics(uri);
  openVersions.delete(uri);
  checkGenerations.delete(uri);
  killActiveChild(`check:${uri}`);
}

function scheduleDocumentAnalysis(uri) {
  clearTimeout(analysisTimers.get(uri));
  const doc = documents.get(uri), version = doc?.version;
  if (lintRun === 'onSave') { scheduleIndex(500); return; }
  analysisTimers.set(uri, setTimeout(() => {
    analysisTimers.delete(uri);
    if (!doc || documents.get(uri)?.version !== version) return;
    checkDocument(uri).catch(error => notify('window/logMessage', { type: 1, message: error.message }));
    scheduleIndex();
  }, lintDelay));
}

function publishDiagnostics(uri) {
  const compilerItems = [...(compilerDiagnostics.get(uri)?.values() || [])].flat();
  notify('textDocument/publishDiagnostics', { uri, version: documents.get(uri)?.version, diagnostics: compilerItems });
  notify('loom/diagnosticCount', { uri, count: compilerItems.length });
}

function uriForSourceIndex(index) {
  const source = semantic.sources?.[index];
  return source ? uriForPath(source.path) : '';
}
function allSymbols() { return semantic.symbols || []; }
function symbolForId(id) { return semanticSymbols.get(id); }
function tokenAt(uri, position) {
  const doc = documents.get(uri);
  if (!doc) return null;
  const tokens = loom.tokenize(doc.text);
  return tokens.find(token => token.line === position.line && token.character <= position.character && position.character <= token.character + token.end - token.start) || null;
}
function offsetAt(text, position) {
  const rows = text.split(/\r?\n/);
  let offset = 0;
  for (let i = 0; i < Math.min(position.line, rows.length); i++) offset += rows[i].length + (text.includes('\r\n') ? 2 : 1);
  return offset + (rows[position.line] || '').slice(0, position.character).length;
}
function symbolAt(uri, position) {
  const token = tokenAt(uri, position);
  if (!token) return null;
  const sourceIndex = semantic.sources?.find(source => path.resolve(source.path) === pathForUri(uri))?.index;
  const sourceRefs = (semantic.expressions || []).filter(expr => expr.sourceIndex === sourceIndex && expr.kind === 'call' && expr.resolvedSymbol && expr.range?.start.line === token.line && expr.range?.end.line === token.line && token.character >= expr.range.start.character && token.character < expr.range.end.character);
  if (sourceRefs.length) return symbolForId(sourceRefs[0].resolvedSymbol);
  return allSymbols().find(symbol => symbol.name === token.value && semantic.sources?.[symbol.sourceIndex]?.index === sourceIndex) || null;
}

function lspSymbol(symbol) {
  const uri = uriForSourceIndex(symbol.sourceIndex);
  return { name: symbol.name, kind: symbol.kind === 'function' || symbol.kind === 'externFunction' ? 12 : symbol.kind === 'record' ? 23 : symbol.kind === 'enum' ? 10 : symbol.kind === 'field' ? 8 : symbol.kind === 'enumMember' ? 20 : 13,
    location: { uri, range: symbol.range }, containerName: symbol.scope !== 'module' ? symbol.scope : symbol.module };
}
function symbolsForDocument(uri) {
  const sourceIndex = semantic.sources?.find(source => path.resolve(source.path) === pathForUri(uri))?.index;
  return allSymbols().filter(symbol => symbol.sourceIndex === sourceIndex);
}

function currentBlockName(uri, position) {
  const sourceIndex = semantic.sources?.find(source => path.resolve(source.path) === pathForUri(uri))?.index;
  const functions = allSymbols().filter(symbol => symbol.sourceIndex === sourceIndex && (symbol.kind === 'function' || symbol.kind === 'externFunction') && symbol.containerRange && rangeContains(symbol.containerRange, position));
  return functions.sort((a, b) => (a.containerRange.end.line - a.containerRange.start.line) - (b.containerRange.end.line - b.containerRange.start.line))[0]?.name || '';
}

function completion(uri, position) {
  const doc = documents.get(uri); if (!doc) return [];
  const words = new Set([...loom.KEYWORDS, ...loom.BUILTIN_TYPES]);
  const block = currentBlockName(uri, position);
  const currentFileIndex = semantic.sources?.find(source => path.resolve(source.path) === pathForUri(uri))?.index;
  for (const symbol of allSymbols()) if ((symbol.sourceIndex === currentFileIndex && (symbol.scope === block || symbol.scope === 'module')) || symbol.kind === 'function' || symbol.kind === 'record' || symbol.kind === 'enum') words.add(symbol.name);
  return [...words].map(label => {
    const symbol = allSymbols().find(item => item.name === label);
    const keyword = loom.KEYWORDS.has(label), type = loom.BUILTIN_TYPES.has(label) || symbol?.kind === 'record' || symbol?.kind === 'enum';
    return { label, kind: keyword ? 14 : type ? 7 : symbol?.kind === 'function' ? 3 : 6, detail: symbol?.type?.name || (keyword ? 'Loom keyword' : type ? 'Loom type' : 'Loom symbol'), documentation: symbol ? `${symbol.name} : ${symbol.type?.name || symbol.kind}` : undefined };
  });
}

function hover(uri, position) {
  const token = tokenAt(uri, position); if (!token) return null;
  const symbol = symbolAt(uri, position);
  if (symbol) {
    const content = `**${symbol.name}** : \`${symbol.type?.name || symbol.kind}\`\n\n${symbol.kind} in ${symbol.module || 'this module'}.`;
    return { contents: { kind: 'markdown', value: content }, range: tokenRange(token) };
  }
  if (loom.KEYWORDS.has(token.value)) return { contents: { kind: 'markdown', value: `Loom keyword: \`${token.value}\`` }, range: tokenRange(token) };
  if (loom.BUILTIN_TYPES.has(token.value)) return { contents: { kind: 'markdown', value: `Built-in Loom type: \`${token.value}\`` }, range: tokenRange(token) };
  return null;
}
function tokenRange(token) { return { start: { line: token.line, character: token.character }, end: { line: token.line, character: token.character + token.end - token.start } }; }

function definition(uri, position) {
  const symbol = symbolAt(uri, position);
  if (!symbol) return null;
  return { uri: uriForSourceIndex(symbol.sourceIndex), range: symbol.range };
}
function references(uri, position, includeDeclaration) {
  const symbol = symbolAt(uri, position); if (!symbol) return [];
  const result = (semanticReferences.get(symbol.id) || []).map(ref => ({ uri: ref.uri, range: ref.range }));
  if (includeDeclaration) result.push({ uri: uriForSourceIndex(symbol.sourceIndex), range: symbol.range });
  return result;
}
function rename(uri, position, newName) {
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(newName)) throw Object.assign(new Error('Loom identifiers must start with a letter or underscore.'), { code: -32602 });
  const symbol = symbolAt(uri, position); if (!symbol) return null;
  const changes = {};
  const refs = references(uri, position, true);
  for (const ref of refs) (changes[ref.uri] ||= []).push({ range: ref.range, newText: newName });
  return { changes };
}

function documentSymbols(uri) {
  const symbols = symbolsForDocument(uri).filter(symbol => symbol.scope === 'module');
  return symbols.map(symbol => {
    const children = symbolsForDocument(uri).filter(child => child.scope === symbol.name && child.id !== symbol.id).map(child => ({ name: child.name, detail: child.type?.name || child.kind, kind: child.kind === 'field' ? 8 : child.kind === 'parameter' ? 13 : 13, range: child.range, selectionRange: child.range }));
    return { name: symbol.name, detail: symbol.type?.name || symbol.kind, kind: symbol.kind === 'function' || symbol.kind === 'externFunction' ? 12 : symbol.kind === 'record' ? 23 : symbol.kind === 'enum' ? 10 : 13,
      range: symbol.range, selectionRange: symbol.range, children };
  });
}
function signatureHelp(uri, position) {
  const doc = documents.get(uri); if (!doc) return null;
  const tokens = loom.tokenize(doc.text), offset = offsetAt(doc.text, position), before = tokens.filter(token => token.end <= offset);
  let depth = 0, open = -1;
  for (let i = before.length - 1; i >= 0; i--) { if (before[i].value === ')') depth++; else if (before[i].value === '(') { if (!depth) { open = i; break; } depth--; } }
  if (open < 1) return null;
  const name = before[open - 1].value, fn = allSymbols().find(symbol => symbol.name === name && (symbol.kind === 'function' || symbol.kind === 'externFunction'));
  if (!fn) return null;
  const params = allSymbols().filter(symbol => symbol.sourceIndex === fn.sourceIndex && symbol.scope === fn.name && symbol.kind === 'parameter');
  let activeParameter = 0, nested = 0;
  for (let i = open + 1; i < before.length; i++) { if (before[i].value === '(') nested++; else if (before[i].value === ')') nested--; else if (before[i].value === ',' && nested === 0) activeParameter++; }
  const info = { label: `${fn.name}(${params.map(param => `${param.name} : ${param.type.name}`).join(', ')})${fn.type?.element?.kind !== 'null' ? ` : ${fn.type.element.name}` : ''}`,
    parameters: params.map(param => ({ label: `${param.name} : ${param.type.name}` })) };
  return { signatures: [info], activeSignature: 0, activeParameter: Math.min(activeParameter, Math.max(0, params.length - 1)) };
}

function semanticTokens(uri) {
  const doc = documents.get(uri); if (!doc) return { data: [] };
  const tokens = loom.tokenize(doc.text), symbols = symbolsForDocument(uri), entries = [];
  const symbolRanges = new Map(symbols.map(symbol => [`${symbol.range.start.line}:${symbol.range.start.character}`, symbol]));
  for (let i = 0; i < tokens.length; i++) {
    const token = tokens[i];
    let type;
    if (token.kind === 'number') type = 'number';
    else if (token.kind === 'string' || token.kind === 'character') type = 'string';
    else if (token.kind === 'identifier') {
      const sym = symbolRanges.get(`${token.line}:${token.character}`);
      if (sym) type = sym.kind === 'function' || sym.kind === 'externFunction' ? 'function' : sym.kind === 'parameter' ? 'parameter' : sym.kind === 'field' || sym.kind === 'enumMember' ? 'property' : sym.kind === 'record' || sym.kind === 'enum' ? 'class' : 'variable';
      else if (loom.KEYWORDS.has(token.value)) type = 'keyword';
      else if (loom.BUILTIN_TYPES.has(token.value)) type = 'type';
      else if (tokens[i + 1]?.value === '(') type = 'function';
      // Leave unresolved identifiers to the TextMate grammar. Marking every
      // unknown word as a semantic variable overrides useful lexical scopes
      // (modules, incomplete expressions, and names typed before indexing).
    }
    if (type) entries.push({ line: token.line, start: token.character, length: token.end - token.start, type, modifier: symbolRanges.has(`${token.line}:${token.character}`) ? 1 : 0 });
  }
  for (const [lineNumber, line] of doc.text.split(/\r?\n/).entries()) {
    let quote = '';
    for (let i = 0; i < line.length; i++) {
      if (quote) { if (line[i] === '\\') i++; else if (line[i] === quote) quote = ''; }
      else if (line[i] === '"' || line[i] === "'") quote = line[i];
      else if (line[i] === '#' || (line[i] === '/' && line[i + 1] === '/')) { entries.push({ line: lineNumber, start: i, length: line.length - i, type: 'comment', modifier: 0 }); break; }
    }
  }
  entries.sort((a, b) => a.line - b.line || a.start - b.start);
  const data = []; let lastLine = 0, lastStart = 0;
  for (const entry of entries) {
    const type = TOKEN_TYPES.indexOf(entry.type); if (type < 0 || !entry.length) continue;
    const deltaLine = entry.line - lastLine, deltaStart = deltaLine ? entry.start : entry.start - lastStart;
    data.push(deltaLine, deltaStart, entry.length, type, entry.modifier);
    lastLine = entry.line; lastStart = entry.start;
  }
  return { data };
}

function highlights(uri, position) { return references(uri, position, true).map(ref => ({ range: ref.range, kind: 1 })); }
function codeActions(params) {
  return (params.context?.diagnostics || []).map(diagnostic => ({ title: 'Explain Loom finding', kind: 'quickfix', diagnostics: [diagnostic], command: { title: 'Explain Loom finding', command: 'loom.explainFinding', arguments: [diagnostic.message] } }));
}

function dispatch(message) {
  if (message.method === '$/cancelRequest') {
    const id = message.params?.id;
    cancelled.add(id);
    requestChildren.get(id)?.kill();
    return;
  }
  if (message.method && message.id === undefined) { handleNotification(message.method, message.params || {}); return; }
  if (message.id === undefined) return;
  Promise.resolve(handleRequest(message.method, message.params || {}, message.id)).then(result => {
    if (!cancelled.has(message.id)) response(message.id, result === undefined ? null : result);
    cancelled.delete(message.id);
  }).catch(error => {
    if (error.code === -32800 || cancelled.has(message.id)) errorResponse(message.id, -32800, 'Request cancelled');
    else errorResponse(message.id, error.code || -32603, error.message || 'Internal error');
    cancelled.delete(message.id);
  });
}

function handleNotification(method, params) {
  if (method === 'initialized') { scheduleIndex(0); return; }
  if (method === 'exit') { shuttingDown = true; process.exit(0); }
  if (method === 'textDocument/didOpen') {
    const item = params.textDocument; documents.set(item.uri, { uri: item.uri, path: pathForUri(item.uri), text: item.text, version: item.version || 1 });
    openVersions.set(item.uri, item.version || 1); scheduleDocumentAnalysis(item.uri); return;
  }
  if (method === 'textDocument/didChange') {
    const doc = documents.get(params.textDocument.uri); if (!doc) return;
    let text = doc.text;
    for (const change of params.contentChanges || []) {
      if (!change.range) text = change.text;
      else text = applyChange(text, change.range, change.text);
    }
    doc.text = text; doc.version = params.textDocument.version; openVersions.set(doc.uri, doc.version); scheduleDocumentAnalysis(doc.uri); return;
  }
  if (method === 'textDocument/didSave') {
    const uri = params.textDocument.uri;
    if (params.text !== undefined && documents.has(uri)) documents.get(uri).text = params.text;
    scheduleDocumentAnalysis(uri); return;
  }
  if (method === 'textDocument/didClose') {
    const uri = params.textDocument.uri; documents.delete(uri); removeDocumentState(uri);
    publishDiagnostics(uri); scheduleIndex(); return;
  }
  if (method === 'loom/checkDocument') {
    const uri = params.uri;
    if (documents.has(uri)) checkDocument(uri).catch(error => notify('window/logMessage', { type: 1, message: error.message }));
    return;
  }
  if (method === 'workspace/didChangeWorkspaceFolders') {
    const removed = (params.event?.removed || []).map(folder => pathForUri(folder.uri));
    const added = (params.event?.added || []).map(folder => pathForUri(folder.uri));
    workspaces = workspaces.filter(root => !removed.includes(root)).concat(added).filter(Boolean); scheduleIndex(0); return;
  }
  if (method === 'workspace/didChangeWatchedFiles') { scheduleIndex(200); return; }
  if (method === 'loom/refreshIndex') { scheduleIndex(0); return; }
}

function applyChange(text, range, inserted) {
  const start = offsetAt(text, range.start), end = offsetAt(text, range.end);
  return text.slice(0, start) + inserted + text.slice(end);
}

async function handleRequest(method, params, requestId) {
  if (method === 'initialize') {
    compilerPath = params.initializationOptions?.compilerPath || '';
    includePaths = params.initializationOptions?.includePaths || [];
    lintRun = params.initializationOptions?.lintRun === 'onSave' ? 'onSave' : 'onType';
    lintDelay = Math.max(100, Math.min(5000, Number(params.initializationOptions?.lintDelay) || 500));
    workspaces = (params.workspaceFolders || []).map(folder => pathForUri(folder.uri)).filter(Boolean);
    if (!workspaces.length && params.rootUri) { const root = pathForUri(params.rootUri); if (root) workspaces = [root]; }
    return { serverInfo: { name: 'Loom Language Server', version: '1.5.5' }, capabilities: {
      textDocumentSync: 1,
      hoverProvider: true, completionProvider: { triggerCharacters: ['.','[',':'] }, signatureHelpProvider: { triggerCharacters: ['(',','] },
      definitionProvider: true, referencesProvider: true, renameProvider: { prepareProvider: true }, documentSymbolProvider: true,
      workspaceSymbolProvider: true, semanticTokensProvider: { legend: { tokenTypes: TOKEN_TYPES, tokenModifiers: ['declaration'] }, full: true },
      codeActionProvider: { codeActionKinds: ['quickfix'] }, documentHighlightProvider: true,
      workspace: { workspaceFolders: { supported: true, changeNotifications: true } }
    } };
  }
  if (method === 'shutdown') { shuttingDown = true; return null; }
  if (method === 'textDocument/hover') return hover(params.textDocument.uri, params.position);
  if (method === 'textDocument/completion') return { isIncomplete: false, items: completion(params.textDocument.uri, params.position) };
  if (method === 'textDocument/signatureHelp') return signatureHelp(params.textDocument.uri, params.position);
  if (method === 'textDocument/definition') return definition(params.textDocument.uri, params.position);
  if (method === 'textDocument/references') return references(params.textDocument.uri, params.position, params.context?.includeDeclaration);
  if (method === 'textDocument/prepareRename') {
    const symbol = symbolAt(params.textDocument.uri, params.position);
    return symbol ? { range: symbol.range, placeholder: symbol.name } : null;
  }
  if (method === 'textDocument/rename') return rename(params.textDocument.uri, params.position, params.newName);
  if (method === 'textDocument/documentSymbol') return documentSymbols(params.textDocument.uri);
  if (method === 'workspace/symbol') return allSymbols().filter(symbol => symbol.name.toLowerCase().includes((params.query || '').toLowerCase())).map(lspSymbol);
  if (method === 'textDocument/semanticTokens/full') return semanticTokens(params.textDocument.uri);
  if (method === 'textDocument/codeAction') return codeActions(params);
  if (method === 'textDocument/documentHighlight') return highlights(params.textDocument.uri, params.position);
  if (method === 'workspace/executeCommand') {
    if (params.command === 'loom.refreshIndex') { scheduleIndex(0); return null; }
    return null;
  }
  if (method === 'loom/refreshIndex') { await reindex(requestId); return null; }
  if (method === 'loom/status') return { server: 'ready', workspaceFiles: semantic.sources.length, symbols: semantic.symbols.length };
  throw Object.assign(new Error(`Method not found: ${method}`), { code: -32601 });
}

// Keep the standard stream unpolluted. Internal errors go to the LSP log channel.
process.on('uncaughtException', error => notify('window/logMessage', { type: 1, message: `Loom server error: ${error.message}` }));
process.on('unhandledRejection', error => notify('window/logMessage', { type: 1, message: `Loom server error: ${error?.message || error}` }));

module.exports = { parseCompilerDiagnostics, semanticTokens, uriForPath, pathForUri };
