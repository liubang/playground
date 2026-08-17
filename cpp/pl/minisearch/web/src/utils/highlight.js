/**
 * 命中词高亮：在已渲染（sanitize 过）的 DOM 内对文本节点做关键词高亮。
 * 只处理普通文本节点，跳过 <mark>/<script>/<style> 等内部，避免嵌套标记。
 */
export function highlightElement(root, terms) {
  if (!root || !terms || terms.length === 0) return
  const cleaned = terms
    .map((t) => String(t).trim())
    .filter((t) => t.length > 0)
    .sort((a, b) => b.length - a.length) // 长词优先，避免短词先命中吃掉长词
  if (cleaned.length === 0) return

  const skipTags = new Set(['MARK', 'SCRIPT', 'STYLE', 'CODE', 'PRE', 'A'])
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, {
    acceptNode(node) {
      if (skipTags.has(node.parentElement?.tagName)) return NodeFilter.FILTER_REJECT
      return NodeFilter.FILTER_ACCEPT
    },
  })

  const nodes = []
  while (walker.nextNode()) nodes.push(walker.currentNode)

  for (const node of nodes) {
    const text = node.nodeValue
    const lower = text.toLowerCase()
    let hit = false
    for (const term of cleaned) {
      if (lower.includes(term.toLowerCase())) {
        hit = true
        break
      }
    }
    if (!hit) continue

    // 收集所有命中区间（按 term 逐个匹配，取非重叠、从左到右）
    const ranges = []
    for (const term of cleaned) {
      const tl = term.toLowerCase()
      let idx = 0
      while (idx < text.length) {
        const at = lower.indexOf(tl, idx)
        if (at === -1) break
        const end = at + term.length
        const overlap = ranges.some(([s, e]) => at < e && end > s)
        if (!overlap) ranges.push([at, end])
        idx = at + 1
      }
    }
    if (ranges.length === 0) continue
    ranges.sort((a, b) => a[0] - b[0])

    const frag = document.createDocumentFragment()
    let cursor = 0
    for (const [s, e] of ranges) {
      if (s > cursor) frag.appendChild(document.createTextNode(text.slice(cursor, s)))
      const mark = document.createElement('mark')
      mark.className = 'mss-mark'
      mark.textContent = text.slice(s, e)
      frag.appendChild(mark)
      cursor = e
    }
    if (cursor < text.length) frag.appendChild(document.createTextNode(text.slice(cursor)))
    node.parentNode.replaceChild(frag, node)
  }
}

/**
 * 从查询文本提取高亮词。
 * @param {string} query
 * @param {Array<{term: string}>} tokens analyze API 的 token（可选，更准）
 */
export function extractTerms(query, tokens) {
  if (tokens && tokens.length > 0) {
    const seen = new Set()
    const out = []
    for (const t of tokens) {
      const term = (t.term || '').trim()
      if (term && !seen.has(term)) {
        seen.add(term)
        out.push(term)
      }
    }
    if (out.length > 0) return out
  }
  if (!query) return []
  // fallback：按空白与常见标点切分；中文连续串保留为整段
  return query
    .split(/[\s,，。.!！?？;；:：'"“”‘’()[\]【】{}<>《》/\\|]+/)
    .map((s) => s.trim())
    .filter((s) => s.length > 0)
    .slice(0, 12)
}
