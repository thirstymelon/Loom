'use strict';

const assert = require('node:assert/strict');
const loom = require('../language');

const source = `# Block Fake\n// Record Hidden\nBlock Combine Takes\n    Left : Signed_32\n    Right : Signed_32\nProduces Signed_32\nDeclares\n    Sum : Signed_32\nImplements\n    Sum = Left + Right\n    Write("Block NotADeclaration")\nEnd Combine\nRecord Pair\n    First : Signed_32\n    Second : Signed_32\nEnd Pair\nEnum State : Signed_8\n    Idle = 0\nEnd State\n`;
const tokens = loom.tokenize(source);
assert.ok(!tokens.some(t => t.value === 'Fake' || t.value === 'Hidden' || t.value === 'NotADeclaration'));
const index = loom.analyze(source, 'file:///sample.loom');
assert.equal(index.blocks.length, 1);
assert.equal(index.blocks[0].result, 'Signed_32');
assert.deepEqual(index.blocks[0].params.map(p => [p.name, p.type]), [['Left', 'Signed_32'], ['Right', 'Signed_32']]);
assert.ok(index.symbols.some(s => s.name === 'Sum' && s.kind === 'variable' && s.scope === 'Combine'));
assert.ok(index.symbols.some(s => s.name === 'First' && s.kind === 'field' && s.scope === 'Pair' && s.type === 'Signed_32'));
assert.ok(index.symbols.some(s => s.name === 'Idle' && s.kind === 'enumMember' && s.scope === 'State'));
assert.equal(loom.currentBlock(index, source.indexOf('Sum =')), 'Combine');
assert.equal(loom.symbolAt(index, source.indexOf('Left +'), 'Combine').type, 'Signed_32');

const pointer = loom.analyze('Block F Takes\n P : Signed_32 Pointer\nProduces Signed_32\nEnd F');
assert.equal(pointer.blocks[0].params[0].type, 'Signed_32 Pointer');
const compound = loom.analyze('Block G Takes\n V : Signed_32 Vector[8]\n T : Task_Handle[Signed_32]\nEnd G');
assert.equal(compound.blocks[0].params[0].type, 'Signed_32 Vector[8]');
assert.equal(compound.blocks[0].params[1].type, 'Task_Handle[Signed_32]');
console.log('Loom language index tests passed');
