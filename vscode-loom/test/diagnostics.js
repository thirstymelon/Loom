'use strict';

const assert = require('node:assert/strict');
const { parseCompilerDiagnostics } = require('../diagnostics');

// Modern compiler output with a source frame.
const modern = [
  "error[E0001]: Unsupported character '@'",
  '  --> /tmp/example.loom:3:4',
  '   |',
  '  3 |    @',
  "   |    ^~~ Unsupported character '@'"
].join('\n');
const parsed = parseCompilerDiagnostics(modern, 5);
assert.equal(parsed.length, 1);
assert.equal(parsed[0].code, 'E0001');
assert.equal(parsed[0].line, 2);
assert.equal(parsed[0].column, 3);
assert.equal(parsed[0].length, 3);
assert.equal(parsed[0].file, '/tmp/example.loom');
assert.equal(parsed[0].severity, 1);

// The driver prints the column on its own line before the source frame.
const splitColumn = [
  'error[E1001]: Expected Signed_32, found Unsigned_32',
  '  --> /tmp/type.loom:3:',
  '4',
  '   |',
  '  3 |   A + B',
  '   |    ^ Expected Signed_32, found Unsigned_32'
].join('\n');
const split = parseCompilerDiagnostics(splitColumn, 10);
assert.equal(split.length, 1);
assert.equal(split[0].line, 2);
assert.equal(split[0].column, 3);
assert.equal(split[0].message, 'Expected Signed_32, found Unsigned_32');

// Legacy and driver-prefixed forms remain supported.
const legacy = parseCompilerDiagnostics('line 7: error: missing End', 10);
assert.equal(legacy[0].line, 6);
assert.equal(legacy[0].severity, 1);
assert.equal(legacy[0].message, 'missing End');
const pathForm = parseCompilerDiagnostics('/tmp/a.loom:2:5: warning: unused value', 10);
assert.equal(pathForm[0].file, '/tmp/a.loom');
assert.equal(pathForm[0].line, 1);
assert.equal(pathForm[0].column, 4);
assert.equal(pathForm[0].severity, 2);

// Noise and duplicate lines are dropped.
const noise = parseCompilerDiagnostics('LLVM module verification failed\nusage: loomc\nline 1: error: same\nline 1: error: same', 5);
assert.equal(noise.length, 1);
assert.equal(parseCompilerDiagnostics('', 5).length, 0);

// Lines are clamped to the document so a stale compiler range cannot throw.
const clamped = parseCompilerDiagnostics('error[E2001]: late\n  --> x.loom:99:1', 3);
assert.equal(clamped[0].line, 2);

console.log('Loom compiler diagnostic parser tests passed');
