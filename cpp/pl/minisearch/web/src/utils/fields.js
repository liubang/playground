/**
 * FieldValue 提取：后端动态类型字段值 {s,n,v:{data}} 转可读文本。
 */
export function fv(val) {
  if (val == null) return ''
  if (typeof val === 'string') return val
  if (typeof val === 'number') return String(val)
  if (typeof val === 'boolean') return String(val)
  if (val.v != null && val.v.data) return '[' + val.v.data.length + ' floats]'
  if (val.s != null) return val.s
  if (val.n != null) return String(val.n)
  if (val.v != null) return '[vector]'
  return JSON.stringify(val)
}

/** 文档详情里的字段原始值（编辑用）：返回 { kind, value } 便于表单回填 */
export function fvKind(val) {
  if (val == null) return { kind: 's', value: '' }
  if (typeof val === 'string') return { kind: 's', value: val }
  if (typeof val === 'number') return { kind: 'n', value: String(val) }
  if (val.v != null && val.v.data) return { kind: 'v', value: val.v.data.length }
  if (val.s != null) return { kind: 's', value: val.s }
  if (val.n != null) return { kind: 'n', value: String(val.n) }
  if (val.v != null) return { kind: 'v', value: 0 }
  return { kind: 's', value: JSON.stringify(val) }
}

/** 从字段名猜测内容正文（搜索结果展示用） */
export function pickBody(fields) {
  if (!fields) return ''
  for (const key of ['content', 'body', 'text', 'markdown', 'content_md', 'Content', 'Body']) {
    const v = fv(fields[key])
    if (v) return v
  }
  // 兜底：取第一个较长的 string 字段
  for (const v of Object.values(fields)) {
    const s = fv(v)
    if (s && s.length > 20) return s
  }
  return ''
}

/** 从字段名猜测标题 */
export function pickTitle(fields) {
  if (!fields) return ''
  for (const key of ['title', 'Title', 'name', 'Name', 'headline', 'doc_name']) {
    const v = fv(fields[key])
    if (v) return v
  }
  return ''
}
