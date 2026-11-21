'use strict';
const cp = require('child_process');
const EventEmitter = require('events');

class LoomLanguageClient extends EventEmitter {
  constructor(node, server) { super(); this.node = node; this.server = server; this.child = null; this.seq = 0; this.pending = new Map(); this.buffer = Buffer.alloc(0); this.state = 'stopped'; }
  async start(root, initializationOptions = {}) {
    await this.stop(); this.root = root;
    this.child = cp.spawn(this.node, [this.server], { stdio: ['pipe', 'pipe', 'pipe'] });
    const child = this.child;
    child.stdout.on('data', b => { if (this.child === child) this._read(b); }); child.stderr.on('data', b => this.emit('log', b.toString()));
    child.on('error', e => { if (this.child !== child) return; this.child = null; this.state = 'stopped'; this.emit('state', this.state); for (const p of this.pending.values()) { p.cancellation?.dispose(); p.reject(e); } this.pending.clear(); this.emit('error', e); });
    child.on('exit', c => { if (this.child !== child) return; this.child = null; this.state = 'stopped'; this.emit('state', this.state, c); for (const p of this.pending.values()) { p.cancellation?.dispose(); p.reject(new Error('Language server exited')); } this.pending.clear(); });
    this.state = 'starting'; this.emit('state', this.state);
    const toUri = value => require('url').pathToFileURL(value).toString();
    // Multi-root workspaces must send every folder at initialize; sending only
    // the first one leaves the extra roots outside the initial index.
    const requestedFolders = Array.isArray(initializationOptions.workspaceFolders) ? initializationOptions.workspaceFolders.filter(folder => folder && folder.uri) : [];
    const workspaceFolders = requestedFolders.length ? requestedFolders : root ? [{ uri: toUri(root), name: 'Loom' }] : null;
    return this.request('initialize', { processId: process.pid, rootUri: root ? toUri(root) : null, initializationOptions, capabilities: { general: { positionEncodings: ['utf-16'] } }, workspaceFolders }).then(result => { this.notify('initialized', {}); this.state = 'running'; this.emit('state', this.state); return result; });
  }
  _read(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (true) { const sep = this.buffer.indexOf('\r\n\r\n'); if (sep < 0) return; const head = this.buffer.subarray(0, sep).toString(); const m = /Content-Length:\s*(\d+)/i.exec(head); if (!m) { this.buffer = this.buffer.subarray(sep + 4); continue; } const n = Number(m[1]), end = sep + 4 + n; if (this.buffer.length < end) return; let msg; try { msg = JSON.parse(this.buffer.subarray(sep + 4, end).toString()); } catch (_) {} this.buffer = this.buffer.subarray(end); if (!msg) continue; if (msg.id !== undefined) { const p = this.pending.get(msg.id); if (p) { this.pending.delete(msg.id); p.cancellation?.dispose(); msg.error ? p.reject(new Error(msg.error.message)) : p.resolve(msg.result); } } else this.emit('notification', msg.method, msg.params); }
  }
  _send(msg) { if (!this.child || !this.child.stdin.writable) return; const body = Buffer.from(JSON.stringify(msg)); this.child.stdin.write(Buffer.concat([Buffer.from(`Content-Length: ${body.length}\r\n\r\n`), body])); }
  request(method, params, token) { const id = ++this.seq; return new Promise((resolve, reject) => { const pending = { resolve, reject, cancellation: null }; this.pending.set(id, pending); this._send({ jsonrpc: '2.0', id, method, params }); if (token) pending.cancellation = token.onCancellationRequested(() => { if (!this.pending.has(id)) return; this.notify('$/cancelRequest', { id }); this.pending.delete(id); reject(new Error('Request cancelled')); }); }); }
  notify(method, params) { this._send({ jsonrpc: '2.0', method, params }); }
  async stop() {
    const child = this.child;
    if (!child) return;
    for (const pending of this.pending.values()) { pending.cancellation?.dispose(); pending.reject(new Error('Language server stopped')); }
    this.pending.clear();
    this.state = 'stopping'; this.emit('state', this.state);
    const shutdownId = ++this.seq;
    const body = Buffer.from(JSON.stringify({ jsonrpc: '2.0', id: shutdownId, method: 'shutdown', params: {} }));
    try { child.stdin.write(Buffer.concat([Buffer.from(`Content-Length: ${body.length}\r\n\r\n`), body])); } catch (_) {}
    let timer;
    await Promise.race([
      new Promise(resolve => { this.pending.set(shutdownId, { resolve, reject: resolve, cancellation: null }); }),
      new Promise(resolve => { timer = setTimeout(resolve, 800); timer.unref?.(); })
    ]);
    clearTimeout(timer);
    this.pending.delete(shutdownId);
    const exit = Buffer.from(JSON.stringify({ jsonrpc: '2.0', method: 'exit', params: {} }));
    try { child.stdin.write(Buffer.concat([Buffer.from(`Content-Length: ${exit.length}\r\n\r\n`), exit])); } catch (_) {}
    this.child = null; this.state = 'stopped'; this.emit('state', this.state);
    setTimeout(() => { if (!child.killed) child.kill(); }, 250).unref?.();
  }
}
module.exports = { LoomLanguageClient };
