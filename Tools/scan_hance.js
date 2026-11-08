// 扫描 .hance 文件中所有长度前缀 ASCII 字符串 + 结构化解析尝试
'use strict';
const fs = require('fs');
const buf = fs.readFileSync(process.argv[2]);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);

// 1) 全文件扫描可打印 ASCII 串（含前缀长度自洽校验）
const strs = [];
for (let i = 0; i + 4 < buf.length; i++) {
  const n = dv.getUint32(i, true);
  if (n >= 2 && n <= 64 && i + 4 + n <= buf.length) {
    let ok = true;
    for (let j = 0; j < n; j++) {
      const c = buf[i + 4 + j];
      if (c < 0x20 || c > 0x7e) { ok = false; break; }
    }
    if (ok) {
      const s = buf.slice(i + 4, i + 4 + n).toString('latin1');
      // 必须像标识符
      if (/^[A-Za-z_][A-Za-z0-9_.]*$/.test(s)) strs.push([i, n, s]);
    }
  }
}
console.log('=== 长度前缀字符串 ===');
for (const [off, n, s] of strs) console.log(off.toString(16).padStart(8, '0'), n, s);
console.log('total:', strs.length, ' filesize:', buf.length.toString(16));
