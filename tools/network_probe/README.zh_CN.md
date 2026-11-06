<p align="right"><strong>简体中文</strong> · <a href="README.md">English</a></p>

# 合成网络诊断

此可选固件将 Wi-Fi/TCP/TLS 与飞书后台任务、音频采集分开。它向本地接收程序
发送有界合成字节模式，校验确认响应，对比正常和压低空闲堆后的传输，并记录
AP选择及DNS。公开站点根路径的HTTPS GET不携带应用令牌。这些检查不等于
语音识别或业务API验收。

## 准备与构建

使用已安装 `cryptography` 的Python。将下列参数替换为接收电脑的局域网IPv4
地址和设备已有SSID；这里不需要Wi-Fi密码。

```text
python tools/network_probe/prepare.py --host <LAN-IPv4> --ssid <SSID>
python tools/network_probe/server.py --config build/network-probe/config.json
```

默认输出到已忽略的 `build/network-probe`。临时CA、测试私钥和生成的头文件
不得提交。TLS验证始终启用，只有此诊断镜像信任该临时CA；更新测试证书后
必须重新构建镜像。工具不修改系统信任库。

在已激活的ESP-IDF环境中，仅为诊断构建设置 `FEISHU_NETWORK_DIAGNOSTIC=1`，
然后执行 `tools/validate.sh --firmware`。CMake从默认输出目录读取生成的头文件。
普通构建应不设置该标志，或显式设为 `0`。

## 设备与服务生命周期

使用正常的、已经获得授权的设备测试流程，并验证分区布局兼容。需要保留配置
时只刷应用镜像，不要为了诊断直接写入合并镜像。诊断期间应用导航关闭；
固件不录音，也不发送即时消息。

设备在完成16项局域网用例及外网检查后输出 `NETPROBE_DONE`。之后应恢复已验证
的普通应用。接收程序默认600秒停止，或在输出目录出现 `server.stop` 时停止；
`--timeout` 允许1–3600秒。测试后不要遗留接收服务或串口监视器。局域网接收
端不可达不能直接归因于设备TLS故障。

日志包含AP/网络元数据及合成测试结果。除调查需要外请留在本机，不要加入
凭据或真实消息内容。
