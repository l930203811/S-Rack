// .hance 完整结构解析器 v2 —— 走读全部节点：名字、算符标签、输入引用、张量维度
'use strict';
const fs = require('fs');
const buf = fs.readFileSync(process.argv[2]);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let pos = 0;

function u8()  { const v = dv.getUint8(pos); pos += 1; return v; }
function u16() { const v = dv.getUint16(pos, true); pos += 2; return v; }
function u32() { const v = dv.getUint32(pos, true); pos += 4; return v; }
function i64() { const v = Number(dv.getBigInt64(pos, true)); pos += 8; return v; }
function f32() { const v = dv.getFloat32(pos, true); pos += 4; return v; }
function str() { const n = u32(); const s = buf.slice(pos, pos + n).toString('latin1'); pos += n; return s; }

const magic = str();
const ver = u32();
console.log(`magic=${magic} ver=0x${ver.toString(16)}`);
const n1 = u32();
console.log(`u32@${(pos-4).toString(16)} = ${n1}`);
// 0x17..0x31 的奇怪字节：先按 f32 试探
console.log(`bytes@0x${pos.toString(16)}: ${Array.from(buf.slice(pos, pos + 0x1B - pos)).map(x => x.toString(16).padStart(2, '0')).join(' ')}`);
const f = f32();
console.log(`f32 = ${f}`);
console.log(`bytes@0x${pos.toString(16)}: ${Array.from(buf.slice(pos, pos + 14)).map(x => x.toString(16).padStart(2, '0')).join(' ')}`);

// 从 0x32 开始按节点走（硬编码起点：已确认 prenorm@0x32）
pos = 0x31;

const OPNAMES = { 0x02: 'conv/dconv?', 0x10: 'prenorm', 0x11: 'act', 0x04: 'tensor-after-refs' };

let nodeCount = 0;
try {
  while (pos < buf.length) {
    const nameOff = pos;
    const name = str();
    const tag = u16();
    const refs = [];
    // 连续读字符串引用
    while (pos + 4 <= buf.length) {
      const n = dv.getUint32(pos, true);
      if (n >= 3 && n <= 20 && pos + 4 + n <= buf.length) {
        let ok = true;
        for (let j = 0; j < n; j++) {
          const c = buf[pos + 4 + j];
          if (c < 0x61 && (c < 0x30 || c > 0x39)) { ok = false; break; }
        }
        if (!ok) break;
        refs.push(str());
      } else break;
    }
    // 之后是属性/张量：tag 01 = int 张量(i64 count + i32×count)；tag 04 = float 张量(u32 rank? + dims + f32)
    const attrs = [];
    while (pos < buf.length) {
      const b = dv.getUint8(pos);
      if (b === 0x01) {
        pos += 1;
        const cnt = i64();
        const vals = [];
        for (let i = 0; i < cnt && pos + 4 <= buf.length; i++) vals.push(u32());
        attrs.push({ t: 'int[]', cnt, vals: vals.slice(0, 16) });
      } else if (b === 0x04) {
        pos += 1;
        // 尝试 rank + dims
        const save = pos;
        const rank = u32();
        if (rank < 1 || rank > 6) { pos = save; break; }
        const dims = [];
        let prod = 1;
        for (let i = 0; i < rank; i++) { const d = u32(); dims.push(d); prod *= d; }
        if (prod > 20000000) { pos = save; break; }
        const first = [];
        for (let i = 0; i < Math.min(3, prod) && pos + 4 <= buf.length; i++) first.push(f32());
        pos += (prod - first.length) * 4;
        attrs.push({ t: 'f32', dims, prod, first });
      } else break;
    }
    console.log(`@${nameOff.toString(16).padStart(6, '0')} ${name} tag=0x${tag.toString(16).padStart(4, '0')} refs=[${refs.join(', ')}] attrs=${JSON.stringify(attrs)}`);
    nodeCount++;
  }
} catch (e) {
  console.log('PARSE STOP @0x' + pos.toString(16), e.message);
  console.log(Array.from(buf.slice(pos, pos + 48)).map(x => x.toString(16).padStart(2, '0')).join(' '));
}
console.log('nodes:', nodeCount, 'pos:', pos.toString(16), 'file:', buf.length.toString(16));
