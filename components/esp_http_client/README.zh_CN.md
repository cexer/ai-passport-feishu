<p align="right"><a href="README.md">English</a></p>

# HTTP 发送缓冲生命周期

本目录从 ESP-IDF v5.5.3 按 Apache-2.0 引入，原始文件哈希见 UPSTREAM.json；系统 SDK 不修改。

只有初始化后、禁止自动重定向且显式调用 esp_http_client_defer_get_tx_buffer 的 GET 客户端启用新策略。TLS 握手后才取得发送工作区，无正文请求的头完整发出后立即清零释放。部分发送、带正文请求和普通 POST 保留原生命周期，公开配置结构布局不改变。

验证由主机生命周期用例与真机读取、取消、压力检查组成；英文说明见 README.md。
