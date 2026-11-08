// 检查 dconv2/3/5、uconv3 完整 tail 区间内所有 04 头，确定真实维度
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

// 全部 type-04 候选头（04 后跟 1~4 个递减合理 u32 dim，且数据区数值有界）
console.log('=== 全文件 04 头扫描（dim1<=4, dim2..4 in [1..1024]，数据>256 floats，f0/fLast 有界） ===');
for (let i = 0; i + 5 < buf.length; i++) {
  if (buf[i] !== 0x04) continue;
  for (let rank = 1; rank <= 4; rank++) {
    const dims = []; let prod = 1; let bad = false;
    for (let k = 0; k < rank; k++) {
      const d = dv.getUint32(i + 1 + k * 4, true);
      if (k === 0 && d > 8) { bad = true; break; }        // 第一维是 kernel，小
      if (d < 1 || d > 4096) { bad = true; break; }
      dims.push(d); prod *= d;
    }
    if (bad) continue;
    if (prod < 256) continue;                              // 只要大张量
    const doff = i + 1 + rank * 4;
    if (doff + prod * 4 > buf.length) continue;
    const f0 = dv.getFloat32(doff, true);
    const fl = dv.getFloat32(doff + (prod - 1) * 4, true);
    if (!isFinite(f0) || !isFinite(fl) || Math.abs(f0) > 100 || Math.abs(fl) > 100) continue;
    console.log(`@${i.toString(16).padStart(6, '0')} rank=${rank} [${dims.join(',')}] prod=${prod} f0=${f0.toFixed(4)} fLast=${fl.toFixed(4)}`);
  }
}
