// SPDX-License-Identifier: MIT
import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import {extensionLinkerScript, hostServesCode128K, HOST_CODE_KIB} from './extension-image.mjs';
import {CODE_BASE, CODE_BASE_128K, SERVICE} from './extension.mjs';

const linkers = path.resolve(import.meta.dirname, '../BootLinkerFiles');
const progmemBlock = (ld) => ld.slice(ld.indexOf('.text.progmem : {'), ld.indexOf('.text.itcm : {'));

test('the stock host is linked at 64 KiB, and 96 KiB moves its code ceiling and withdraws bit 21', () => {
  const stock = extensionLinkerScript(linkers);
  assert.equal(stock, extensionLinkerScript(linkers, 64));
  assert.match(stock, /ASSERT\(__exidx_end <= 0x10000,/);
  assert.equal(extensionLinkerScript(linkers, 96), stock
    .replace('__exidx_end <= 0x10000, "Host code exceeds its 64 KiB', '__exidx_end <= 0x18000, "Host code exceeds its 96 KiB')
    .replace('_vm_host_code_128k_service = 0x200000;', '_vm_host_code_128k_service = 0x0;'));
  assert.match(stock, /_itcm_block_count = 6;/);
  for (const invalid of [0, 32, 65, 128, '64', NaN]) {
    assert.throws(() => extensionLinkerScript(linkers, invalid), /64 or 96/);
  }
});

test('no host code budget reaches into the module code window', () => {
  for (const kib of HOST_CODE_KIB) assert.ok(kib * 1024 <= CODE_BASE, `${kib} KiB`);
});

// VMHost.h takes bit 21 of its descriptor from this symbol, so the service and
// the ceiling that makes it safe come out of one link.
test('only a host whose code budget ends below the 128 KiB window serves bit 21', () => {
  const served = (kib) => Number(extensionLinkerScript(linkers, kib)
    .match(/_vm_host_code_128k_service = (0x[0-9a-f]+);/)[1]);
  assert.equal(SERVICE.CODE_128K, 1 << 21);
  assert.equal(served(64), SERVICE.CODE_128K);
  assert.equal(served(96), 0);
  assert.ok(hostServesCode128K(64) && !hostServesCode128K(96));
  assert.equal(CODE_BASE_128K, 64 * 1024);
  for (const kib of HOST_CODE_KIB) {
    assert.match(extensionLinkerScript(linkers, kib),
      /ASSERT\(_vm_host_code_128k_service == 0 \|\| __exidx_end <= 0x10000,/);
  }
});

// ld matches a file-name pattern with fnmatch(pattern, name, 0): '*', '?', '[...]', and
// '\' escaping the next character, inside a bracket or out.
function fnmatchRegExp(pattern) {
  const escape = (c) => c.replace(/[.*+?^${}()|[\]\\/-]/g, '\\$&');
  let source = '';
  for (let i = 0; i < pattern.length; i++) {
    const c = pattern[i];
    if (c === '*') source += '.*';
    else if (c === '?') source += '.';
    else if (c === '\\') source += escape(pattern[++i]);
    else if (c !== '[') source += escape(c);
    else {
      let members = '';
      for (i++; pattern[i] !== ']'; i++) members += escape(pattern[pattern[i] === '\\' ? ++i : i]);
      source += `[${members}]`;
    }
  }
  return new RegExp(`^${source}$`);
}

const flashTextPatterns = (ld) =>
  [...progmemBlock(ld).matchAll(/^\t\t(\S+)\(\.text\*\)$/gm)].map((match) => fnmatchRegExp(match[1]));
const placedInFlash = (patterns, objectPath) => patterns.some((pattern) => pattern.test(objectPath));

// ld gives an input section to the first output section that names it, so a library
// pattern outside .text.progmem, or after it, leaves that code in ITCM.
test('the SD card libraries run from flash, on either path separator', () => {
  const patterns = flashTextPatterns(extensionLinkerScript(linkers));
  for (const library of ['SdFat', 'SD', 'SPI']) {
    for (const objectPath of [`/b/ext/libraries/${library}/src/x.cpp.o`, `C:\\b\\ext\\libraries\\${library}\\src\\x.cpp.o`]) {
      assert.ok(placedInFlash(patterns, objectPath), objectPath);
    }
  }
  for (const objectPath of ['/b/ext/libraries/SdFatX/x.cpp.o', '/b/ext/libraries/SPIFlash/x.cpp.o',
    'C:\\b\\ext\\sketch\\VMBoot.ino.cpp.o', '/b/ext/sketch/VMBoot.ino.cpp.o', 'core.a']) {
    assert.ok(!placedInFlash(patterns, objectPath), objectPath);
  }
});
