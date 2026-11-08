// .hance 结构解析器 v3 —— 用已知节点名集合校验
'use strict';
const fs = require('fs');
const buf = fs.readFileSync(process.argv[2]);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);

// 已知节点名（来自全文件扫描）
const NAMES = new Set();
{
  const raw = fs.readFileSync(process.argv[2]);
  const d2 = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
  for (let i = 0; i + 4 < raw.length; i++) {
    const n = d2.getUint32(i, true);
    if (n >= 2 && n <= 24 && i + 4 + n <= raw.length) {
      let ok = true;
      for (let j = 0; j < n; j++) { const c = raw[i + 4 + j]; if (c < 0x20 || c > 0x7e) { ok = false; break; } }
      if (ok) { const s = raw.slice(i + 4, i + 4 + n).toString('latin1'); if (/^[A-Za-z_][A-Za-z0-9_.]*$/.test(s)) NAMES.add(s); }
    }
  }
}

let pos = 0;
function u8()  { const v = dv.getUint8(pos); pos += 1; return v; }
function u16() { const v = dv.getUint16(pos, true); pos += 2; return v; }
function u32() { const v = dv.getUint32(pos, true); pos += 4; return v; }
function i64() { const v = Number(dv.getBigInt64(pos, true)); pos += 8; return v; }
function f32() { const v = dv.getFloat32(pos, true); pos += 4; return v; }
function tryStr() { // 若当前位置是「长度+已知节点名」则消费并返回，否则 null
  if (pos + 4 > buf.length) return null;
  const n = dv.getUint32(pos, true);
  if (n < 3 || n > 24 || pos + 4 + n > buf.length) return null;
  const s = buf.slice(pos + 4, pos + 4 + n).toString('latin1');
  if (!NAMES.has(s)) return null;
  pos += 4 + n;
  return s;
}

// 顶层
pos = 0;
const mlen = u32(); const magicName = buf.slice(pos, pos + mlen).toString('latin1'); pos += mlen;
const ver = u32();
console.log(`magic=${magicName} ver=0x${ver.toString(16)} u32=${u32()}`);
// 头部剩余字节打印
console.log('hdr bytes:', Array.from(buf.slice(pos, 0x32)).map(x => x.toString(16).padStart(2, '0')).join(' '));
pos = 0x31;

const nodes = [];
let lastGood = pos;
while (pos < buf.length) {
  const nameOff = pos;
  const name = tryStr();
  if (name === null) { pos += 1; continue; }   // 滑动对齐
  if (process.env.DBG && nodes.length < 3) console.log(`DBG name=${name} @0x${nameOff.toString(16)} pos=0x${pos.toString(16)}`);
  const tag = u16();
  if (process.env.DBG && nodes.length < 3) console.log(`DBG tag=0x${tag.toString(16)} pos=0x${pos.toString(16)}`);
  const refs = [];
  let r;
  while ((r = tryStr()) !== null && refs.length < 4) {
    if (process.env.DBG && nodes.length < 3) console.log(`DBG ref=${r} pos=0x${pos.toString(16)}`);
    refs.push(r);
  }
  if (process.env.DBG && nodes.length < 3) console.log(`DBG after refs pos=0x${pos.toString(16)} bytes=${Array.from(buf.slice(pos, pos + 16)).map(x => x.toString(16).padStart(2, '0')).join(' ')}`);
  const attrs = [];
  let guard = 0;
  while (guard++ < 8 && pos < buf.length) {
    const b = dv.getUint8(pos);
    if (b === 0x01) {
      // int 张量：tag(01) + u32 count(LE) + count×u32(大端)。
      // 证据：prenorm @0x48 = 01 | 08 00 00 00 | "00 00 00 3E"×8，恰好结束于
      // 0x6D 的 dconv0 节点；按大端读为 62（62 频带，语义成立），小端是垃圾值。
      pos += 1;
      const cnt = u32();
      if (cnt > 1000000 || pos + cnt * 4 > buf.length) break;
      const vals = [];
      for (let i = 0; i < cnt; i++) { vals.push(dv.getUint32(pos, false)); pos += 4; }
      attrs.push({ t: 'int', cnt, vals: vals.slice(0, 12) });
    } else if (b === 0x04) {
      pos += 1;
      const save = pos;
      const rank = u32();
      if (rank < 1 || rank > 5) { pos = save; break; }
      const dims = [];
      let prod = 1;
      for (let i = 0; i < rank; i++) { const d = u32(); dims.push(d); prod *= d; }
      if (prod > 5000000 || pos + prod * 4 > buf.length) { pos = save; break; }
      pos += prod * 4;
      attrs.push({ t: 'f32', dims });
    } else break;
  }
  nodes.push({ nameOff, name, tag, refs, attrs });
  lastGood = pos;
}
console.log('parsed nodes:', nodes.length, ' final pos=0x' + lastGood.toString(16), ' filesize=0x' + buf.length.toString(16));
for (const nd of nodes.slice(0, 40))
  console.log(`@${nd.nameOff.toString(16).padStart(6, '0')} ${nd.name} tag=0x${nd.tag.toString(16).padStart(4, '0')} refs=[${nd.refs.join(',')}] attrs=${nd.attrs.map(a => a.t === 'int' ? `int[${a.cnt}]=${JSON.stringify(a.vals)}` : `f32[${a.dims.join('x')}]`).join(' ')}`);
console.log('...');
for (const nd of nodes.slice(-25))
  console.log(`@${nd.nameOff.toString(16).padStart(6, '0')} ${nd.name} tag=0x${nd.tag.toString(16).padStart(4, '0')} refs=[${nd.refs.join(',')}] attrs=${nd.attrs.map(a => a.t === 'int' ? `int[${a.cnt}]=${JSON.stringify(a.vals)}` : `f32[${a.dims.join('x')}]`).join(' ')}`);
// 未解析到的尾部
console.log('tail bytes:', Array.from(buf.slice(lastGood, Math.min(buf.length, lastGood + 64))).map(x => x.toString(16).padStart(2, '0')).join(' '));
