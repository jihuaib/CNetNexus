<script setup>
defineProps({
    linkingMode:       { type: Boolean, default: false },
    terminalMinimized: { type: Boolean, default: false },
    terminalCount:     { type: Number,  default: 0 }
});
const emit = defineEmits(['toggle-linking', 'restore-terminal']);

const devices = [
    { type: 'netnexus', name: 'NetNexus' },
    { type: 'frr', name: 'FRR' }
];

function onDragStart(ev, type)
{
    ev.dataTransfer.effectAllowed = 'copy';
    ev.dataTransfer.setData('application/x-device', type);
}
</script>

<template>
    <div class="shelf">
        <h3>设备货架</h3>
        <div class="list">
            <div
                v-for="d in devices"
                :key="d.type"
                class="device"
                draggable="true"
                @dragstart="onDragStart($event, d.type)"
            >
                <img src="/icon.ico" class="icon" alt="" />
                <div class="name">{{ d.name }}</div>
            </div>
        </div>

        <h3 class="section">操作</h3>
        <button
            class="link-btn"
            :class="{ active: linkingMode }"
            @click="emit('toggle-linking')"
        >
            <span class="dot" :class="{ on: linkingMode }"></span>
            {{ linkingMode ? '退出连线' : '连线' }}
        </button>
        <p class="hint" v-if="linkingMode">点击两台设备并选择端口</p>

        <button
            v-if="terminalMinimized && terminalCount > 0"
            class="tray-btn"
            @click="emit('restore-terminal')"
            title="恢复终端窗口"
        >
            <span class="tray-ico"></span>
            <span class="tray-label">终端</span>
            <span class="tray-badge">{{ terminalCount }}</span>
        </button>
    </div>
</template>

<style scoped>
.shelf {
    padding: 14px 16px;
    -webkit-user-select: none;
    -moz-user-select: none;
    -ms-user-select: none;
    user-select: none;
    display: flex;
    flex-direction: column;
}
.shelf, .shelf * {
    -webkit-user-select: none;
    -moz-user-select: none;
    -ms-user-select: none;
    user-select: none;
}
h3 {
    margin: 4px 0 12px;
    font-size: 12px;
    text-transform: uppercase;
    color: #6b7280;
    letter-spacing: 1px;
}
h3.section { margin-top: 18px; }

.list {
    display: flex;
    flex-direction: column;
    gap: 8px;
}
.device {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 12px;
    border: 1px solid #e3e8ef;
    border-radius: 6px;
    background: #ffffff;
    cursor: grab;
    transition: transform 0.05s, border-color 0.1s, box-shadow 0.1s;
}
.device:hover {
    border-color: #3a6cf6;
    box-shadow: 0 2px 8px rgba(58, 108, 246, 0.12);
}
.device:active {
    cursor: grabbing;
    transform: scale(0.98);
}
.icon {
    width: 28px;
    height: 28px;
    display: block;
    flex-shrink: 0;
    -webkit-user-drag: none;
}
.name {
    font-size: 13px;
    font-weight: 600;
    color: #1f2937;
}

.link-btn {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    width: 100%;
    padding: 9px 12px;
    background: #ffffff;
    border: 1px solid #d6dde8;
    border-radius: 6px;
    color: #1f2937;
    font-size: 13px;
    font-family: inherit;
    cursor: pointer;
    transition: all 0.1s;
}
.link-btn:hover {
    border-color: #3a6cf6;
    color: #3a6cf6;
    background: #f3f6ff;
}
.link-btn.active {
    background: #3a6cf6;
    border-color: #3a6cf6;
    color: #ffffff;
    box-shadow: 0 2px 8px rgba(58, 108, 246, 0.3);
}
.link-btn.active:hover {
    background: #4d7cff;
    color: #ffffff;
}
.dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: #cbd5e1;
}
.dot.on {
    background: #ffffff;
    box-shadow: 0 0 0 3px rgba(255, 255, 255, 0.35);
}

.hint {
    color: #6b7280;
    font-size: 12px;
    margin: 8px 0 0;
    line-height: 1.5;
}

.tray-btn {
    display: flex;
    align-items: center;
    gap: 8px;
    width: 100%;
    margin-top: 10px;
    padding: 8px 12px;
    background: #eef2ff;
    border: 1px solid #c7d2fe;
    border-radius: 6px;
    color: #3a6cf6;
    font-size: 13px;
    font-family: inherit;
    cursor: pointer;
    transition: all 0.1s;
}
.tray-btn:hover {
    background: #3a6cf6;
    border-color: #3a6cf6;
    color: #ffffff;
}
.tray-ico {
    width: 14px;
    height: 10px;
    border: 1.5px solid currentColor;
    border-radius: 2px;
    position: relative;
    flex-shrink: 0;
}
.tray-ico::after {
    content: '';
    position: absolute;
    left: 2px;
    right: 2px;
    top: 1px;
    height: 2px;
    background: currentColor;
    border-radius: 1px;
}
.tray-label {
    flex: 1;
    text-align: left;
    font-weight: 600;
}
.tray-badge {
    min-width: 18px;
    padding: 1px 6px;
    background: #3a6cf6;
    color: #ffffff;
    border-radius: 9px;
    font-size: 11px;
    font-weight: 600;
    text-align: center;
}
.tray-btn:hover .tray-badge {
    background: #ffffff;
    color: #3a6cf6;
}
</style>
