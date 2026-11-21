const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const path = require('node:path');
const fs = require('node:fs');
const os = require('node:os');

const server = path.resolve(__dirname, '..', 'server.js');
const child = spawn(process.execPath, [server], { stdio: ['pipe', 'pipe', 'pipe'] });
let buffer = Buffer.alloc(0);
const pending = new Map();
let nextId = 1;
const notifications = [];
const notificationWaiters = [];
function send(method, params) {
  const id = nextId++;
  const body = Buffer.from(JSON.stringify({ jsonrpc: '2.0', id, method, params }));
  child.stdin.write(Buffer.concat([Buffer.from(`Content-Length: ${body.length}\r\n\r\n`), body]));
  return new Promise((resolve, reject) => { pending.set(id, { resolve, reject }); setTimeout(() => { if (pending.has(id)) { pending.delete(id); reject(new Error(`Timed out waiting for ${method}`)); } }, 3000).unref(); });
}
function notify(method, params) {
  const body = Buffer.from(JSON.stringify({ jsonrpc: '2.0', method, params }));
  child.stdin.write(Buffer.concat([Buffer.from(`Content-Length: ${body.length}\r\n\r\n`), body]));
}
child.stdout.on('data', chunk => {
  buffer = Buffer.concat([buffer, chunk]);
  while (true) {
    const sep = buffer.indexOf('\r\n\r\n'); if (sep < 0) return;
    const match = /Content-Length:\s*(\d+)/i.exec(buffer.subarray(0, sep).toString()); if (!match) throw new Error('Invalid LSP frame');
    const end = sep + 4 + Number(match[1]); if (buffer.length < end) return;
    const msg = JSON.parse(buffer.subarray(sep + 4, end).toString()); buffer = buffer.subarray(end);
    const task = pending.get(msg.id); if (task) { pending.delete(msg.id); msg.error ? task.reject(new Error(msg.error.message)) : task.resolve(msg.result); }
    else if (msg.method) {
      let consumed = false;
      for (let i = notificationWaiters.length - 1; i >= 0; i--) if (notificationWaiters[i].test(msg)) { const waiter = notificationWaiters.splice(i, 1)[0]; waiter.resolve(msg.params); consumed = true; }
      if (!consumed) notifications.push(msg);
    }
  }
});
function waitNotification(test, timeoutMs = 10000) {
  const foundIndex = notifications.findIndex(message => test(message)); if (foundIndex >= 0) return Promise.resolve(notifications.splice(foundIndex, 1)[0].params);
  return new Promise((resolve, reject) => { notificationWaiters.push({ test, resolve }); setTimeout(() => reject(new Error(`Timed out waiting for LSP notification; received ${JSON.stringify(notifications.map(message => ({ method: message.method, params: message.params })))}`)), timeoutMs).unref(); });
}
(async () => {
  const workspace = fs.mkdtempSync(path.join(os.tmpdir(), 'loom-lsp-workspace-'));
  fs.writeFileSync(path.join(workspace, 'Math.loom'), 'Module Math\nBlock Add Takes\n A : Signed_32\n B : Signed_32\nProduces Signed_32\nImplements\n Give A + B\nEnd Add\n');
  fs.writeFileSync(path.join(workspace, 'second.loom'), 'Module Main\nImport Math\nBlock OnlySecond Declares\n X : Signed_32\n P : Signed_32 Pointer\nImplements\n X = Math.Add(1, 2)\nEnd OnlySecond\n');
  const secondWorkspace = fs.mkdtempSync(path.join(os.tmpdir(), 'loom-lsp-workspace-b-'));
  fs.writeFileSync(path.join(secondWorkspace, 'Extra.loom'), 'Module Extra\nBlock MultiOnly Declares\n Y : Signed_32\nImplements\n Y = 1\nEnd MultiOnly\n');
  const loomRoot = path.resolve(__dirname, '../..');
  const compiler = [process.env.LOOMC_PATH, path.join(loomRoot, '.build/loomc'), path.join(loomRoot, 'bin/loomc')].find(file => file && fs.existsSync(file));
  const uri = require('node:url').pathToFileURL(workspace).toString();
  const secondUri = require('node:url').pathToFileURL(secondWorkspace).toString();
  const initialized = await send('initialize', { processId: process.pid, rootUri: uri, workspaceFolders: [{ uri, name: 'test' }, { uri: secondUri, name: 'extra' }], initializationOptions: { compilerPath: compiler || '' }, capabilities: {} });
  for (const capability of ['hoverProvider','completionProvider','definitionProvider','referencesProvider','renameProvider','documentSymbolProvider','workspaceSymbolProvider','semanticTokensProvider','codeActionProvider','documentHighlightProvider']) assert.ok(initialized.capabilities[capability], `missing ${capability}`);
  const indexed = waitNotification(item => item.method === 'loom/indexStatus' && item.params.status === 'ready');
  notify('initialized', {});
  const unindexedOpenFile = path.join(workspace, 'second.loom');
  const indexedUri = require('node:url').pathToFileURL(unindexedOpenFile).toString();
  if (compiler) await indexed;
  const workspaceSymbols = compiler ? await send('workspace/symbol', { query: 'OnlySecond' }) : [];
  if (compiler) assert.ok(workspaceSymbols.some(symbol => symbol.name === 'OnlySecond' && symbol.location.uri === indexedUri), 'workspace symbols must include unopened files');
  if (compiler) {
    const multiSymbols = await send('workspace/symbol', { query: 'MultiOnly' });
    const multiUri = require('node:url').pathToFileURL(path.join(secondWorkspace, 'Extra.loom')).toString();
    assert.ok(multiSymbols.some(symbol => symbol.name === 'MultiOnly' && symbol.location.uri === multiUri), 'every workspace folder must be indexed');
  }
  const dirtyText = 'Module Main\nImport Math\nBlock UnsavedOnly Declares\n X : Signed_32\n P : Signed_32 Pointer\nImplements\n X = Math.Add(1, 2)\n P = Address(X)\n Loop while X < 3\n  X += 1\n End Loop\nEnd UnsavedOnly\n';
  if (compiler) {
    const changedIndex = waitNotification(item => item.method === 'loom/indexStatus' && item.params.status === 'ready');
    notify('textDocument/didOpen', { textDocument: { uri: indexedUri, languageId: 'loom', version: 1, text: dirtyText } });
    await changedIndex;
    const dirtySymbols = await send('workspace/symbol', { query: 'UnsavedOnly' });
    assert.ok(dirtySymbols.some(symbol => symbol.name === 'UnsavedOnly' && symbol.location.uri === indexedUri), `unsaved buffer content must override disk during indexing; found ${JSON.stringify(dirtySymbols)}; states ${JSON.stringify(notifications.filter(n => n.method === 'loom/indexStatus').map(n => n.params))}`);
    assert.equal((await send('workspace/symbol', { query: 'OnlySecond' })).length, 0, 'stale saved declaration must disappear from the semantic index');
    const textDocument = { uri: indexedUri };
    assert.ok((await send('textDocument/completion', { textDocument, position: { line: 6, character: 9 } })).items.some(item => item.label === 'Add'));
    const hoverResult = await send('textDocument/hover', { textDocument, position: { line: 6, character: 11 } });
    assert.ok(hoverResult, `hover should resolve the imported call: ${JSON.stringify((await send('workspace/symbol', { query: 'Add' })))}`);
    assert.match(hoverResult.contents.value, /Add/);
    assert.match((await send('textDocument/signatureHelp', { textDocument, position: { line: 6, character: 14 } })).signatures[0].label, /Add\(/);
    assert.ok((await send('textDocument/documentSymbol', { textDocument })).some(symbol => symbol.name === 'UnsavedOnly'));
    const semanticTokens = (await send('textDocument/semanticTokens/full', { textDocument })).data;
    assert.ok(semanticTokens.length > 0);
    const decodedTokens = [];
    let semanticLine = 0, semanticCharacter = 0;
    for (let i = 0; i < semanticTokens.length; i += 5) {
      semanticLine += semanticTokens[i];
      semanticCharacter = semanticTokens[i] ? semanticTokens[i + 1] : semanticCharacter + semanticTokens[i + 1];
      decodedTokens.push({ line: semanticLine, start: semanticCharacter, length: semanticTokens[i + 2], type: semanticTokens[i + 3] });
    }
    assert.ok(decodedTokens.some(token => token.type === 7), 'keywords receive explicit semantic coloring');
    assert.ok(decodedTokens.some(token => token.type === 1), 'primitive types receive explicit semantic coloring');
    assert.ok(!decodedTokens.some(token => token.line === 1 && token.start <= 7 && 7 < token.start + token.length), 'unresolved module names remain under TextMate lexical highlighting');
    assert.equal((await send('textDocument/documentHighlight', { textDocument, position: { line: 6, character: 11 } })).length, 2);
    assert.equal((await send('textDocument/codeAction', { textDocument, range: { start: { line: 6, character: 11 }, end: { line: 6, character: 14 } }, context: { diagnostics: [{ message: 'Example finding', range: { start: { line: 6, character: 11 }, end: { line: 6, character: 14 } } }] } })).length, 1);
    const definition = await send('textDocument/definition', { textDocument: { uri: indexedUri }, position: { line: 6, character: 11 } });
    assert.equal(definition.uri, require('node:url').pathToFileURL(path.join(workspace, 'Math.loom')).toString(), 'definition must navigate into an unopened file');
    const references = await send('textDocument/references', { textDocument: { uri: indexedUri }, position: { line: 6, character: 11 }, context: { includeDeclaration: true } });
    assert.equal(references.length, 2, 'references must include the declaration and cross-file call');
    const edit = await send('textDocument/rename', { textDocument: { uri: indexedUri }, position: { line: 6, character: 11 }, newName: 'Renamed' });
    assert.ok(edit.changes[require('node:url').pathToFileURL(path.join(workspace, 'Math.loom')).toString()]?.some(change => change.newText === 'Renamed'));
    assert.ok(edit.changes[indexedUri]?.some(change => change.newText === 'Renamed'));
    const createdFile = path.join(workspace, 'created.loom');
    const createdUri = require('node:url').pathToFileURL(createdFile).toString();
    fs.writeFileSync(createdFile, 'Module Created\nBlock CreatedAfterIndex Implements\nEnd CreatedAfterIndex\n');
    const createdIndex = waitNotification(item => item.method === 'loom/indexStatus' && item.params.status === 'ready');
    notify('workspace/didChangeWatchedFiles', { changes: [{ uri: createdUri, type: 1 }] });
    await createdIndex;
    assert.ok((await send('workspace/symbol', { query: 'CreatedAfterIndex' })).some(symbol => symbol.name === 'CreatedAfterIndex' && symbol.location.uri === createdUri), 'file creation must invalidate and refresh the workspace index');
    const brokenFile = path.join(workspace, 'broken.loom');
    const brokenUri = require('node:url').pathToFileURL(brokenFile).toString();
    fs.writeFileSync(brokenFile, 'Module Broken\nBlock ???\n');
    const partialIndex = waitNotification(item => item.method === 'loom/indexStatus' && item.params.status === 'ready');
    notify('workspace/didChangeWatchedFiles', { changes: [{ uri: brokenUri, type: 1 }] });
    await partialIndex;
    assert.ok((await send('workspace/symbol', { query: 'CreatedAfterIndex' })).some(symbol => symbol.name === 'CreatedAfterIndex'), 'a malformed file must not discard semantic symbols from unrelated sources');
    const brokenDiagnostics = waitNotification(item => item.method === 'textDocument/publishDiagnostics' && item.params.uri === brokenUri && item.params.diagnostics.length > 0);
    notify('textDocument/didOpen', { textDocument: { uri: brokenUri, languageId: 'loom', version: 1, text: 'Module Broken\nBlock ???\n' } });
    assert.ok((await brokenDiagnostics).diagnostics[0].code, 'compiler diagnostics need stable codes');
    const clearedDiagnostics = waitNotification(item => item.method === 'textDocument/publishDiagnostics' && item.params.uri === brokenUri && item.params.diagnostics.length === 0);
    notify('textDocument/didChange', { textDocument: { uri: brokenUri, version: 2 }, contentChanges: [{ text: 'Module Broken\nBlock Repaired Implements\nEnd Repaired\n' }] });
    await clearedDiagnostics;
    const repairedIndex = waitNotification(item => item.method === 'loom/indexStatus' && item.params.status === 'ready');
    await repairedIndex;
    assert.ok((await send('workspace/symbol', { query: 'Repaired' })).some(symbol => symbol.name === 'Repaired'), 'a repaired document must reenter the workspace index');
    fs.rmSync(createdFile);
    const deletedIndex = waitNotification(item => item.method === 'loom/indexStatus' && item.params.status === 'ready');
    notify('workspace/didChangeWatchedFiles', { changes: [{ uri: createdUri, type: 3 }] });
    await deletedIndex;
    assert.equal((await send('workspace/symbol', { query: 'CreatedAfterIndex' })).length, 0, 'file deletion must remove stale symbols from the workspace index');
  }
  await send('shutdown', {});
  const body = Buffer.from(JSON.stringify({ jsonrpc: '2.0', method: 'exit', params: {} }));
  child.stdin.end(Buffer.concat([Buffer.from(`Content-Length: ${body.length}\r\n\r\n`), body]));
  await new Promise((resolve, reject) => { child.on('exit', code => code === 0 ? resolve() : reject(new Error(`server exit ${code}`))); setTimeout(() => { child.kill(); reject(new Error('server did not stop')); }, 3000).unref(); });
  fs.rmSync(workspace, { recursive: true, force: true });
  console.log('Loom LSP lifecycle tests passed');
})().catch(error => { child.kill(); console.error(error); process.exitCode = 1; });
