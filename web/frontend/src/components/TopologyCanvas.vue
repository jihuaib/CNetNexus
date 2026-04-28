<script setup>
import { ref, computed, watch } from 'vue';

const props = defineProps({
    nodes: { type: Array, required: true },
    links: { type: Array, required: true },
    selectedId: { type: String, default: null },
    selectedLinkId: { type: String, default: null },
    linkingMode: { type: Boolean, default: false },
    maxPorts: { type: Number, default: 8 },
    allPorts: { type: Array, default: () => ['GE-1', 'GE-2', 'GE-3', 'GE-4', 'GE-5', 'GE-6', 'GE-7', 'GE-8'] },
    linkCountOf: { type: Function, default: () => 0 },
    freePortsOf: { type: Function, default: () => [] }
});

const emit = defineEmits([
    'drop-device', 'move-node', 'select-node', 'select-link', 'create-link', 'delete-link',
    'delete-node', 'open-terminal', 'start-node', 'stop-node', 'exit-linking'
]);

// 连线模式状态
const linkSource = ref(null);    // { nodeId, port } 已确定起点和起点端口后
const portMenu = ref(null);      // { node, x, y, role: 'from' | 'to' } 端口选择浮窗

const mouse = ref({ x: 0, y: 0 });

function onCanvasMouseMove(e)
{
    if (!props.linkingMode) return;
    const rect = canvasRef.value.getBoundingClientRect();
    mouse.value = { x: e.clientX - rect.left, y: e.clientY - rect.top };
}

const NODE_W = 92;
const NODE_H = 92;
const NODE_R = 30; // 连线端点距节点中心的半径（贴近图标边缘）
const LINK_GAP = 10; // 同一对节点间多条平行线之间的间距

const canvasRef = ref(null);

function onDragOver(e) { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; }

function onDrop(e)
{
    e.preventDefault();
    const type = e.dataTransfer.getData('application/x-device');
    if (!type) return;
    const rect = canvasRef.value.getBoundingClientRect();
    const x = e.clientX - rect.left - NODE_W / 2;
    const y = e.clientY - rect.top - NODE_H / 2;
    emit('drop-device', { deviceType: type, x: Math.max(0, x), y: Math.max(0, y) });
}

// 节点拖动
let dragState = null;
function onNodeMouseDown(e, node)
{
    if (e.button !== 0) return;
    e.preventDefault();
    e.stopPropagation();

    // 连线模式下不拖动，仅作为点击源
    if (props.linkingMode) return;

    emit('select-link', null);
    emit('select-node', node.id);
    const rect = canvasRef.value.getBoundingClientRect();
    dragState = {
        id: node.id,
        offsetX: e.clientX - rect.left - node.x,
        offsetY: e.clientY - rect.top - node.y
    };
    window.addEventListener('mousemove', onMouseMove);
    window.addEventListener('mouseup', onMouseUp);
}

function onNodeClick(e, node)
{
    if (!props.linkingMode) return;
    e.stopPropagation();

    // 第二次点击必须是另一台设备
    if (linkSource.value && linkSource.value.nodeId === node.id)
    {
        linkSource.value = null;
        portMenu.value = null;
        return;
    }
    if (props.linkCountOf(node.id) >= props.maxPorts) return;

    // 弹出端口菜单（位置贴近节点中心略偏右）
    const role = linkSource.value ? 'to' : 'from';
    portMenu.value = {
        node,
        role,
        x: node.x + NODE_W + 6,
        y: node.y + 4
    };
}

function onPickPort(e, port)
{
    if (!portMenu.value) return;
    const node = portMenu.value.node;
    if (portMenu.value.role === 'from')
    {
        // 用当前事件坐标初始化预览线终点，避免还没动鼠标就甩到 (0,0)
        if (e && canvasRef.value)
        {
            const rect = canvasRef.value.getBoundingClientRect();
            mouse.value = { x: e.clientX - rect.left, y: e.clientY - rect.top };
        }
        linkSource.value = { nodeId: node.id, port };
        portMenu.value = null;
    }
    else
    {
        emit('create-link', {
            from: linkSource.value.nodeId,
            fromPort: linkSource.value.port,
            to: node.id,
            toPort: port
        });
        linkSource.value = null;
        portMenu.value = null;
    }
}
function onMouseMove(e)
{
    if (!dragState) return;
    const rect = canvasRef.value.getBoundingClientRect();
    const x = Math.max(0, e.clientX - rect.left - dragState.offsetX);
    const y = Math.max(0, e.clientY - rect.top - dragState.offsetY);
    emit('move-node', { id: dragState.id, x, y });
}
function onMouseUp()
{
    dragState = null;
    window.removeEventListener('mousemove', onMouseMove);
    window.removeEventListener('mouseup', onMouseUp);
}

function nodeCenter(node)
{
    return { x: node.x + NODE_W / 2, y: node.y + NODE_H / 2 };
}

const linkPaths = computed(() =>
{
    // 按 "无序节点对" 分组，多条线相互平行偏移
    const groups = new Map();
    for (const l of props.links)
    {
        const key = [l.from, l.to].sort().join('|');
        if (!groups.has(key)) groups.set(key, []);
        groups.get(key).push(l);
    }

    const out = [];
    for (const list of groups.values())
    {
        const total = list.length;
        list.forEach((l, idx) =>
        {
            const a = props.nodes.find(n => n.id === l.from);
            const b = props.nodes.find(n => n.id === l.to);
            if (!a || !b) return;
            const ca = nodeCenter(a);
            const cb = nodeCenter(b);

            const dx = cb.x - ca.x;
            const dy = cb.y - ca.y;
            const len = Math.max(1, Math.hypot(dx, dy));
            const ux = dx / len, uy = dy / len;
            const nx = -uy, ny = ux;

            // 居中分布的平行偏移
            const offset = (idx - (total - 1) / 2) * LINK_GAP;

            // 起止点缩到节点图标边缘外，避免线穿进设备里
            const sx = ca.x + ux * NODE_R + nx * offset;
            const sy = ca.y + uy * NODE_R + ny * offset;
            const ex = cb.x - ux * NODE_R + nx * offset;
            const ey = cb.y - uy * NODE_R + ny * offset;

            // 端口标签贴近各自端点
            const lerp = (t) => ({ x: sx + (ex - sx) * t, y: sy + (ey - sy) * t });
            const pa = lerp(0.18);
            const pb = lerp(0.82);

            out.push({
                id: l.id,
                x1: sx, y1: sy, x2: ex, y2: ey,
                fromPort: l.fromPort || '',
                toPort: l.toPort || '',
                fromLabel: pa,
                toLabel: pb
            });
        });
    }
    return out;
});

function onCanvasMouseDown(e)
{
    if (e.target !== canvasRef.value) return;
    if (e.button !== 0) return;

    // 连线模式下点空白：取消已选起点和端口菜单
    if (props.linkingMode)
    {
        linkSource.value = null;
        portMenu.value = null;
        return;
    }
    emit('select-node', null);
    emit('select-link', null);
    closeMenu();
}

function onLinkClick(e, linkId)
{
    if (props.linkingMode) return;
    e.stopPropagation();
    emit('select-link', linkId);
    emit('select-node', null);
    // 确保画布拿到焦点，Delete 键才能触发
    canvasRef.value?.focus({ preventScroll: true });
}

function onLinkContextMenu(e, linkId)
{
    if (props.linkingMode) return;
    e.preventDefault();
    e.stopPropagation();
    emit('select-link', linkId);
    // 借用现有的右键菜单：在 menu.value 标记 isLink
    const rect = canvasRef.value.getBoundingClientRect();
    menu.value = {
        link: linkId,
        x: e.clientX - rect.left,
        y: e.clientY - rect.top
    };
    window.addEventListener('mousedown', onGlobalMouseDown, true);
    window.addEventListener('keydown', onGlobalKeyDown, true);
}

function onCanvasKeyDown(e)
{
    if (e.key === 'Escape')
    {
        if (props.linkingMode)
        {
            if (portMenu.value)   { portMenu.value = null;   e.preventDefault(); return; }
            if (linkSource.value) { linkSource.value = null; e.preventDefault(); return; }
            emit('exit-linking');
            e.preventDefault();
        }
        else if (props.selectedLinkId)
        {
            emit('select-link', null);
            e.preventDefault();
        }
        return;
    }
    if ((e.key === 'Delete' || e.key === 'Backspace') && props.selectedLinkId && !props.linkingMode)
    {
        emit('delete-link', props.selectedLinkId);
        emit('select-link', null);
        e.preventDefault();
    }
}

function onKey(e, node)
{
    if (e.key === 'Delete' || e.key === 'Backspace')
    {
        emit('delete-node', node.id);
    }
}

// 不再向 window 注册任何 keydown，避免与终端等其他场景冲突
// Esc / Delete 由画布自身的 keydown 处理（见 onCanvasKeyDown），
// 画布 tabindex=0，需要焦点才生效。进入连线模式时自动聚焦。
watch(() => props.linkingMode, (v) =>
{
    if (v && canvasRef.value) canvasRef.value.focus({ preventScroll: true });
    if (!v) { linkSource.value = null; portMenu.value = null; }
});

// 右键菜单
const menu = ref(null); // { node, x, y }
function onNodeContextMenu(e, node)
{
    e.preventDefault();
    e.stopPropagation();
    emit('select-link', null);
    emit('select-node', node.id);
    const rect = canvasRef.value.getBoundingClientRect();
    menu.value = {
        node,
        x: e.clientX - rect.left,
        y: e.clientY - rect.top
    };
    window.addEventListener('mousedown', onGlobalMouseDown, true);
    window.addEventListener('keydown', onGlobalKeyDown, true);
}
function closeMenu()
{
    if (!menu.value) return;
    menu.value = null;
    window.removeEventListener('mousedown', onGlobalMouseDown, true);
    window.removeEventListener('keydown', onGlobalKeyDown, true);
}
function onGlobalMouseDown(e)
{
    if (e.target.closest('.ctx-menu')) return;
    closeMenu();
}
function onGlobalKeyDown(e)
{
    if (e.key === 'Escape') closeMenu();
}

function menuAction(action)
{
    if (!menu.value) return;
    const m = menu.value;
    closeMenu();
    if (m.link)
    {
        if (action === 'delete-link')
        {
            emit('delete-link', m.link);
            emit('select-link', null);
        }
        return;
    }
    const node = m.node;
    if (action === 'start')    emit('start-node', node);
    if (action === 'stop')     emit('stop-node', node);
    if (action === 'terminal') emit('open-terminal', node);
    if (action === 'delete')   emit('delete-node', node.id);
}
</script>

<template>
    <div
        ref="canvasRef"
        class="canvas"
        :class="{ 'mode-linking': linkingMode }"
        tabindex="0"
        @dragover="onDragOver"
        @drop="onDrop"
        @mousedown="onCanvasMouseDown"
        @mousemove="onCanvasMouseMove"
        @keydown="onCanvasKeyDown"
        @contextmenu.prevent
    >
        <svg class="links" xmlns="http://www.w3.org/2000/svg">
            <g v-for="p in linkPaths" :key="p.id">
                <line
                    :x1="p.x1" :y1="p.y1" :x2="p.x2" :y2="p.y2"
                    stroke="transparent" stroke-width="14"
                    stroke-linecap="round"
                    class="link-hit"
                    @mousedown.stop="onLinkClick($event, p.id)"
                    @contextmenu="onLinkContextMenu($event, p.id)"
                />
                <line
                    :x1="p.x1" :y1="p.y1" :x2="p.x2" :y2="p.y2"
                    :stroke="p.id === props.selectedLinkId ? '#3a6cf6' : '#94a3b8'"
                    :stroke-width="p.id === props.selectedLinkId ? 2.5 : 1.6"
                    stroke-linecap="round"
                    class="link-line"
                />
                <text :x="p.fromLabel.x" :y="p.fromLabel.y - 4" class="port-label">{{ p.fromPort }}</text>
                <text :x="p.toLabel.x"   :y="p.toLabel.y   - 4" class="port-label">{{ p.toPort }}</text>
            </g>
            <line
                v-if="linkingMode && linkSource"
                :x1="nodes.find(n => n.id === linkSource.nodeId) ? nodeCenter(nodes.find(n => n.id === linkSource.nodeId)).x : 0"
                :y1="nodes.find(n => n.id === linkSource.nodeId) ? nodeCenter(nodes.find(n => n.id === linkSource.nodeId)).y : 0"
                :x2="mouse.x"
                :y2="mouse.y"
                stroke="#3a6cf6"
                stroke-width="2"
                stroke-dasharray="6 4"
            />
        </svg>

        <div
            v-for="node in nodes"
            :key="node.id"
            class="node"
            :class="{
                selected: node.id === selectedId,
                'link-source': linkSource && node.id === linkSource.nodeId,
                'link-target': linkingMode && linkSource && linkSource.nodeId !== node.id,
                'link-full': linkCountOf(node.id) >= maxPorts,
                ['status-' + node.status]: true
            }"
            :style="{ left: node.x + 'px', top: node.y + 'px', width: NODE_W + 'px', height: NODE_H + 'px' }"
            :data-node-id="node.id"
            tabindex="0"
            @mousedown="onNodeMouseDown($event, node)"
            @click.stop="onNodeClick($event, node)"
            @dblclick.prevent.stop="$emit('open-terminal', node)"
            @contextmenu="onNodeContextMenu($event, node)"
            @keydown="onKey($event, node)"
        >
            <img src="/icon.ico" class="node-icon" alt="" draggable="false" />
            <div class="node-label" :title="node.label">{{ node.label }}</div>
            <div class="node-ports">{{ linkCountOf(node.id) }}/{{ maxPorts }}</div>
        </div>

        <div class="hint" v-if="nodes.length === 0">从左侧拖一个设备过来 →</div>
        <div class="banner" v-if="linkingMode">
            <template v-if="!linkSource && !portMenu">连线模式：点击第一台设备并选择端口（Esc 取消）</template>
            <template v-else-if="!linkSource && portMenu">为起点选择端口</template>
            <template v-else-if="linkSource && !portMenu">起点已选 {{ linkSource.port }}，点击目标设备</template>
            <template v-else>为目标设备选择端口</template>
        </div>

        <ul
            v-if="portMenu"
            class="port-menu"
            :style="{ left: portMenu.x + 'px', top: portMenu.y + 'px' }"
            @mousedown.stop
            @click.stop
        >
            <li class="port-menu-title">{{ portMenu.role === 'from' ? '起点端口' : '目标端口' }}</li>
            <li
                v-for="p in allPorts"
                :key="p"
                :class="{ disabled: !freePortsOf(portMenu.node.id).includes(p) }"
                @click="freePortsOf(portMenu.node.id).includes(p) && onPickPort($event, p)"
            >{{ p }}</li>
        </ul>

        <ul
            v-if="menu"
            class="ctx-menu"
            :style="{ left: menu.x + 'px', top: menu.y + 'px' }"
            @contextmenu.prevent
            @mousedown.stop
        >
            <template v-if="menu.link">
                <li class="danger" @click="menuAction('delete-link')">删除连线</li>
            </template>
            <template v-else>
                <li
                    :class="{ disabled: menu.node.status === 'running' || menu.node.status === 'starting' }"
                    @click="menuAction('start')"
                >启动</li>
                <li
                    :class="{ disabled: menu.node.status !== 'running' }"
                    @click="menuAction('stop')"
                >停止</li>
                <li
                    :class="{ disabled: menu.node.status !== 'running' }"
                    @click="menuAction('terminal')"
                >网页连接</li>
                <li class="sep"></li>
                <li class="danger" @click="menuAction('delete')">删除</li>
            </template>
        </ul>
    </div>
</template>

<style scoped>
.canvas {
    position: relative;
    width: 100%;
    height: 100%;
    overflow: auto;
    outline: none;
}
.canvas.mode-linking {
    cursor: crosshair;
}
.canvas.mode-linking .node {
    cursor: crosshair;
}
.links {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    pointer-events: none;
}
.links .link-hit {
    pointer-events: stroke;
    cursor: pointer;
}
.links .link-line {
    pointer-events: none;
    transition: stroke 0.1s;
}
.canvas.mode-linking .links .link-hit {
    pointer-events: none;
    cursor: default;
}
.node {
    position: absolute;
    background: transparent;
    border: 2px solid transparent;
    border-radius: 8px;
    padding: 6px 4px;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 4px;
    cursor: move;
    transition: border-color 0.1s, box-shadow 0.1s, background 0.1s;
    -webkit-user-select: none;
    -moz-user-select: none;
    -ms-user-select: none;
    user-select: none;
    -webkit-user-drag: none;
}
.node, .node * {
    -webkit-user-select: none;
    -moz-user-select: none;
    -ms-user-select: none;
    user-select: none;
}
.node:focus { outline: none; }
.node:hover .node-label {
    color: #3a6cf6;
}
.node.selected .node-label {
    color: #3a6cf6;
    background: rgba(58, 108, 246, 0.08);
    border-radius: 3px;
}

.node-icon {
    width: 48px;
    height: 48px;
    display: block;
    flex-shrink: 0;
    -webkit-user-drag: none;
}
.node.status-running .node-icon  { filter: drop-shadow(0 0 4px rgba(22, 163, 74, 0.7)); }
.node.status-starting .node-icon { filter: drop-shadow(0 0 4px rgba(217, 119, 6, 0.7)); }
.node.status-error .node-icon    { filter: drop-shadow(0 0 4px rgba(220, 38, 38, 0.7)); }
.node.status-stopped .node-icon  { filter: grayscale(0.4) opacity(0.85); }

.node-label {
    max-width: 100%;
    font-size: 12px;
    font-weight: 600;
    color: #1f2937;
    text-align: center;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    padding: 0 4px;
}
.node-ports {
    position: absolute;
    bottom: -2px;
    right: -2px;
    background: #3a6cf6;
    color: #ffffff;
    font-size: 10px;
    line-height: 1;
    padding: 2px 5px;
    border-radius: 8px;
    font-family: ui-monospace, Menlo, Consolas, monospace;
    opacity: 0;
    transition: opacity 0.15s;
    pointer-events: none;
}
.node:hover .node-ports,
.node.selected .node-ports,
.canvas.mode-linking .node-ports { opacity: 1; }
.node.link-full .node-ports { background: #dc2626; }

.node.link-source .node-icon {
    filter: drop-shadow(0 0 6px rgba(58, 108, 246, 0.85));
}
.node.link-source .node-label {
    color: #3a6cf6;
    background: rgba(58, 108, 246, 0.10);
    border-radius: 3px;
}
.canvas.mode-linking .node.link-target:hover:not(.link-full) .node-icon {
    filter: drop-shadow(0 0 6px rgba(22, 163, 74, 0.85));
}
.canvas.mode-linking .node.link-target:hover:not(.link-full) .node-label {
    color: #16a34a;
}
.canvas.mode-linking .node.link-full {
    cursor: not-allowed;
    opacity: 0.6;
}

.banner {
    position: absolute;
    top: 12px;
    left: 50%;
    transform: translateX(-50%);
    background: #3a6cf6;
    color: #ffffff;
    font-size: 12px;
    padding: 6px 14px;
    border-radius: 16px;
    box-shadow: 0 4px 12px rgba(58, 108, 246, 0.3);
    pointer-events: none;
}

.hint {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    color: #94a3b8;
    font-size: 14px;
    pointer-events: none;
}

.ctx-menu {
    position: absolute;
    z-index: 50;
    list-style: none;
    margin: 0;
    padding: 4px 0;
    min-width: 140px;
    background: #ffffff;
    border: 1px solid #e3e8ef;
    border-radius: 6px;
    box-shadow: 0 8px 24px rgba(31, 41, 55, 0.12);
    user-select: none;
}
.ctx-menu li {
    padding: 7px 14px;
    font-size: 13px;
    color: #1f2937;
    cursor: pointer;
}
.ctx-menu li:hover:not(.disabled):not(.sep) {
    background: #eef2ff;
    color: #3a6cf6;
}
.ctx-menu li.disabled {
    color: #cbd5e1;
    cursor: not-allowed;
}
.ctx-menu li.danger { color: #dc2626; }
.ctx-menu li.danger:hover { background: #fee2e2; color: #b91c1c; }
.ctx-menu li.sep {
    height: 1px;
    margin: 4px 0;
    padding: 0;
    background: #e3e8ef;
    cursor: default;
}

.port-label {
    fill: #6b7280;
    font-size: 10px;
    font-family: ui-monospace, Menlo, Consolas, monospace;
    paint-order: stroke;
    stroke: #f5f7fa;
    stroke-width: 3px;
    stroke-linejoin: round;
}

.port-menu {
    position: absolute;
    z-index: 60;
    list-style: none;
    margin: 0;
    padding: 4px 0;
    min-width: 100px;
    background: #ffffff;
    border: 1px solid #d6dde8;
    border-radius: 6px;
    box-shadow: 0 8px 24px rgba(31, 41, 55, 0.18);
    user-select: none;
}
.port-menu li {
    padding: 6px 14px;
    font-size: 12px;
    font-family: ui-monospace, Menlo, Consolas, monospace;
    color: #1f2937;
    cursor: pointer;
}
.port-menu li:hover:not(.disabled):not(.port-menu-title) {
    background: #eef2ff;
    color: #3a6cf6;
}
.port-menu li.disabled {
    color: #cbd5e1;
    cursor: not-allowed;
    text-decoration: line-through;
}
.port-menu li.port-menu-title {
    padding: 4px 14px;
    font-size: 10px;
    text-transform: uppercase;
    color: #94a3b8;
    cursor: default;
    border-bottom: 1px solid #e3e8ef;
    margin-bottom: 4px;
    font-family: inherit;
    letter-spacing: 0.5px;
}
</style>
