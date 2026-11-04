const vscode = require('vscode');
const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function parseCompilerDiagnostics(text, lineCount) {
  const found = [];
  const seen = new Set();
  for (const raw of text.split(/\r?\n/)) {
    const diagnosticText = raw.trim();
    if (!diagnosticText || /^LLVM (?:function|module) verification/i.test(diagnosticText) || /^usage:/i.test(diagnosticText)) continue;
    let modern = /^\s*(error|warning|note)\[([A-Z]\d{4})\]:\s*(.*)$/i.exec(raw);
    if (modern) {
      const level = modern[1].toLowerCase();
      const item = {
        line: 0, column: 0, length: 1, code: modern[2], message: modern[3],
        severity: level === 'warning' ? vscode.DiagnosticSeverity.Warning : level === 'note' ? vscode.DiagnosticSeverity.Information : vscode.DiagnosticSeverity.Error
      };
      found.push(item);
      continue;
    }
    let sourceLocation = /^\s*-->\s+(.*?)(?::(\d+):(\d+))?\s*$/.exec(raw);
    if (sourceLocation) {
      if (found.length) {
        const item = found[found.length - 1];
        item.file = sourceLocation[1];
        if (sourceLocation[2]) item.line = Math.max(0, Number(sourceLocation[2]) - 1);
        if (sourceLocation[3]) item.column = Math.max(0, Number(sourceLocation[3]) - 1);
      }
      continue;
    }
    const caret = /^\s*\|\s*(\^~*)/.exec(raw);
    if (caret && found.length) { found[found.length - 1].length = caret[1].length; continue; }
    if (/^\s*(?:\d+\s*\||\|)\s*$/.test(raw) || /^\s*\d+\s*\|/.test(raw)) continue;
    let line = 0, column = 0, severity = vscode.DiagnosticSeverity.Error, message = diagnosticText;
    let match = /(?:^|\b)line\s+(\d+)(?::(\d+))?:\s*(?:(error|warning|note):\s*)?(.*)$/i.exec(diagnosticText);
    if (match) {
      line = Math.max(0, Number(match[1]) - 1);
      column = match[2] ? Math.max(0, Number(match[2]) - 1) : 0;
      const level = (match[3] || 'error').toLowerCase();
      message = match[4] || diagnosticText;
      severity = level === 'warning' ? vscode.DiagnosticSeverity.Warning : level === 'note' ? vscode.DiagnosticSeverity.Information : vscode.DiagnosticSeverity.Error;
    } else {
      match = /:(\d+):(\d+):\s*(error|warning|note):\s*(.*)$/i.exec(diagnosticText);
      if (match) {
        line = Math.max(0, Number(match[1]) - 1);
        column = Math.max(0, Number(match[2]) - 1);
        const level = match[3].toLowerCase();
        message = match[4];
        severity = level === 'warning' ? vscode.DiagnosticSeverity.Warning : level === 'note' ? vscode.DiagnosticSeverity.Information : vscode.DiagnosticSeverity.Error;
      } else {
        const generic = /(?:loomc:\s*)?(warning|error|note):\s*(.*)$/i.exec(diagnosticText);
        if (generic) {
          const level = generic[1].toLowerCase();
          message = generic[2];
          severity = level === 'warning' ? vscode.DiagnosticSeverity.Warning : level === 'note' ? vscode.DiagnosticSeverity.Information : vscode.DiagnosticSeverity.Error;
        }
      }
    }
    line = Math.min(line, Math.max(0, lineCount - 1));
    const key = `${line}:${column}:${severity}:${message}`;
    if (seen.has(key)) continue;
    seen.add(key);
    found.push({ line, column, message, severity, length: 1 });
  }
  return found;
}

function activate(context) {
  const diagnostics = vscode.languages.createDiagnosticCollection('loom');
  const output = vscode.window.createOutputChannel('Loom');
  const timers = new Map();
  const processes = new Map();
  const status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 20);
  status.text = '$(check) Loom';
  status.tooltip = 'Loom language support';
  context.subscriptions.push(diagnostics, output, status);
  status.show();

  function compilerPath() {
    const configured = vscode.workspace.getConfiguration('loom').get('compilerPath', '').trim();
    if (configured) return configured;
    const candidates = ['/usr/local/bin/loomc', '/usr/bin/loomc'];
    for (const folder of vscode.workspace.workspaceFolders || []) {
      let current = folder.uri.fsPath;
      for (let depth = 0; depth < 6; depth++) {
        candidates.push(path.join(current, 'bin', 'loomc'));
        candidates.push(path.join(current, 'loomc'));
        const parent = path.dirname(current);
        if (parent === current) break;
        current = parent;
      }
    }
    if (context.extensionPath) candidates.push(path.resolve(context.extensionPath, '..', 'bin', 'loomc'));
    for (const candidate of candidates) {
      try { if (fs.statSync(candidate).isFile()) return candidate; } catch (_) {}
    }
    return 'loomc';
  }

  function findSdkPaths() {
    const candidates = [];
    if (process.env.LOOM_SDK_PATH) candidates.push(process.env.LOOM_SDK_PATH);
    if (process.env.LOOM_SDK) candidates.push(process.env.LOOM_SDK);
    if (process.env.HOME) candidates.push(path.join(process.env.HOME, '.loom', 'sdk'));
    candidates.push('/usr/local/share/loom/sdk');
    candidates.push('/opt/loom/sdk');
    for (const folder of vscode.workspace.workspaceFolders || []) {
      candidates.push(path.join(folder.uri.fsPath, 'sdk'));
    }
    if (context.extensionPath) {
      candidates.push(path.resolve(context.extensionPath, '..', 'sdk'));
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
    const key = document.uri.toString();
    const version = document.version;
    clearTimeout(timers.get(key));
    timers.delete(key);
    processes.get(key)?.kill();
    let tmp;
    let proc;
    const cleanup = () => { if (tmp) fs.promises.rm(tmp, { recursive: true, force: true }).catch(() => {}); };
    try {
      tmp = await fs.promises.mkdtemp(path.join(os.tmpdir(), 'loom-vscode-'));
      const input = path.join(tmp, path.basename(document.fileName || 'untitled.loom'));
      const result = path.join(tmp, 'output.ll');
      await fs.promises.writeFile(input, document.getText(), 'utf8');

      // Sync other unsaved open Loom documents in workspace into tmp
      for (const openDoc of vscode.workspace.textDocuments || []) {
        if (openDoc !== document && openDoc.languageId === 'loom' && openDoc.isDirty && openDoc.fileName) {
          const dirtyCopy = path.join(tmp, path.basename(openDoc.fileName));
          fs.promises.writeFile(dirtyCopy, openDoc.getText(), 'utf8').catch(() => {});
        }
      }

      if (document.version !== version) { cleanup(); schedule(document); return; }
      const executable = compilerPath();
      const docDir = document.fileName ? path.dirname(document.fileName) : undefined;
      const root = findWorkspaceRoot(document);
      const cwd = root || docDir || (vscode.workspace.workspaceFolders?.[0]?.uri?.fsPath) || process.cwd();

      const args = ['--check', input, result];
      if (docDir) args.push('-I', docDir);
      if (root) {
        args.push('-I', path.join(root, 'src'));
        args.push('-I', root);
        const bspSrc = path.join(root, 'src', 'bsp');
        if (fs.existsSync(bspSrc)) args.push('-I', bspSrc);
        const bspRoot = path.join(root, 'bsp');
        if (fs.existsSync(bspRoot)) args.push('-I', bspRoot);
      }
      for (const folder of vscode.workspace.workspaceFolders || []) {
        const fPath = folder.uri.fsPath;
        args.push('-I', path.join(fPath, 'src'));
        args.push('-I', fPath);
        const bspSrc = path.join(fPath, 'src', 'bsp');
        if (fs.existsSync(bspSrc)) args.push('-I', bspSrc);
        const bspRoot = path.join(fPath, 'bsp');
        if (fs.existsSync(bspRoot)) args.push('-I', bspRoot);
      }
      for (const sdkPath of findSdkPaths()) {
        args.push('-I', sdkPath);
      }

      proc = cp.spawn(executable, args, { cwd, windowsHide: true });
      processes.set(key, proc);
      let stderr = '', stdout = '';
      proc.stderr.setEncoding('utf8').on('data', s => stderr += s);
      proc.stdout.setEncoding('utf8').on('data', s => stdout += s);
      proc.on('error', err => {
        if (processes.get(key) !== proc) return;
        processes.delete(key);
        const message = `Cannot start loomc at '${executable}': ${err.message}. Set loom.compilerPath or install the compiler at /usr/bin/loomc.`;
        const diagnostic = new vscode.Diagnostic(new vscode.Range(0, 0, 0, 0), message, vscode.DiagnosticSeverity.Error);
        diagnostic.source = 'Loom';
        diagnostics.set(document.uri, [diagnostic]);
        output.appendLine(message);
        cleanup();
      });
      proc.on('close', code => {
        cleanup();
        if (processes.get(key) !== proc) return;
        processes.delete(key);
        if (code === null) return;
        if (document.version !== version) { schedule(document); return; }
        const text = `${stderr}\n${stdout}`;
        if (code === 0 && !/\b(?:error|warning):/i.test(text)) { diagnostics.set(document.uri, []); return; }
        const found = parseCompilerDiagnostics(text, document.lineCount).map(item => {
          const line = Math.min(item.line, Math.max(0, document.lineCount - 1));
          const lineText = document.lineAt(line);
          const start = Math.min(item.column, lineText.text.length);
          const end = lineText.isEmpty ? start : Math.min(lineText.text.length, start + (item.length || lineText.text.length - start));
          const diagnostic = new vscode.Diagnostic(new vscode.Range(line, start, line, Math.max(start, end)), item.message, item.severity);
          if (item.code) diagnostic.code = item.code;
          diagnostic.source = 'loomc';
          return diagnostic;
        });
        if (!found.length) found.push(new vscode.Diagnostic(new vscode.Range(0, 0, 0, 0), text.trim() || 'Loom compilation failed without a diagnostic.', vscode.DiagnosticSeverity.Error));
        diagnostics.set(document.uri, found);
      });
    } catch (err) {
      cleanup();
      output.appendLine(`Loom check failed: ${err.message}`);
    }
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
    const selectedCompiler = compilerPath();
    const candidates = ['/usr/bin/loom', path.join(path.dirname(selectedCompiler), 'loom'), path.join(root, 'bin', 'loom'), path.join(root, 'loom')];
    let current = root;
    for (let depth = 0; depth < 6; depth++) {
      candidates.push(path.join(current, 'bin', 'loom'));
      candidates.push(path.join(current, 'loom'));
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

  async function runWorkspaceCommand(command) {
    const root = findWorkspaceRoot(vscode.window.activeTextEditor?.document);
    if (!root) {
      vscode.window.showErrorMessage('Loom: open a workspace containing config.toml to build, clean, or run.');
      return;
    }
    const executable = loomToolPath(root);
    const config = path.join(root, 'config.toml');
    output.clear(); output.show(true);
    output.appendLine(`> ${executable} ${command} --config ${config}`);
    status.text = `$(sync~spin) Loom ${command}`;
    return new Promise(resolve => {
      const proc = cp.spawn(executable, [command, '--config', config], { cwd: root, windowsHide: true });
      let stderr = '';
      proc.stdout.setEncoding('utf8').on('data', chunk => output.append(chunk));
      proc.stderr.setEncoding('utf8').on('data', chunk => { stderr += chunk; output.append(chunk); });
      proc.on('error', err => {
        status.text = '$(error) Loom';
        output.appendLine(`Could not run '${executable}': ${err.message}. Set loom.toolPath.`);
        vscode.window.showErrorMessage(`Loom ${command} failed to start. See the Loom output channel.`);
        resolve();
      });
      proc.on('close', code => {
        status.text = code === 0 ? '$(check) Loom' : '$(error) Loom';
        output.appendLine(code === 0 ? `Loom ${command} completed.` : `Loom ${command} exited with code ${code}.`);
        if (code === 0) vscode.window.showInformationMessage(`Loom ${command} completed.`);
        else vscode.window.showErrorMessage(`Loom ${command} failed. See the Loom output channel.`);
        resolve();
      });
    });
  }

  async function openDependencyGraph() {
    const root = findWorkspaceRoot(vscode.window.activeTextEditor?.document);
    if (!root) { vscode.window.showErrorMessage('Loom: open a workspace containing config.toml to inspect dependencies.'); return; }
    const view = await vscode.window.showQuickPick(['calls', 'data'], { placeHolder: 'Choose a semantic dependency view' });
    if (!view) return;
    const executable = loomToolPath(root);
    output.clear(); output.show(true);
    const graphPath = path.join(root, 'loom-graph.json');
    const model = await new Promise((resolve, reject) => {
      const proc = cp.spawn(executable, ['graph', '--view', view, '--format', 'json'], { cwd: root, windowsHide: true });
      let stderr = '';
      proc.stdout.setEncoding('utf8').on('data', chunk => output.append(chunk));
      proc.stderr.setEncoding('utf8').on('data', chunk => { stderr += chunk; output.append(chunk); });
      proc.on('error', reject);
      proc.on('close', code => {
        if (code !== 0) reject(new Error(stderr || `loom graph exited with status ${code}`));
        else fs.promises.readFile(graphPath, 'utf8').then(text => resolve(JSON.parse(text)), reject);
      });
    }).catch(error => { vscode.window.showErrorMessage(`Loom graph failed: ${error.message}`); return undefined; });
    if (!model) return;
    const panel = vscode.window.createWebviewPanel('loomDependencyGraph', `Loom ${view} graph`, vscode.ViewColumn.Beside, { enableScripts: true, retainContextWhenHidden: true });
    panel.webview.html = dependencyGraphHtml(panel.webview, model, view);
    panel.webview.onDidReceiveMessage(async message => {
      if (message?.type !== 'view' || !['calls', 'data'].includes(message.view)) return;
      const next = await new Promise((resolve, reject) => {
        const proc = cp.spawn(executable, ['graph', '--view', message.view, '--format', 'json'], { cwd: root, windowsHide: true });
        let stderr = '';
        proc.stderr.setEncoding('utf8').on('data', chunk => stderr += chunk);
        proc.on('error', reject);
        proc.on('close', code => code === 0 ? fs.promises.readFile(graphPath, 'utf8').then(text => resolve(JSON.parse(text)), reject) : reject(new Error(stderr || `loom graph exited with status ${code}`)));
      }).catch(error => { vscode.window.showErrorMessage(`Loom graph failed: ${error.message}`); return undefined; });
      if (next) panel.webview.postMessage({ type: 'model', model: next, view: message.view });
    });
  }

  function dependencyGraphHtml(webview, model, view) {
    const nonce = require('crypto').randomBytes(16).toString('hex');
    const payload = JSON.stringify(model).replace(/</g, '\\u003c');
    return `<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-${nonce}';"><style>
      body{font:13px var(--vscode-font-family);color:var(--vscode-foreground);background:var(--vscode-editor-background);margin:18px}header{display:flex;gap:10px;align-items:center;flex-wrap:wrap}h1{font-size:18px;font-weight:600;margin:0 auto 0 0}button{color:var(--vscode-button-foreground);background:var(--vscode-button-background);border:0;border-radius:4px;padding:6px 11px;cursor:pointer}button:focus-visible{outline:2px solid var(--vscode-focusBorder)}.note{color:var(--vscode-descriptionForeground);margin:12px 0}svg{width:100%;min-height:440px;border:1px solid var(--vscode-panel-border);border-radius:7px}text{fill:var(--vscode-foreground);font:12px var(--vscode-font-family)}.node rect{fill:var(--vscode-editorWidget-background);stroke:var(--vscode-editorWidget-border);rx:6}.edge{stroke:var(--vscode-descriptionForeground);stroke-width:1.4;fill:none;marker-end:url(#arrow)}.edge-label{font-size:10px;fill:var(--vscode-descriptionForeground)}
      </style></head><body><header><h1>Loom semantic dependencies</h1><button id="calls">Call graph</button><button id="data">Data accesses</button></header><p class="note" id="note"></p><svg role="img" aria-label="Loom semantic dependency graph" id="graph" viewBox="0 0 900 500"></svg><script nonce="${nonce}">const api=acquireVsCodeApi();let model=${payload};let view=${JSON.stringify(view)};const ns='http://www.w3.org/2000/svg';for(const v of ['calls','data'])document.getElementById(v).onclick=()=>api.postMessage({type:'view',view:v});window.addEventListener('message',e=>{if(e.data?.type==='model'){model=e.data.model;view=e.data.view;render()}});function el(tag,attrs={}){const x=document.createElementNS(ns,tag);for(const [k,v] of Object.entries(attrs))x.setAttribute(k,v);return x}function render(){const svg=document.getElementById('graph');svg.replaceChildren();document.getElementById('note').textContent=view==='calls'?'Blocks and validated call relationships.':'Aggregated variable reads and writes per block; this is not a statement-level dataflow or parallel-safety analysis.';const blocks=model.nodes.filter(n=>n.kind==='block'),vars=model.nodes.filter(n=>n.kind!=='block');const nodes=[...blocks,...vars],pos=new Map();let height=Math.max(440,Math.max(blocks.length,vars.length,1)*76+60);svg.setAttribute('viewBox','0 0 900 '+height);const defs=el('defs'),marker=el('marker',{id:'arrow',markerWidth:8,markerHeight:8,refX:7,refY:3,orient:'auto'});marker.append(el('path',{d:'M0,0 L0,6 L8,3 z',fill:'currentColor'}));defs.append(marker);svg.append(defs);nodes.forEach((n,i)=>{const col=n.kind==='block'?0:1,row=n.kind==='block'?blocks.indexOf(n):vars.indexOf(n),x=col?520:90,y=40+row*76;pos.set(n.id,{x,y});const g=el('g',{class:'node'}),r=el('rect',{x,y,width:290,height:44});const t=el('text',{x:x+12,y:y+27});t.textContent=n.name;g.append(r,t);svg.append(g)});for(const edge of model.edges){const a=pos.get(edge.from),b=pos.get(edge.to);if(!a||!b)continue;const x1=a.x+(edge.kind==='calls'?290:290),y1=a.y+22,x2=b.x,y2=b.y+22;const path=el('path',{class:'edge',d:'M'+x1+' '+y1+' C '+((x1+x2)/2)+' '+y1+', '+((x1+x2)/2)+' '+y2+', '+x2+' '+y2});svg.append(path);const label=el('text',{class:'edge-label',x:(x1+x2)/2,y:(y1+y2)/2-5,'text-anchor':'middle'});label.textContent=edge.kind;svg.append(label)}}render();</script></body></html>`;
  }

  function schedule(document) {
    if (vscode.workspace.getConfiguration('loom').get('lint.run') === 'onSave') return;
    clearTimeout(timers.get(document.uri.toString()));
    const delay = vscode.workspace.getConfiguration('loom').get('lint.delay', 500);
    timers.set(document.uri.toString(), setTimeout(() => check(document), delay));
  }

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(doc => { if (doc.languageId === 'loom') schedule(doc); }),
    vscode.workspace.onDidChangeTextDocument(e => { if (e.document.languageId === 'loom') schedule(e.document); }),
    vscode.workspace.onDidSaveTextDocument(doc => { if (doc.languageId === 'loom') check(doc); }),
    vscode.workspace.onDidCloseTextDocument(doc => { const key = doc.uri.toString(); diagnostics.delete(doc.uri); clearTimeout(timers.get(key)); timers.delete(key); processes.get(key)?.kill(); processes.delete(key); }),
    vscode.commands.registerCommand('loom.checkFile', () => { const ed = vscode.window.activeTextEditor; if (ed?.document.languageId === 'loom') return check(ed.document); }),
    vscode.commands.registerCommand('loom.setCompilerPath', async () => {
      const pick = await vscode.window.showOpenDialog({ canSelectFiles: true, canSelectFolders: false, canSelectMany: false, openLabel: 'Select loomc' });
      if (pick?.[0]) await vscode.workspace.getConfiguration('loom').update('compilerPath', pick[0].fsPath, vscode.ConfigurationTarget.Workspace);
    }),
    vscode.commands.registerCommand('loom.formatDocument', () => vscode.commands.executeCommand('editor.action.formatDocument')),
    vscode.commands.registerCommand('loom.build', () => runWorkspaceCommand('build')),
    vscode.commands.registerCommand('loom.clean', () => runWorkspaceCommand('clean')),
    vscode.commands.registerCommand('loom.run', () => runWorkspaceCommand('run')),
    vscode.commands.registerCommand('loom.showDependencyGraph', openDependencyGraph),
    vscode.languages.registerDocumentFormattingEditProvider('loom', {
      provideDocumentFormattingEdits(document, options) {
        const size = vscode.workspace.getConfiguration('loom').get('format.indentSize', 4);
        const unit = options.insertSpaces ? ' '.repeat(size) : '\t';
        let inBlock = false;
        let inType = false;
        let blockHasInlineImplements = false;
        let blockHasInlineTakes = false;
        let currentSection = '';
        let controlStack = [];
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
          const isEndGeneral = /^end\s+[a-z_][a-z0-9_]*\s*$/i.test(raw) && !isEndIf && !isEndLoop && !isEndMatch && !isEndEnsure;

          let lineDepth = 0;

          if (isEndIf || isEndLoop || isEndMatch || isEndEnsure) {
            if (controlStack.length > 0 && controlStack[controlStack.length - 1].type === 'when') controlStack.pop();
            if (controlStack.length > 0) controlStack.pop();
            const base = blockHasInlineImplements ? 1 : 2;
            lineDepth = base + controlStack.length;
          } else if (isElseIf || isElse) {
            if (controlStack.length > 0 && controlStack[controlStack.length - 1].type === 'when') controlStack.pop();
            const base = blockHasInlineImplements ? 1 : 2;
            lineDepth = base + (controlStack.length > 0 ? controlStack.length - 1 : 0);
          } else if (isWhen) {
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
              lineDepth = base + controlStack.length;
            }
          } else {
            lineDepth = 0;
          }

          const desired = unit.repeat(Math.max(0, lineDepth)) + raw;
          if (line.text !== desired) edits.push(vscode.TextEdit.replace(line.range, desired));

          if (/^if\b/i.test(raw)) controlStack.push({ type: 'if' });
          else if (/^loop\b/i.test(raw)) controlStack.push({ type: 'loop' });
          else if (/^match\b/i.test(raw)) controlStack.push({ type: 'match' });
          else if (/^ensure\s*$/i.test(raw)) controlStack.push({ type: 'ensure' });
          else if (isWhen || /^else\s*:\s*$/i.test(raw)) controlStack.push({ type: 'when' });
        }
        return edits;
      }
    }),
    vscode.languages.registerCompletionItemProvider('loom', {
      provideCompletionItems() {
        const words = [
          'Block','Takes','Produces','Declares','Implements','End','If','Else','Until','While','Loop','Exit','Skip','Give','Write','Write_UART',
          'Match','When','use','Use','while','until',
          'Address','Deref','Pointer','Slice','Array','Ensure','Cast','Null','Extern','Import','Record','Enum','Module','Constant',
          'And','Or','Not','LSL','LSR','Mod','True','False',
          'Signed_8','Signed_16','Signed_32','Signed_64','Unsigned_8','Unsigned_16','Unsigned_32','Unsigned_64',
          'Float_32','Float_64','Boolean','String','Character','Integer','Int',
          'Vol_Read_8','Vol_Read_16','Vol_Read_32','Vol_Read_64',
          'Vol_Write_8','Vol_Write_16','Vol_Write_32','Vol_Write_64',
          'Nop','WaitForInterrupt','MemoryBarrier'
        ];
        return words.map(word => { const item = new vscode.CompletionItem(word, vscode.CompletionItemKind.Keyword); item.insertText = word; return item; });
      }
    }, /[A-Za-z_]/),
    vscode.languages.registerDocumentSymbolProvider('loom', {
      provideDocumentSymbols(document) {
        const symbols = [];
        for (let i = 0; i < document.lineCount; i++) {
          const match = /^\s*Block\s+([A-Za-z_][A-Za-z0-9_]*)/.exec(document.lineAt(i).text);
          if (!match) continue;
          let end = i;
          for (let j = i + 1; j < document.lineCount; j++) {
            if (new RegExp('^\\s*End\\s+' + match[1] + '\\s*$').test(document.lineAt(j).text)) { end = j; break; }
          }
          const range = new vscode.Range(i, 0, end, document.lineAt(end).text.length);
          const selection = new vscode.Range(i, match[0].indexOf(match[1]), i, match[0].indexOf(match[1]) + match[1].length);
          symbols.push(new vscode.DocumentSymbol(match[1], 'Block', vscode.SymbolKind.Function, range, selection));
        }
        return symbols;
      }
    }),
    vscode.languages.registerHoverProvider('loom', {
      provideHover(document, position) {
        const range = document.getWordRangeAtPosition(position); if (!range) return;
        const word = document.getText(range), key = word.toLowerCase();
        const help = {
          block:'`Block Name` begins a function or procedure. Close it with `End Name`.',
          takes:'Starts the multiline parameter list. Each entry is `Name : Type`.',
          produces:'Declares the function result type (supports scalars, pointers, and Records).',
          declares:'Begins local variable declarations.',
          implements:'Begins the function body.',
          call:'Note: The `Call` keyword has been removed from Loom. Invoke functions and procedures directly: `FunctionName(...)` or `Var = FunctionName(...)`.',
          address:'`Address(Variable)` takes the memory address of an lvalue and produces a `T Pointer`.',
          deref:'`Deref(Pointer)` accesses the value pointed to by a pointer. Can be read or assigned (`Deref(P) = Val`).',
          pointer:'`T Pointer` represents a typed raw memory pointer to type `T`.',
          slice:'`T Slice` represents a fat-pointer slice view containing a data pointer and 64-bit length (`Slice(Arr)`).',
          array:'`T Array[N]` represents a contiguous fixed-size array of `N` elements on stack or global storage.',
          ensure:'`Ensure { statement }` registers one cleanup statement to execute in LIFO order upon scope exit.',
          cast:'`Cast[Target_Type](Expression)` explicitly converts between types with compile-time narrowing warnings.',
          null:'`Null` is the typed null pointer literal compatible with all pointer types.',
          extern:'`Extern Block Name ... End Name` declares an external C ABI function without a body.',
          import:'`Import ModuleName` automatically resolves and includes an external Loom file.',
          record:'`Record Name ... End Name` declares a structured data type with named fields.',
          enum:'`Enum Name : IntegerType ... End Name` declares an enumeration with explicit numeric values.',
          constant:'`Constant` marks global declarations as immutable read-only values.',
          write:'Prints a value to stdout. Formats scalars, pointers (%p), and structured Records.',
          give:'Returns a value from a value-producing block.',
          exit:'Immediately exits a `Loop` or exits early from a procedure (executing active `Ensure` cleanup in LIFO order).',
          skip:'Continues at the start of the nearest enclosing `Loop`.',
          if:'Begins a conditional block. Close it with `End If`.',
          else:'Alternative branch for `If`. Supports chained `Else If`.',
          loop:'Begins a loop. Can be infinite, `Loop while <condition>`, or `Loop until <condition>`.',
          until:'Conditional exit clause for `Loop until <condition>`.',
          while:'`Loop while <condition>` executes the loop body while the condition is true.',
          match:'`Match <expression>` multi-way branching over enums, integers, characters, or booleans. Close with `End Match`.',
          when:'`When <pattern>:` defines a case arm inside a `Match` block.',
          use:'`use <ModuleName>` imports external routines into the current file or block scope without qualification.',
          boolean:'Boolean value type; literals are `True` and `False`.',
          float_32:'32-bit single-precision floating point number (IEEE 754).',
          float_64:'64-bit double-precision floating point number (IEEE 754).',
          signed_8:'Signed 8-bit integer type.', signed_16:'Signed 16-bit integer type.', signed_32:'Signed 32-bit integer type.', signed_64:'Signed 64-bit integer type.',
          unsigned_8:'Unsigned 8-bit integer type.', unsigned_16:'Unsigned 16-bit integer type.', unsigned_32:'Unsigned 32-bit integer type.', unsigned_64:'Unsigned 64-bit integer type.',
          string:'Null-terminated immutable UTF-8 string pointer.',
          character:'32-bit Unicode scalar character literal, e.g. `\'é\'`.',
          integer:'`Integer` is a built-in scalar type alias for `Signed_64`.',
          int:'`Int` is a built-in scalar type alias for `Signed_64`.',
          vol_read_8:'`Vol_Read_8(Address)` performs a volatile 8-bit memory-mapped I/O load.',
          vol_read_16:'`Vol_Read_16(Address)` performs a volatile 16-bit memory-mapped I/O load.',
          vol_read_32:'`Vol_Read_32(Address)` performs a volatile 32-bit memory-mapped I/O load.',
          vol_read_64:'`Vol_Read_64(Address)` performs a volatile 64-bit memory-mapped I/O load.',
          vol_write_8:'`Vol_Write_8(Address, Value)` performs a volatile 8-bit memory-mapped I/O store.',
          vol_write_16:'`Vol_Write_16(Address, Value)` performs a volatile 16-bit memory-mapped I/O store.',
          vol_write_32:'`Vol_Write_32(Address, Value)` performs a volatile 32-bit memory-mapped I/O store.',
          vol_write_64:'`Vol_Write_64(Address, Value)` performs a volatile 64-bit memory-mapped I/O store.',
          nop:'`Nop()` emits a hardware no-operation instruction.',
          waitforinterrupt:'`WaitForInterrupt()` enters low-power standby until an interrupt occurs (`wfi`).',
          memorybarrier:'`MemoryBarrier()` emits a full hardware data memory barrier (`dmb sy`).'
        };
        return help[key] ? new vscode.Hover(new vscode.MarkdownString(help[key]), range) : undefined;
      }
    })
  );

  const snippets = [
    ['block', 'Block ${1:Name}\n    Takes\n        ${2:Value} : ${3:Signed_32}\n    Produces ${4:Signed_32}\n    Implements\n        Give ${2:Value}\nEnd ${1:Name}', 'Block with parameter and result'],
    ['concise', 'Block ${1:Name} Takes\n    ${2:A} : ${3:Signed_32}\nProduces (${4:$2 + 1})\nEnd ${1:Name}', 'Concise single-line routine'],
    ['procedure', 'Block ${1:Name}\n    Implements\n        $0\nEnd ${1:Name}', 'Parameterless procedure'],
    ['if', 'If ${1:condition}\n    $0\nEnd If', 'If block'],
    ['loop', 'Loop\n    $0\nEnd Loop', 'Infinite loop'],
    ['loopwhile', 'Loop while ${1:condition}\n    $0\nEnd Loop', 'Conditional while loop'],
    ['loopuntil', 'Loop until ${1:condition}\n    $0\nEnd Loop', 'Conditional until loop'],
    ['match', 'Match ${1:Expression}\n    When ${2:Pattern}:\n        $0\n    Else:\n        \nEnd Match', 'Pattern matching block'],
    ['declares', 'Declares\n    ${1:Name} : ${2:Signed_32}', 'Local declaration'],
    ['write', 'Write(\"${1:text}\")$0', 'Print a string literal'],
    ['use', 'use ${1:ModuleName}', 'Unqualified module import directive']
  ];
  context.subscriptions.push(vscode.languages.registerCompletionItemProvider('loom', {
    provideCompletionItems() { return snippets.map(([label, body, detail]) => { const i = new vscode.CompletionItem(label, vscode.CompletionItemKind.Snippet); i.insertText = new vscode.SnippetString(body); i.detail = detail; return i; }); }
  }, ' '));

  for (const doc of vscode.workspace.textDocuments) if (doc.languageId === 'loom') schedule(doc);
}

function deactivate() {}
module.exports = { activate, deactivate, parseCompilerDiagnostics };
