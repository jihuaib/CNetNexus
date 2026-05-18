<script setup>
import { ref, onMounted, onBeforeUnmount, nextTick, watch } from 'vue';
import TerminalPane from './TerminalPane.vue';

const props = defineProps({
    nodes: { type: Array, required: true },
    activeId: { type: String, default: null },
    minimized: { type: Boolean, default: false }
});

const emit = defineEmits(['close', 'close-tab', 'switch-tab', 'minimize']);

const TOPBAR_H = 52;
const MIN_W = 420;
const MIN_H = 240;

const W0 = 900;
const H0 = 520;

const pos  = ref({ x: 0, y: 0 });
const size = ref({ w: W0, h: H0 });
const paneRefs = ref({});

const maximized = ref(false);
const savedState = ref(null);

function clamp(val, min, max) { return Math.max(min, Math.min(max, val)); }

function fitActivePane()
{
    const active = paneRefs.value[props.activeId];
    active?.fit?.();
}

function setPaneRef(id, el)
{
    if (el) paneRefs.value[id] = el;
    else    delete paneRefs.value[id];
}

function terminalTitle(node)
{
    const endpoint = node.type === 'frr' ? 'vtysh' : `127.0.0.1:${node.instance?.hostPort || '-'}`;
    return `${node.label}  ${node.image}  ${endpoint}`;
}

function initPos()
{
    const w = Math.min(size.value.w, window.innerWidth - 40);
    const h = Math.min(size.value.h, window.innerHeight - TOPBAR_H - 40);
    size.value = { w: Math.max(MIN_W, w), h: Math.max(MIN_H, h) };
    pos.value = {
        x: Math.max(20, (window.innerWidth - size.value.w) / 2),
        y: Math.max(TOPBAR_H + 10, (window.innerHeight - size.value.h) / 2)
    };
}

function toggleMaximize()
{
    if (maximized.value)
    {
        if (savedState.value)
        {
            pos.value = { ...savedState.value.pos };
            size.value = { ...savedState.value.size };
        }
        maximized.value = false;
    }
    else
    {
        savedState.value = { pos: { ...pos.value }, size: { ...size.value } };
        pos.value = { x: 0, y: TOPBAR_H };
        size.value = { w: window.innerWidth, h: window.innerHeight - TOPBAR_H };
        maximized.value = true;
    }
    nextTick(fitActivePane);
}

// 拖动
let dragState = null;
function onHeaderMouseDown(e)
{
    if (e.target.closest('.no-drag')) return;
    if (e.button !== 0) return;
    if (maximized.value) return;
    e.preventDefault();
    dragState = { offX: e.clientX - pos.value.x, offY: e.clientY - pos.value.y };
    window.addEventListener('mousemove', onDragMove);
    window.addEventListener('mouseup', onDragEnd);
}
function onHeaderDblClick() { toggleMaximize(); }
function onDragMove(e)
{
    if (!dragState) return;
    pos.value = {
        x: clamp(e.clientX - dragState.offX, -size.value.w + 120, window.innerWidth - 120),
        y: clamp(e.clientY - dragState.offY, 0, window.innerHeight - 40)
    };
}
function onDragEnd()
{
    dragState = null;
    window.removeEventListener('mousemove', onDragMove);
    window.removeEventListener('mouseup', onDragEnd);
}

// 右下角缩放
let resizeState = null;
function onResizeMouseDown(e)
{
    if (e.button !== 0) return;
    e.preventDefault();
    e.stopPropagation();
    resizeState = {
        startX: e.clientX,
        startY: e.clientY,
        startW: size.value.w,
        startH: size.value.h
    };
    window.addEventListener('mousemove', onResizeMove);
    window.addEventListener('mouseup', onResizeEnd);
}
function onResizeMove(e)
{
    if (!resizeState) return;
    size.value = {
        w: clamp(resizeState.startW + (e.clientX - resizeState.startX), MIN_W, window.innerWidth - pos.value.x - 10),
        h: clamp(resizeState.startH + (e.clientY - resizeState.startY), MIN_H, window.innerHeight - pos.value.y - 10)
    };
    fitActivePane();
}
function onResizeEnd()
{
    resizeState = null;
    window.removeEventListener('mousemove', onResizeMove);
    window.removeEventListener('mouseup', onResizeEnd);
    nextTick(fitActivePane);
}

function onWindowResize()
{
    if (maximized.value)
    {
        pos.value = { x: 0, y: TOPBAR_H };
        size.value = { w: window.innerWidth, h: window.innerHeight - TOPBAR_H };
        nextTick(fitActivePane);
        return;
    }
    pos.value = {
        x: clamp(pos.value.x, -size.value.w + 120, window.innerWidth - 120),
        y: clamp(pos.value.y, 0, window.innerHeight - 40)
    };
    size.value = {
        w: Math.min(size.value.w, window.innerWidth - 20),
        h: Math.min(size.value.h, window.innerHeight - 20)
    };
    nextTick(fitActivePane);
}

onMounted(() =>
{
    initPos();
    window.addEventListener('resize', onWindowResize);
    // 初次打开后多做几次 fit，避免 xterm 首次渲染时容器尺寸还没稳定
    nextTick(() => fitActivePane());
    setTimeout(fitActivePane, 60);
    setTimeout(fitActivePane, 200);
});

watch(() => props.minimized, (v) =>
{
    if (!v) nextTick(fitActivePane);
});

watch(() => props.activeId, () =>
{
    nextTick(fitActivePane);
    setTimeout(fitActivePane, 40);
});

// 新增 tab 时也重新 fit
watch(() => props.nodes.length, () =>
{
    nextTick(fitActivePane);
    setTimeout(fitActivePane, 60);
});

onBeforeUnmount(() =>
{
    window.removeEventListener('resize', onWindowResize);
    window.removeEventListener('mousemove', onDragMove);
    window.removeEventListener('mouseup', onDragEnd);
    window.removeEventListener('mousemove', onResizeMove);
    window.removeEventListener('mouseup', onResizeEnd);
});
</script>

<template>
    <div
        v-show="!minimized"
        class="win"
        :class="{ maximized }"
        :style="{ left: pos.x + 'px', top: pos.y + 'px', width: size.w + 'px', height: size.h + 'px' }"
    >
        <header class="win-header" @mousedown="onHeaderMouseDown" @dblclick="onHeaderDblClick">
            <div class="tabs">
                <div
                    v-for="n in nodes"
                    :key="n.id"
                    class="tab no-drag"
                    :class="{ active: n.id === activeId }"
                    :title="terminalTitle(n)"
                    @mousedown.stop
                    @click="emit('switch-tab', n.id)"
                >
                    <span class="tab-dot"></span>
                    <span class="tab-label">{{ n.label }}</span>
                    <span
                        class="tab-close"
                        @click.stop="emit('close-tab', n.id)"
                        title="关闭该标签"
                    >×</span>
                </div>
            </div>
            <div class="win-actions no-drag">
                <button class="win-btn" @click="emit('minimize')" title="最小化">
                    <span class="ico ico-min"></span>
                </button>
                <button
                    class="win-btn"
                    @click="toggleMaximize"
                    :title="maximized ? '还原' : '最大化'"
                >
                    <span class="ico" :class="maximized ? 'ico-restore' : 'ico-max'"></span>
                </button>
                <button class="win-btn win-close" @click="emit('close')" title="关闭终端窗口">×</button>
            </div>
        </header>

        <div class="win-body">
            <div
                v-for="n in nodes"
                :key="n.id"
                class="pane"
                v-show="n.id === activeId"
            >
                <TerminalPane
                    :ref="(el) => setPaneRef(n.id, el)"
                    :node="n"
                    :visible="n.id === activeId"
                />
            </div>
        </div>

        <div
            v-if="!maximized"
            class="resizer"
            @mousedown="onResizeMouseDown"
            title="拖拽调整大小"
        ></div>
    </div>
</template>

<style scoped>
.win {
    position: fixed;
    z-index: 100;
    background: #0f1320;
    border: 1px solid #d6dde8;
    border-radius: 8px;
    box-shadow: 0 20px 60px rgba(31, 41, 55, 0.35);
    display: flex;
    flex-direction: column;
    overflow: hidden;
    min-width: 420px;
    min-height: 240px;
}
.win.maximized { border-radius: 0; box-shadow: none; }

.win-header {
    height: 38px;
    background: #ffffff;
    border-bottom: 1px solid #e3e8ef;
    display: flex;
    align-items: stretch;
    cursor: move;
    -webkit-user-select: none;
    user-select: none;
    flex-shrink: 0;
}
.win.maximized .win-header { cursor: default; }

.tabs {
    flex: 1;
    display: flex;
    align-items: stretch;
    overflow-x: auto;
    overflow-y: hidden;
    min-width: 0;
}
.tab {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 0 10px 0 12px;
    font-size: 12px;
    color: #6b7280;
    cursor: pointer;
    border-right: 1px solid #e3e8ef;
    max-width: 180px;
    background: #f5f7fa;
    flex-shrink: 0;
}
.tab:hover { background: #eef2ff; color: #3a6cf6; }
.tab.active {
    background: #ffffff;
    color: #1f2937;
    font-weight: 600;
    border-bottom: 2px solid #3a6cf6;
}
.tab-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: #16a34a;
    flex-shrink: 0;
}
.tab-label {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}
.tab-close {
    margin-left: 4px;
    color: #9ca3af;
    padding: 0 4px;
    border-radius: 3px;
    font-size: 16px;
    line-height: 1;
}
.tab-close:hover { background: #fee2e2; color: #dc2626; }

.win-actions {
    display: flex;
    align-items: stretch;
    border-left: 1px solid #e3e8ef;
}
.win-btn {
    background: transparent;
    border: none;
    color: #6b7280;
    width: 38px;
    cursor: pointer;
    padding: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    border-left: 1px solid #e3e8ef;
}
.win-btn:first-child { border-left: none; }
.win-btn:hover { color: #3a6cf6; background: #eef2ff; }
.win-close { font-size: 20px; line-height: 1; }
.win-close:hover { color: #dc2626; background: #fee2e2; }

.ico {
    display: inline-block;
    width: 12px;
    height: 12px;
    position: relative;
}
.ico-min::before {
    content: '';
    position: absolute;
    left: 0; right: 0; bottom: 1px;
    height: 2px;
    background: currentColor;
    border-radius: 1px;
}
.ico-max {
    border: 2px solid currentColor;
    border-radius: 2px;
}
.ico-restore::before,
.ico-restore::after {
    content: '';
    position: absolute;
    width: 9px;
    height: 9px;
    border: 2px solid currentColor;
    border-radius: 2px;
    box-sizing: border-box;
}
.ico-restore::before { top: 0; right: 0; background: transparent; }
.ico-restore::after  { bottom: 0; left: 0; background: #ffffff; }
.win-btn:hover .ico-restore::after { background: #eef2ff; }

.win-body {
    flex: 1;
    position: relative;
    background: #0f1320;
    min-height: 0;
}
.pane {
    position: absolute;
    inset: 0;
}

.resizer {
    position: absolute;
    right: 0;
    bottom: 0;
    width: 16px;
    height: 16px;
    cursor: nwse-resize;
    background:
        linear-gradient(135deg, transparent 50%, #6b7280 50%, #6b7280 60%, transparent 60%, transparent 70%, #6b7280 70%, #6b7280 80%, transparent 80%);
    opacity: 0.55;
}
.resizer:hover { opacity: 1; }
</style>
