<template>
    <div class="classic-wrap">
        <div class="classic-banner">
            <div class="container banner-inner">
                <p class="banner-text">
                    当前页为<strong>客户端软件</strong>介绍（默认首页）。本仓库中由 <strong>C 实现的 NetNexus</strong> 及其 Web 拓扑编排位于独立路径
                    <code class="mono">/netnexus</code>，与客户端站点解耦。
                </p>
                <div class="banner-actions">
                    <RouterLink to="/netnexus" class="banner-btn-primary">进入 NetNexus（C / Web）</RouterLink>
                    <RouterLink to="/netnexus/top" class="banner-btn-ghost">直接进入拓扑编排</RouterLink>
                </div>
            </div>
        </div>
        <div class="landing-root app-container">
            <Header />
            <Hero />
            <Features />
            <Download />
            <Footer />
        </div>
    </div>
</template>

<script setup>
import { onMounted, onBeforeUnmount } from 'vue';
import Header from '../landing/components/Header.vue';
import Hero from '../landing/components/Hero.vue';
import Features from '../landing/components/Features.vue';
import Download from '../landing/components/Download.vue';
import Footer from '../landing/components/Footer.vue';
import '../landing/landing.css';

onMounted(() =>
{
    document.body.classList.add('landing-active');
});

onBeforeUnmount(() =>
{
    document.body.classList.remove('landing-active');
});
</script>

<style scoped>
.classic-wrap {
    min-height: 100vh;
    display: flex;
    flex-direction: column;
}

.classic-banner {
    flex-shrink: 0;
    background: linear-gradient(135deg, #0c4a6e 0%, #1e3a5f 100%);
    border-bottom: 1px solid rgba(24, 144, 255, 0.35);
    padding: 14px 0;
}

.banner-inner {
    max-width: 1200px;
    margin: 0 auto;
    padding: 0 2rem;
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    justify-content: space-between;
    gap: 14px;
}

.banner-text {
    margin: 0;
    font-size: 0.9rem;
    line-height: 1.5;
    color: #e0f2fe;
    max-width: 640px;
}

.banner-text strong {
    color: #fff;
}

.banner-actions {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
    align-items: center;
}

.banner-btn-primary {
    display: inline-flex;
    align-items: center;
    padding: 8px 16px;
    border-radius: 6px;
    background: #3a6cf6;
    color: #fff !important;
    font-weight: 600;
    font-size: 0.875rem;
    text-decoration: none;
    border: 1px solid #5b8cff;
}

.banner-btn-primary:hover {
    background: #2f5ce6;
}

.banner-btn-ghost {
    display: inline-flex;
    align-items: center;
    padding: 8px 14px;
    border-radius: 6px;
    border: 1px solid rgba(255, 255, 255, 0.35);
    color: #e0f2fe !important;
    font-size: 0.85rem;
    text-decoration: none;
}

.banner-btn-ghost:hover {
    background: rgba(255, 255, 255, 0.08);
}

.mono {
    font-family: ui-monospace, Menlo, Consolas, monospace;
    font-size: 0.85em;
    padding: 0.1em 0.35em;
    border-radius: 4px;
    background: rgba(0, 0, 0, 0.25);
}

.app-container {
    flex: 1;
    display: flex;
    flex-direction: column;
}
</style>
