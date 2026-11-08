**简体中文** · [English](README.md)

# 小智助手静音专项诊断

显式开启后，在已绑定设备正常初始化飞书之后运行，使用真实云端 MCP 和真实 Opus
解码器。PCM夹具覆盖麦克风采样，输出音量始终为零，不提供直接发送消息的工具。
诊断固件不能作为普通版交付。

准备五段合成的16kHz、单声道、PCM16 WAV，分别表达单一意图：

| 输入 | 命令意图 |
| --- | --- |
| 列出会话 | 列出当前缓存的飞书会话。 |
| 导航 | 打开设备设置界面。 |
| 草稿 | 给第一个会话准备草稿，不要发送。 |
| 打开会话 | 打开第一个会话并展示消息列表。 |
| 读取消息 | 读取第一个会话的最新消息。 |

```powershell
python tools/assistant_probe/make_fixture.py --list-chats list.wav --navigate navigate.wav --draft draft.wav --open-chat open.wav --read-latest read.wav --goodbye goodbye.wav
$env:FEISHU_VOICE_AUTOTEST='1'
$env:FEISHU_ASSISTANT_AUTOTEST='1'
idf.py -B build/assistant-probe -DFEISHU_ASR_BACKEND=xiaozhi "-DSDKCONFIG=$PWD/build/assistant-probe/sdkconfig" reconfigure build
```

输出仅写入已忽略的 `build/`。已有普通语音夹具保留；全新检出时生成带编译保护的别名，
仅供共用注入驱动编译，不能拿来运行普通语音识别测试。合成音频总量限制为768KiB，
给应用Flash留出余量。

七个场景依次为列出会话、页面切换、草稿、400ms后取消、400ms后注入采集失败、读取最新消息恢复、打开会话。
控制程序还需观察消息列表实际加载并应用。
检查实际工具结果，不能只看收到JSON请求。草稿场景核对本轮对象和UUID后，只清理本轮
创建的草稿，不影响已有草稿。草稿区满时测试失败，不能因此删除其他草稿。

复合指令需要模型规划，另行评估稳定性。模型只列出会话，不能算读取消息或保存草稿已完成。
控制程序还应核对实际选中的会话和工具完成结果。

保留 `ASSISTANT_RPC`、`ASSISTANT_TOOL_*`、`ASSISTANT_AUDIO`、`ASSISTANT_DONE`、
`ASSISTANT_SMOKE_DONE` 和 `ASSISTANT_SUITE_DONE` 标记。正常场景要求有解码音频帧；
取消和失败场景核对预期错误。握手、本地回放、真实远程调用、设备PCM写入及人耳听感
是不同证据，最后一项仍需物理验收。

刷机前遵守仓库已授权的设备测试策略，检查准确目标和分区布局，使用 `finally`
恢复已验证的普通APP，保留绑定、身份及数据。构建交付版前清除两个诊断环境变量，
并使用独立构建目录。普通APP不得含 `ASSISTANT_SUITE_DONE` 或合成夹具音频。

连续会话第八例：使用同一连接先招呼，再查询会话、读取消息，最后“再见”退出；验证静默提交、真实解码及退出，不发送业务消息。

FEISHU_ASSISTANT_AUTOTEST=2仅跑持续会话例。变更诊断环境变量后必须执行reconfigure，单独增量Ninja构建可能保留上次模式。
