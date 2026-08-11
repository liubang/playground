// sidebar.js — 工作区树形侧栏（docs/WORKSPACE_DESIGN.md §11.3）。
// 顶层是每个工作区一个可折叠的文件夹节点，组内是该工作区的会话；
// 会话条目保留子 agent 层级缩进与悬停操作（归档/删除）。

import { el } from './blocks.js'
import { relTime, shortId } from '../format.js'
import { icon } from '../icons.js'

const COLLAPSE_KEY = 'loom_ws_collapsed'

export class Sidebar {
  // onSelect(id, action 由 onAction 处理)：action ∈ "archive" | "unarchive" | "delete"
  // onNewSession(workspaceId)：在该工作区下新建会话（"" = 默认工作区）。
  // onDeleteWorkspace(workspaceId)：删除工作区并级联删除其下全部会话。
  constructor(listEl, { onSelect, onAction, onNewSession, onDeleteWorkspace }) {
    this.listEl = listEl
    this.onSelect = onSelect
    this.onAction = onAction
    this.onNewSession = onNewSession
    this.onDeleteWorkspace = onDeleteWorkspace
    this.activeId = null
    this.archivedView = false // 归档视图：条目显示取消归档而非归档
    this.workspaces = []
    this._lastSessions = []
    this.collapsed = this._loadCollapsed()
  }

  _loadCollapsed() {
    try {
      return new Set(JSON.parse(sessionStorage.getItem(COLLAPSE_KEY) || '[]'))
    } catch {
      return new Set()
    }
  }

  _saveCollapsed() {
    try {
      sessionStorage.setItem(COLLAPSE_KEY, JSON.stringify([...this.collapsed]))
    } catch {
      /* ignore */
    }
  }

  // setActive(id, {scroll})：scroll=true 时（用户主动打开会话）把条目滚动
  // 到可视区域，并在其所在组被折叠时展开该组——主动选中的会话必须看得见。
  // 轮询刷新后的重设（scroll=false）不滚动也不动折叠态：用户主动折叠包含
  // 当前会话的组是合法选择（组名着色 + 待审批徽标保持定位能力，见 app.css
  // .ws-node.has-active），不能被后台轮询悄悄撤销。
  setActive(id, { scroll = false } = {}) {
    this.activeId = id
    if (scroll) {
      const sess = this._lastSessions.find((s) => s.id === id)
      if (sess) {
        const wsId = sess.workspace_id || ''
        if (this.collapsed.has(wsId)) {
          this.collapsed.delete(wsId)
          this._saveCollapsed()
          this.render(this._lastSessions, this.workspaces)
        }
      }
    }
    for (const item of this.listEl.querySelectorAll('.sess-item')) {
      const on = item.dataset.id === id
      item.classList.toggle('is-active', on)
      if (on && scroll) item.scrollIntoView({ block: 'nearest' })
    }
  }

  // focusWorkspace 把最近活跃的工作区组设为视觉焦点：展开它、收起其他组。
  // 启动时调用一次（desktop 每次启动都是新的页面会话，折叠态不带入）。
  // wsId 为空（无历史会话）时不做任何事，保持全部展开。
  focusWorkspace(wsId) {
    if (!wsId) return
    let found = false
    for (const w of this.workspaces) {
      if (w.id === wsId) found = true
      else this.collapsed.add(w.id)
    }
    if (!found) return
    this.collapsed.delete(wsId)
    this._saveCollapsed()
    this.render(this._lastSessions, this.workspaces)
  }

  // render(sessions, workspaces)：sessions 携带 workspace_id；workspaces 为
  // [{id, name, root_path, ...}]（newest first）。顶层按工作区分组渲染。
  render(sessions, workspaces) {
    this._lastSessions = sessions || []
    this.workspaces = workspaces || []
    this.listEl.textContent = ''

    const byWs = new Map() // workspace_id（"" = 默认/历史）→ [sessions]
    for (const s of this._lastSessions) {
      const k = s.workspace_id || ''
      if (!byWs.has(k)) byWs.set(k, [])
      byWs.get(k).push(s)
    }
    // 工作区顺序：已注册 workspaces（newest first），再补上有会话但未注册
    // 的（默认工作区 ""）。
    const ordered = this.workspaces.map((w) => w.id)
    for (const k of byWs.keys()) {
      if (!ordered.includes(k)) ordered.push(k)
    }
    const ids = new Set(this._lastSessions.map((s) => s.id))
    for (const wsId of ordered) {
      const ws = this.workspaces.find((w) => w.id === wsId)
      const wsSessions = byWs.get(wsId) || []
      // 归档视图是只读历史：跳过无归档会话的工作区组（空组只用于活跃视图的新建入口）。
      if (this.archivedView && wsSessions.length === 0) continue
      this._appendGroup(wsId, ws, wsSessions, ids)
    }
  }

  _appendGroup(wsId, ws, wsSessions, ids) {
    // wsId 非空但查无实体 = 所属工作区已被删除。级联删除落地前的历史数据
    // 可能留下这种悬空会话，仍归入只读的「已删除的工作区」分组展示；
    // 删光组内会话后该分组自然消失。
    const name = ws ? ws.name : wsId ? '已删除的工作区' : '默认工作区'
    const collapsed = this.collapsed.has(wsId)
    // 组内待审批会话数（折叠时唯一可见的求救信号）与当前会话归属标记。
    const attnCount = wsSessions.filter((s) => s.state === 'awaiting_approval').length
    const hasActive = wsSessions.some((s) => s.id === this.activeId)
    const group = el('div', 'ws-group')

    const node = el(
      'div',
      'ws-node' + (collapsed ? ' is-collapsed' : '') + (hasActive ? ' has-active' : '')
    )
    node.dataset.wsId = wsId
    const caret = el('span', 'ws-caret')
    caret.innerHTML = icon(collapsed ? 'caret-right' : 'caret-down')
    node.appendChild(caret)
    const wsIc = el('span', 'ws-icon')
    wsIc.innerHTML = icon(collapsed ? 'folder' : 'folder-open')
    node.appendChild(wsIc)
    const nameEl = el('span', 'ws-name', name)
    if (ws && ws.root_path) nameEl.title = ws.root_path
    node.appendChild(nameEl)
    node.appendChild(el('span', 'ws-count', String(wsSessions.length)))
    // 待审批徽标：折叠/展开都显示，琥珀色计数，点击组名即展开定位。
    if (attnCount > 0) {
      const attn = el('span', 'ws-attn', String(attnCount))
      attn.title = `${attnCount} 个会话等待审批`
      node.appendChild(attn)
    }
    // 新建入口只在活跃视图显示（归档视图是只读历史）。
    if (!this.archivedView) {
      const newBtn = el('button', 'ws-new')
      newBtn.innerHTML = icon('file-document-plus')
      newBtn.type = 'button'
      newBtn.title = '在该工作区新建会话'
      newBtn.onclick = (e) => {
        e.stopPropagation()
        this.onNewSession(wsId)
      }
      node.appendChild(newBtn)
      // 删除入口只对已注册且非 default 的工作区显示（default 不可删；
      // 悬空工作区组没有实体可删）。
      if (ws && !ws.is_default) {
        const delBtn = el('button', 'ws-del')
        delBtn.innerHTML = icon('trash')
        delBtn.type = 'button'
        delBtn.title = '删除工作区（其下会话一并删除，磁盘目录保留）'
        delBtn.onclick = (e) => {
          e.stopPropagation()
          this.onDeleteWorkspace(wsId)
        }
        node.appendChild(delBtn)
      }
    }
    node.onclick = () => this._toggle(wsId)
    group.appendChild(node)

    if (!collapsed) {
      const wrap = el('div', 'ws-sessions')
      if (wsSessions.length === 0) {
        wrap.appendChild(el('div', 'ws-empty', '无会话'))
      } else {
        this._renderTree(wrap, wsSessions, ids)
      }
      group.appendChild(wrap)
    }
    this.listEl.appendChild(group)
  }

  // revealWorkspace(wsId)：展开指定工作区组并把它与组内当前会话滚动进
  // 可视区域（header 面包屑点击定位；侧栏本体的展开由调用方负责）。
  revealWorkspace(wsId) {
    const key = wsId || ''
    if (this.collapsed.delete(key)) this._saveCollapsed()
    this.render(this._lastSessions, this.workspaces)
    const node = this.listEl.querySelector(`.ws-node[data-ws-id="${key}"]`)
    if (node) node.scrollIntoView({ block: 'nearest' })
    const active = this.listEl.querySelector('.sess-item.is-active')
    if (active) active.scrollIntoView({ block: 'nearest' })
  }

  _toggle(wsId) {
    if (this.collapsed.has(wsId)) this.collapsed.delete(wsId)
    else this.collapsed.add(wsId)
    this._saveCollapsed()
    this.render(this._lastSessions, this.workspaces)
  }

  // 组内会话层级：子 agent 会话缩进挂在父会话下方，父会话不在组内时按
  // 顶层渲染（分页边界兜底）。
  _renderTree(wrap, wsSessions, ids) {
    const childrenOf = new Map()
    const tops = []
    for (const s of wsSessions) {
      if (s.parent_session_id && ids.has(s.parent_session_id)) {
        let arr = childrenOf.get(s.parent_session_id)
        if (!arr) childrenOf.set(s.parent_session_id, (arr = []))
        arr.push(s)
      } else {
        tops.push(s)
      }
    }
    for (const s of tops) {
      this._appendItem(wrap, s, false)
      for (const c of childrenOf.get(s.id) || []) this._appendItem(wrap, c, true)
    }
  }

  _appendItem(wrap, s, isChild) {
    const item = el(
      'button',
      'sess-item' + (s.id === this.activeId ? ' is-active' : '') + (isChild ? ' is-child' : '')
    )
    item.dataset.id = s.id
    item.title = (s.title || shortId(s.id)) + (s.model_name ? ` · ${s.model_name}` : '')
    if (isChild) {
      const cm = el('span', 'child-mark')
      cm.innerHTML = icon('robot')
      cm.title = '子智能体会话'
      item.appendChild(cm)
    }
    // live 状态灯：awaiting_approval 用琥珀色呼吸灯（最需要抢注意力），
    // running/cancelling 用绿色；其余状态不显示，保持列表安静。
    const st = s.state || ''
    if (st === 'awaiting_approval' || st === 'running' || st === 'cancelling') {
      const dot = el('span', 'st-dot ' + (st === 'awaiting_approval' ? 'is-attn' : 'is-run'))
      dot.title = st === 'awaiting_approval' ? '等待审批' : '运行中'
      item.appendChild(dot)
    }
    item.appendChild(el('span', 't', s.title || shortId(s.id)))
    item.appendChild(el('span', 'rt', relTime(s.created_at)))
    // 悬停操作：归档/取消归档 + 删除（不占常态宽度，hover 时替换时间戳）
    const acts = el('span', 'acts')
    const archBtn = el('button', 'act')
    archBtn.innerHTML = icon(this.archivedView ? 'rotate-left' : 'box-archive')
    archBtn.title = this.archivedView ? '取消归档' : '归档'
    archBtn.onclick = (e) => {
      e.stopPropagation()
      this.onAction(s.id, this.archivedView ? 'unarchive' : 'archive')
    }
    const delBtn = el('button', 'act act-del')
    delBtn.innerHTML = icon('trash')
    delBtn.title = '删除会话'
    delBtn.onclick = (e) => {
      e.stopPropagation()
      this.onAction(s.id, 'delete')
    }
    acts.appendChild(archBtn)
    acts.appendChild(delBtn)
    item.appendChild(acts)
    item.onclick = () => this.onSelect(s.id)
    wrap.appendChild(item)
  }
}
