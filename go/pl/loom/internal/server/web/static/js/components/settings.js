// settings.js — 设置面板（config.yaml 的图形化编辑）。
// 设计：字段用声明式 spec 描述（key 即 config.yaml 的键路径），同一套 spec
// 驱动渲染与收集 —— 新增配置项只需加一行 spec。控件归属最近的
// [data-cfg-scope] 容器，嵌套结构（provider → models）因此可以复用同一套
// 填充/收集逻辑而不串层。
//
// 空值语义与文件一致：留空 = 不写入该键（omitempty），默认值全部隐式。
// 密钥控件展示服务端的脱敏占位符，未修改时原样回传由服务端还原；点击眼睛
// 按钮经 POST /v1/config/reveal 按需取回明文（明文不随整体配置下发）。
//
// 列表类 tab（模型 / MCP）是「概览 → 详情」两级导航：所有卡片 DOM 始终
// 挂载在同一个列表容器里（收集逻辑因此不变），层级切换只改 CSS 类——
// 概览态卡片折叠成一行摘要，详情态只显示当前卡片。模型 tab 还有第三级
// （provider → 模型明细）。

import { el } from "./blocks.js";
import { icon, iconEl } from "../icons.js";
import { createSelect, closeSelects } from "./select.js";

// 与 internal/config/edit.go 的 SecretMask 保持一致。
const SECRET_MASK = "••••••••••";

// UI 未管理的配置路径：保存时从已加载的配置原样带回，避免静默丢失
// （merge 的语义是「未提供的 key = 从文件删除」）。PRESERVE_PATHS 覆盖
// 已知但 UI 未做编辑器的嵌套键；KNOWN_TOP_KEYS 之外的顶层键（未来新增
// 的配置节）也一律保留 —— UI 完整性不该是正确性的前提。
// skills.disabled 由技能 tab 的禁用开关经专用端点直写（不在表单里），
// 保存时原样带回。
const PRESERVE_PATHS = ["ui.keymap", "skills.disabled"];
const KNOWN_TOP_KEYS = new Set([
  "default", "providers", "limits", "context", "runaway", "prompt",
  "skills", "rules", "approval", "tracing", "storage", "logging",
  "ui", "subagent", "memory", "image", "mcp_servers", "workspaces",
]);

// ---------- 路径工具 ----------

function getPath(obj, path) {
  return path.split(".").reduce((o, k) => (o == null ? undefined : o[k]), obj);
}

function setPath(obj, path, value) {
  const keys = path.split(".");
  let o = obj;
  for (let i = 0; i < keys.length - 1; i++) {
    if (typeof o[keys[i]] !== "object" || o[keys[i]] === null) o[keys[i]] = {};
    o = o[keys[i]];
  }
  o[keys[keys.length - 1]] = value;
}

function preserveUnmanaged(cfg, orig) {
  for (const [k, v] of Object.entries(orig)) {
    if (!KNOWN_TOP_KEYS.has(k) && cfg[k] === undefined) cfg[k] = v;
  }
  for (const path of PRESERVE_PATHS) {
    const v = getPath(orig, path);
    if (v !== undefined && getPath(cfg, path) === undefined) setPath(cfg, path, v);
  }
}

// ---------- 字段控件 ----------

// spec: {key, label, hint, ph, type, options, step, rows, def, revealRef}
// type ∈ text | password | number | bool | tristate | select | textarea |
//       list-text（每行一项 → []string）| kv-text（每行 k=v → map）|
//       float-list（逗号分隔 → []number）
// revealRef: password 控件的明文定位（对象或返回对象的函数），见
//       POST /v1/config/reveal 的 secretReveal。
function makeControl(spec) {
  const t = spec.type || "text";
  let ctl;
  if (t === "select" || t === "tristate") {
    const opts = t === "tristate"
      ? [["", `默认（${spec.def || "开"}）`], ["true", "开"], ["false", "关"]]
      : spec.options;
    ctl = createSelect({ className: "set-input", options: opts });
  } else if (t === "bool") {
    ctl = el("input", "set-check");
    ctl.type = "checkbox";
  } else if (t === "textarea" || t === "list-text" || t === "kv-text") {
    ctl = el("textarea", "set-input mono");
    ctl.rows = spec.rows || 3;
    ctl.spellcheck = false;
  } else {
    ctl = el("input", "set-input");
    ctl.type = t === "password" ? "password" : t === "number" ? "number" : "text";
    if (spec.step) ctl.step = String(spec.step);
    ctl.spellcheck = false;
    ctl.autocomplete = "off";
  }
  if (spec.ph) ctl.placeholder = spec.ph;
  ctl.dataset.cfgKey = spec.key;
  ctl.dataset.cfgType = t;
  return ctl;
}

// onReveal: 可选，async () => 明文 | null（失败已自行提示）；仅 password
// 控件且当前值是掩码时参与——用户已输入新值时直接切换可见性即可。
function fieldRow(spec, onReveal) {
  const row = el("div", "set-row");
  row.appendChild(el("label", "set-label", spec.label));
  const body = el("div", "set-field");
  const ctl = makeControl(spec);
  if ((spec.type || "text") === "password") {
    // 密钥控件：眼睛按钮显示明文（掩码值先经 reveal 接口换回明文）
    const wrap = el("div", "set-secret");
    wrap.appendChild(ctl);
    const eye = el("button", "icon-btn set-eye");
    eye.type = "button";
    eye.title = "显示/隐藏";
    eye.innerHTML = icon("eye");
    eye.onclick = async () => {
      if (ctl.type === "text") {
        ctl.type = "password";
        eye.innerHTML = icon("eye");
        return;
      }
      if (ctl.value === SECRET_MASK && onReveal) {
        eye.disabled = true;
        const plaintext = await onReveal();
        eye.disabled = false;
        if (plaintext == null) return;
        ctl.value = plaintext;
      }
      ctl.type = "text";
      eye.innerHTML = icon("eye-slash");
    };
    wrap.appendChild(eye);
    body.appendChild(wrap);
  } else {
    body.appendChild(ctl);
  }
  if (spec.hint) body.appendChild(el("div", "set-hint", spec.hint));
  row.appendChild(body);
  return row;
}

// 控件归属最近的 scope 容器：嵌套卡片（provider → models）各收各的。
function ownControls(scopeEl) {
  return [...scopeEl.querySelectorAll("[data-cfg-key]")].filter(
    (c) => c.closest("[data-cfg-scope]") === scopeEl,
  );
}

function fillControl(ctl, value) {
  const t = ctl.dataset.cfgType;
  if (value === undefined || value === null) value = "";
  switch (t) {
    case "bool":
      ctl.checked = value === true;
      break;
    case "tristate":
      ctl.value = value === "" ? "" : String(value);
      break;
    case "list-text":
      ctl.value = (value || []).join("\n");
      break;
    case "kv-text":
      ctl.value = Object.entries(value || {}).map(([k, v]) => `${k}=${v}`).join("\n");
      break;
    case "float-list":
      ctl.value = (value || []).join(", ");
      break;
    default:
      ctl.value = value === "" ? "" : String(value);
  }
}

function fillScope(scopeEl, obj) {
  for (const ctl of ownControls(scopeEl)) fillControl(ctl, getPath(obj, ctl.dataset.cfgKey));
}

function collectControl(ctl, obj) {
  const key = ctl.dataset.cfgKey;
  switch (ctl.dataset.cfgType) {
    case "password": {
      if (ctl.value !== "") setPath(obj, key, ctl.value); // 密钥不 trim
      break;
    }
    case "number": {
      if (ctl.value.trim() !== "") setPath(obj, key, Number(ctl.value));
      break;
    }
    case "bool": {
      if (ctl.checked) setPath(obj, key, true); // false = 默认，不写入
      break;
    }
    case "tristate": {
      if (ctl.value !== "") setPath(obj, key, ctl.value === "true");
      break;
    }
    case "select": {
      if (ctl.value !== "") setPath(obj, key, ctl.value);
      break;
    }
    case "list-text": {
      const items = ctl.value.split("\n").map((s) => s.trim()).filter(Boolean);
      if (items.length) setPath(obj, key, items);
      break;
    }
    case "kv-text": {
      const m = {};
      for (const line of ctl.value.split("\n")) {
        const i = line.indexOf("=");
        if (i > 0 && line.slice(0, i).trim()) m[line.slice(0, i).trim()] = line.slice(i + 1).trim();
      }
      if (Object.keys(m).length) setPath(obj, key, m);
      break;
    }
    case "float-list": {
      const nums = ctl.value.split(/[,\s]+/).filter(Boolean).map(Number).filter((n) => !Number.isNaN(n));
      if (nums.length) setPath(obj, key, nums);
      break;
    }
    default: {
      const v = ctl.value.trim();
      if (v !== "") setPath(obj, key, v);
    }
  }
}

function collectScope(scopeEl, obj) {
  for (const ctl of ownControls(scopeEl)) collectControl(ctl, obj);
}

// ---------- 字段 spec（key 即 config.yaml 键路径） ----------

const EFFORT_OPTS = [
  ["", "默认（provider 决定）"],
  ["off", "off"], ["low", "low"], ["medium", "medium"], ["high", "high"],
];

const REASONING_FIELDS = [
  { key: "reasoning.effort", label: "推理强度", type: "select", options: EFFORT_OPTS },
  { key: "reasoning.budget_tokens", label: "推理 token 预算", type: "number", ph: "0", hint: "显式预算，>0 时优先于强度推导值" },
];

const PROVIDER_BASE_FIELDS = [
  { key: "type", label: "协议类型", type: "select", options: [["openai", "openai（兼容网关）"], ["anthropic", "anthropic（Messages API）"]] },
  { key: "base_url", label: "Base URL", ph: "https://api.deepseek.com/v1" },
  { key: "api_key", label: "API Key", type: "password", hint: "与「Key 环境变量」二选一；同时填写会报错" },
  { key: "api_key_env", label: "Key 环境变量", ph: "如 DEEPSEEK_API_KEY", hint: "只存变量名，启动时读取值" },
  { key: "default_model", label: "默认模型", ph: "留空取模型列表第一个" },
];

const PROVIDER_ADV_FIELDS = [
  { key: "wire_api", label: "请求协议", type: "select", options: [["", "默认"], ["chat", "chat（Chat Completions）"], ["responses", "responses（Responses API）"], ["messages", "messages（仅 anthropic）"]] },
  { key: "auth_type", label: "认证头方式", type: "select", options: [["", "默认（x-api-key）"], ["x-api-key", "x-api-key"], ["bearer", "bearer"]], hint: "仅 anthropic 类型有意义" },
  { key: "api_version", label: "协议版本头", hint: "仅 anthropic 类型；留空取内置版本" },
  { key: "max_retries", label: "失败重试次数", type: "number", ph: "2" },
  ...REASONING_FIELDS,
];

const MODEL_FIELDS = [
  { key: "name", label: "模型名", ph: "如 deepseek-chat" },
  { key: "context_window", label: "上下文窗口", type: "number", ph: "如 65536" },
  { key: "max_output_tokens", label: "单次输出上限", type: "number", ph: "如 8192" },
  { key: "wire_api", label: "协议覆盖", type: "select", options: [["", "跟随 provider"], ["chat", "chat"], ["responses", "responses"]] },
  { key: "window_utilization", label: "窗口利用率覆盖", type: "number", step: 0.01, ph: "跟随全局" },
  ...REASONING_FIELDS,
];

const TABS = [
  { id: "providers", label: "模型" }, // 自定义渲染（概览 → 详情两级）
  {
    id: "limits", label: "限额与保护", sections: [
      ["运行预算", [
        { key: "limits.max_input_tokens", label: "回退上下文窗口", type: "number", ph: "200000", hint: "模型未声明 context_window 时使用" },
        { key: "limits.max_output_tokens", label: "单次输出上限", type: "number", ph: "16384" },
        { key: "limits.max_cost_usd", label: "成本上限 (USD)", type: "number", step: 0.01, ph: "5.0", hint: "会话级累计估算成本，0 = 不限（需配置追踪成本费率）" },
        { key: "limits.max_tokens", label: "Token 总预算", type: "number", ph: "0", hint: "会话级累计 token，0 = 不限" },
        { key: "limits.max_tool_output_bytes", label: "工具输出保留字节", type: "number", ph: "49152" },
        { key: "limits.max_artifact_bytes", label: "Artifact 最大字节", type: "number", ph: "104857600" },
      ]],
      ["上下文压缩", [
        { key: "context.utilization", label: "窗口利用率", type: "number", step: 0.01, ph: "0.95" },
        { key: "context.compact_trigger_ratio", label: "压缩触发线", type: "number", step: 0.01, ph: "0.80" },
        { key: "context.compact_target_ratio", label: "压缩目标", type: "number", step: 0.01, ph: "0.50", hint: "必须小于触发线" },
        { key: "context.notice_levels", label: "占用提醒档位", type: "float-list", ph: "0.60, 0.75", hint: "逗号分隔，升序且小于触发线" },
      ]],
      ["失控检测", [
        { key: "runaway.max_repeated_calls", label: "重复调用上限", type: "number", ph: "3" },
        { key: "runaway.max_consecutive_failures", label: "连续失败上限", type: "number", ph: "5" },
        { key: "runaway.stall_warn_turns", label: "停滞提醒回合数", type: "number", ph: "10", hint: "0 = 关闭" },
        { key: "runaway.stall_timeout", label: "停滞看门狗", ph: "15m", hint: "Go duration 语法；0 = 关闭" },
      ]],
    ],
  },
  {
    id: "permission", label: "权限与审批", sections: [
      ["审批基线", [
        {
          key: "approval.mode", label: "审批模式", type: "select",
          options: [
            ["", "默认（on-request）"],
            ["on-request", "on-request · 沙箱内/工作区内免审批"],
            ["unless-dangerous", "unless-dangerous · 黑名单模式"],
            ["never", "never · 无人值守"],
          ],
          hint: "无规则/记忆命中时的决策策略",
        },
      ]],
      ["规则层", [
        { key: "rules.enabled", label: "启用规则", type: "tristate" },
        { key: "rules.builtin", label: "内置只读命令集", type: "tristate" },
        { key: "rules.project", label: "项目层规则", type: "tristate" },
        { key: "rules.project_allow", label: "项目层允许 allow 规则", type: "tristate", def: "关", hint: "不可信仓库只能收紧、不能放宽" },
        { key: "rules.persist_remembered", label: "持久化「始终允许」", type: "tristate", hint: "写入用户层规则文件供后续会话继承" },
      ]],
    ],
  },
  {
    id: "agent", label: "智能体", sections: [
      ["系统提示词", [
        { key: "prompt.extra", label: "附加指令", type: "textarea", hint: "追加到内置系统提示词末尾" },
        { key: "prompt.disable_builtin", label: "禁用内置提示词", type: "bool" },
        { key: "prompt.managed.name", label: "托管提示词名", hint: "Langfuse 托管提示词（需配置追踪）" },
        { key: "prompt.managed.label", label: "托管提示词标签", ph: "production" },
      ]],
      ["子智能体", [
        { key: "subagent.enabled", label: "启用子智能体", type: "tristate" },
        { key: "subagent.model", label: "固定模型", ph: "provider/model", hint: "留空跟随当前轮次模型" },
        { key: "subagent.max_tokens", label: "Token 上限", type: "number", ph: "0", hint: "0 = 继承运行预算" },
        { key: "subagent.max_output_tokens", label: "单次输出上限", type: "number", ph: "8192" },
      ]],
      ["长期记忆", [
        { key: "memory.enabled", label: "启用记忆", type: "tristate" },
        { key: "memory.extract_model", label: "提取模型", ph: "provider/model", hint: "建议用便宜快速的模型；留空跟随默认模型" },
        { key: "memory.consolidation_model", label: "归纳模型", ph: "provider/model" },
        { key: "memory.max_jobs_per_run", label: "每轮任务上限", type: "number", ph: "8" },
        { key: "memory.run_interval", label: "流水线周期", ph: "30m", hint: "0 = 只在启动时运行一次" },
        { key: "memory.min_session_idle", label: "会话静默阈值", ph: "1h" },
        { key: "memory.max_session_age", label: "会话最大年龄", ph: "720h" },
      ]],
      ["文生图", [
        { key: "image.enabled", label: "启用文生图", type: "tristate", def: "自动", hint: "缺省：provider 与 model 都设置时启用" },
        { key: "image.provider", label: "凭据 provider", hint: "复用其 base_url/api_key（须为 openai 类型）" },
        { key: "image.model", label: "生图模型" },
        { key: "image.size", label: "默认尺寸", ph: "如 1024x1024" },
        { key: "image.quality", label: "默认质量", type: "select", options: [["", "自动"], ["low", "low"], ["medium", "medium"], ["high", "high"]] },
      ]],
    ],
  },
  { id: "skills", label: "Skills" }, // 自定义渲染（配置 + 运行时发现视图）
  { id: "mcp", label: "MCP" }, // 自定义渲染（概览 → 详情两级）
  {
    id: "system", label: "系统", sections: [
      ["Langfuse 追踪", [
        { key: "tracing.host", label: "服务地址", ph: "https://langfuse.internal" },
        { key: "tracing.public_key", label: "公钥", type: "password", revealRef: { kind: "tracing", field: "public_key" } },
        { key: "tracing.public_key_env", label: "公钥环境变量" },
        { key: "tracing.secret_key", label: "密钥", type: "password", revealRef: { kind: "tracing", field: "secret_key" } },
        { key: "tracing.secret_key_env", label: "密钥环境变量" },
        { key: "tracing.environment", label: "环境标签", ph: "dev" },
        { key: "tracing.include_content", label: "上送对话原文", type: "tristate" },
        { key: "tracing.user", label: "归属用户", hint: "留空依次取 git user.email、$USER" },
        { key: "tracing.cost_input_usd_per_mtok", label: "输入费率 (USD/Mtok)", type: "number", step: 0.01, ph: "0" },
        { key: "tracing.cost_output_usd_per_mtok", label: "输出费率 (USD/Mtok)", type: "number", step: 0.01, ph: "0" },
      ]],
      ["存储与日志", [
        { key: "storage.base_dir", label: "数据根目录", hint: "留空为 ~/.loom" },
        { key: "logging.max_file_mb", label: "单日志文件上限 (MiB)", type: "number", ph: "2048" },
        { key: "logging.max_total_mb", label: "日志总量上限 (MiB)", type: "number", ph: "10240" },
      ]],
      ["终端界面（TUI）", [
        { key: "ui.icons", label: "图标集", type: "select", options: [["", "默认（nerd）"], ["nerd", "nerd（Nerd Font）"], ["plain", "plain（纯文本）"]] },
        { key: "ui.alt_screen", label: "使用备用屏幕", type: "bool", hint: "退出后恢复滚屏" },
      ]],
    ], // workspaces 追加为自定义小节（见 _renderWorkspaces）
  },
];

// ---------- 面板 ----------

export class SettingsPanel {
  constructor({ api, toast, confirm }) {
    this.api = api;
    this.toast = toast;
    this.confirm = confirm;
    this.wrap = document.getElementById("settings-wrap");
    this.revision = "";
    this.cfg = {};
    this.dirty = false;
    this.activeTab = "providers";
    this._tabRefs = {}; // tab id → 自定义渲染器收集的 DOM 引用
    this._skippedCards = 0; // 收集时被丢弃的非空卡片计数（保存时提示）

    document.getElementById("settings-close").onclick = () => this.close();
    document.getElementById("settings-save").onclick = () => this._save();
    document.getElementById("settings-reload").onclick = () => this._load(true);
    this.wrap.addEventListener("click", (e) => { if (e.target === this.wrap) this.close(); });
    // 任意编辑即标记脏；Esc 关闭（脏时确认）
    this.wrap.addEventListener("input", () => this._markDirty());
    this.wrap.addEventListener("change", () => this._markDirty());
    document.addEventListener("keydown", (e) => {
      if (e.key !== "Escape" || this.wrap.hidden) return;
      // 确认弹窗（放弃修改）开着时由它自己消费 Esc，避免重复弹窗。
      if (!document.getElementById("confirm-modal").hidden) return;
      e.stopPropagation();
      this.close();
    }, true);
  }

  async open() {
    this.wrap.hidden = false;
    await this._load();
  }

  async close() {
    if (this.wrap.hidden || this._closing) return;
    closeSelects();
    if (this.dirty) {
      this._closing = true; // 重入守卫：Esc/× 在 confirm 等待期间再次触发
      try {
        const ok = await this.confirm({
          title: "放弃修改",
          body: "设置中有未保存的修改，关闭后将丢失。",
          okLabel: "放弃修改",
        });
        if (!ok) return;
      } finally {
        this._closing = false;
      }
    }
    this.wrap.hidden = true;
    this.dirty = false;
  }

  _markDirty() {
    this.dirty = true;
    document.getElementById("settings-save").classList.add("is-dirty");
  }

  _msg(text, isError) {
    const m = document.getElementById("settings-msg");
    m.textContent = text || "";
    m.classList.toggle("is-error", !!isError);
  }

  // 居中加载/错误占位（面板打开与手动重新加载期间替代空白内容区）。
  _loadingEl(text, isError) {
    const d = el("div", "set-loading" + (isError ? " is-error" : ""));
    d.appendChild(iconEl(isError ? "xmark" : "rotate-left"));
    d.appendChild(document.createTextNode(text));
    return d;
  }

  // manual=true（「重新加载」按钮）时：未保存修改需确认（重载会丢弃），
  // 完成后 toast 反馈——对比 revision 区分「已是最新」与「已重新加载」。
  async _load(manual) {
    if (manual && this.dirty) {
      const ok = await this.confirm({
        title: "重新加载",
        body: "设置中有未保存的修改，重新加载将丢弃它们。",
        okLabel: "重新加载",
      });
      if (!ok) return;
    }
    this._msg("加载中…");
    const body = document.getElementById("settings-content");
    body.textContent = "";
    body.appendChild(this._loadingEl("加载配置中…"));
    try {
      const prevRevision = this.revision;
      const r = await this.api.getConfig();
      this.revision = r.revision || "";
      this.cfg = r.config || {};
      document.getElementById("settings-path").textContent = r.exists ? r.path : `${r.path}（尚未创建，保存后生成）`;
      this.dirty = false;
      document.getElementById("settings-save").classList.remove("is-dirty");
      this._msg(r.exists ? "" : "首次配置：请先在「模型」页添加至少一个 provider");
      this._renderContent();
      this._renderTabs();
      if (this.activeTab === "skills") this._loadSkills();
      if (manual) this.toast(r.revision === prevRevision ? "已是最新，配置无变化" : "配置已重新加载", true);
    } catch (e) {
      if (e.status === 401) {
        this.wrap.hidden = true; // gate 即将弹出，面板让位
        return;
      }
      body.textContent = "";
      body.appendChild(this._loadingEl("加载配置失败: " + e.message, true));
      this._msg("加载配置失败: " + e.message, true);
    }
  }

  _renderTabs() {
    const nav = document.getElementById("settings-tabs");
    nav.textContent = "";
    for (const t of TABS) {
      const b = el("button", "settings-tab" + (t.id === this.activeTab ? " is-active" : ""), t.label);
      b.type = "button";
      b.onclick = () => this._switchTab(t.id);
      nav.appendChild(b);
    }
  }

  _switchTab(id) {
    this.activeTab = id;
    this._renderTabs();
    for (const panel of document.getElementById("settings-content").children) {
      panel.hidden = panel.dataset.tabId !== id;
    }
    if (id === "skills") this._loadSkills();
  }

  // 一次性渲染全部 tab 面板（切换只 toggle hidden）：收集针对整棵 DOM，
  // 从未点开的 tab 的字段才不会在保存时丢失。
  _renderContent() {
    const body = document.getElementById("settings-content");
    body.textContent = "";
    body.dataset.cfgScope = "";
    this._tabRefs = {};
    for (const tab of TABS) {
      const panel = el("div", "settings-panel");
      panel.dataset.tabId = tab.id;
      panel.hidden = tab.id !== this.activeTab;
      if (tab.id === "providers") this._renderProviders(panel);
      else if (tab.id === "mcp") this._renderMcp(panel);
      else if (tab.id === "skills") this._renderSkills(panel);
      else {
        for (const [title, fields] of tab.sections) panel.appendChild(this._renderSection(title, fields));
        if (tab.id === "system") this._renderWorkspaces(panel);
      }
      body.appendChild(panel);
    }
    // 简单 tab 的字段一次性填充；卡片类结构（provider/mcp/workspace）有
    // 自己的 scope，由各渲染器自行填充。
    fillScope(body, this.cfg);
  }

  _renderSection(title, fields) {
    const sec = el("section", "set-sec");
    sec.appendChild(el("h3", "set-sec-title", title));
    for (const spec of fields) sec.appendChild(this._fieldRow(spec));
    return sec;
  }

  // spec.revealRef（或调用方显式给的 resolver）转成眼睛按钮的取明文回调。
  _fieldRow(spec, resolveRef) {
    const resolve = resolveRef || (spec.revealRef ? () => spec.revealRef : null);
    return fieldRow(spec, resolve ? () => this._reveal(resolve()) : null);
  }

  // 按需取回一个已存密钥的明文；失败已 toast，返回 null。
  async _reveal(ref) {
    if (!ref || (ref.name === "" && ref.kind !== "tracing")) {
      this.toast("先填写名称并保存配置后才能查看");
      return null;
    }
    try {
      const r = await this.api.revealSecret(ref);
      return r.value;
    } catch (e) {
      if (e.status === 401) return null;
      this.toast(e.status === 404 ? "该位置没有已保存的密钥（先保存配置）" : "查看密钥失败: " + e.message);
      return null;
    }
  }

  // ---------- 层级导航共享 ----------

  // 面包屑条：返回按钮 + 路径文本，详情态显示。
  _navBar(onBack) {
    const nav = el("div", "set-nav");
    nav.hidden = true;
    const back = el("button", "btn btn-secondary btn-sm set-nav-back");
    back.type = "button";
    back.appendChild(iconEl("arrow-left"));
    back.appendChild(document.createTextNode("返回"));
    back.onclick = onBack;
    nav.appendChild(back);
    const crumb = el("span", "set-nav-crumb");
    nav.appendChild(crumb);
    return { nav, crumb };
  }

  // 概览行摘要：名称 + 元信息 + 展开箭头；点击进入详情。
  _rowSummary(onOpen) {
    const summary = el("button", "set-card-summary");
    summary.type = "button";
    const name = el("span", "set-card-name");
    const meta = el("span", "set-card-meta");
    summary.appendChild(name);
    summary.appendChild(meta);
    const caret = iconEl("caret-right");
    caret.classList.add("set-card-caret");
    summary.appendChild(caret);
    summary.onclick = onOpen;
    return summary;
  }

  // ---------- 模型 tab ----------

  _renderProviders(body) {
    const refs = (this._tabRefs.providers = {});
    body.classList.add("set-hier");

    const top = el("section", "set-sec set-sec-top");
    top.dataset.cfgScope = "";
    top.appendChild(el("h3", "set-sec-title", "启动模型"));
    top.appendChild(this._fieldRow({ key: "default", label: "默认模型", ph: "provider/model", hint: "留空取第一个 provider 的默认模型" }));
    body.appendChild(top);
    fillScope(top, this.cfg);
    refs.top = top;

    const { nav, crumb } = this._navBar(() => this._providersBack());
    body.appendChild(nav);
    refs.nav = nav;
    refs.crumb = crumb;

    const listSec = el("div", "set-list-sec");
    listSec.appendChild(el("h3", "set-sec-title", "模型提供方（至少一个）"));
    const list = el("div", "set-cards");
    listSec.appendChild(list);
    const add = el("button", "btn btn-secondary btn-sm set-add", "+ 添加 provider");
    add.type = "button";
    add.onclick = () => {
      const card = this._providerCard({});
      list.appendChild(card);
      this._markDirty();
      this._openProvider(card);
    };
    listSec.appendChild(add);
    body.appendChild(listSec);
    refs.list = list;
    for (const p of this.cfg.providers || []) list.appendChild(this._providerCard(p));
  }

  _providerCard(p) {
    const card = el("div", "set-card");
    card.dataset.cfgScope = "";

    const head = el("div", "set-card-head");
    head.appendChild(this._rowSummary(() => this._openProvider(card)));
    const name = makeControl({ key: "name", type: "text", ph: "provider 名（全局唯一，必填）" });
    head.appendChild(name);
    head.appendChild(this._cardDelBtn(card, "删除该 provider"));
    card.appendChild(head);

    const cardBody = el("div", "set-card-body");
    for (const spec of PROVIDER_BASE_FIELDS) {
      cardBody.appendChild(spec.key === "api_key"
        ? this._fieldRow(spec, () => ({ kind: "provider", name: name.value.trim() }))
        : this._fieldRow(spec));
    }
    cardBody.appendChild(this._advDetails(PROVIDER_ADV_FIELDS));

    cardBody.appendChild(el("div", "set-subtitle", "模型目录"));
    const models = el("div", "set-models");
    cardBody.appendChild(models);
    const add = el("button", "btn btn-secondary btn-sm set-add", "+ 添加模型");
    add.type = "button";
    add.onclick = () => {
      const mc = this._modelCard(card, {});
      models.appendChild(mc);
      this._markDirty();
      this._openModel(card, mc);
    };
    cardBody.appendChild(add);
    card.appendChild(cardBody);

    for (const m of p.models || []) models.appendChild(this._modelCard(card, m));

    fillScope(card, p); // 直属字段；models 在嵌套 scope 中不受影响
    this._refreshProviderRow(card);
    card.addEventListener("input", () => this._onProviderEdit(card));
    card.addEventListener("change", () => this._onProviderEdit(card));
    return card;
  }

  _modelCard(providerCard, m) {
    const card = el("div", "set-card is-nested");
    card.dataset.cfgScope = "";
    const head = el("div", "set-card-head");
    head.appendChild(this._rowSummary(() => this._openModel(providerCard, card)));
    head.appendChild(el("span", "set-card-tag", "model"));
    head.appendChild(this._cardDelBtn(card, "删除该模型"));
    card.appendChild(head);
    const body = el("div", "set-card-body");
    for (const spec of MODEL_FIELDS) body.appendChild(this._fieldRow(spec));
    card.appendChild(body);
    fillScope(card, m);
    this._refreshModelRow(card);
    card.addEventListener("input", () => {
      this._refreshModelRow(card);
      if (this._tabRefs.providers?.openModel === card) this._setProviderCrumb();
    });
    return card;
  }

  _onProviderEdit(card) {
    this._refreshProviderRow(card);
    if (this._tabRefs.providers?.open === card) this._setProviderCrumb();
  }

  _refreshProviderRow(card) {
    const obj = {};
    collectScope(card, obj);
    const sName = card.querySelector(":scope > .set-card-head .set-card-name");
    const sMeta = card.querySelector(":scope > .set-card-head .set-card-meta");
    sName.textContent = obj.name || "（未命名 provider）";
    sName.classList.toggle("is-empty", !obj.name);
    const nModels = card.querySelectorAll(":scope > .set-card-body > .set-models > .set-card").length;
    sMeta.textContent = [obj.type || "openai", obj.base_url || "未配置 Base URL", `${nModels} 个模型`].join(" · ");
  }

  _refreshModelRow(card) {
    const obj = {};
    collectScope(card, obj);
    const sName = card.querySelector(":scope > .set-card-head .set-card-name");
    const sMeta = card.querySelector(":scope > .set-card-head .set-card-meta");
    sName.textContent = obj.name || "（未命名模型）";
    sName.classList.toggle("is-empty", !obj.name);
    const parts = [];
    if (obj.context_window) parts.push(`上下文 ${obj.context_window}`);
    if (obj.max_output_tokens) parts.push(`输出上限 ${obj.max_output_tokens}`);
    sMeta.textContent = parts.join(" · ") || "跟随 provider / 全局默认";
  }

  _openProvider(card) {
    const refs = this._tabRefs.providers;
    const panel = refs.list.closest(".settings-panel");
    panel.classList.add("is-detail");
    card.classList.add("is-open");
    refs.open = card;
    refs.nav.hidden = false;
    this._setProviderCrumb();
  }

  _openModel(providerCard, modelCard) {
    const refs = this._tabRefs.providers;
    providerCard.classList.add("is-model-detail");
    modelCard.classList.add("is-open");
    refs.openModel = modelCard;
    this._setProviderCrumb();
  }

  _providersBack() {
    const refs = this._tabRefs.providers;
    const card = refs.open;
    if (!card) return;
    // 第三级（模型明细）→ 第二级（provider 详情）
    if (refs.openModel) {
      this._refreshModelRow(refs.openModel);
      refs.openModel.classList.remove("is-open");
      refs.openModel = null;
      card.classList.remove("is-model-detail");
      this._setProviderCrumb();
      return;
    }
    // 第二级 → 概览
    this._refreshProviderRow(card);
    card.classList.remove("is-open");
    refs.open = null;
    refs.list.closest(".settings-panel").classList.remove("is-detail");
    refs.nav.hidden = true;
  }

  _setProviderCrumb() {
    const refs = this._tabRefs.providers;
    const card = refs.open;
    if (!card) return;
    const name = card.querySelector(":scope > .set-card-head .set-input").value.trim() || "未命名";
    let text = "模型提供方 / " + name;
    if (refs.openModel) {
      const mn = refs.openModel.querySelector('[data-cfg-key="name"]').value.trim();
      text += " / " + (mn || "未命名模型");
    }
    refs.crumb.textContent = text;
  }

  _collectProviders(cfg) {
    const refs = this._tabRefs.providers;
    collectScope(refs.top, cfg); // default
    const providers = [];
    for (const card of refs.list.children) {
      const p = {};
      collectScope(card, p);
      const models = [];
      let skippedModels = 0;
      for (const mc of card.querySelector(":scope > .set-card-body > .set-models").children) {
        const m = {};
        collectScope(mc, m);
        if (m.name) models.push(m);
        else if (Object.keys(m).length) skippedModels++;
      }
      if (models.length) p.models = models;
      if (p.name) providers.push(p);
      else if (Object.keys(p).length) this._skippedCards++;
      this._skippedCards += skippedModels;
    }
    if (providers.length) cfg.providers = providers;
  }

  // ---------- MCP tab ----------

  _renderMcp(body) {
    const refs = (this._tabRefs.mcp = {});
    body.classList.add("set-hier");
    const tip = el("div", "set-hint set-tip", "两种传输二选一：command（stdio 子进程）或 url（远程 HTTP）。header 值支持 ${VAR} 环境变量引用（令牌不落盘）。工具名格式 mcp__{服务器名}__{工具名}。");
    body.appendChild(tip);

    const { nav, crumb } = this._navBar(() => this._mcpBack());
    body.appendChild(nav);
    refs.nav = nav;
    refs.crumb = crumb;

    const listSec = el("div", "set-list-sec");
    const list = el("div", "set-cards");
    listSec.appendChild(list);
    const add = el("button", "btn btn-secondary btn-sm set-add", "+ 添加 MCP 服务器");
    add.type = "button";
    add.onclick = () => {
      const card = this._mcpCard("", {});
      list.appendChild(card);
      this._markDirty();
      this._openMcp(card);
    };
    listSec.appendChild(add);
    body.appendChild(listSec);
    refs.list = list;
    for (const [name, srv] of Object.entries(this.cfg.mcp_servers || {})) list.appendChild(this._mcpCard(name, srv));
    this._refreshMcpStatus();
  }

  // 拉取进程级 MCP 实时状态并刷新各卡片徽标（打开面板与保存后调用）。
  async _refreshMcpStatus() {
    const refs = this._tabRefs.mcp;
    if (!refs || !refs.list) return;
    let servers = [];
    try {
      const r = await this.api.listMcpServers();
      servers = r.servers || [];
    } catch {
      return; // 状态查询失败不影响编辑
    }
    const byName = new Map(servers.map((s) => [s.name, s]));
    for (const card of refs.list.children) {
      const name = card.querySelector(":scope > .set-card-head .set-input").value.trim();
      this._setMcpBadge(card, name ? byName.get(name) || null : undefined);
    }
  }

  // badge 三态：undefined（未命名卡片）→ 不显示；null（已命名但未连接）→
  // 「保存后连接」；status → 已连接 N 工具 / 连接失败。每次刷新徽标时
  // 同步详情页的「已注册工具」小节（数据同源于进程级实时状态）。
  _setMcpBadge(card, status) {
    const badge = card.querySelector(":scope > .set-card-head .mcp-status");
    if (!badge) return;
    badge.className = "mcp-status";
    badge.title = "";
    card._mcpStatus = status;
    if (status === undefined) {
      badge.textContent = "";
    } else if (status === null) {
      badge.textContent = "保存后连接";
    } else if (status.connected) {
      badge.classList.add("is-live");
      badge.textContent = `已连接 · ${(status.tools || []).length} 工具`;
      badge.title = (status.tools || []).map((t) => t.name).join("\n");
    } else {
      badge.classList.add("is-dead");
      badge.textContent = "连接失败";
      badge.title = status.error || "";
    }
    this._renderMcpTools(card);
  }

  // 「已注册工具」小节（详情表单上方，只读）：每行显示服务器本地名 +
  // 简介（两行截断，点击展开），hover 名称见完整限定名
  // （mcp__{服务器}__{工具}）。工具很多时列表限高内部滚动。
  _renderMcpTools(card) {
    const sec = card.querySelector(":scope > .set-card-body > .mcp-tools-sec");
    if (!sec) return;
    sec.textContent = "";
    sec.appendChild(el("div", "set-subtitle", "已注册工具（实时状态 · 白/黑名单过滤后的生效集）"));
    const st = card._mcpStatus;
    if (!st || !st.connected) {
      const hint = st && st.error ? "连接失败，修复后点击右上角重连" : "保存并连接成功后展示该服务器的工具列表";
      sec.appendChild(el("div", "set-hint", hint));
      return;
    }
    if (!st.tools || !st.tools.length) {
      sec.appendChild(el("div", "set-hint", "该服务器未暴露工具（或已被白/黑名单全部过滤）"));
      return;
    }
    const name = card.querySelector(":scope > .set-card-head .set-input").value.trim();
    const prefix = `mcp__${name}__`;
    // 简介里的 [MCP server "…"] 前缀是适配层给模型看的归因（mcp/tool.go），
    // UI 上服务器归属显而易见，剥掉更干净
    const descPrefix = `[MCP server "${name}"] `;
    const list = el("div", "mcp-tool-list");
    for (const t of st.tools) {
      const row = el("div", "mcp-tool-row");
      const nm = el("span", "mcp-tool-name mono", t.name.startsWith(prefix) ? t.name.slice(prefix.length) : t.name);
      nm.title = t.name;
      row.appendChild(nm);
      let descText = t.description || "";
      if (descText.startsWith(descPrefix)) descText = descText.slice(descPrefix.length);
      const desc = el("span", "mcp-tool-desc" + (descText ? "" : " is-empty"), descText || "（无简介）");
      if (descText) {
        desc.title = descText;
        desc.onclick = () => desc.classList.toggle("is-expanded");
      }
      row.appendChild(desc);
      list.appendChild(row);
    }
    sec.appendChild(list);
  }

  async _reconnectMcp(card) {
    const name = card.querySelector(":scope > .set-card-head .set-input").value.trim();
    if (!name) {
      this.toast("先填写服务器名并保存配置");
      return;
    }
    const badge = card.querySelector(":scope > .set-card-head .mcp-status");
    badge.className = "mcp-status";
    badge.textContent = "连接中…";
    const btn = card.querySelector(":scope > .set-card-head .mcp-reconnect");
    btn.classList.add("is-spinning"); // 图标旋转 = 进行中
    btn.disabled = true;
    try {
      const status = await this.api.reconnectMcpServer(name);
      this._setMcpBadge(card, status);
      if (status.connected) this.toast(`MCP 服务器 ${name} 已连接`, true);
      else this.toast(`MCP 服务器 ${name} 连接失败: ${status.error || "unknown"}`);
    } catch (e) {
      if (e.status === 401) return;
      this._setMcpBadge(card, null);
      if (e.message && e.message.includes("unknown mcp server")) {
        this.toast("该服务器不在已保存的配置中（改名或新增后请先保存）");
      } else {
        this.toast("重连失败: " + e.message);
      }
    } finally {
      btn.classList.remove("is-spinning");
      btn.disabled = false;
    }
  }

  _mcpCard(name, srv) {
    const card = el("div", "set-card");
    const head = el("div", "set-card-head");
    head.appendChild(this._rowSummary(() => this._openMcp(card)));
    const nameCtl = el("input", "set-input");
    nameCtl.type = "text";
    nameCtl.placeholder = "服务器名（必填）";
    nameCtl.value = name;
    nameCtl.spellcheck = false;
    head.appendChild(nameCtl);
    // 实时状态徽标（_refreshMcpStatus 填充）与手动重连
    const status = el("span", "mcp-status");
    head.appendChild(status);
    const reconnect = el("button", "icon-btn mcp-reconnect");
    reconnect.type = "button";
    reconnect.title = "重新连接";
    reconnect.innerHTML = icon("rotate-left");
    reconnect.onclick = () => this._reconnectMcp(card);
    head.appendChild(reconnect);
    head.appendChild(this._cardDelBtn(card, "删除该服务器"));
    card.appendChild(head);

    const cardBody = el("div", "set-card-body");
    // 「已注册工具」只读小节（_renderMcpTools 随徽标刷新填充）
    cardBody.appendChild(el("div", "set-group mcp-tools-sec"));

    // 传输形态切换：由 command/url 哪个有值推定；切换只影响展示哪组字段
    const transport = createSelect({
      className: "set-input set-transport",
      options: [["stdio", "command（stdio 子进程）"], ["http", "url（远程 HTTP）"]],
    });
    transport.value = srv.url ? "http" : "stdio";
    const tRow = el("div", "set-row");
    tRow.appendChild(el("label", "set-label", "传输方式"));
    const tField = el("div", "set-field");
    tField.appendChild(transport);
    tRow.appendChild(tField);
    cardBody.appendChild(tRow);

    const stdio = el("div", "set-group");
    stdio.dataset.cfgScope = "";
    stdio.dataset.transport = "stdio";
    for (const spec of [
      { key: "command", label: "命令", ph: "如 npx" },
      { key: "args", label: "参数", type: "list-text", hint: "每行一个参数" },
      { key: "env", label: "环境变量", type: "kv-text", hint: "每行一个 KEY=VALUE（追加到进程环境）" },
      { key: "cwd", label: "工作目录", hint: "留空继承 loom 的工作目录" },
    ]) stdio.appendChild(this._fieldRow(spec));
    cardBody.appendChild(stdio);

    const http = el("div", "set-group");
    http.dataset.cfgScope = "";
    http.dataset.transport = "http";
    for (const spec of [
      { key: "url", label: "URL", ph: "https://mcp.example.com/mcp" },
      { key: "headers", label: "请求头", type: "kv-text", hint: "每行一个 KEY=VALUE；值支持 ${VAR} 引用" },
    ]) http.appendChild(this._fieldRow(spec));
    cardBody.appendChild(http);

    const common = el("div", "set-group");
    common.dataset.cfgScope = "";
    common.dataset.transport = "common";
    for (const spec of [
      { key: "startup_timeout_sec", label: "启动超时 (秒)", type: "number", ph: "30" },
      { key: "tool_timeout_sec", label: "工具调用超时 (秒)", type: "number", ph: "300" },
      { key: "enabled_tools", label: "工具白名单", type: "list-text", hint: "留空注册全部工具" },
      { key: "disabled_tools", label: "工具黑名单", type: "list-text" },
    ]) common.appendChild(this._fieldRow(spec));
    cardBody.appendChild(common);
    card.appendChild(cardBody);

    const syncTransport = () => {
      stdio.hidden = transport.value !== "stdio";
      http.hidden = transport.value !== "http";
    };
    transport.addEventListener("change", syncTransport);
    syncTransport();

    fillScope(stdio, srv);
    fillScope(http, srv);
    fillScope(common, srv);
    this._renderMcpTools(card); // 状态未拉取前先展示占位提示
    this._refreshMcpRow(card);
    card.addEventListener("input", () => this._onMcpEdit(card));
    card.addEventListener("change", () => this._onMcpEdit(card));
    return card;
  }

  _onMcpEdit(card) {
    this._refreshMcpRow(card);
    if (this._tabRefs.mcp?.open === card) this._setMcpCrumb(card);
  }

  _refreshMcpRow(card) {
    const name = card.querySelector(":scope > .set-card-head .set-input").value.trim();
    const sName = card.querySelector(":scope > .set-card-head .set-card-name");
    sName.textContent = name || "（未命名服务器）";
    sName.classList.toggle("is-empty", !name);
    card.querySelector(":scope > .set-card-head .set-card-meta").textContent = this._mcpMeta(card);
  }

  // 概览行元信息：传输方式 + 命令/URL 摘要（与收集逻辑同源地读当前传输组）。
  _mcpMeta(card) {
    const transport = card.querySelector(".set-transport").value;
    const srv = {};
    for (const group of card.querySelectorAll(":scope > .set-card-body > .set-group")) {
      if (group.dataset.transport === "common" || group.dataset.transport === transport) collectScope(group, srv);
    }
    if (transport === "http") return "HTTP · " + (srv.url || "未配置 URL");
    const cmd = [srv.command, ...(srv.args || [])].filter(Boolean).join(" ");
    return "stdio · " + (cmd || "未配置命令");
  }

  _openMcp(card) {
    const refs = this._tabRefs.mcp;
    refs.list.closest(".settings-panel").classList.add("is-detail");
    card.classList.add("is-open");
    refs.open = card;
    refs.nav.hidden = false;
    this._setMcpCrumb(card);
  }

  _mcpBack() {
    const refs = this._tabRefs.mcp;
    const card = refs.open;
    if (!card) return;
    this._refreshMcpRow(card);
    card.classList.remove("is-open");
    refs.open = null;
    refs.list.closest(".settings-panel").classList.remove("is-detail");
    refs.nav.hidden = true;
  }

  _setMcpCrumb(card) {
    const name = card.querySelector(":scope > .set-card-head .set-input").value.trim() || "未命名";
    this._tabRefs.mcp.crumb.textContent = "MCP 服务器 / " + name;
  }

  _collectMcp(cfg) {
    const refs = this._tabRefs.mcp;
    const servers = {};
    for (const card of refs.list.children) {
      const name = card.querySelector(":scope > .set-card-head .set-input").value.trim();
      if (!name) continue;
      const transport = card.querySelector(".set-transport").value;
      const srv = {};
      for (const group of card.querySelectorAll(":scope > .set-card-body > .set-group")) {
        if (group.dataset.transport === "common" || group.dataset.transport === transport) collectScope(group, srv);
      }
      if (servers[name]) this._skippedCards++; // 重名：后者覆盖前者
      servers[name] = srv;
    }
    if (Object.keys(servers).length) cfg.mcp_servers = servers;
  }

  // ---------- Skills tab（配置 + 运行时发现视图） ----------

  _renderSkills(body) {
    const refs = (this._tabRefs.skills = {});
    // 配置小节：控件归属 settings-content 的全局 scope（本面板不设
    // data-cfg-scope），由通用的填充/收集逻辑处理
    const sec = el("section", "set-sec");
    sec.appendChild(el("h3", "set-sec-title", "技能配置"));
    sec.appendChild(this._fieldRow({ key: "skills.enabled", label: "启用技能", type: "tristate" }));
    sec.appendChild(this._fieldRow({ key: "skills.extra_roots", label: "额外搜索目录", type: "list-text", hint: "每行一个目录" }));
    body.appendChild(sec);

    const bar = el("div", "set-skills-bar");
    bar.appendChild(el("h3", "set-sec-title", "发现的技能（运行时状态）"));
    const refresh = el("button", "btn btn-secondary btn-sm set-skills-refresh");
    refresh.type = "button";
    refresh.title = "重新扫描磁盘";
    refresh.appendChild(iconEl("rotate-left"));
    refresh.appendChild(document.createTextNode("刷新"));
    refresh.onclick = () => this._loadSkills(true);
    refs.refreshBtn = refresh;
    bar.appendChild(refresh);
    body.appendChild(bar);
    body.appendChild(el("div", "set-hint set-tip", "所有工作区发现的 skill。禁用按名称对所有工作区生效（立即生效，写入 config 的 skills.disabled）；删除会从磁盘移除整个目录。编辑内容请直接修改对应的 SKILL.md；上方的配置改动保存后生效（重启后应用）。"));
    const list = el("div", "set-skills");
    body.appendChild(list);
    refs.list = list;
    refs.loaded = false;
  }

  async _loadSkills(force) {
    const refs = this._tabRefs.skills;
    if (!refs || !refs.list || refs.loading || (refs.loaded && !force)) return;
    refs.loaded = true;
    refs.loading = true;
    if (refs.refreshBtn) {
      refs.refreshBtn.disabled = true;
      refs.refreshBtn.classList.add("is-spinning");
    }
    refs.list.textContent = "";
    refs.list.appendChild(this._loadingEl("扫描技能目录中…"));
    try {
      const r = await this.api.listSkills();
      refs.list.textContent = "";
      if (!r.enabled) {
        refs.list.appendChild(el("div", "set-hint", `技能已禁用（${r.reason || "未知原因"}）。可在上方「启用技能」开启并保存。`));
        return;
      }
      let total = 0, issues = 0;
      for (const g of r.groups || []) {
        total += (g.skills || []).length;
        issues += (g.issues || []).length;
        refs.list.appendChild(this._skillGroup(g));
      }
      if (total === 0) {
        refs.list.appendChild(el("div", "set-hint", "未发现任何 skill。目录约定：工作区 .loom/skills/、.agents/skills/，用户级 ~/.loom/skills/、~/.agents/skills/。"));
      }
      // 手动刷新的结果摘要：数字让用户确认扫描覆盖了预期目录
      if (force) this.toast(`发现 ${total} 个 skill` + (issues ? `，${issues} 个加载失败` : ""), issues === 0);
    } catch (e) {
      refs.list.textContent = "";
      if (e.status !== 401) {
        refs.list.appendChild(this._loadingEl("加载失败: " + e.message, true));
        if (force) this.toast("技能扫描失败: " + e.message);
      }
    } finally {
      refs.loading = false;
      if (refs.refreshBtn) {
        refs.refreshBtn.disabled = false;
        refs.refreshBtn.classList.remove("is-spinning");
      }
    }
  }

  _skillGroup(g) {
    const sec = el("section", "set-sec");
    sec.appendChild(el("h3", "set-sec-title", g.workspace_name));
    if (g.root) sec.appendChild(el("div", "set-hint mono set-skill-root", g.root));
    if (!g.skills || !g.skills.length) {
      sec.appendChild(el("div", "set-hint", g.shared ? "（无用户级 skill）" : "（该工作区无 repo 级 skill）"));
    }
    for (const sk of g.skills || []) {
      sec.appendChild(this._skillRow(sk));
    }
    for (const issue of g.issues || []) {
      const line = el("div", "skill-issue");
      line.appendChild(iconEl("triangle-exclamation"));
      line.appendChild(document.createTextNode(issue));
      sec.appendChild(line);
    }
    return sec;
  }

  _skillRow(sk) {
    const row = el("div", "skill-row" + (sk.disabled ? " is-disabled" : ""));
    const head = el("div", "skill-head");
    head.appendChild(el("span", "skill-name mono", sk.name));
    head.appendChild(el("span", "skill-scope" + (sk.scope === "repo" ? " is-repo" : ""), sk.scope));
    if (sk.disabled) head.appendChild(el("span", "skill-scope is-off", "已禁用"));
    const actions = el("div", "skill-actions");
    // 禁用/启用：按名称写入 config 的 skills.disabled，服务端热应用
    const toggle = el("button", "icon-btn skill-action");
    toggle.type = "button";
    toggle.title = (sk.disabled ? "启用" : "禁用") + "（按名称对所有工作区生效）";
    toggle.innerHTML = icon(sk.disabled ? "check" : "ban");
    toggle.onclick = () => this._toggleSkill(sk, toggle);
    actions.appendChild(toggle);
    // 删除：从磁盘移除整个 skill 目录（不可恢复，需确认）
    const del = el("button", "icon-btn skill-action skill-del");
    del.type = "button";
    del.title = "从磁盘删除该 skill";
    del.innerHTML = icon("trash");
    del.onclick = () => this._deleteSkill(sk);
    actions.appendChild(del);
    head.appendChild(actions);
    row.appendChild(head);
    // 简介默认截断为两行（CSS line-clamp），hover 见全文，点击展开/收起
    const desc = el("div", "skill-desc", sk.description);
    desc.title = sk.description;
    desc.onclick = () => desc.classList.toggle("is-expanded");
    row.appendChild(desc);
    row.appendChild(el("div", "skill-path mono", sk.path));
    return row;
  }

  async _toggleSkill(sk, btn) {
    btn.disabled = true;
    try {
      const resp = await this.api.setSkillDisabled(sk.name, !sk.disabled);
      // 端点改写了 config 文件：同步面板持有的 revision 与 disabled 列表，
      // 否则后续保存设置会 409 冲突，或把 skills.disabled 回滚成旧值
      if (resp.revision) this.revision = resp.revision;
      setPath(this.cfg, "skills.disabled", resp.disabled || []);
      this.toast(sk.disabled ? `已启用 ${sk.name}` : `已禁用 ${sk.name}（立即生效）`, true);
      await this._loadSkills(true);
    } catch (e) {
      btn.disabled = false;
      if (e.status !== 401) this.toast((sk.disabled ? "启用失败: " : "禁用失败: ") + e.message);
    }
  }

  async _deleteSkill(sk) {
    const ok = await this.confirm({
      title: "删除 skill",
      body: `将从磁盘删除「${sk.name}」所在目录（含目录内全部文件）：${sk.path}。该操作不可恢复。`,
      okLabel: "删除",
    });
    if (!ok) return;
    try {
      await this.api.deleteSkill(sk.path);
      this.toast(`已删除 ${sk.name}`, true);
      await this._loadSkills(true);
    } catch (e) {
      if (e.status !== 401) this.toast("删除失败: " + e.message);
    }
  }

  // ---------- workspaces（系统 tab 附加小节） ----------

  _renderWorkspaces(body) {
    const refs = (this._tabRefs.workspaces = {});
    const sec = el("section", "set-sec");
    sec.appendChild(el("h3", "set-sec-title", "预注册工作区"));
    sec.appendChild(el("div", "set-hint set-tip", "启动时注册的固定工作区（启动目录始终自动注册，无需在此列出）。root 支持 ~ 开头。"));
    const list = el("div", "set-cards");
    sec.appendChild(list);
    refs.list = list;
    for (const ws of this.cfg.workspaces || []) list.appendChild(this._wsCard(ws));
    const add = el("button", "btn btn-secondary btn-sm set-add", "+ 添加工作区");
    add.type = "button";
    add.onclick = () => list.appendChild(this._wsCard({}));
    sec.appendChild(add);
    body.appendChild(sec);
  }

  _wsCard(ws) {
    const card = el("div", "set-card");
    card.dataset.cfgScope = "";
    const head = el("div", "set-card-head");
    head.appendChild(makeControl({ key: "name", type: "text", ph: "显示名（可选）" }));
    head.appendChild(this._cardDelBtn(card, "删除该工作区"));
    card.appendChild(head);
    card.appendChild(this._fieldRow({ key: "root", label: "根目录", ph: "~/workspace/project" }));
    fillScope(card, ws);
    return card;
  }

  _collectWorkspaces(cfg) {
    const refs = this._tabRefs.workspaces;
    if (!refs || !refs.list) return;
    const out = [];
    for (const card of refs.list.children) {
      const ws = {};
      collectScope(card, ws);
      if (ws.root) out.push(ws);
      else if (ws.name) this._skippedCards++;
    }
    if (out.length) cfg.workspaces = out;
  }

  // ---------- 共享小件 ----------

  _advDetails(fields) {
    const det = el("details", "disclosure set-adv");
    det.appendChild(el("summary", "", "高级选项"));
    const inner = el("div", "set-adv-body");
    for (const spec of fields) inner.appendChild(this._fieldRow(spec));
    det.appendChild(inner);
    return det;
  }

  _cardDelBtn(card, title) {
    const del = el("button", "icon-btn set-card-del");
    del.type = "button";
    del.title = title;
    del.innerHTML = icon("trash");
    del.onclick = () => {
      // 删除处于详情态的卡片时先逐级退回概览，避免停留在已卸载的卡片上
      const refs = this._tabRefs.providers;
      if (refs?.open === card) {
        this._providersBack(); // 模型明细 → provider 详情（无则直达概览）
        this._providersBack(); // provider 详情 → 概览
      } else if (refs?.openModel === card) {
        this._providersBack();
      }
      const mrefs = this._tabRefs.mcp;
      if (mrefs?.open === card) this._mcpBack();
      card.remove();
      this._markDirty();
    };
    return del;
  }

  // ---------- 保存 ----------

  _collectAll(cfg) {
    const body = document.getElementById("settings-content");
    collectScope(body, cfg); // 简单 tab 的字段（卡片结构有自己的 scope，跳过）
    this._collectProviders(cfg);
    this._collectMcp(cfg);
    this._collectWorkspaces(cfg);
    if (this._skippedCards > 0) {
      this.toast(`${this._skippedCards} 张卡片因缺少必填字段（名称/根目录）未被保存`);
      this._skippedCards = 0;
    }
  }

  _validate(cfg) {
    if (!cfg.providers || cfg.providers.length === 0) return "请先在「模型」页添加至少一个 provider";
    for (const p of cfg.providers) {
      if (!p.base_url) return `provider「${p.name}」缺少 base_url`;
      if (!p.models || p.models.length === 0) return `provider「${p.name}」至少需要一个模型`;
      if (p.api_key && p.api_key_env) return `provider「${p.name}」的 api_key 与 api_key_env 只能二选一`;
    }
    return "";
  }

  // 保存结果消息：按服务端返回的分级报告说明每类配置的生效时机。
  _applyMsg(resp) {
    if (resp.apply_error) return `已保存，但热应用失败（重启后生效）: ${resp.apply_error}`;
    const a = resp.applied;
    if (!a) return "已保存";
    const parts = [];
    if (a.immediate && a.immediate.length) parts.push("立即生效: " + a.immediate.join("、"));
    if (a.next_turn && a.next_turn.length) parts.push("下一轮生效: " + a.next_turn.join("、"));
    if (a.restart && a.restart.length) parts.push("重启后生效: " + a.restart.join("、"));
    return parts.length ? "已保存 — " + parts.join("；") : "已保存（配置无变化）";
  }

  async _save() {
    if (this._saving) return; // 双击/连点保护：重复 PUT 会带旧 revision 必然 409
    this._saving = true;
    const saveBtn = document.getElementById("settings-save");
    saveBtn.disabled = true;
    try {
      const cfg = {};
      this._collectAll(cfg);
      preserveUnmanaged(cfg, this.cfg);
      const err = this._validate(cfg);
      if (err) {
        this._msg(err, true);
        this.toast(err);
        return;
      }
      this._msg("保存中…（MCP 变更需连接，可能耗时数秒）");
      const r = await this.api.putConfig(this.revision, cfg);
      this.revision = r.revision || this.revision;
      this.dirty = false;
      saveBtn.classList.remove("is-dirty");
      const msg = this._applyMsg(r);
      this._msg(msg);
      this.toast(msg, true);
      // 成功瞬间绿色 outline 一闪（与 is-dirty 的 warning 色 outline 呼应）
      saveBtn.classList.remove("flash-success");
      void saveBtn.offsetWidth; // 重置动画以便连续保存时重触发
      saveBtn.classList.add("flash-success");
      setTimeout(() => saveBtn.classList.remove("flash-success"), 1300);
      // 热应用可能改变 MCP 连接状态（新增/删除/重连），刷新徽标
      this._refreshMcpStatus();
    } catch (e) {
      if (e.status === 401) return;
      if (e.code === "config_conflict") {
        this._msg("配置文件已被外部修改 — 点击「重新加载」后再保存", true);
      } else {
        this._msg("保存失败: " + e.message, true);
      }
    } finally {
      this._saving = false;
      saveBtn.disabled = false;
    }
  }
}
