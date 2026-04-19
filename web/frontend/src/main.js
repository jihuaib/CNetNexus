import { createApp } from 'vue';
import App from './App.vue';
import { router } from './router';
import '@xterm/xterm/css/xterm.css';
import './styles.css';

createApp(App).use(router).mount('#app');
