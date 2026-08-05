# Vendored JavaScript dependencies

零构建链约束下（docs/WEB_DESIGN.md §2），SPA 的全部第三方依赖以 ESM 单文件
vendor 于此，禁止新增其他包。更新任一依赖 = 显式 PR：下载新版本、核对
SHA-256、更新本表。

| 包 | 版本 | 文件 | SHA-256 | 用途 |
|---|---|---|---|---|
| marked | 18.0.9 | `static/vendor/marked.esm.js` | `2c9b2113f8ef95ad21349952644fc1b3f759f0f92aff2ad4016a574b5ce19de5` | assistant 文本 markdown → HTML |
| dompurify | 3.4.13 | `static/vendor/purify.es.mjs` | `1939de7b9b248a4ffdf7f8065af45116a1babb96362eaf84d8f9fc3756c26fad` | HTML sanitize 白名单（XSS 防线之一） |
| highlight.js | 11.11.1 | `static/vendor/highlight.es.min.js` | `7865839949f0764d9e0a21e311a4e2c42633eeaee8ca5ec127b86438565731fe` | 代码块 / diff 行内语法高亮（common 语言包） |

来源：jsdelivr（npm 官方包 / 官方 cdn-release 仓库的 CDN 镜像）。

```bash
curl -sfL -o static/vendor/marked.esm.js  https://cdn.jsdelivr.net/npm/marked@<ver>/lib/marked.esm.js
curl -sfL -o static/vendor/purify.es.mjs  https://cdn.jsdelivr.net/npm/dompurify@<ver>/dist/purify.es.mjs
curl -sfL -o static/vendor/highlight.es.min.js https://cdn.jsdelivr.net/gh/highlightjs/cdn-release@<ver>/build/es/highlight.min.js
shasum -a 256 static/vendor/*
```
