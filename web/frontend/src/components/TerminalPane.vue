<script setup>
import { ref, onMounted, onBeforeUnmount, watch } from 'vue';
import { Terminal } from '@xterm/xterm';
import { FitAddon } from '@xterm/addon-fit';

const props = defineProps({
    node: { type: Object, required: true },
    visible: { type: Boolean, default: true }
});

const termContainer = ref(null);
let term = null;
let fit = null;
let ws = null;
let ro = null;
let connected = false;

function doFit()
{
    try
    {
        fit?.fit();
        if (term) term.refresh(0, term.rows - 1);
    }
    catch (_) { /* ignore */ }
}

function connect()
{
    if (!props.node || connected) return;
    connected = true;
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    const url = `${proto}://${location.host}/ws/terminal?id=${encodeURIComponent(props.node.id)}`;
    ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    ws.onopen = () =>
    {
        doFit();
        if (props.visible) term?.focus();
    };
    ws.onmessage = (ev) =>
    {
        if (!term) return;
        if (typeof ev.data === 'string') term.write(ev.data);
        else                             term.write(new Uint8Array(ev.data));
    };
    ws.onclose = () => term?.write('\r\n*** disconnected ***\r\n');
    ws.onerror = () => term?.write('\r\n*** websocket error ***\r\n');
}

function waitForContainerReady()
{
    return new Promise((resolve) =>
    {
        const el = termContainer.value;
        const check = () =>
        {
            if (el && el.clientWidth > 0 && el.clientHeight > 0)
            {
                resolve();
            }
            else
            {
                requestAnimationFrame(check);
            }
        };
        check();
    });
}

onMounted(async () =>
{
    term = new Terminal({
        cursorBlink: true,
        fontFamily: 'ui-monospace, Menlo, Consolas, monospace',
        fontSize: 13,
        scrollback: 5000,
        allowProposedApi: true,
        theme: { background: '#0f1320', foreground: '#e6e8ee' }
    });
    fit = new FitAddon();
    term.loadAddon(fit);
    term.open(termContainer.value);

    term.onData((data) =>
    {
        if (ws && ws.readyState === WebSocket.OPEN) ws.send(data);
    });

    ro = new ResizeObserver(() =>
    {
        // 容器尺寸变化时重新测一次
        requestAnimationFrame(doFit);
    });
    ro.observe(termContainer.value);

    // 1) 等容器真正有尺寸
    await waitForContainerReady();

    // 2) 等字体加载完成再测量字符宽高，否则 FitAddon 算出来的行列数是错的
    try
    {
        if (document.fonts && document.fonts.ready) await document.fonts.ready;
    }
    catch (_) { /* ignore */ }

    // 3) 再等两个 rAF 让布局彻底稳定
    await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));

    // 4) 最终 fit 一次，再建立 WebSocket，保证 banner 首次写入就是对的尺寸
    doFit();
    connect();

    // 兜底：防抖再 fit 几次
    setTimeout(doFit, 80);
    setTimeout(doFit, 300);
});

onBeforeUnmount(() =>
{
    try { ws?.close(); } catch (_) {}
    try { ro?.disconnect(); } catch (_) {}
    try { term?.dispose(); } catch (_) {}
});

// 标签切换回这个 pane 时重新 fit 并把焦点交给终端
watch(() => props.visible, (v) =>
{
    if (v)
    {
        requestAnimationFrame(() =>
        {
            doFit();
            term?.focus();
        });
    }
});

defineExpose({ fit: doFit });
</script>

<template>
    <div ref="termContainer" class="term-pane"></div>
</template>

<style scoped>
.term-pane {
    width: 100%;
    height: 100%;
    background: #0f1320;
    /* 不要加 padding：FitAddon 以 clientHeight 为准，带 padding 会多算出一行，
       导致最后一行被塞进 padding 区域、视觉上半截被切掉 */
    padding: 0;
    box-sizing: border-box;
    overflow: hidden;
}
.term-pane :deep(.xterm),
.term-pane :deep(.xterm-viewport),
.term-pane :deep(.xterm-screen) {
    padding: 0 !important;
}
</style>
