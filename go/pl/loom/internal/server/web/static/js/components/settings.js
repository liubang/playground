// settings.js — 设置面板（config.yaml 的图形化编辑）。
// 设计：字段用声明式 spec 描述（key 即 config.yaml 的键路径），同一套 spec
// 驱动渲染与收集 —— 新增配置项只需加一行 spec。控件归属最近的
// [data-cfg-scope] 容器，嵌套结构（provider → models）因此可以复用同一套
// 填充/收集逻辑而不串层。
//
// 空值语义与文件一致：留空 = 不写入该键（omitempty），默认值全部隐式。
// 密钥控件展示服务端的脱敏占位符，未修改时原样回传由服务端还原。

import { el } from "./blocks.js";
import { icon } from "../icons.js";

// UI 未管理的配置路径：保存时从已加载的配置原样带回，避免静默丢失
// （merge 的语义是「未提供的 key = 从文件删除」）。PRESERVE_PATHS 覆盖
// 已知但 UI 未做编辑器的嵌套键；KNOWN_TOP_KEYS 之外的顶层键（未来新增
// 的配置节）也一律保留 —— UI 完整性不该是正确性的前提。
const PRESERVE_PATHS = ["ui.keymap"];
const KNOWN_TOP_KEYS = new Set([
  "default", "providers", "limits", "context", "runaway", "prompt",
  "skills", "rules", "approval", "tracing", "storage", "logging",
  "ui", "subagent", "memory", "image", "mcp_servers", "workspaces",
]);

function preserveUnmanaged(cfg, orig) {
  for (const [k, v] of Object.entries(orig)) {
    if (!KNOWN_TOP_KEYS.has(k) && cfg[k] === undefined) cfg[k] = v;
  }
  for (const path of PRESERVE_PATHS) {
    const v = getPath(orig, path);
    if (v !== undefined && getPath(cfg, path) === undefined) setPath(cfg, path, v);
  }
}

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

// ---------- 字段控件 ----------

// spec: {key, label, hint, ph, type, options, step, rows, def}
// type ∈ text | password | number | bool | tristate | select | textarea |
//       list-text（每行一项 → []string）| kv-text（每行 k=v → map）|
//       float-list（逗号分隔 → []number）
function makeControl(spec) {
  const t = spec.type || "text";
  let ctl;
  if (t === "select" || t === "tristate") {
    ctl = el("select", "set-input");
    const opts = t === "tristate"
      ? [["", `默认（${spec.def || "开"}）`], ["true", "开"], ["false", "关"]]
      : spec.options;
    for (const [v, label] of opts) {
      const o = el("option", "", label);
      o.value = v;
      ctl.appendChild(o);
    }
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

function fieldRow(spec) {
  const row = el("div", "set-row");
  row.appendChild(el("label", "set-label", spec.label));
  const body = el("div", "set-field");
  const ctl = makeControl(spec);
  if ((spec.type || "text") === "password") {
    // 密钥控件：眼睛按钮临时显示明文（掩码值也可查看——它本来就不是密钥）
    const wrap = el("div", "set-secret");
    wrap.appendChild(ctl);
    const eye = el("button", "icon-btn set-eye");
    eye.type = "button";
    eye.title = "显示/隐藏";
    eye.innerHTML = icon("eye");
    eye.onclick = () => {
      const show = ctl.type === "password";
      ctl.type = show ? "text" : "password";
      eye.innerHTML = icon(show ? "eye-slash" : "eye");
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
  { id: "providers", label: "模型" }, // 自定义渲染（列表嵌套）
  {
    id: "limits", label: "预算与上下文", sections: [
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
    id: "permission", label: "权限", sections: [
      ["审批基线", [
        {
          key: "approval.mode", label: "审批模式", type: "select",
          options: [
            ["", "默认（on-request）"],
            ["on-request", "on-request · 沙箱内非危险命令免审批"],
            ["unless-dangerous", "unless-dangerous · 黑名单模式"],
            ["unless-trusted", "unless-trusted · 保守白名单"],
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
    id: "agent", label: "代理行为", sections: [
      ["系统提示词", [
        { key: "prompt.extra", label: "附加指令", type: "textarea", hint: "追加到内置系统提示词末尾" },
        { key: "prompt.disable_builtin", label: "禁用内置提示词", type: "bool" },
        { key: "prompt.managed.name", label: "托管提示词名", hint: "Langfuse 托管提示词（需配置追踪）" },
        { key: "prompt.managed.label", label: "托管提示词标签", ph: "production" },
      ]],
      ["Skills", [
        { key: "skills.enabled", label: "启用技能", type: "tristate" },
        { key: "skills.extra_roots", label: "额外搜索目录", type: "list-text", hint: "每行一个目录" },
      ]],
      ["子代理", [
        { key: "subagent.enabled", label: "启用子代理", type: "tristate" },
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
  {
    id: "system", label: "系统", sections: [
      ["Langfuse 追踪", [
        { key: "tracing.host", label: "服务地址", ph: "https://langfuse.internal" },
        { key: "tracing.public_key", label: "公钥", type: "password" },
        { key: "tracing.public_key_env", label: "公钥环境变量" },
        { key: "tracing.secret_key", label: "密钥", type: "password" },
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
  { id: "mcp", label: "MCP" }, // 自定义渲染（map + 双传输形态）
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
    document.getElementById("settings-reload").onclick = () => this._load();
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

  async _load() {
    this._msg("加载中…");
    try {
      const r = await this.api.getConfig();
      this.revision = r.revision || "";
      this.cfg = r.config || {};
      document.getElementById("settings-path").textContent = r.exists ? r.path : `${r.path}（尚未创建，保存后生成）`;
      this.dirty = false;
      document.getElementById("settings-save").classList.remove("is-dirty");
      this._msg(r.exists ? "" : "首次配置：请先在「模型」页添加至少一个 provider");
      this._renderContent();
      this._renderTabs();
    } catch (e) {
      if (e.status === 401) {
        this.wrap.hidden = true; // gate 即将弹出，面板让位
        return;
      }
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
    for (const spec of fields) sec.appendChild(fieldRow(spec));
    return sec;
  }

  // ---------- 模型 tab ----------

  _renderProviders(body) {
    const refs = (this._tabRefs.providers = {});
    const top = el("section", "set-sec");
    top.dataset.cfgScope = "";
    top.appendChild(el("h3", "set-sec-title", "启动模型"));
    top.appendChild(fieldRow({ key: "default", label: "默认模型", ph: "provider/model", hint: "留空取第一个 provider 的默认模型" }));
    body.appendChild(top);
    fillScope(top, this.cfg);
    refs.top = top;

    body.appendChild(el("h3", "set-sec-title", "模型提供方（至少一个）"));
    const list = el("div", "set-cards");
    body.appendChild(list);
    refs.list = list;
    for (const p of this.cfg.providers || []) list.appendChild(this._providerCard(p));
    const add = el("button", "btn btn-secondary btn-sm set-add", "+ 添加 provider");
    add.type = "button";
    add.onclick = () => list.appendChild(this._providerCard({}));
    body.appendChild(add);
  }

  _providerCard(p) {
    const card = el("div", "set-card");
    card.dataset.cfgScope = "";
    const head = el("div", "set-card-head");
    const name = makeControl({ key: "name", type: "text", ph: "provider 名（全局唯一，必填）" });
    head.appendChild(name);
    head.appendChild(this._cardDelBtn(card, "删除该 provider"));
    card.appendChild(head);
    for (const spec of PROVIDER_BASE_FIELDS) card.appendChild(fieldRow(spec));
    card.appendChild(this._advDetails(PROVIDER_ADV_FIELDS));

    card.appendChild(el("div", "set-subtitle", "模型目录"));
    const models = el("div", "set-models");
    card.appendChild(models);
    for (const m of p.models || []) models.appendChild(this._modelCard(m));
    const add = el("button", "btn btn-secondary btn-sm set-add", "+ 添加模型");
    add.type = "button";
    add.onclick = () => models.appendChild(this._modelCard({}));
    card.appendChild(add);

    fillScope(card, p); // 直属字段；models 在嵌套 scope 中不受影响
    return card;
  }

  _modelCard(m) {
    const card = el("div", "set-card is-nested");
    card.dataset.cfgScope = "";
    const head = el("div", "set-card-head");
    head.appendChild(el("span", "set-card-tag", "model"));
    head.appendChild(this._cardDelBtn(card, "删除该模型"));
    card.appendChild(head);
    for (const spec of MODEL_FIELDS) card.appendChild(fieldRow(spec));
    fillScope(card, m);
    return card;
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
      for (const mc of card.querySelector(":scope > .set-models").children) {
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
    const tip = el("div", "set-hint set-tip", "两种传输二选一：command（stdio 子进程）或 url（远程 HTTP）。header 值支持 ${VAR} 环境变量引用（令牌不落盘）。工具名格式 mcp__{服务器名}__{工具名}。");
    body.appendChild(tip);
    const list = el("div", "set-cards");
    body.appendChild(list);
    refs.list = list;
    for (const [name, srv] of Object.entries(this.cfg.mcp_servers || {})) list.appendChild(this._mcpCard(name, srv));
    const add = el("button", "btn btn-secondary btn-sm set-add", "+ 添加 MCP 服务器");
    add.type = "button";
    add.onclick = () => list.appendChild(this._mcpCard("", {}));
    body.appendChild(add);
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
  // 「保存后连接」；status → 已连接 N 工具 / 连接失败。
  _setMcpBadge(card, status) {
    const badge = card.querySelector(":scope > .set-card-head .mcp-status");
    if (!badge) return;
    badge.className = "mcp-status";
    badge.title = "";
    if (status === undefined) {
      badge.textContent = "";
      return;
    }
    if (status === null) {
      badge.textContent = "保存后连接";
      return;
    }
    if (status.connected) {
      badge.classList.add("is-live");
      badge.textContent = `已连接 · ${(status.tools || []).length} 工具`;
      badge.title = (status.tools || []).join("\n");
    } else {
      badge.classList.add("is-dead");
      badge.textContent = "连接失败";
      badge.title = status.error || "";
    }
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
    }
  }

  _mcpCard(name, srv) {
    const card = el("div", "set-card");
    const head = el("div", "set-card-head");
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

    // 传输形态切换：由 command/url 哪个有值推定；切换只影响展示哪组字段
    const transport = el("select", "set-input");
    for (const [v, label] of [["stdio", "command（stdio 子进程）"], ["http", "url（远程 HTTP）"]]) {
      const o = el("option", "", label);
      o.value = v;
      transport.appendChild(o);
    }
    transport.value = srv.url ? "http" : "stdio";
    const tRow = el("div", "set-row");
    tRow.appendChild(el("label", "set-label", "传输方式"));
    const tField = el("div", "set-field");
    tField.appendChild(transport);
    tRow.appendChild(tField);
    card.appendChild(tRow);

    const stdio = el("div", "set-group");
    stdio.dataset.cfgScope = "";
    stdio.dataset.transport = "stdio";
    for (const spec of [
      { key: "command", label: "命令", ph: "如 npx" },
      { key: "args", label: "参数", type: "list-text", hint: "每行一个参数" },
      { key: "env", label: "环境变量", type: "kv-text", hint: "每行一个 KEY=VALUE（追加到进程环境）" },
      { key: "cwd", label: "工作目录", hint: "留空继承 loom 的工作目录" },
    ]) stdio.appendChild(fieldRow(spec));
    card.appendChild(stdio);

    const http = el("div", "set-group");
    http.dataset.cfgScope = "";
    http.dataset.transport = "http";
    for (const spec of [
      { key: "url", label: "URL", ph: "https://mcp.example.com/mcp" },
      { key: "headers", label: "请求头", type: "kv-text", hint: "每行一个 KEY=VALUE；值支持 ${VAR} 引用" },
    ]) http.appendChild(fieldRow(spec));
    card.appendChild(http);

    const common = el("div", "set-group");
    common.dataset.cfgScope = "";
    common.dataset.transport = "common";
    for (const spec of [
      { key: "startup_timeout_sec", label: "启动超时 (秒)", type: "number", ph: "30" },
      { key: "tool_timeout_sec", label: "工具调用超时 (秒)", type: "number", ph: "300" },
      { key: "enabled_tools", label: "工具白名单", type: "list-text", hint: "留空注册全部工具" },
      { key: "disabled_tools", label: "工具黑名单", type: "list-text" },
    ]) common.appendChild(fieldRow(spec));
    card.appendChild(common);

    const syncTransport = () => {
      stdio.hidden = transport.value !== "stdio";
      http.hidden = transport.value !== "http";
    };
    transport.onchange = syncTransport;
    syncTransport();

    fillScope(stdio, srv);
    fillScope(http, srv);
    fillScope(common, srv);
    return card;
  }

  _collectMcp(cfg) {
    const refs = this._tabRefs.mcp;
    const servers = {};
    for (const card of refs.list.children) {
      const name = card.querySelector(":scope > .set-card-head .set-input").value.trim();
      if (!name) continue;
      const transport = card.querySelector(":scope > .set-row .set-input").value;
      const srv = {};
      for (const group of card.querySelectorAll(":scope > .set-group")) {
        if (group.dataset.transport === "common" || group.dataset.transport === transport) collectScope(group, srv);
      }
      if (name) {
        if (servers[name]) this._skippedCards++; // 重名：后者覆盖前者
        servers[name] = srv;
      } else if (Object.keys(srv).length) {
        this._skippedCards++;
      }
    }
    if (Object.keys(servers).length) cfg.mcp_servers = servers;
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
    card.appendChild(fieldRow({ key: "root", label: "根目录", ph: "~/workspace/project" }));
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
    for (const spec of fields) inner.appendChild(fieldRow(spec));
    det.appendChild(inner);
    return det;
  }

  _cardDelBtn(card, title) {
    const del = el("button", "icon-btn set-card-del");
    del.type = "button";
    del.title = title;
    del.innerHTML = icon("trash");
    del.onclick = () => {
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
