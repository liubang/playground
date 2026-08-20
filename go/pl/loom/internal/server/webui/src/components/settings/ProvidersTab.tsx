// ProvidersTab.tsx — 模型 tab：启动模型 + provider 卡片（概览 → provider
// 详情 → 模型明细 三级导航）。所有卡片 DOM 始终挂载在同一个列表容器里，
// 层级切换只改 CSS 类（概览态折叠成一行摘要，详情态只显示当前卡片）。

import { useMemo } from 'react'
import type { SettingsDraft, ProviderDraft, CardDraft, ControlState } from './SettingsPanel'
import { draftId } from './draft'
import {
  PROVIDER_BASE_FIELDS,
  PROVIDER_ADV_FIELDS,
  MODEL_FIELDS,
  DEFAULT_MODEL_FIELD,
  type FieldSpec,
} from './spec'
import { collectFields } from './convert'
import { FieldRow, SetNavBar, SetCardSummary, CardDelBtn } from './controls'
import { useSettingsCtx } from './SettingsPanel'

const PROVIDER_ALL_FIELDS: FieldSpec[] = [
  ...PROVIDER_BASE_FIELDS,
  ...PROVIDER_ADV_FIELDS,
  { key: 'name' },
]

// 概览行摘要数据：与收集逻辑同源地读当前草稿。
function providerSummary(card: ProviderDraft): { name: string; meta: string } {
  const p: Record<string, unknown> = {}
  collectFields(PROVIDER_ALL_FIELDS, card.fields, p)
  return {
    name: (p.name as string) || '（未命名 provider）',
    meta: [
      (p.type as string) || 'openai',
      (p.base_url as string) || '未配置 Base URL',
      `${card.models.length} 个模型`,
    ].join(' · '),
  }
}

function modelSummary(card: CardDraft): { name: string; meta: string } {
  const m: Record<string, unknown> = {}
  collectFields(MODEL_FIELDS, card.fields, m)
  const parts: string[] = []
  if (m.context_window) parts.push(`上下文 ${m.context_window}`)
  if (m.max_output_tokens) parts.push(`输出上限 ${m.max_output_tokens}`)
  if (Array.isArray(m.modalities) && (m.modalities as string[]).includes('image')) {
    parts.push('多模态')
  }
  return {
    name: (m.name as string) || '（未命名模型）',
    meta: parts.join(' · ') || '跟随 provider / 全局默认',
  }
}

export function ProvidersTab({
  draft,
  setDraft,
  openProviderId,
  openModelId,
  setOpenProviderId,
  setOpenModelId,
  setGlobal,
}: {
  draft: SettingsDraft
  setDraft: React.Dispatch<React.SetStateAction<SettingsDraft>>
  openProviderId: string | null
  openModelId: string | null
  setOpenProviderId: (id: string | null) => void
  setOpenModelId: (id: string | null) => void
  setGlobal: (key: string, v: ControlState) => void
}) {
  const { markDirty, invalid, reveal } = useSettingsCtx()

  const openCard = draft.providers.find((c) => c.id === openProviderId) || null
  const openModel = openCard?.models.find((m) => m.id === openModelId) || null

  const crumb = useMemo(() => {
    if (!openCard) return ''
    const name = String(openCard.fields.name || '').trim() || '未命名'
    let text = '模型提供方 / ' + name
    if (openModel) {
      text += ' / ' + (String(openModel.fields.name || '').trim() || '未命名模型')
    }
    return text
  }, [openCard, openModel])

  // 返回：第三级（模型明细）→ 第二级（provider 详情）→ 概览
  const back = () => {
    if (openModelId) {
      setOpenModelId(null)
      return
    }
    setOpenProviderId(null)
  }

  const patchCard = (cardId: string, key: string, v: ControlState) => {
    setDraft((d) => ({
      ...d,
      providers: d.providers.map((c) =>
        c.id === cardId ? { ...c, fields: { ...c.fields, [key]: v } } : c,
      ),
    }))
    markDirty()
  }

  const patchModel = (cardId: string, modelId: string, key: string, v: ControlState) => {
    setDraft((d) => ({
      ...d,
      providers: d.providers.map((c) =>
        c.id === cardId
          ? {
              ...c,
              models: c.models.map((m) =>
                m.id === modelId ? { ...m, fields: { ...m.fields, [key]: v } } : m,
              ),
            }
          : c,
      ),
    }))
    markDirty()
  }

  const addProvider = () => {
    const card: ProviderDraft = { id: draftId(), fields: { type: 'openai' }, models: [] }
    setDraft((d) => ({ ...d, providers: [...d.providers, card] }))
    markDirty()
    setOpenProviderId(card.id)
  }

  const deleteProvider = (card: ProviderDraft) => {
    if (openProviderId === card.id) {
      setOpenModelId(null)
      setOpenProviderId(null)
    }
    setDraft((d) => ({ ...d, providers: d.providers.filter((c) => c.id !== card.id) }))
    markDirty()
  }

  const addModel = (card: ProviderDraft) => {
    const mc: CardDraft = { id: draftId(), fields: {} }
    setDraft((d) => ({
      ...d,
      providers: d.providers.map((c) =>
        c.id === card.id ? { ...c, models: [...c.models, mc] } : c,
      ),
    }))
    markDirty()
    setOpenProviderId(card.id)
    setOpenModelId(mc.id)
  }

  const deleteModel = (card: ProviderDraft, modelId: string) => {
    if (openModelId === modelId) setOpenModelId(null)
    setDraft((d) => ({
      ...d,
      providers: d.providers.map((c) =>
        c.id === card.id ? { ...c, models: c.models.filter((m) => m.id !== modelId) } : c,
      ),
    }))
    markDirty()
  }

  const renderField = (
    spec: FieldSpec,
    fields: Record<string, ControlState>,
    onChange: (key: string, v: ControlState) => void,
    fieldIdPrefix: string,
    onReveal?: (() => Promise<string | null>) | null,
  ) => (
    <div key={spec.key} data-field-id={`${fieldIdPrefix}:${spec.key}`}>
      <FieldRow
        spec={spec}
        value={fields[spec.key] ?? (spec.type === 'bool' || spec.type === 'flag-list' ? false : '')}
        onChange={(v) => onChange(spec.key, v)}
        onReveal={onReveal}
        invalid={invalid === `${fieldIdPrefix}:${spec.key}`}
      />
    </div>
  )

  return (
    <>
      {/* 启动模型（全局 scope）；详情态由 CSS（.set-hier.is-detail）隐藏 */}
      <section className="set-sec set-sec-top">
        <h3 className="set-sec-title">启动模型</h3>
        <div data-field-id={DEFAULT_MODEL_FIELD.key}>
          <FieldRow
            spec={DEFAULT_MODEL_FIELD}
            value={draft.globals[DEFAULT_MODEL_FIELD.key] ?? ''}
            onChange={(v) => setGlobal(DEFAULT_MODEL_FIELD.key, v)}
            invalid={invalid === DEFAULT_MODEL_FIELD.key}
          />
        </div>
      </section>

      <SetNavBar hidden={!openCard} crumb={crumb} onBack={back} />

      <div className="set-list-sec">
        <h3 className="set-sec-title">模型提供方（至少一个）</h3>
        <div className="set-cards">
          {draft.providers.map((card) => {
            const sum = providerSummary(card)
            const isOpen = openProviderId === card.id
            const isModelDetail = isOpen && !!openModelId
            return (
              <div
                key={card.id}
                className={
                  'set-card' +
                  (isOpen ? ' is-open' : '') +
                  (isModelDetail ? ' is-model-detail' : '')
                }
              >
                <div className="set-card-head">
                  <SetCardSummary
                    name={sum.name}
                    nameEmpty={!String(card.fields.name || '').trim()}
                    meta={sum.meta}
                    onOpen={() => setOpenProviderId(card.id)}
                  />
                  <input
                    className="set-input"
                    type="text"
                    placeholder="provider 名（全局唯一，必填）"
                    spellCheck={false}
                    data-field-id={card.id + ':name'}
                    value={String(card.fields.name || '')}
                    readOnly={isModelDetail}
                    onChange={(e) => patchCard(card.id, 'name', e.target.value)}
                  />
                  <CardDelBtn
                    title="删除该 provider"
                    getConfirm={() => ({
                      title: '删除 provider',
                      body:
                        `将删除 provider「${String(card.fields.name || '').trim() || '未命名'}」` +
                        (card.models.length
                          ? `及其下 ${card.models.length} 个模型的配置`
                          : '的配置') +
                        '。保存后生效，未保存前重新加载可恢复。',
                      okLabel: '删除',
                    })}
                    onDelete={() => deleteProvider(card)}
                  />
                </div>
                <div className="set-card-body">
                  {PROVIDER_BASE_FIELDS.map((spec) =>
                    renderField(
                      spec,
                      card.fields,
                      (key, v) => patchCard(card.id, key, v),
                      card.id,
                      spec.key === 'api_key'
                        ? () =>
                            reveal({
                              kind: 'provider',
                              name: String(card.fields.name || '').trim(),
                            })
                        : null,
                    ),
                  )}
                  <details className="disclosure set-adv">
                    <summary>高级选项</summary>
                    <div className="set-adv-body">
                      {PROVIDER_ADV_FIELDS.map((spec) =>
                        renderField(
                          spec,
                          card.fields,
                          (key, v) => patchCard(card.id, key, v),
                          card.id,
                        ),
                      )}
                    </div>
                  </details>

                  <div className="set-subtitle">模型目录</div>
                  <div className="set-models">
                    {card.models.map((mc) => {
                      const ms = modelSummary(mc)
                      const mOpen = openModelId === mc.id
                      return (
                        <div
                          key={mc.id}
                          className={'set-card is-nested' + (mOpen ? ' is-open' : '')}
                        >
                          <div className="set-card-head">
                            <SetCardSummary
                              name={ms.name}
                              nameEmpty={!String(mc.fields.name || '').trim()}
                              meta={ms.meta}
                              onOpen={() => {
                                setOpenProviderId(card.id)
                                setOpenModelId(mc.id)
                              }}
                            />
                            <span className="set-card-tag">model</span>
                            <CardDelBtn
                              title="删除该模型"
                              onDelete={() => deleteModel(card, mc.id)}
                            />
                          </div>
                          <div className="set-card-body">
                            {MODEL_FIELDS.map((spec) =>
                              renderField(
                                spec,
                                mc.fields,
                                (key, v) => patchModel(card.id, mc.id, key, v),
                                mc.id,
                              ),
                            )}
                          </div>
                        </div>
                      )
                    })}
                  </div>
                  <button
                    type="button"
                    className="btn btn-secondary btn-sm set-add"
                    data-field-id={card.id + ':add-model'}
                    onClick={() => addModel(card)}
                  >
                    + 添加模型
                  </button>
                </div>
              </div>
            )
          })}
        </div>
        <button
          type="button"
          className="btn btn-secondary btn-sm set-add"
          data-field-id="add-provider"
          onClick={addProvider}
        >
          + 添加 provider
        </button>
      </div>
    </>
  )
}
