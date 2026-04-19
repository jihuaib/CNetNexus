/**
 * 路由按「软件形态」拆分：
 *   /              — 既有客户端软件的介绍站（另一套技术栈的官网复用）
 *   /netnexus      — 本仓库 C 实现的 NetNexus 在浏览器里的说明与入口
 *   /netnexus/top  — 拓扑编排（Docker / 画布 / 终端等，仅属于 C 侧 Web）
 */
import { createRouter, createWebHistory } from 'vue-router';
import ClassicLandingView from '../views/ClassicLandingView.vue';
import HomeView from '../views/HomeView.vue';
import TopologyView from '../views/TopologyView.vue';

export const router = createRouter({
    history: createWebHistory(),
    routes: [
        { path: '/', name: 'client-site', component: ClassicLandingView },
        { path: '/netnexus', name: 'nn-hub', component: HomeView },
        { path: '/netnexus/top', name: 'nn-topology', component: TopologyView },
        // 旧书签兼容
        { path: '/top', redirect: '/netnexus/top' },
        { path: '/home', redirect: '/netnexus' }
    ]
});
