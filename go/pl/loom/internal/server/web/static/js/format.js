// format.js — 展示格式化小工具。

export function fmtTokens(n) {
  if (n == null || isNaN(n)) return "";
  if (n >= 1_000_000) return (n / 1_000_000).toFixed(1) + "M";
  if (n >= 1000) return (n / 1000).toFixed(1) + "k";
  return String(n);
}

export function fmtBytes(n) {
  if (n == null || isNaN(n)) return "";
  if (n >= 1 << 20) return (n / (1 << 20)).toFixed(1) + " MB";
  if (n >= 1 << 10) return (n / (1 << 10)).toFixed(1) + " KB";
  return n + " B";
}

// 与后端 agent.estTokens 同算法（bytes/4，图片按 1500 token 保守估计），
// 用于 snapshot 未带 occupancy 字段时的首屏兜底估算。
export function estTranscriptTokens(messages) {
  let total = 0;
  for (const m of messages || []) {
    for (const p of m.parts || []) {
      switch (p.kind) {
        case "text":
          total += (p.text || "").length;
          break;
        case "reasoning":
          total += ((p.reasoning && p.reasoning.text) || "").length;
          break;
        case "tool_call": {
          // arguments 是 json.RawMessage，到前端已是解析后的对象；
          // Go 端按原始 JSON 字节计，这里用序列化长度对齐口径
          const args = p.tool_call ? p.tool_call.arguments : null;
          if (args != null) {
            total += typeof args === "string" ? args.length : JSON.stringify(args).length;
          }
          break;
        }
        case "tool_result":
          for (const c of (p.tool_result && p.tool_result.content) || []) {
            if (c.kind === "text") total += (c.text || "").length;
            else if (c.kind === "image") total += 1500 * 4;
          }
          break;
        case "image":
          total += 1500 * 4;
          break;
        default:
          break;
      }
    }
  }
  return Math.round(total / 4);
}

export function relTime(iso) {
  const t = new Date(iso);
  if (isNaN(t)) return "";
  const s = Math.max(0, (Date.now() - t.getTime()) / 1000);
  if (s < 60) return "just now";
  if (s < 3600) return Math.floor(s / 60) + "m ago";
  if (s < 86400) return Math.floor(s / 3600) + "h ago";
  if (s < 86400 * 7) return Math.floor(s / 86400) + "d ago";
  return t.toLocaleDateString();
}

export function shortId(id) {
  if (!id) return "";
  return id.length > 12 ? id.slice(0, 12) + "…" : id;
}
