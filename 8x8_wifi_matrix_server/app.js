const express = require('express');
const path = require('path');
const os = require('os'); // 用于获取本机 IP

const app = express();
const PORT = 3000;

// ==========================================
// 1. 中间件配置
// ==========================================

// 允许解析 JSON (虽然目前主要只用静态托管，但预留着是个好习惯)
app.use(express.json());

// 托管 public 文件夹下的静态文件 (核心功能)
app.use(express.static(path.join(__dirname, 'public')));

// ==========================================
// 2. 路由处理
// ==========================================

// 首页路由 (其实 express.static 已经处理了 index.html，这里是双重保险)
app.get('/', (req, res) => {
    res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

// 404 处理 (当访问不存在的页面时)
app.use((req, res) => {
    res.status(404).send('<h1>404 Not Found</h1><p>找不到这个页面，请检查 URL。</p>');
});

// ==========================================
// 3. 辅助函数：获取本机局域网 IP
// ==========================================
function getLocalIP() {
    const interfaces = os.networkInterfaces();
    for (const devName in interfaces) {
        const iface = interfaces[devName];
        for (let i = 0; i < iface.length; i++) {
            const alias = iface[i];
            // 跳过 IPv6 和 127.0.0.1 (本地回环)
            if (alias.family === 'IPv4' && alias.address !== '127.0.0.1' && !alias.internal) {
                return alias.address;
            }
        }
    }
    return '127.0.0.1';
}

// ==========================================
// 4. 启动服务器
// ==========================================

// 监听 '0.0.0.0' 允许局域网内其他设备(如手机)访问
app.listen(PORT, '0.0.0.0', () => {
    const ip = getLocalIP();
    console.log('\n==================================================');
    console.log(`🚀 上位机画板已启动!`);
    console.log(`--------------------------------------------------`);
    console.log(`👉 本机访问:   http://localhost:${PORT}`);
    console.log(`📱 手机/局域网访问: http://${ip}:${PORT}`); // <--- 重点看这里
    console.log(`==================================================\n`);
});