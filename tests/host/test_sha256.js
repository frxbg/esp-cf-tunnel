'use strict';
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const {sha256Bytes} = require('../../examples/system_monitor/web/sha256.js');
for (const bytes of [Buffer.from(''),Buffer.from('abc'),
  ...[1,55,56,63,64,65,1000,1572864].map(n => crypto.randomBytes(n))]) {
  assert.equal(sha256Bytes(bytes),crypto.createHash('sha256').update(bytes).digest('hex'));
}
console.log('PASS: SHA256 empty, abc, padding boundaries, and full-slot size');
