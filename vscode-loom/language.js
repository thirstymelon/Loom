'use strict';

// Loom-aware tokenizer and lightweight declaration index for editor features.
// Semantic type validity always comes from loomc; this module never guesses it.
const { KEYWORDS, BUILTIN_TYPES, TYPE_ALIASES, tokenize } = require('./lexer');

function analyze(text, uri = '') {
  const tokens = tokenize(text);
  const symbols = [];
  const blocks = [];
  const records = [];
  const enums = [];
  const typeNames = new Set(BUILTIN_TYPES);
  const isId = t => t && t.kind === 'identifier';
  const add = (token, kind, type, scope, end = token.end, detail = '') => {
    if (!token) return;
    symbols.push({ name: token.value, kind, type: type || '', scope, uri, token, start: token.start, end, detail });
  };

  for (let n = 0; n < tokens.length - 1; n++) {
    const t = tokens[n], next = tokens[n + 1];
    if (t.value === 'Block' && isId(next) && next.value !== 'Global') {
      let end = text.length;
      for (let j = n + 2; j < tokens.length - 1; j++) {
        if (tokens[j].value === 'End' && tokens[j + 1].value === next.value) { end = tokens[j + 1].end; break; }
      }
      const b = { name: next.value, token: next, start: t.start, end, params: [], result: '', uri };
      blocks.push(b);
      add(next, 'function', '', 'workspace', end, 'Loom block');
    } else if ((t.value === 'Record' || t.value === 'Enum') && isId(next)) {
      let end = text.length;
      for (let j = n + 2; j < tokens.length - 1; j++) {
        if (tokens[j].value === 'End' && tokens[j + 1].value === next.value) { end = tokens[j + 1].end; break; }
      }
      const kind = t.value.toLowerCase();
      const item = { name: next.value, token: next, start: t.start, end, kind, uri };
      (kind === 'record' ? records : enums).push(item);
      typeNames.add(next.value);
      add(next, kind, kind, 'workspace', end, `Loom ${kind}`);
    }
  }

  function parseType(at) {
    let i = at;
    if (tokens[i]?.value === 'Constant') i++;
    if (!isId(tokens[i])) return null;
    let value = tokens[i].value;
    const begin = i;
    i++;
    if ((value === 'Pointer' || value === 'Block_Pointer') && tokens[i]?.value === '[') {
      const inner = tokens[i + 1]?.value || '';
      i++;
      while (i < tokens.length && tokens[i].value !== ']') i++;
      if (tokens[i]?.value === ']') i++;
      value += `[${inner}]`;
    } else if (value === 'Array' && tokens[i]?.value === '[') {
      let depth = 1;
      const parts = [];
      i++;
      while (i < tokens.length && depth) {
        if (tokens[i].value === '[') depth++;
        else if (tokens[i].value === ']') { depth--; if (!depth) { i++; break; } }
        if (depth) parts.push(tokens[i].value);
        i++;
      }
      value += `[${parts.join(' ')}]`;
    } else if (['Array','Slice','Vector','Text','HMap','Task_Handle'].includes(value) && tokens[i]?.value === '[') {
      const parts = [];
      i++;
      let depth = 1;
      while (i < tokens.length && depth) {
        if (tokens[i].value === '[') depth++;
        else if (tokens[i].value === ']') { depth--; if (!depth) { i++; break; } }
        if (depth) parts.push(tokens[i].value);
        i++;
      }
      value += `[${parts.join(' ')}]`;
    } else if (['Pointer','Slice','Array','Vector','Task_Handle','Block_Pointer'].includes(tokens[i]?.value)) {
      value += ` ${tokens[i].value}`;
      i++;
      if (tokens[i]?.value === '[') {
        const parts = [];
        i++;
        let depth = 1;
        while (i < tokens.length && depth) {
          if (tokens[i].value === '[') depth++;
          else if (tokens[i].value === ']') { depth--; if (!depth) { i++; break; } }
          if (depth) parts.push(tokens[i].value);
          i++;
        }
        value += `[${parts.join(' ')}]`;
      }
      if (tokens[i]?.value === '(') {
        let depth = 0;
        do {
          if (tokens[i].value === '(') depth++;
          else if (tokens[i].value === ')') depth--;
          value += tokens[i].value;
          i++;
        } while (i < tokens.length && depth > 0);
      }
    }
    if (!BUILTIN_TYPES.has(tokens[begin].value) && !typeNames.has(tokens[begin].value)) return null;
    return { type: value, next: i };
  }

  for (const block of blocks) {
    let section = '';
    const header = tokens.findIndex(t => t.start === block.start);
    for (let i = header + 2; i < tokens.length && tokens[i].start < block.end; i++) {
      const t = tokens[i];
      if (t.value === 'Takes') { section = 'parameter'; continue; }
      if (t.value === 'Declares') { section = 'variable'; continue; }
      if (t.value === 'Produces') {
        section = '';
        const parsed = parseType(i + 1);
        if (parsed) block.result = parsed.type;
        continue;
      }
      if (t.value === 'Implements') { section = ''; continue; }
      if ((section === 'parameter' || section === 'variable') && isId(t) && tokens[i + 1]?.value === ':') {
        const parsed = parseType(i + 2);
        if (!parsed) continue;
        const kind = section;
        const symbol = { name: t.value, kind, type: parsed.type, scope: block.name, uri, token: t, start: t.start, end: t.end, detail: `${parsed.type} ${kind}` };
        symbols.push(symbol);
        if (kind === 'parameter') block.params.push(symbol);
      }
    }
  }

  for (const record of records) {
    const startAt = tokens.findIndex(t => t.start === record.start);
    for (let i = startAt + 2; i < tokens.length && tokens[i].start < record.end - record.name.length; i++) {
      const t = tokens[i];
      if (isId(t) && tokens[i + 1]?.value === ':') {
        const parsed = parseType(i + 2);
        if (parsed) add(t, 'field', parsed.type, record.name, t.end, `${record.name}.${t.value} : ${parsed.type}`);
      }
    }
  }

  for (const item of enums) {
    const startAt = tokens.findIndex(t => t.start === item.start);
    for (let i = startAt + 2; i < tokens.length && tokens[i].start < item.end; i++) {
      if (isId(tokens[i]) && tokens[i + 1]?.value === '=') add(tokens[i], 'enumMember', item.name, item.name, tokens[i].end, `${item.name}.${tokens[i].value}`);
    }
  }

  const seen = new Set();
  const uniqueSymbols = symbols.filter(s => {
    const key = `${s.name}:${s.kind}:${s.scope}:${s.start}`;
    if (seen.has(key)) return false;
    seen.add(key); return true;
  });
  return { tokens, symbols: uniqueSymbols, blocks, records, enums, typeNames };
}

function symbolAt(index, offset, currentBlock) {
  const token = index.tokens.find(t => t.start <= offset && offset <= t.end && t.kind === 'identifier');
  if (!token) return null;
  const found = index.symbols.filter(s => s.name === token.value && (s.scope === currentBlock || s.scope === 'workspace'));
  return found.sort((a, b) => (a.scope === currentBlock ? -1 : 1))[0] || null;
}

function currentBlock(index, offset) {
  return index.blocks.filter(b => b.start <= offset && offset <= b.end).sort((a, b) => (a.end - a.start) - (b.end - b.start))[0]?.name || '';
}

module.exports = { KEYWORDS, BUILTIN_TYPES, TYPE_ALIASES, tokenize, analyze, symbolAt, currentBlock };
