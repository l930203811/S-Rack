// 解析 Acon Digital .hance 模型文件格式（自研逆向工具）
'use strict';
const fs = require('fs');

const buf = fs.readFileSync(process.argv[2]);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let pos = 0;

function u8()  { const v = dv.getUint8(pos); pos += 1; return v; }
function u32() { const v = dv.getUint32(pos, true); pos += 4; return v; }
function i64() { const v = dv.getBigInt64(pos, true); pos += 8; return v; }
function f32() { const v = dv.getFloat32(pos, true); pos += 4; return v; }
function f64() { const v = dv.getFloat64(pos, true); pos += 8; return v; }
function str() { const n = u32(); const s = buf.slice(pos, pos + n).toString('latin1'); pos += n; return s; }

// 顶层：魔数
const magicLen = u32();
const magic = buf.slice(pos, pos + magicLen).toString('latin1'); pos += magicLen;
console.log('magic:', JSON.stringify(magic));

const version = u32();
console.log('version:', version.toString(16));

// 接下来猜测是对象图。试探性递归打印前 N 字节的结构。
// 已知片段：0A 00 00 00 = 10（可能是字符串数/层名数）
// 打印 hex 供人工分析
function hex(off, len) {
  const b = buf.slice(off, off + len);
  let out = '';
  for (let i = 0; i < b.length; i += 16) {
    const line = b.slice(i, i + 16);
    out += (off + i).toString(16).padStart(8, '0') + '  ';
    out += Array.from(line).map(x => x.toString(16).padStart(2, '0')).join(' ').padEnd(47) + '  ';
    out += Array.from(line).map(x => x >= 32 && x < 127 ? String.fromCharCode(x) : '.').join('');
    out += '\n';
  }
  return out;
}

console.log(hex(0, 512));
console.log('... tail ...');
console.log(hex(buf.length - 256, 256));
