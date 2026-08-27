// 粗略 C/C++ 括号配平自检：剥掉注释与字符串字面量后统计 {} 配平。
// 用于本地无 ESP-IDF 工具链时的快速结构检查（编译权威仍是 CI）。
const fs = require('fs');

function strip(src) {
  let out = '';
  let i = 0;
  const n = src.length;
  while (i < n) {
    const c = src[i];
    if (c === '/' && src[i + 1] === '/') {
      while (i < n && src[i] !== '\n') i++;
      continue;
    }
    if (c === '/' && src[i + 1] === '*') {
      i += 2;
      while (i < n && !(src[i] === '*' && src[i + 1] === '/')) i++;
      i += 2;
      continue;
    }
    if (c === '"') {
      i++;
      while (i < n) {
        if (src[i] === '\\') { i += 2; continue; }
        if (src[i] === '"') { i++; break; }
        i++;
      }
      continue;
    }
    if (c === "'") {
      i++;
      while (i < n) {
        if (src[i] === '\\') { i += 2; continue; }
        if (src[i] === "'") { i++; break; }
        i++;
      }
      continue;
    }
    out += c;
    i++;
  }
  return out;
}

function balance(s) {
  let d = 0;
  for (const ch of s) {
    if (ch === '{') d++;
    else if (ch === '}') d--;
  }
  return d;
}

const file = process.argv[2];
const src = fs.readFileSync(file, 'utf8');
const stripped = strip(src);
console.log(`${file} file balance: ${balance(stripped)}`);

if (process.argv[3]) {
  const start = stripped.indexOf(process.argv[3]);
  const endMarker = process.argv[4] || '';
  const end = endMarker ? stripped.indexOf(endMarker, start) : -1;
  const seg = stripped.slice(start, end > start ? end : undefined);
  let d = 0;
  let firstNegative = -1;
  for (let k = 0; k < seg.length; k++) {
    const ch = seg[k];
    if (ch === '{') d++;
    else if (ch === '}') {
      d--;
      if (d < 0 && firstNegative < 0) firstNegative = k;
    }
  }
  console.log(
    `segment [${process.argv[3]}] balance: ${d}, firstNegativeAt: ${firstNegative}`,
  );
}

