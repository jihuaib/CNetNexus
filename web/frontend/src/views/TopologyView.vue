<script setup>
import { ref, reactive, onMounted, onBeforeUnmount, computed, watch } from 'vue';
import DeviceShelf from '../components/DeviceShelf.vue';
import TopologyCanvas from '../components/TopologyCanvas.vue';
import WebTerminal from '../components/WebTerminal.vue';

const STORAGE_KEY = 'netnexus-topology-v1';
const TOPOLOGY_VERSION = 1;
const DEVICE_KIND_NETNEXUS = 'netnexus';
const DEVICE_KIND_FRR = 'frr';
const DEFAULT_IMAGE_BY_TYPE = {
    [DEVICE_KIND_NETNEXUS]: 'netnexus:latest',
    [DEVICE_KIND_FRR]: 'netnexus-frr-ci:localtest'
};

const images = ref([]);
const nodes = reactive([]);    // { id, type, x, y, image, status, instance }
const links = reactive([]);    // { id, from, to }
const selectedNodeId = ref(null);
const selectedLinkId = ref(null);
const terminalNodes = ref([]); // 当前打开终端的节点列表（按打开顺序）
const activeTerminalId = ref(null);
const terminalMinimized = ref(false);
const log = ref([]);
const linkingMode = ref(false);
const fileInputRef = ref(null);
const captureState = ref(null);
const captureBusy = ref(false);

const NETNEXUS_PORTS = 8;
const FRR_PORTS = 4;
const MAX_PORTS = NETNEXUS_PORTS;
const ALL_PORTS = ['GE-1', 'GE-2', 'GE-3', 'GE-4', 'GE-5', 'GE-6', 'GE-7', 'GE-8'];

let nextId = 1;
let suppressPersist = false;
let capturePollTimer = null;
let capturePollLinkId = null;

function isFrrType(type)
{
    return String(type || '').toLowerCase() === DEVICE_KIND_FRR;
}

function isFrrImageName(name)
{
    return /(^|[-_/])frr($|[-_:/.])/i.test(String(name || ''));
}

function ensureDefaultImages(list)
{
    const normalized = Array.isArray(list) ? [...list] : [];
    const has = name => normalized.some(img => img.name === name);
    if (!normalized.some(img => !isFrrImageName(img.name)) && !has(DEFAULT_IMAGE_BY_TYPE[DEVICE_KIND_NETNEXUS]))
    {
        normalized.push({ name: DEFAULT_IMAGE_BY_TYPE[DEVICE_KIND_NETNEXUS], id: '-', size: '(not built)' });
    }
    if (!normalized.some(img => isFrrImageName(img.name)) && !has(DEFAULT_IMAGE_BY_TYPE[DEVICE_KIND_FRR]))
    {
        normalized.push({ name: DEFAULT_IMAGE_BY_TYPE[DEVICE_KIND_FRR], id: '-', size: '(not built)' });
    }
    return normalized;
}

function imagesForDeviceType(type)
{
    const wantFrr = isFrrType(type);
    const matched = images.value.filter(img => isFrrImageName(img.name) === wantFrr);
    if (matched.length > 0) return matched;
    return [{ name: DEFAULT_IMAGE_BY_TYPE[wantFrr ? DEVICE_KIND_FRR : DEVICE_KIND_NETNEXUS], id: '-', size: '(not built)' }];
}

function defaultImageForDeviceType(type)
{
    return imagesForDeviceType(type)[0]?.name || DEFAULT_IMAGE_BY_TYPE[isFrrType(type) ? DEVICE_KIND_FRR : DEVICE_KIND_NETNEXUS];
}

function maxPortsForType(type)
{
    return isFrrType(type) ? FRR_PORTS : NETNEXUS_PORTS;
}

function maxPortsOf(nodeId)
{
    const node = nodes.find(n => n.id === nodeId);
    return maxPortsForType(node?.type);
}

function portsForType(type)
{
    return ALL_PORTS.slice(0, maxPortsForType(type));
}

function portsOfNode(node)
{
    return portsForType(node?.type);
}

function portLabelOf(node, port)
{
    if (!isFrrType(node?.type)) return port;
    const m = /^GE-(\d+)$/.exec(String(port || ''));
    return m ? `eth${m[1]}` : port;
}

function portLabelOfNodeId(nodeId, port)
{
    return portLabelOf(nodes.find(n => n.id === nodeId), port);
}

function linkCount(nodeId)
{
    let n = 0;
    for (const l of links)
    {
        if (l.from === nodeId || l.to === nodeId) n++;
    }
    return n;
}

function usedPortsOf(nodeId)
{
    const used = new Set();
    for (const l of links)
    {
        if (l.from === nodeId && l.fromPort) used.add(l.fromPort);
        if (l.to   === nodeId && l.toPort)   used.add(l.toPort);
    }
    return used;
}

function freePortsOf(nodeId)
{
    const used = usedPortsOf(nodeId);
    const node = nodes.find(n => n.id === nodeId);
    return portsOfNode(node).filter(p => !used.has(p));
}

function mergeLinkState(target, data)
{
    if (!target) return;
    target.networkName = data?.networkName || '';
    target.wired = !!data?.wired;
}

function stopCapturePolling()
{
    if (capturePollTimer)
    {
        clearInterval(capturePollTimer);
        capturePollTimer = null;
    }
    capturePollLinkId = null;
}

function startCapturePolling(linkId)
{
    if (capturePollTimer && capturePollLinkId === linkId) return;
    stopCapturePolling();
    if (!linkId) return;
    capturePollLinkId = linkId;
    capturePollTimer = setInterval(() =>
    {
        loadCaptureState(linkId, { silent: true });
    }, 1000);
}

async function refreshLinksFromBackend({ silent = true } = {})
{
    try
    {
        const r = await fetch('/api/links');
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        const data = await r.json();
        const known = new Map((data.links || []).map(l => [l.id, l]));
        for (const link of links)
        {
            mergeLinkState(link, known.get(link.id));
        }
    }
    catch (e)
    {
        if (!silent) pushLog(`同步链路状态失败: ${e.message}`);
    }
}

async function loadCaptureState(linkId = selectedLinkId.value, { silent = false } = {})
{
    if (!linkId)
    {
        captureState.value = null;
        return;
    }
    try
    {
        const r = await fetch(`/api/links/${encodeURIComponent(linkId)}/capture`);
        const data = await r.json();
        if (!r.ok) throw new Error(data?.error || `HTTP ${r.status}`);
        if (selectedLinkId.value === linkId)
        {
            captureState.value = data.capture || null;
            const status = captureState.value?.status;
            const active = status === 'running' || status === 'starting' || status === 'stopping';
            if (!active && capturePollLinkId === linkId)
            {
                stopCapturePolling();
            }
        }
    }
    catch (e)
    {
        if (!silent) pushLog(`读取抓包状态失败: ${e.message}`);
    }
}

function toggleLinkingMode()
{
    linkingMode.value = !linkingMode.value;
    pushLog(linkingMode.value ? '进入连线模式：依次点击两台设备' : '退出连线模式');
}

function pushLog(msg)
{
    const ts = new Date().toLocaleTimeString();
    log.value.unshift(`[${ts}] ${msg}`);
    if (log.value.length > 80) log.value.pop();
}

async function loadImages()
{
    try
    {
        const r = await fetch('/api/images');
        const data = await r.json();
        images.value = ensureDefaultImages(data.images || []);
        pushLog(`镜像列表加载成功，共 ${images.value.length} 个`);
    }
    catch (e)
    {
        pushLog(`镜像列表加载失败: ${e.message}`);
    }
}

// ---- 拓扑持久化 ----

function serializeTopology()
{
    return {
        version: TOPOLOGY_VERSION,
        savedAt: new Date().toISOString(),
        nextId,
        nodes: nodes.map(n => ({
            id: n.id,
            type: n.type,
            label: n.label,
            x: n.x,
            y: n.y,
            image: n.image
        })),
        links: links.map(l => ({
            id: l.id,
            from: l.from,
            fromPort: l.fromPort,
            to: l.to,
            toPort: l.toPort
        }))
    };
}

let persistTimer = null;
function persistToLocalStorage()
{
    if (suppressPersist) return;
    if (persistTimer) return;
    persistTimer = setTimeout(() =>
    {
        persistTimer = null;
        try
        {
            localStorage.setItem(STORAGE_KEY, JSON.stringify(serializeTopology()));
        }
        catch (_) { /* quota / privacy mode, 忽略 */ }
    }, 250);
}

async function hydrateFromSnapshot(snap, { reason = '导入', restoreRunning = false } = {})
{
    if (!snap || !Array.isArray(snap.nodes) || !Array.isArray(snap.links))
    {
        pushLog(`${reason}失败：数据格式不正确`);
        return false;
    }

    suppressPersist = true;

    // 先清掉当前画布状态（不删后端容器，后端由 id 决定复用）
    nodes.splice(0, nodes.length);
    links.splice(0, links.length);
    selectedNodeId.value = null;
    selectedLinkId.value = null;
    terminalNodes.value = [];
    activeTerminalId.value = null;
    terminalMinimized.value = false;
    captureState.value = null;
    stopCapturePolling();

    for (const n of snap.nodes)
    {
        nodes.push(reactive({
            id: n.id,
            type: n.type,
            label: n.label ?? `${n.type}`,
            x: Number(n.x) || 0,
            y: Number(n.y) || 0,
            image: n.image ?? defaultImageForDeviceType(n.type),
            status: 'stopped',
            instance: null,
            // 从 JSON 里带出来的 db，等启动时回灌给后端；
            // 注意：pendingDb 不参与 serializeTopology，所以不会落到 localStorage
            pendingDb: n.dbBase64 || null
        }));
    }
    for (const l of snap.links)
    {
        links.push({
            id: l.id || `link-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 6)}`,
            from: l.from,
            fromPort: l.fromPort || '',
            to: l.to,
            toPort: l.toPort || '',
            networkName: '',
            wired: false
        });
    }

    const maxSeq = snap.nextId && Number.isFinite(snap.nextId) ? snap.nextId : 1;
    nextId = Math.max(nextId, maxSeq);

    // 同步后端当前仍在跑的实例，把 status/instance 补回来
    try
    {
        const r = await fetch('/api/instances');
        if (r.ok)
        {
            const data = await r.json();
            const known = new Map((data.instances || []).map(i => [i.id, i]));
            for (const n of nodes)
            {
                const inst = known.get(n.id);
                if (inst)
                {
                    n.instance = inst;
                    // 后端保留了停掉但还活着的容器，按其真实 status 还原
                    n.status = inst.status === 'running' ? 'running' : 'stopped';
                }
            }
        }
    }
    catch (_) { /* ignore */ }

    // 把链路重新登记到后端（后端重启会丢内存，这里补一次；两端都运行则自动 wire）
    for (const l of links)
    {
        try
        {
            const r = await fetch('/api/links', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(l)
            });
            const data = await r.json().catch(() => ({}));
            if (r.ok && data.link) mergeLinkState(l, data.link);
        }
        catch (_) { /* ignore */ }
    }

    // 导入模式：不主动拉起容器。用户点击"启动"时 startNode 会把 pendingDb 交给后端回灌。
    // 先把同 id 的旧容器清掉，给导入的 db 让位（留着旧容器，点启动只会走 docker start 复用旧数据）。
    if (restoreRunning)
    {
        const withDb = nodes.filter(n => n.pendingDb);
        if (withDb.length > 0)
        {
            pushLog(`已导入 ${withDb.length} 个设备的 db，点击"启动"时会自动恢复`);
            for (const n of withDb)
            {
                try
                {
                    await fetch(`/api/instances/${encodeURIComponent(n.id)}`, { method: 'DELETE' });
                }
                catch (_) { /* 没跑就忽略 */ }
                n.status = 'stopped';
                n.instance = null;
            }
        }
    }

    suppressPersist = false;
    persistToLocalStorage();
    await refreshLinksFromBackend();
    pushLog(`${reason}完成：${nodes.length} 个设备 / ${links.length} 条连线`);
    return true;
}

function loadFromLocalStorage()
{
    try
    {
        const raw = localStorage.getItem(STORAGE_KEY);
        if (!raw) return null;
        const snap = JSON.parse(raw);
        return snap;
    }
    catch (_) { return null; }
}

function saveTopology()
{
    persistToLocalStorage();
    pushLog('拓扑已保存到本地');
}

async function exportTopology()
{
    const snap = serializeTopology();

    // 对每个运行中的设备，从后端拉 db 塞进对应 node
    const runningNodes = nodes.filter(n => n.status === 'running' && n.instance);
    if (runningNodes.length > 0)
    {
        pushLog(`正在导出 ${runningNodes.length} 个设备的配置 db ...`);
    }
    let dbCount = 0;
    for (const n of runningNodes)
    {
        try
        {
            const r = await fetch(`/api/instances/${encodeURIComponent(n.id)}/db`);
            if (!r.ok) continue;
            const data = await r.json();
            if (data.dbBase64)
            {
                const target = snap.nodes.find(x => x.id === n.id);
                if (target)
                {
                    target.dbBase64 = data.dbBase64;
                    target.dbSize = data.size || 0;
                    dbCount++;
                }
            }
        }
        catch (_) { /* 某个设备 db 拉不到不影响整体导出 */ }
    }

    const blob = new Blob([JSON.stringify(snap, null, 2)], { type: 'application/json' });
    const url  = URL.createObjectURL(blob);
    const a    = document.createElement('a');
    const ts   = new Date().toISOString().replace(/[:.]/g, '-');
    a.href     = url;
    a.download = `netnexus-topology-${ts}.json`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
    pushLog(`已导出拓扑文件：${a.download}（含 ${dbCount} 份设备 db）`);
}

function triggerImport()
{
    fileInputRef.value?.click();
}

async function onImportFile(e)
{
    const file = e.target.files?.[0];
    e.target.value = '';
    if (!file) return;
    try
    {
        const text = await file.text();
        const snap = JSON.parse(text);
        await hydrateFromSnapshot(snap, { reason: `导入 ${file.name}`, restoreRunning: true });
    }
    catch (err)
    {
        pushLog(`导入失败：${err.message}`);
    }
}

async function clearTopology()
{
    if (nodes.length === 0 && links.length === 0)
    {
        pushLog('拓扑已是空的');
        return;
    }
    if (!window.confirm('确定清空当前拓扑吗？运行中的容器会一并停止。'))
    {
        return;
    }
    const linkIds = links.map(l => l.id);
    for (const id of linkIds)
    {
        await fetch(`/api/links/${encodeURIComponent(id)}`, { method: 'DELETE' }).catch(() => {});
    }
    const ids = nodes.filter(n => n.instance).map(n => n.id);
    for (const id of ids)
    {
        await fetch(`/api/instances/${id}`, { method: 'DELETE' }).catch(() => {});
    }
    suppressPersist = true;
    nodes.splice(0, nodes.length);
    links.splice(0, links.length);
    selectedNodeId.value = null;
    selectedLinkId.value = null;
    terminalNodes.value = [];
    activeTerminalId.value = null;
    terminalMinimized.value = false;
    captureState.value = null;
    stopCapturePolling();
    suppressPersist = false;
    persistToLocalStorage();
    pushLog('拓扑已清空');
}

onMounted(async () =>
{
    document.body.classList.add('topology-active');
    await loadImages();
    const snap = loadFromLocalStorage();
    if (snap)
    {
        await hydrateFromSnapshot(snap, { reason: '自动恢复' });
    }
});

onBeforeUnmount(() =>
{
    document.body.classList.remove('topology-active');
    stopCapturePolling();
});

// 任何节点 / 连线的变动都自动落盘（深度 watch）
watch([nodes, links], () => persistToLocalStorage(), { deep: true });

function onDropDevice({ deviceType, x, y })
{
    const defaultImage = defaultImageForDeviceType(deviceType);
    const seq = nextId++;
    // 用时间戳 + 序号生成唯一 id，避免刷新页面后与后端残留实例冲突
    const uniq = `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 6)}`;
    const node = reactive({
        id: `node-${uniq}-${seq}`,
        type: deviceType,
        label: `${deviceType}-${seq}`,
        x, y,
        image: defaultImage,
        status: 'stopped',     // stopped | starting | running | error
        instance: null
    });
    nodes.push(node);
    selectedNodeId.value = node.id;
    selectedLinkId.value = null;
    captureState.value = null;
    stopCapturePolling();
    pushLog(`新增设备 ${node.label}`);
}

function onMoveNode({ id, x, y })
{
    const n = nodes.find(n => n.id === id);
    if (n) { n.x = x; n.y = y; }
}

function onSelectNode(id)
{
    selectedNodeId.value = id;
    if (id) selectedLinkId.value = null;
}

function onSelectLink(id)
{
    selectedLinkId.value = id;
    captureState.value = null;
    stopCapturePolling();
    if (id)
    {
        selectedNodeId.value = null;
        loadCaptureState(id, { silent: true }).then(() =>
        {
            if (captureState.value?.status === 'running' || captureState.value?.status === 'starting' || captureState.value?.status === 'stopping')
            {
                startCapturePolling(id);
            }
        });
    }
}

async function onCreateLink({ from, fromPort, to, toPort })
{
    if (from === to) return;
    const a = nodes.find(n => n.id === from);
    const b = nodes.find(n => n.id === to);
    if (!a || !b) return;
    if (usedPortsOf(from).has(fromPort))
    {
        pushLog(`${a.label} 的 ${portLabelOf(a, fromPort)} 已被占用`);
        return;
    }
    if (usedPortsOf(to).has(toPort))
    {
        pushLog(`${b.label} 的 ${portLabelOf(b, toPort)} 已被占用`);
        return;
    }
    const uniq = `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 6)}`;
    const link = { id: `link-${uniq}-${nextId++}`, from, fromPort, to, toPort, networkName: '', wired: false };
    links.push(link);
    pushLog(`连线 ${a.label}:${portLabelOf(a, fromPort)} <-> ${b.label}:${portLabelOf(b, toPort)}`);

    try
    {
        const r = await fetch('/api/links', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(link)
        });
        const data = await r.json();
        if (r.ok)
        {
            if (data.link) mergeLinkState(link, data.link);
            if (data.note)
            {
                pushLog(data.note);
            }
            else if (data.needRestart)
            {
                pushLog(`链路已登记，但两端已在运行。请重启相关设备使 ${portLabelOf(a, fromPort)}/${portLabelOf(b, toPort)} 生效`);
            }
            else if (data.wired)
            {
                pushLog(`链路已热接通（${data.link.networkName}）`);
            }
            else if (data.hotApplied)
            {
                pushLog(`链路已热接入运行端（${data.link.networkName}）`);
            }
            else
            {
                pushLog(`链路已登记（${data.link.networkName}），启动两端设备后会自动接通`);
            }
        }
        else if (data?.error)
        {
            pushLog(`后端登记链路失败：${data.error}`);
        }
    }
    catch (e)
    {
        pushLog(`后端登记链路失败：${e.message}`);
    }
}

async function onDeleteLink(linkId)
{
    const idx = links.findIndex(l => l.id === linkId);
    if (idx < 0) return;
    const l = links[idx];
    const a = nodes.find(n => n.id === l.from);
    const b = nodes.find(n => n.id === l.to);
    links.splice(idx, 1);
    if (selectedLinkId.value === linkId)
    {
        selectedLinkId.value = null;
        captureState.value = null;
        stopCapturePolling();
    }
    pushLog(`删除连线 ${a?.label || l.from}:${portLabelOf(a, l.fromPort)} <-> ${b?.label || l.to}:${portLabelOf(b, l.toPort)}`);
    await fetch(`/api/links/${encodeURIComponent(linkId)}`, { method: 'DELETE' }).catch(() => {});
}

function onDeleteNode(id)
{
    const idx = nodes.findIndex(n => n.id === id);
    if (idx < 0) return;
    const node = nodes[idx];
    if (node.instance)
    {
        fetch(`/api/instances/${node.id}`, { method: 'DELETE' }).catch(() => {});
    }
    const removedLinkIds = links.filter(l => l.from === id || l.to === id).map(l => l.id);
    nodes.splice(idx, 1);
    for (let i = links.length - 1; i >= 0; i--)
    {
        if (links[i].from === id || links[i].to === id) links.splice(i, 1);
    }
    if (selectedNodeId.value === id) selectedNodeId.value = null;
    if (removedLinkIds.includes(selectedLinkId.value))
    {
        selectedLinkId.value = null;
        captureState.value = null;
        stopCapturePolling();
    }
    for (const linkId of removedLinkIds)
    {
        fetch(`/api/links/${encodeURIComponent(linkId)}`, { method: 'DELETE' }).catch(() => {});
    }
    closeTerminalTab(id);
    pushLog(`删除设备 ${node.label}`);
}

const selectedNode = computed(() => nodes.find(n => n.id === selectedNodeId.value) || null);
const selectedLink = computed(() => links.find(l => l.id === selectedLinkId.value) || null);
const selectedLinkEndpoints = computed(() =>
{
    const link = selectedLink.value;
    if (!link) return null;
    const fromNode = nodes.find(n => n.id === link.from);
    const toNode = nodes.find(n => n.id === link.to);
    return {
        fromLabel: fromNode?.label || link.from,
        toLabel: toNode?.label || link.to,
        fromPortLabel: portLabelOf(fromNode, link.fromPort),
        toPortLabel: portLabelOf(toNode, link.toPort)
    };
});

function formatTime(ts)
{
    if (!ts) return '-';
    return new Date(ts).toLocaleString();
}

async function startNode(node)
{
    if (!node) return;
    node.status = 'starting';
    const hasDb = !!node.pendingDb;
    pushLog(hasDb
        ? `启动 ${node.label} (${node.image}) ... 同时恢复 db`
        : `启动 ${node.label} (${node.image}) ...`);
    try
    {
        const body = { id: node.id, image: node.image, kind: node.type };
        if (hasDb) body.dbBase64 = node.pendingDb;

        const r = await fetch('/api/instances', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
        });
        const text = await r.text();
        let data;
        try { data = text ? JSON.parse(text) : {}; }
        catch (_)
        {
            throw new Error(`HTTP ${r.status} ${r.statusText} - ${text.slice(0, 200)}`);
        }
        if (!r.ok) throw new Error(data.detail || data.error || `HTTP ${r.status}`);
        node.instance = data.instance;
        node.status = 'running';
        if (hasDb) node.pendingDb = null;
        await refreshLinksFromBackend();
        pushLog(isFrrType(node.type)
            ? `${node.label} 已启动，网页终端将连接 vtysh`
            : `${node.label} 已启动，宿主机端口 ${data.instance.hostPort}`);
    }
    catch (e)
    {
        node.status = 'error';
        pushLog(`启动失败: ${e.message}`);
    }
}

async function stopNode(node)
{
    if (!node) return;
    try
    {
        const r = await fetch(`/api/instances/${node.id}/stop`, { method: 'POST' });
        if (!r.ok)
        {
            const text = await r.text();
            throw new Error(text || `HTTP ${r.status}`);
        }
        node.status = 'stopped';
        // 保留 node.instance：容器还在，只是停了，再点启动会原地 docker start
        await refreshLinksFromBackend();
        await loadCaptureState(selectedLinkId.value, { silent: true });
        pushLog(`${node.label} 已停止（容器保留，配置不丢）`);
        closeTerminalTab(node.id);
    }
    catch (e)
    {
        pushLog(`停止失败: ${e.message}`);
    }
}

async function startSelectedLinkCapture()
{
    const link = selectedLink.value;
    if (!link || captureBusy.value) return;
    captureBusy.value = true;
    try
    {
        const r = await fetch(`/api/links/${encodeURIComponent(link.id)}/capture/start`, { method: 'POST' });
        const data = await r.json().catch(() => ({}));
        if (!r.ok) throw new Error(data?.detail || data?.error || `HTTP ${r.status}`);
        captureState.value = data.capture || null;
        startCapturePolling(link.id);
        pushLog(data.reused
            ? `链路 ${link.id} 已有抓包会话，直接复用`
            : `链路 ${link.id} 已开始抓包`);
    }
    catch (e)
    {
        pushLog(`开始抓包失败: ${e.message}`);
    }
    finally
    {
        captureBusy.value = false;
    }
}

async function stopSelectedLinkCapture()
{
    const link = selectedLink.value;
    if (!link || captureBusy.value) return;
    captureBusy.value = true;
    try
    {
        const r = await fetch(`/api/links/${encodeURIComponent(link.id)}/capture/stop`, { method: 'POST' });
        const data = await r.json().catch(() => ({}));
        if (!r.ok) throw new Error(data?.error || `HTTP ${r.status}`);
        captureState.value = data.capture || null;
        stopCapturePolling();
        pushLog(`链路 ${link.id} 抓包已停止`);
    }
    catch (e)
    {
        pushLog(`停止抓包失败: ${e.message}`);
    }
    finally
    {
        captureBusy.value = false;
    }
}

function downloadSelectedCapture()
{
    const downloadUrl = captureState.value?.downloadUrl;
    if (!downloadUrl) return;
    const a = document.createElement('a');
    a.href = downloadUrl;
    a.download = captureState.value?.downloadName || 'capture.pcap';
    document.body.appendChild(a);
    a.click();
    a.remove();
    pushLog(`已下载抓包文件 ${a.download}`);
}

function openTerminal(node)
{
    if (!node || node.status !== 'running')
    {
        pushLog('请先启动设备');
        return;
    }
    if (!terminalNodes.value.find(n => n.id === node.id))
    {
        terminalNodes.value = [...terminalNodes.value, node];
    }
    activeTerminalId.value = node.id;
    terminalMinimized.value = false;
}

function minimizeTerminal()
{
    terminalMinimized.value = true;
}

function restoreTerminal()
{
    terminalMinimized.value = false;
}

function closeTerminalTab(id)
{
    const idx = terminalNodes.value.findIndex(n => n.id === id);
    if (idx < 0) return;
    const next = terminalNodes.value.filter(n => n.id !== id);
    terminalNodes.value = next;
    if (activeTerminalId.value === id)
    {
        activeTerminalId.value = next[Math.min(idx, next.length - 1)]?.id || null;
    }
}

function closeTerminalWindow()
{
    terminalNodes.value = [];
    activeTerminalId.value = null;
    terminalMinimized.value = false;
}
</script>

<template>
    <div class="layout">
        <header class="topbar">
            <div class="brand">
                <RouterLink to="/netnexus" class="brand-home" title="返回 NetNexus Web 说明">
                    <img src="/icon.ico" class="logo" alt="logo" />
                </RouterLink>
                <span>NetNexus 拓扑编排</span>
            </div>
            <div class="actions">
                <button class="btn" @click="loadImages" title="从后端重新拉取镜像列表">刷新镜像</button>
                <button class="btn" @click="saveTopology" title="保存到浏览器本地">保存</button>
                <button class="btn" @click="exportTopology" title="下载 JSON 文件">导出</button>
                <button class="btn" @click="triggerImport" title="从 JSON 文件导入">导入</button>
                <button class="btn btn-danger" @click="clearTopology" title="清空画布并停止运行中的容器">清空</button>
                <input
                    ref="fileInputRef"
                    type="file"
                    accept="application/json,.json"
                    style="display: none"
                    @change="onImportFile"
                />
            </div>
        </header>

        <div class="main">
            <DeviceShelf
                class="shelf"
                :linking-mode="linkingMode"
                :terminal-minimized="terminalMinimized"
                :terminal-count="terminalNodes.length"
                @toggle-linking="toggleLinkingMode"
                @restore-terminal="restoreTerminal"
            />

            <TopologyCanvas
                class="canvas"
                :nodes="nodes"
                :links="links"
                :selected-id="selectedNodeId"
                :selected-link-id="selectedLinkId"
                :linking-mode="linkingMode"
                :max-ports="MAX_PORTS"
                :all-ports="ALL_PORTS"
                :link-count-of="linkCount"
                :free-ports-of="freePortsOf"
                :ports-of="portsOfNode"
                :max-ports-of="maxPortsOf"
                :port-label-of="portLabelOf"
                @drop-device="onDropDevice"
                @move-node="onMoveNode"
                @select-node="onSelectNode"
                @select-link="onSelectLink"
                @create-link="onCreateLink"
                @delete-node="onDeleteNode"
                @open-terminal="openTerminal"
                @start-node="startNode"
                @stop-node="stopNode"
                @delete-link="onDeleteLink"
                @exit-linking="linkingMode = false"
            />

            <aside class="inspector">
                <h3>{{ selectedLink ? '链路属性' : '设备属性' }}</h3>
                <template v-if="selectedNode">
                    <div class="row">
                        <label>名称</label>
                        <input v-model="selectedNode.label" />
                    </div>
                    <div class="row">
                        <label>设备</label>
                        <span class="value">{{ selectedNode.type }}</span>
                    </div>
                    <div class="row">
                        <label>ID</label>
                        <span class="value mono">{{ selectedNode.id }}</span>
                    </div>
                    <div class="row">
                        <label>镜像</label>
                        <select v-model="selectedNode.image" :disabled="selectedNode.status === 'running' || selectedNode.status === 'starting'">
                            <option v-for="img in imagesForDeviceType(selectedNode.type)" :key="img.name" :value="img.name">
                                {{ img.name }}
                            </option>
                        </select>
                    </div>
                    <div class="row">
                        <label>状态</label>
                        <span class="value" :class="'status-' + selectedNode.status">{{ selectedNode.status }}</span>
                    </div>
                    <div class="row">
                        <label>接口</label>
                        <span class="value">{{ linkCount(selectedNode.id) }} / {{ maxPortsOf(selectedNode.id) }}</span>
                    </div>
                    <div class="ports">
                        <span
                            v-for="p in portsOfNode(selectedNode)"
                            :key="p"
                            class="port-pill"
                            :class="{ used: usedPortsOf(selectedNode.id).has(p) }"
                            :title="p"
                        >{{ portLabelOf(selectedNode, p) }}</span>
                    </div>
                    <div v-if="selectedNode.instance" class="row">
                        <label>容器</label>
                        <span class="value mono">{{ selectedNode.instance.containerName }}</span>
                    </div>
                    <div v-if="selectedNode.instance" class="row">
                        <label>端口</label>
                        <span class="value mono">
                            {{ isFrrType(selectedNode.type) ? 'vtysh' : `127.0.0.1:${selectedNode.instance.hostPort}` }}
                        </span>
                    </div>
                    <p class="tip">右键节点可启动 / 停止 / 连接 / 删除</p>
                </template>
                <template v-else-if="selectedLink">
                    <div class="row">
                        <label>ID</label>
                        <span class="value mono">{{ selectedLink.id }}</span>
                    </div>
                    <div class="row">
                        <label>起点</label>
                        <span class="value">{{ selectedLinkEndpoints?.fromLabel }} · {{ selectedLinkEndpoints?.fromPortLabel }}</span>
                    </div>
                    <div class="row">
                        <label>终点</label>
                        <span class="value">{{ selectedLinkEndpoints?.toLabel }} · {{ selectedLinkEndpoints?.toPortLabel }}</span>
                    </div>
                    <div class="row">
                        <label>状态</label>
                        <span class="value" :class="selectedLink.wired ? 'status-running' : 'status-stopped'">
                            {{ selectedLink.wired ? 'wired' : 'registered' }}
                        </span>
                    </div>
                    <div class="row">
                        <label>网络</label>
                        <span class="value mono">{{ selectedLink.networkName || '(未创建)' }}</span>
                    </div>
                    <div class="capture-actions">
                        <button
                            class="btn"
                            :disabled="captureBusy || captureState?.status === 'running' || captureState?.status === 'starting'"
                            @click="startSelectedLinkCapture"
                        >开始抓包</button>
                        <button
                            class="btn"
                            :disabled="captureBusy || !captureState || (captureState.status !== 'running' && captureState.status !== 'starting' && captureState.status !== 'stopping')"
                            @click="stopSelectedLinkCapture"
                        >停止抓包</button>
                        <button
                            class="btn"
                            :disabled="!captureState?.downloadUrl"
                            @click="downloadSelectedCapture"
                        >下载 pcap</button>
                    </div>
                    <div class="capture-meta">
                        <div>抓包状态：<span :class="captureState ? 'status-' + captureState.status : 'status-stopped'">{{ captureState?.status || 'idle' }}</span></div>
                        <div>开始时间：{{ formatTime(captureState?.startedAt) }}</div>
                        <div>结束时间：{{ formatTime(captureState?.stoppedAt) }}</div>
                        <div>文件大小：{{ captureState?.bytes || 0 }} B</div>
                    </div>
                    <pre class="capture-output">{{ captureState?.lines?.join('\n') || '选中链路后可开始抓包；页面会显示 tcpdump 文本输出，停止后可下载 pcap。' }}</pre>
                    <p class="tip">抓包依赖本地 `netnexus` 镜像内已带 `tcpdump`；重建 `netnexus:latest` 后即可直接使用。</p>
                </template>
                <p v-else class="hint">从左侧拖拽设备到画布开始编排<br/>右键节点进行操作，或单击连线查看抓包面板</p>

                <h3>日志</h3>
                <div class="log">
                    <div v-for="(line, i) in log" :key="i">{{ line }}</div>
                </div>
            </aside>
        </div>

        <WebTerminal
            v-if="terminalNodes.length"
            :nodes="terminalNodes"
            :active-id="activeTerminalId"
            :minimized="terminalMinimized"
            @close="closeTerminalWindow"
            @close-tab="closeTerminalTab"
            @switch-tab="id => activeTerminalId = id"
            @minimize="minimizeTerminal"
        />
    </div>
</template>

<style scoped>
.layout {
    display: flex;
    flex-direction: column;
    height: 100vh;
}
.topbar {
    height: 52px;
    background: #ffffff;
    border-bottom: 1px solid #e3e8ef;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0 18px;
}
.brand {
    display: flex;
    align-items: center;
    gap: 10px;
    font-weight: 600;
    letter-spacing: 0.3px;
    color: #1f2937;
}
.logo {
    width: 24px;
    height: 24px;
    display: block;
}
.brand-home {
    display: flex;
    align-items: center;
    text-decoration: none;
    color: inherit;
}
.btn-ghost {
    text-decoration: none;
    display: inline-flex;
    align-items: center;
}
.main {
    flex: 1;
    display: grid;
    grid-template-columns: 220px 1fr 320px;
    min-height: 0;
}
.shelf {
    border-right: 1px solid #e3e8ef;
    background: #ffffff;
    overflow: auto;
}
.canvas {
    background:
        radial-gradient(circle, #d6dde8 1px, transparent 1px) 0 0/20px 20px,
        #f5f7fa;
    position: relative;
}
.inspector {
    border-left: 1px solid #e3e8ef;
    background: #ffffff;
    padding: 14px 16px;
    overflow: auto;
    display: flex;
    flex-direction: column;
    gap: 8px;
}
.inspector h3 {
    margin: 8px 0 4px;
    font-size: 12px;
    text-transform: uppercase;
    color: #6b7280;
    letter-spacing: 1px;
}
.row {
    display: flex;
    align-items: center;
    gap: 8px;
    margin: 6px 0;
    font-size: 13px;
}
.row label {
    width: 56px;
    color: #6b7280;
}
.row input, .row select {
    flex: 1;
    background: #ffffff;
    border: 1px solid #d6dde8;
    color: #1f2937;
    padding: 5px 8px;
    border-radius: 4px;
    font-size: 13px;
}
.row input:focus, .row select:focus {
    outline: none;
    border-color: #3a6cf6;
    box-shadow: 0 0 0 2px rgba(58, 108, 246, 0.15);
}
.value {
    color: #1f2937;
    font-size: 13px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}
.value.mono {
    font-family: ui-monospace, Menlo, Consolas, monospace;
    font-size: 12px;
    color: #374151;
}
.status-running { color: #16a34a; font-weight: 600; }
.status-stopped { color: #6b7280; }
.status-starting { color: #d97706; font-weight: 600; }
.status-stopping { color: #b45309; font-weight: 600; }
.status-error { color: #dc2626; font-weight: 600; }

.btn {
    background: #ffffff;
    color: #1f2937;
    border: 1px solid #d6dde8;
    border-radius: 4px;
    padding: 6px 12px;
    font-size: 12px;
    cursor: pointer;
}
.btn:hover { background: #f3f5f9; border-color: #3a6cf6; color: #3a6cf6; }
.btn.active {
    background: #3a6cf6;
    border-color: #3a6cf6;
    color: #ffffff;
}
.btn.active:hover { background: #4d7cff; color: #ffffff; }
.btn-danger:hover {
    background: #fee2e2;
    border-color: #dc2626;
    color: #dc2626;
}
.actions { display: flex; gap: 8px; }

.hint {
    color: #6b7280;
    font-size: 13px;
    line-height: 1.7;
}
.tip {
    color: #9ca3af;
    font-size: 12px;
    margin: 10px 0 0;
    padding-top: 8px;
    border-top: 1px dashed #e3e8ef;
}
.ports {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    margin: 4px 0 0 64px;
}
.port-pill {
    display: inline-block;
    font-family: ui-monospace, Menlo, Consolas, monospace;
    font-size: 11px;
    padding: 2px 8px;
    border-radius: 10px;
    background: #f3f5f9;
    color: #6b7280;
    border: 1px solid #e3e8ef;
}
.port-pill.used {
    background: #3a6cf6;
    color: #ffffff;
    border-color: #3a6cf6;
}

.capture-actions {
    display: flex;
    gap: 8px;
    margin: 10px 0 6px;
}

.capture-meta {
    display: grid;
    gap: 4px;
    margin: 6px 0 10px;
    font-size: 12px;
    color: #4b5563;
}

.capture-output {
    margin: 0;
    min-height: 180px;
    max-height: 260px;
    overflow: auto;
    padding: 10px;
    border-radius: 6px;
    border: 1px solid #d6dde8;
    background: #0f172a;
    color: #dbeafe;
    font-family: ui-monospace, Menlo, Consolas, monospace;
    font-size: 12px;
    line-height: 1.45;
    white-space: pre-wrap;
    word-break: break-word;
}

.log {
    background: #f9fafc;
    border: 1px solid #e3e8ef;
    border-radius: 4px;
    padding: 8px;
    height: 220px;
    overflow: auto;
    font-family: ui-monospace, Menlo, Consolas, monospace;
    font-size: 12px;
    color: #4b5563;
}
</style>
