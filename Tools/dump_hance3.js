// dump uatt3/uatt4 tail 开头 + 相邻 def 边界
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
console.log('=== uatt3 def 区域 (def@0x2f447b, tail 0x2f4492, 下个def uconcat4 ~0x33449x) ===');
console.log(hex(0x2f4460, 80));
console.log('... tail 末端 (uconcat4 def 前 64B):');
console.log(hex(0x334460, 80));
console.log('\n=== uatt4 def 区域 (def@0x1ed3a6, tail 0x1ed3bd, 下个def uattact4 @0x2343cf) ===');
console.log(hex(0x1ed390, 96));
console.log('... tail 末端 (uattact4 def 前 64B):');
console.log(hex(0x234390, 80));
