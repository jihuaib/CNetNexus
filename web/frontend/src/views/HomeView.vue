<template>
    <div class="home">
        <header class="topbar">
            <div class="topbar-inner">
                <RouterLink to="/" class="brand">
                    <img src="/icon.ico" class="logo" alt="" />
                    <span class="brand-title">NetNexus</span>
                </RouterLink>
                <nav class="nav">
                    <a href="#about" class="nav-a">产品概览</a>
                    <a href="#features" class="nav-a">主要功能</a>
                    <a href="#lab" class="nav-a">浏览器实验</a>
                </nav>
                <div class="github-bar">
                    <a
                        href="https://github.com/jihuaib/CNetNexus"
                        target="_blank"
                        rel="noopener noreferrer"
                        class="github-link"
                    >
                        <svg class="github-icon" height="22" viewBox="0 0 16 16" width="22" aria-hidden="true">
                            <path
                                fill="currentColor"
                                d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"
                            />
                        </svg>
                        <span class="repo-name">jihuaib/CNetNexus</span>
                    </a>
                    <div class="github-stats">
                        <a href="https://github.com/jihuaib/CNetNexus/stargazers" target="_blank" rel="noopener noreferrer">
                            <img
                                src="https://img.shields.io/github/stars/jihuaib/CNetNexus?style=flat-square&color=1890ff&label=Stars"
                                alt="GitHub stars"
                                class="badge-img"
                            />
                        </a>
                        <a href="https://github.com/jihuaib/CNetNexus/network/members" target="_blank" rel="noopener noreferrer">
                            <img
                                src="https://img.shields.io/github/forks/jihuaib/CNetNexus?style=flat-square&color=1890ff&label=Forks"
                                alt="GitHub forks"
                                class="badge-img"
                            />
                        </a>
                    </div>
                </div>
            </div>
        </header>

        <main class="main">
            <section class="hero">
                <div class="hero-content container">
                    <div class="hero-badge">开源 · C / Linux · 控制平面</div>
                    <h1 class="hero-main-title">
                        <span class="gradient-text">NetNexus</span>
                        <br />
                        <span class="hero-sub-line">面向研发与验证的网络控制平面</span>
                    </h1>
                    <p class="hero-desc">
                        NetNexus 用 C 在 Linux 上实现类路由器的<strong>控制平面</strong>：多进程协同，覆盖 BGP、IS-IS、路由与转发隔离、接口与 Telnet 配置面等。适合协议开发、行为验证、自动化回归以及与真实或容器网络对接的实验。
                    </p>
                    <div class="hero-actions">
                        <a href="#features" class="btn btn-primary">了解主要功能</a>
                        <a href="#lab" class="btn btn-secondary">浏览器实验</a>
                    </div>
                </div>
                <div class="hero-background">
                    <div class="glow-sphere sphere-1"></div>
                    <div class="glow-sphere sphere-2"></div>
                </div>
            </section>

            <section id="about" class="about-wrap">
                <div class="container">
                    <div class="glass-card about-card">
                        <h2 class="block-title">产品概览</h2>
                        <p class="prose">
                            你可以把它理解成一台「软件路由器」的核心：有<strong>命令行</strong>（Telnet 接入、多级配置视图）、有<strong>配置与状态持久化</strong>，各协议子系统独立运行、通过内部消息协作。
                            主进程负责监督子进程与生命周期；BGP、路由、接口、VRF、BMP、IS-IS 等各自承担一块能力，便于扩展和单独测试。整体面向<strong>研发与测试场景</strong>，而不是替代现网商用网管。
                        </p>
                    </div>
                </div>
            </section>

            <section id="features" class="features-wrap">
                <div class="container">
                    <div class="features-head">
                        <h2 class="title text-gradient-on-light">主要功能</h2>
                        <p class="subtitle">控制平面各板块能力一览，与主页「强大功能」区块同一信息层次。</p>
                    </div>
                    <div class="feature-grid">
                        <div
                            v-for="(item, index) in featureCards"
                            :key="item.title"
                            class="feature-card glass-card"
                            :style="{ animationDelay: `${index * 0.05}s` }"
                        >
                            <div class="icon-wrapper">
                                <span class="icon" v-html="item.icon"></span>
                            </div>
                            <h3 class="card-title">{{ item.title }}</h3>
                            <p class="card-desc">{{ item.desc }}</p>
                        </div>
                    </div>
                </div>
            </section>

            <section id="lab" class="lab-wrap">
                <div class="container">
                    <div class="glass-card lab-card">
                        <h2 class="block-title">浏览器里做多实例实验</h2>
                        <p class="prose">
                            若本机已装 Docker，还可在<strong>网页里</strong>拖设备、选镜像、按端口连线，并用<strong>网页终端</strong>直接进各实例的命令行——适合演示、多人协作或不想单独开多个 Telnet 窗口时的本地联调。这是<strong>可选的辅助能力</strong>，协议与功能仍以命令行与自动化用例为准。
                        </p>
                        <RouterLink to="/netnexus/top" class="btn btn-secondary btn-large">打开网页实验台</RouterLink>
                    </div>
                </div>
            </section>
        </main>

        <footer class="footer">
            <div class="container footer-content">
                <div class="copyright">© 2026 CNetNexus / NetNexus. All Rights Reserved.</div>
                <div class="links">
                    <RouterLink to="/">站点首页</RouterLink>
                    <a href="https://github.com/jihuaib/CNetNexus" target="_blank" rel="noopener noreferrer">GitHub</a>
                    <a href="https://github.com/jihuaib/CNetNexus/tree/master/docs" target="_blank" rel="noopener noreferrer">文档</a>
                    <a href="https://github.com/jihuaib/CNetNexus/issues" target="_blank" rel="noopener noreferrer">反馈</a>
                </div>
            </div>
        </footer>
    </div>
</template>

<script setup>
import { onMounted, onBeforeUnmount } from 'vue';

const featureCards = [
    {
        title: 'BGP',
        desc: '支持对等体会话与状态机、路由收发与属性处理、本地与出向 RIB 维护；可按策略做路由分组与发布优化。集成 BMP，便于把 BGP 会话与路由变化送到监控侧做观测与排障。命令行里提供多地址族相关配置与展示。',
        icon: '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"></circle><line x1="2" y1="12" x2="22" y2="12"></line><path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"></path></svg>'
    },
    {
        title: 'IS-IS 与 SBMP',
        desc: '提供 IS-IS 路由协议能力与对应配置、诊断入口；SBMP 侧能力与 BGP 体系配合，满足特定监控或信令场景下的联调需求。',
        icon: '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M22 12h-4l-3 9L9 3l-3 9H2"></path></svg>'
    },
    {
        title: '路由与 VRF',
        desc: '维护路由信息与工作线程模型；支持 VRF 多实例，把不同转发表与控制面策略隔离开，方便模拟多租户、跨实例 BGP 与路由策略。',
        icon: '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"></rect><path d="M7 11V7a5 5 0 0 1 10 0v4"></path></svg>'
    },
    {
        title: '接口与连通',
        desc: '管理逻辑与物理接口、与 Linux 内核网络栈对接；支持虚拟以太等形态，便于和容器网、实验拓扑对接，并在 CLI 里按端口名配置地址与状态。',
        icon: '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="2" y="2" width="20" height="8" rx="2" ry="2"></rect><rect x="2" y="14" width="20" height="8" rx="2" ry="2"></rect><line x1="6" y1="6" x2="6.01" y2="6"></line><line x1="6" y1="18" x2="6.01" y2="18"></line></svg>'
    },
    {
        title: '命令行与持久化',
        desc: '多用户 Telnet 会话、用户态与配置态视图切换；配置按模块组织，可落库保存，重启后恢复。适合脚本化拨测、手工验证与演示环境。',
        icon: '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M13 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z"></path><polyline points="13 2 13 9 20 9"></polyline></svg>'
    },
    {
        title: '自动化与质量',
        desc: '配套在 Docker 里起多设备、建链、跑 CLI 用例的自动化流程，用于回归与持续集成；开发侧可结合常见内存检测工具做质量保障。',
        icon: '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"></circle><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"></path></svg>'
    }
];

onMounted(() =>
{
    document.body.classList.add('home-active');
});

onBeforeUnmount(() =>
{
    document.body.classList.remove('home-active');
});
</script>

<style scoped>
.home {
    --accent-primary: #1890ff;
    --accent-secondary: #0050b3;
    --accent-glow: rgba(24, 144, 255, 0.35);
    --glass-bg: rgba(255, 255, 255, 0.78);
    --glass-border: rgba(24, 144, 255, 0.22);
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    background:
        radial-gradient(1200px 500px at 10% -10%, rgba(24, 144, 255, 0.08), transparent 55%),
        radial-gradient(900px 400px at 90% 0%, rgba(99, 102, 241, 0.06), transparent 50%),
        #f0f4f8;
    color: #1e293b;
    font-family: -apple-system, 'Segoe UI', 'PingFang SC', 'Microsoft YaHei', sans-serif;
}

.container {
    max-width: 1200px;
    margin: 0 auto;
    padding: 0 2rem;
}

/* —— 顶栏（与主页顶栏信息密度相近）—— */
.topbar {
    flex-shrink: 0;
    background: linear-gradient(180deg, #f8fbff 0%, #eef4ff 100%);
    border-bottom: 1px solid var(--glass-border);
    box-shadow: 0 1px 0 rgba(255, 255, 255, 0.85) inset;
}

.topbar-inner {
    max-width: 1200px;
    margin: 0 auto;
    min-height: 52px;
    padding: 10px 2rem;
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 14px;
    flex-wrap: wrap;
}

.brand {
    display: flex;
    align-items: center;
    gap: 10px;
    font-weight: 600;
    letter-spacing: 0.3px;
    text-decoration: none;
}

.brand-title {
    background: linear-gradient(to right, var(--accent-primary), var(--accent-secondary));
    -webkit-background-clip: text;
    background-clip: text;
    -webkit-text-fill-color: transparent;
    font-size: 1.15rem;
}

.logo {
    width: 24px;
    height: 24px;
    display: block;
}

.github-bar {
    display: flex;
    align-items: center;
    gap: 12px;
    flex-wrap: wrap;
}

.github-link {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    color: #1e3a5f;
    font-weight: 600;
    font-size: 13px;
    text-decoration: none;
}

.github-link:hover {
    color: var(--accent-primary);
}

.github-icon {
    flex-shrink: 0;
}

.github-stats {
    display: flex;
    align-items: center;
    gap: 8px;
}

.badge-img {
    height: 20px;
    display: block;
}

.nav {
    display: flex;
    align-items: center;
    gap: 6px;
    flex-wrap: wrap;
}

.nav-a {
    font-size: 13px;
    color: #475569;
    text-decoration: none;
    padding: 6px 10px;
    border-radius: 4px;
}

.nav-a:hover {
    color: var(--accent-primary);
    background: rgba(24, 144, 255, 0.08);
}

/* —— 主区：主页式 Hero —— */
.main {
    flex: 1;
    width: 100%;
}

.hero {
    position: relative;
    min-height: 72vh;
    display: flex;
    align-items: center;
    justify-content: center;
    overflow: hidden;
    padding: 3rem 0 4rem;
}

.hero-content {
    position: relative;
    z-index: 10;
    text-align: center;
    max-width: 800px;
}

.hero-badge {
    display: inline-block;
    padding: 0.35rem 1rem;
    border-radius: 9999px;
    border: 1px solid rgba(99, 102, 241, 0.35);
    background: rgba(99, 102, 241, 0.1);
    color: #4338ca;
    font-size: 0.85rem;
    font-weight: 500;
    margin-bottom: 1.25rem;
    letter-spacing: 0.03em;
}

.hero-main-title {
    font-size: clamp(2rem, 5vw, 3.25rem);
    font-weight: 800;
    line-height: 1.2;
    margin: 0 0 1.25rem;
    letter-spacing: -0.02em;
}

.gradient-text {
    background: linear-gradient(to right, var(--accent-primary), var(--accent-secondary));
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
    background-clip: text;
}

.hero-sub-line {
    color: #0f172a;
    -webkit-text-fill-color: #0f172a;
    font-weight: 700;
}

.hero-desc {
    font-size: 1.1rem;
    line-height: 1.65;
    color: #475569;
    margin: 0 auto 2rem;
    max-width: 640px;
}

.hero-desc strong {
    color: #1e293b;
}

.hero-actions {
    display: flex;
    gap: 1rem;
    justify-content: center;
    flex-wrap: wrap;
}

.btn {
    padding: 0.875rem 2rem;
    border-radius: 9999px;
    font-weight: 600;
    font-size: 1rem;
    transition: all 0.25s ease;
    text-decoration: none;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    border: none;
    cursor: pointer;
}

.btn-primary {
    background: linear-gradient(135deg, var(--accent-primary), var(--accent-secondary));
    color: #fff;
    box-shadow: 0 4px 15px var(--accent-glow);
}

.btn-primary:hover {
    transform: translateY(-2px);
    box-shadow: 0 8px 22px var(--accent-glow);
}

.btn-secondary {
    background: var(--glass-bg);
    color: #1e293b;
    border: 1px solid var(--glass-border);
}

.btn-secondary:hover {
    background: #fff;
    border-color: var(--accent-primary);
    color: var(--accent-primary);
}

.btn-large {
    margin-top: 1rem;
}

.hero-background {
    position: absolute;
    inset: 0;
    z-index: 1;
    pointer-events: none;
}

.glow-sphere {
    position: absolute;
    border-radius: 50%;
    filter: blur(100px);
    opacity: 0.22;
}

.sphere-1 {
    width: 480px;
    height: 480px;
    background: var(--accent-primary);
    top: -120px;
    left: -180px;
    animation: float 10s infinite alternate;
}

.sphere-2 {
    width: 380px;
    height: 380px;
    background: #6366f1;
    bottom: -80px;
    right: -120px;
    animation: float 8s infinite alternate-reverse;
}

@keyframes float {
    0% {
        transform: translate(0, 0);
    }
    100% {
        transform: translate(40px, 40px);
    }
}

/* —— 内容块：与主页 Features 区同款栅格与玻璃卡 —— */
.about-wrap {
    padding: 0 0 2.5rem;
}

.about-card {
    padding: 2rem 2.25rem;
}

.block-title {
    margin: 0 0 1rem;
    font-size: 1.25rem;
    font-weight: 700;
    color: #0f172a;
}

.prose {
    margin: 0;
    font-size: 1rem;
    line-height: 1.7;
    color: #475569;
}

.prose strong {
    color: #1e293b;
}

.glass-card {
    background: var(--glass-bg);
    backdrop-filter: blur(12px);
    border: 1px solid var(--glass-border);
    border-radius: 1rem;
    box-shadow: 0 4px 24px rgba(15, 23, 42, 0.06);
}

.features-wrap {
    padding: 2rem 0 4rem;
}

.features-head {
    text-align: center;
    margin-bottom: 3rem;
}

.title {
    font-size: clamp(1.75rem, 4vw, 2.75rem);
    font-weight: 700;
    margin: 0 0 1rem;
}

.text-gradient-on-light {
    background: linear-gradient(to right, #0f172a, #64748b);
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
    background-clip: text;
}

.subtitle {
    font-size: 1.1rem;
    color: #64748b;
    margin: 0;
}

.feature-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
    gap: 2rem;
}

.feature-card {
    padding: 2rem;
    transition:
        transform 0.25s ease,
        box-shadow 0.25s ease;
}

.feature-card:hover {
    transform: translateY(-4px);
    box-shadow: 0 12px 36px rgba(15, 23, 42, 0.1);
}

.icon-wrapper {
    width: 48px;
    height: 48px;
    background: linear-gradient(135deg, rgba(59, 130, 246, 0.15), rgba(99, 102, 241, 0.15));
    border-radius: 12px;
    display: flex;
    align-items: center;
    justify-content: center;
    margin-bottom: 1.25rem;
    color: var(--accent-primary);
}

.icon :deep(svg) {
    display: block;
}

.card-title {
    font-size: 1.25rem;
    font-weight: 600;
    margin: 0 0 0.75rem;
    color: #0f172a;
}

.card-desc {
    margin: 0;
    font-size: 0.95rem;
    line-height: 1.6;
    color: #64748b;
}

.lab-wrap {
    padding: 0 0 4rem;
}

.lab-card {
    padding: 2rem 2.25rem;
}

/* —— 页脚：结构与主页 Footer 一致（浅色底上的同款排版）—— */
.footer {
    margin-top: auto;
    background: #e8eef5;
    border-top: 1px solid var(--glass-border);
    padding: 2rem 0;
}

.footer-content {
    display: flex;
    justify-content: space-between;
    align-items: center;
    color: #64748b;
    font-size: 0.875rem;
}

.links {
    display: flex;
    gap: 1.5rem;
    flex-wrap: wrap;
    align-items: center;
}

.links a,
.links :deep(a) {
    color: inherit;
    text-decoration: none;
}

.links a:hover,
.links :deep(a:hover) {
    color: var(--accent-primary);
}

@media (max-width: 720px) {
    .github-bar {
        width: 100%;
        justify-content: flex-start;
    }

    .repo-name {
        max-width: 160px;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .footer-content {
        flex-direction: column;
        gap: 1rem;
        text-align: center;
    }

    .links {
        justify-content: center;
    }
}
</style>
