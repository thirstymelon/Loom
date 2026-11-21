const assert = require('node:assert/strict');
const path = require('node:path');
const { LoomLanguageClient } = require('../lsp-client');

(async () => {
  const client = new LoomLanguageClient(process.execPath, path.resolve(__dirname, '../server.js'));
  await client.start(null);
  assert.equal(client.state, 'running');
  const status = await client.request('loom/status', {});
  assert.equal(status.server, 'ready');
  let cancel;
  const token = { onCancellationRequested(listener) { cancel = listener; return { dispose() {} }; } };
  const request = client.request('workspace/symbol', { query: 'CancelMe' }, token);
  cancel();
  await assert.rejects(request, /cancelled/i);
  await client.stop();
  assert.equal(client.state, 'stopped');
  assert.equal(client.child, null);
  console.log('Loom LSP client transport tests passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
