// hexdump 指定区域，查漏掉的权重头
'use strict';
const fs = require('fs');
const file = 'C:/ProgramData/Acon Digital/AIModels/VocalsSeparation.hance';
const buf = fs.readFileSync(file);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);

function hex(off, len) {
  const lines = [];
  for (let i = off; i < Math.min(off + len, buf.length); i += 16) {
    let s = i.toString(16).padStart(6, '0') + ': ';
    for (let j = i; j < Math.min(i + 16, buf.length); j++) s += buf[j].toString(16).padStart(2, '0') + ' ';
    lines.push(s);
  }
  return lines.join('\n');
}

console.log('=== uconv5 尾部起点 (0x1dd354 之后 96B，defs 序: bnact@0x1dd329 → uconcat5@0x22d3c6) ===');
console.log(hex(0x1dd354, 96));
console.log('\n=== dconv3 尾部 (tensor@0x3d17f 前后) ===');
console.log(hex(0x3d170, 128));
console.log('\n=== dconv0 尾部剩余 (0x198~0x3b0) ===');
console.log(hex(0x198, 96));
console.log('\n=== uconv4 区域 (tensor@0x234429 = [3,128]?) ===');
console.log(hex(0x234420, 128));
console.log('\n=== bndense/bnact 之间 (0x1d9b20 dact5[28,128] 后 → bnact@0x1dd329) ===');
console.log(hex(0x1d9b30, 160));
