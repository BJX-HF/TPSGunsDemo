#!/usr/bin/env node
/**
 * verify-debug-html.js —— 后坐力调试文档（Docs/Recoil/后坐力系统调试.html）的结构自检
 *
 * 为什么需要它
 * ------------
 * 这份 HTML 是**人工维护**的（不是从 Markdown 生成的），而且很长（1800+ 行、40+ 张表）。
 * 每次往里面加一节，最容易出的三类问题都不会报错、只会"看起来怪"：
 *   1. 标签没配对（`<tr>` 少一个闭合）→ 后面的表格全塌进前一列
 *   2. 转义漏了（写成 `&amp;amp;` / 裸 `&`）→ 页面显示成字面量
 *   3. 锚点断了（`href="#spread"` 但 `id` 改了名）→ 目录点了没反应
 * 这三类问题**肉眼很难发现**，所以做成脚本。
 *
 * 用法
 * ----
 *   & "C:\Users\yeyuxiang\.workbuddy\binaries\node\versions\22.22.2-3\node.exe" `
 *       "E:\TPSGunsDemo\Docs\Recoil\Tools\verify-debug-html.js"
 *
 * 退出码：0 = 全部通过；1 = 有检查组失败（可直接接进 CI / run-all-checks）。
 * 输出同时写一份到 Docs/Recoil/verify-debug-html.log，方便贴进验收请求。
 */

'use strict';

const fs = require('fs');
const path = require('path');

const HTML_PATH = path.resolve(__dirname, '..', '后坐力系统调试.html');
const LOG_PATH = path.resolve(__dirname, '..', 'verify-debug-html.log');

const html = fs.readFileSync(HTML_PATH, 'utf8');
const L = [];
let failedGroups = 0;

const count = (re) => (html.match(re) || []).length;

// ---------------------------------------------------------------- 1) 标签配对
L.push('=== 1) tag balance ===');
const TAGS = [
  'table', 'thead', 'tbody', 'tr', 'th', 'td', 'div', 'pre', 'code',
  'p', 'h1', 'h2', 'h3', 'h4', 'ul', 'ol', 'li', 'span', 'footer', 'header',
];
{
  let bad = 0;
  for (const t of TAGS) {
    const open = count(new RegExp('<' + t + '(\\s|>)', 'g'));
    const close = count(new RegExp('</' + t + '>', 'g'));
    const ok = open === close;
    if (!ok) { bad++; }
    L.push(`  ${ok ? 'OK  ' : '*** '} <${t}> open=${open} close=${close}`);
  }
  if (bad) { failedGroups++; L.push(`  => ${bad} mismatched tag(s)`); }
}

// ---------------------------------------------------------------- 2) 转义
L.push('=== 2) escape sanity (all must be 0) ===');
{
  const CHECKS = [
    ['双重转义 &amp;amp;', /&amp;amp;/g],
    ['误转义 &lt;span', /&lt;span/g],
    ['双重转义 &amp;lt;', /&amp;lt;/g],
    ['双重转义 &amp;#', /&amp;#/g],
    ['Markdown 加粗残留 **', /\*\*/g],
    ['零宽空格 U+200B', /\u200b/g],
    ['零宽非连接符 U+200C', /\u200c/g],
  ];
  let bad = 0;
  for (const [name, re] of CHECKS) {
    const n = count(re);
    if (n) { bad++; }
    L.push(`  ${n === 0 ? 'OK  ' : '*** '} ${name} = ${n}`);
  }
  if (bad) { failedGroups++; L.push(`  => ${bad} escape problem(s)`); }
}

// ---------------------------------------------------------------- 3) 裸 &
L.push('=== 3) bare "&" (not part of an entity) ===');
{
  const reAmp = /&(?!(#[0-9]+;|#x[0-9a-fA-F]+;|[a-zA-Z][a-zA-Z0-9]*;))/g;
  const hits = [];
  let m;
  while ((m = reAmp.exec(html)) !== null) {
    const line = html.slice(0, m.index).split('\n').length;
    hits.push('    L' + line + ': ' + html.slice(Math.max(0, m.index - 45), m.index + 25).replace(/\n/g, ' '));
  }
  L.push(`  ${hits.length === 0 ? 'OK  ' : '*** '} bare & count = ${hits.length}`);
  hits.slice(0, 20).forEach((h) => L.push(h));
  if (hits.length) failedGroups++;
}

// ---------------------------------------------------------------- 4) 锚点
L.push('=== 4) anchor integrity ===');
{
  const ids = new Set();
  for (const mm of html.matchAll(/id="([^"]+)"/g)) { ids.add(mm[1]); }
  const hrefs = [];
  for (const mm of html.matchAll(/href="#([^"]+)"/g)) { hrefs.push(mm[1]); }
  const missing = [...new Set(hrefs)].filter((h) => !ids.has(h));
  L.push(`  ids = ${ids.size} (${[...ids].join(', ')})`);
  L.push(`  distinct hrefs = ${new Set(hrefs).size}`);
  L.push(`  ${missing.length === 0 ? 'OK  ' : '*** '} hrefs without target: ${missing.join(', ') || 'none'}`);
  if (missing.length) failedGroups++;
}

// ---------------------------------------------------------------- 5) 每张表的行/列数
L.push('=== 5) per-table row/cell counts ===');
{
  let ti = 0;
  for (const mm of html.matchAll(/<table>([\s\S]*?)<\/table>/g)) {
    ti++;
    const rows = (mm[1].match(/<tr\b/g) || []).length;
    const cells = (mm[1].match(/<t[hd]\b/g) || []).length;
    const line = html.slice(0, mm.index).split('\n').length;
    const partial = html.slice(mm.index, mm.index + 120).replace(/\s+/g, ' ');
    L.push(`  table#${ti} @L${line}: tr=${rows} cells=${cells}  ${partial.slice(0, 90)}`);
  }
  L.push(`  total tables = ${ti}`);
}

// ---------------------------------------------------------------- 6) 总体统计
L.push('=== 6) totals ===');
L.push(`  lines              = ${html.split('\n').length}`);
for (const t of ['h2', 'h3', 'h4', 'table', 'tr', 'pre']) {
  L.push(`  <${t}> count       = ${count(new RegExp('<' + t + '\\b', 'g'))}`);
}

// ---------------------------------------------------------------- 7) 散布章节（§11）
L.push('=== 7) section 11 (spread) ===');
{
  const idx = html.indexOf('<h2 id="spread"');
  const s = idx >= 0 ? html.slice(idx) : '';
  if (!s) {
    failedGroups++;
    L.push('  *** <h2 id="spread"> not found');
  } else {
    L.push(`  h3 inside  = ${(s.match(/<h3\b/g) || []).length}`);
    L.push(`  h4 inside  = ${(s.match(/<h4\b/g) || []).length}`);
    L.push(`  tables     = ${(s.match(/<table\b/g) || []).length}`);
    L.push(`  tr         = ${(s.match(/<tr\b/g) || []).length}`);
    L.push(`  pre        = ${(s.match(/<pre\b/g) || []).length}`);
  }
}

// ---------------------------------------------------------------- 收尾
L.push('=== overall ===');
L.push(failedGroups === 0 ? '  ALL CHECKS PASSED' : `  ${failedGroups} CHECK GROUP(S) FAILED`);

const text = L.join('\n');
fs.writeFileSync(LOG_PATH, text, 'utf8');
console.log(text);
process.exit(failedGroups === 0 ? 0 : 1);
