'use strict';

// Shared parser for compiler output. Used by the extension (build diagnostics)
// and by the language server (per-document checks) so the two cannot drift.
// Severity uses LSP numbering: 1 error, 2 warning, 3 information, 4 hint.

const SEVERITY = { error: 1, warning: 2, note: 3, info: 3, hint: 4 };

function levelOf(value) {
  return SEVERITY[String(value || '').toLowerCase()] || SEVERITY.error;
}

function parseCompilerDiagnostics(text, lineCount) {
  const maxLine = Number.isFinite(lineCount) && lineCount > 0 ? lineCount : Number.MAX_SAFE_INTEGER;
  const found = [];
  const seen = new Set();
  let pending = null;

  const add = item => {
    if (!item || !item.message) return;
    const line = Math.min(Math.max(0, item.line || 0), Math.max(0, maxLine - 1));
    const column = Math.max(0, item.column || 0);
    const length = Math.max(1, item.length || 1);
    const key = `${line}:${column}:${item.severity}:${item.message}`;
    if (seen.has(key)) return;
    seen.add(key);
    found.push({ line, column, length, message: item.message, severity: item.severity, code: item.code || '', file: item.file || '' });
  };
  const flush = () => { if (pending) { add(pending); pending = null; } };

  for (const raw of String(text == null ? '' : text).split(/\r?\n/)) {
    const line = raw.trim();
    if (!line) continue;
    if (/^LLVM (?:function|module) verification/i.test(line) || /^usage:/i.test(line)) continue;
    if (/^=+\s*help:/i.test(line) || /^help:/i.test(line)) continue;

    const header = /^(error|warning|note)(?:\[([^\]]+)\])?:\s*(.*)$/i.exec(line);
    if (header) {
      flush();
      pending = { line: 0, column: 0, length: 1, code: header[2] || '', message: header[3], severity: levelOf(header[1]) };
      continue;
    }

    // --> path:line:column, --> path:line (compiler may omit the column), --> path
    const location = /^-->\s+(.+?):(\d+)(?::(\d*))?\s*$/.exec(line);
    if (location && pending) {
      pending.file = location[1].trim();
      pending.line = Math.max(0, Number(location[2]) - 1);
      if (location[3]) pending.column = Math.max(0, Number(location[3]) - 1);
      continue;
    }
    const bareLocation = /^-->\s+(.+?)\s*$/.exec(line);
    if (bareLocation && pending) { pending.file = bareLocation[1].trim(); continue; }

    // Older diagnostics print the column on its own line before the frame.
    if (pending && /^\d+$/.test(line)) { pending.column = Math.max(0, Number(line) - 1); continue; }

    const caret = /^\|\s*(\^~*)/.exec(line);
    if (caret && pending) { pending.length = Math.max(1, caret[1].length); continue; }
    if (/^\d+\s*\|/.test(line) || line === '|') continue;

    let match = /(?:^|\b)line\s+(\d+)(?::(\d+))?:\s*(?:(error|warning|note):\s*)?(.*)$/i.exec(line);
    if (match) {
      flush();
      add({
        line: Math.max(0, Number(match[1]) - 1),
        column: match[2] ? Math.max(0, Number(match[2]) - 1) : 0,
        severity: levelOf(match[3] || 'error'),
        message: match[4] || line,
        code: ''
      });
      continue;
    }
    match = /^(.*?):(\d+):(\d+):\s*(error|warning|note):\s*(.*)$/i.exec(line);
    if (match) {
      flush();
      add({
        file: match[1].trim(),
        line: Math.max(0, Number(match[2]) - 1),
        column: Math.max(0, Number(match[3]) - 1),
        severity: levelOf(match[4]),
        message: match[5],
        code: ''
      });
      continue;
    }
    // Unknown output (progress lines, statistics) is intentionally ignored.
  }
  flush();
  return found;
}

module.exports = { parseCompilerDiagnostics };
