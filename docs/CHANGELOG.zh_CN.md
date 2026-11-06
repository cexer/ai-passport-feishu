<p align="right">
  <strong>简体中文</strong> · <a href="CHANGELOG.md">English</a>
</p>

# Changelog

## Unreleased

- Wi-Fi列表下方固定添加网络及手动配网入口，用留白区分，默认选中添加。每个网络增加连接、断开、忘记、设置密码；清空状态明确持久化，改错密码后使用实际生效的凭据恢复原连接。消息/Wi-Fi列表支持双击上回内容、双击下到发送/添加，单击仍可跨列表和操作区边界。临时页面状态纳入重绘判断，确保焦点及编辑变化立即显示。

- 增加网络连接页：最多记忆10个Wi-Fi、选择附近网络、明文密码键盘及手机配网入口。设备端切换在连接成功后提交，失败时尝试恢复原网络。飞书账号的选择及授权界面统一采用BOT文案。

- 进入小智设置自动获取配对信息，缺失或过期时在前台更新；单请求、失败间隔重试，离开即取消。新增按需创建、离页释放的官网控制台二维码；草稿会话名与状态/字节数采用不同颜色。

- 账号、图片及草稿操作统一采用黄色描边选中行；草稿序号移到底部操作提示旁，配对异常只显示在底部红字，移除重复标题，并增大顶部横线下方的内容间距，保留消息列表原有间距。

- 配网与消息页面共用顶部、内容、底部布局，统一电量、分隔线及操作间隔，两个扫码步骤使用蓝色、绿色二维码。新增双击下键定位发消息，以及“小智设置”中的有界设备码/配对状态查询；未配对的语音输入转入配置引导，不丢弃已有草稿。

- 设置列表改为紧凑的单行纯文字；底部操作分组使用实际像素间隔，录音状态与计时合并为一行。通过模拟器检查配网、消息、录音、草稿及异常状态的共用排版；示例画面不代替真机验收。

- 小智临时队列采用逐帧缩放6bit PCM，节省5,688字节且保持2.4秒缓冲。更省的4bit候选因正文回归被拒绝；最终方案通过原语音基准及真机压力/恢复检查。

- 麦克风预热期间暂停可选缓存线程，切走、失败或录音结束后恢复。新增显式启用的设备只读MCP与有界内存诊断，普通飞书终端不开放MCP工具。

- 发送按钮一次确认直接录音，聚焦时可选静音预热；发送成功后刷新当前会话。消息气泡按内容调整高度，设置改为说明卡片，文字配色更柔和，同字号改用Medium字重。APP与字库配套升级/回滚，保留绑定和草稿。

- 未固定BSSID时，全信道扫描并按信号比较同名Wi-Fi AP，避免沿旧信道提示找到第一个匹配项就停止。实机未挪动即连接更强同名AP；小智语音用例及持久审阅、发送链路随后通过。

- FlashDB 的16 KiB逻辑扇区先擦数据单元、最后擦头部单元。物理4 KiB擦除间复位时保留GC恢复标记；回归测试覆盖每个单元后中断、重新打开、原数据读取及后续正常写入。

- 增加按需使用的小智识别候选：TLS WebSocket、20毫秒 PCM 帧与有界本地IMA4队列，保留文字审阅和手动确认后发飞书，接收的语音不播放；音频队列有界，全零采集不生成草稿。录音时暂停可选本地缓存任务，音频与网络资源全部释放后恢复。飞书后端保留为显式构建选项，交付前必须通过真机资格测试。

- 设备与自有配网页统一深蓝/青绿主题及70%背光；识别正文与状态/操作分离，去掉详情顶部空行，使用有界512字节文字窗口；会话一屏六项，消息四条预览，增加电量背景。核对520mAh电量计曲线，SOC刷新不再依赖网络队列空闲。

- 固定LVGL 9.5.0并保留24 KiB界面池；固件校验检查实际ELF，拒绝依赖解析时悄然变为64 KiB的静态界面池。

- 使用同一采集任务将麦克风稳定预读与识别连接准备并行；消息列表先展示再保存快照和已读状态，切换会话取消旧消息请求，冷快照一次预热多个碎片的位置，并记录就绪耗时。保留两秒麦克风稳定保护及首帧保留后才提示录音的规则。

- 对不可达碎片进行有上限的批量回收，小型冷对象合并为一次原子目录淘汰；保留活动读者与受保护草稿，临时分配失败退回单键路径，不增加常驻缓存。

- FlashDB目录已满时，优先复用无人使用且无引用的旧版本目录项，避免普通草稿状态更新先全池扫描；保留当前受保护版本、活动读者、原子发布与ID不复用。

- 录音准备阶段增加有界两秒麦克风预读，首帧保留后才显示正在录音；复用已有 PCM 缓冲，准备阶段可取消，预读不计入录音时长。用户反馈的开头漏字仍需实机确认修复效果。

- 普通短语音使用有界 RAM 预览和确认发送，不再要求持久草稿或数据库空槽；超过容量明确拒绝，既有落盘草稿保留。本地压缩排队，每次请求合并六块解码后的采集音频，写入后立即归还缓冲；仅在录音期间按空闲堆增加抖动缓冲，采集就绪前保持准备提示。

- 修复提交配网后状态文字不刷新的问题，区分 Wi-Fi 连接与飞书服务准备；支持长按取消连接返回免密码配网。兼容 514 字节的设备授权码，轮询表单按实际编码长度分配并支持取消。

- 首次配网的设备热点无需密码，点选热点即可连接；首屏不再显示设备热点密码。

- 首次配网改为直接展示设备热点名称和配网页地址（无需热点密码），不再要求扫码；飞书应用选择与用户授权仍使用二维码。设备字体改为 14 px、2 bpp 灰度字库，保留完整中文范围和既有行高。

- 修复 SQLite 日志截断前未刷出待写缓冲的问题，避免后续关闭或读取时把文件重新写长；截断前刷写失败时保留错误，不继续截断。

- 为本地文件短写、中断和ENOSPC增加有界恢复；嵌套缓存操作共享回收预算，草稿流式上传复用已有页缓存并在握手前及结束后释放，未确认发送保持待确认状态。

- 按需申请及释放录音codec/I2S DMA，取消和失败时清理；先释放会话列表临时页再预取消息，延后流式解析工作区，并跳过不变画面的重复重绘，降低TLS内存峰值。

- 新增联网后设备端选择/创建应用二维码，分离当前应用重新授权与更换应用/账号；候选凭据待用户授权成功后才替换旧绑定。首次配网页只填Wi-Fi，无需应用密钥。

- 新增USB首次配网及电脑侧官方SDK应用选择/创建流程，自动转写凭据并转发用户授权链接，无需靠近设备查看屏幕。

- 数据库损坏时保留故障库及日志，在共享池内建立可用存储；保留独立配置，隔离旧句柄与旧草稿状态，并提示旧草稿待恢复。

- 新增固定8条窗口的消息历史分页、压缩历史页缓存与离线阅读；异常分页响应在发布正文前中止，后台重连保留当前页。

- 会话列表支持固定窗口分页与压缩页缓存；异常游标和加载取消保留当前页，旧页可按配额回收。

- 新增全局待发送草稿入口，重录时保持原发送目标；长消息按页还原提及名称，不重复保存第二份全文。

- 新增会话/消息持久化缓存、离线启动、长文本分块压缩与分页续读；配置和 Wi-Fi 迁入同一共享池，校验后迁移旧 NVS 应用键，隔离不同绑定的令牌，并通过增量收缩向必要配置归还缓存空间。
- 权限、异常响应及临时故障不再清除绑定；仅在刷新授权明确被拒绝后重新扫码，并保留主人应用。启动失败自动重试，消息页显示发送结果。
- 产品排除未使用的演示页，优化应用及 LVGL 体积；缓存移出 Passport 身份保留区，并增加验证覆盖。

- 将发布者内置飞书凭据改为高级用户自带应用：通用 Web Serial 固件不包含私人应用，同一网页可通过 USB 在本地写入用户自己的 App ID/Secret，随后由设备获取并保存自己的用户授权。
- 新增发布后收尾流程：`issue-suggestions` skill 用于把用户反馈作为 issue 提交到上游项目；`experience-pr` skill 用于把可复用的开发经验作为文档 PR 提交；新增 `docs/experiences/` 目录保存单条经验文件；并配套 `after-release`、`file-issues` 与经验索引文档。
- 修复飞书 device-code 轮询使用错误 token 地址的问题；未扫码时继续等待，临时网络错误自动重试，二维码过期后自动刷新。
- 将 LVGL 的有限 CJK 子集替换为驻留 Flash 的 1-bit Source Han Sans 设备字库，覆盖 U+4E00-U+9FFF 和中文标点，解决首启引导及飞书动态消息里的缺字方块。
- 新增设备直连飞书消息 MVP：支持加密 BLUFI 配网、会话未读标识、最近消息浏览、回复指定消息、飞书原生流式 ASR、检查/重录/取消，以及按键二次确认发送。
- 精简仓库根目录：将 GitHub 可识别的社区治理文档迁入 `.github/`，将变更记录迁入 `docs/`，同步全部引用，并在仓库检查中加入根目录文档白名单。
- 全仓库文档语言规范：所有维护中的 Markdown 默认 `.md` 文件使用英文，简体中文使用配对的 `.zh_CN.md`，双方提供语言切换；静态检查会阻止缺失配对、缺失切换链接或英文默认页混入中文正文。
- AI 开发流程一期：精简按任务加载的上下文入口，统一本地/CI 验证脚本，新增 PR 自动构建与模板，并提交依赖锁文件以提高构建可复现性。
- PR 审查修复：GitHub Actions 固定到完整 commit SHA，构建与发布 job 按最小权限拆分，同步 checkout 关闭凭证持久化；补充 Feature Request / Usage Question issue 表单；启用并修正私密安全报告兜底说明；清理 README 路径、CI 触发条件与历史分支描述漂移。
- 语言规范变更：commit 标题、PR 标题与 body 由"默认中文"改为**使用英文**（`docs/contribution/commit-and-pr.md` 更新）；中文写作规范（全角标点）适用范围剔除 PR/MR 描述（`doc-conventions.md` 更新）。
- CI 构建改造：`build-firmware.yml` 显式传入 `SDKCONFIG_DEFAULTS=sdkconfig.defaults` 再 `idf.py build`，由 defaults 启用自定义分区表（`CONFIG_PARTITION_TABLE_CUSTOM=y`，文件名为 `partitions.csv`）；`CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE` 改为 `n`，再用 `idf.py merge-bin -o build/FoloToy-AI-Passport-full.bin` 合并可直刷完整固件；产物精简为仅 full.bin；`actions/cache` 升级到 v5 以消除 GitHub Actions Node.js 20 弃用警告；CI 文档同步更新。
- 合并上游 PR #6（wireless-low-power-demos）以解决 PR #4 冲突：引入无线/低功耗 demo（`main/demo_wifi.c`、`demo_ble.c`、`demo_radio.c`、`demo_low_power.c`）、`partitions.csv`（NVS/PHY/3 MB factory-app 分区）、`main/CMakeLists.txt`/`main.c`/`demo.h`/`sdkconfig.defaults` 更新；同步硬件指南的 Wi-Fi/BLE/低功耗章节；README 能力契约表补充 Wi-Fi/Bluetooth LE/Low power 三项（中英双语）。
- 提交规范补充：`docs/contribution/commit-and-pr.md` 明确 PR 标题与 commit 标题使用相同的 Conventional Commit 格式和英文祈使句，不用名词短语当标题。
- CI 与文档清理：`sync-main.yml` 移除 `test_mode` 残留模板注释；`docs/development/coding-conventions.md` 将「Redis TTL」条目泛化为「缓存组件」条目（当前固件无 TTL 约束需求，消除从模板带入的无关约定）。
- 补充通用规范（借鉴 Shinku）：`docs/contribution/doc-conventions.md` 新增中文全角标点规范（正文 `，`；`（`）`，代码/命令/路径保留英文原样）、凭证不入仓规范（token/密钥/私钥绝不入仓，提交前 git diff 扫描敏感前缀）、文件删除安全规范（删除走系统回收站，不用 rm -rf/git clean -fd）。
- 代码注释规范强化：`docs/development/coding-conventions.md` 补充完善注释要求——函数说明（用途/参数/返回值/副作用/线程上下文/内存所有权/初始化顺序）、变量说明（语义/取值范围/生命周期/同步要求）、逻辑注释（状态机/时序/寄存器/魔数依据），覆盖范围宁多勿少，中文注释保留英文技术术语。
- 文档去 AI 化：`docs/README.md` / `docs/README.zh_CN.md` 移除 AI 专属章节（Entry point、Source-of-truth、提需求格式、BSP 边界、Runtime invariants、验收交付格式、构建命令），README 只保留给人看的项目介绍、硬件能力契约、demo 案例与项目结构；构建命令章节删除（与 `docs/development/build-and-test.md` 重复）。
- 新增 `docs/development/agent-guide.md`：集中承载"AI 如何在本仓库工作"（上下文建立顺序、事实来源优先级、提需求格式、BSP 边界、运行时规则、交付格式），并链接 build-and-test 与硬件指南，不重复构建命令与验收矩阵。
- 同步更新索引：`AGENTS.md` 规则索引新增 agent-guide 条目；`docs/INDEX.md` 与 `docs/development/README.md` 新增 agent-guide 索引行。
- 文档补充：`docs/fork-guide.md` 说明「为什么根目录不放置 README」——根目录 README 预留给 fork 开发者自行放置（上游留空），fork 后可将自己的内容写入根目录 `README.md` 介绍 fork 后的项目；GitHub 显示优先级（根 README > docs/README.md）契合该预留意图。
- 分支合并：创建 `main-update` 分支（基于与上游一致的 main），将 `feature/repo-structure`、`ci/build-firmware`、`ci/sync-main` 三个分支合并进来，统一 docs 结构（CI 文档归入 `docs/development/`，workflow 文件随 ci 分支引入 `.github/workflows/`）；解决 development/software-design README 的 add/add 冲突。
- 合并后审查修复：`docs/INDEX.md` 补充 CI 文档索引；`docs/fork-guide.md` 修正 workflow 引用为 `.github/workflows/sync-main.yml`；`docs/README` 双语项目结构块补充 `.github/workflows/` 与 CI 文档说明。
- ci 分支 CI 文档路径调整：`ci/build-firmware` 的 `docs/software-design/CI-build-and-release.md` 与 `ci/sync-main` 的 `docs/software-design/CI-sync-main.md` 均移入各分支的 `docs/development/`（CI 属工程规范）；`docs/software-design/README.md` 保留为软件设计索引；feature 分支的 software-design 索引同步更新引用。
- fork 补充文档目录迁移：`assets/docs/` 移至 `docs/assets/`（文档素材归入 docs/ 更合理），新增 `docs/assets/.gitkeep` 空目录占位；同步更新 AGENTS.md / INDEX / doc-conventions / fork-guide 的路径引用。
- 文档结构调整：根目录不再放 README——上游英文 README 移入 `docs/README.md`、中文移入 `docs/README.zh_CN.md`（GitHub 从 docs/ 识别主 README）；原 `docs/README.md` 根总索引更名为 `docs/INDEX.md`；同步更新 AGENTS.md / CONTRIBUTING / SUPPORT / fork-guide / doc-conventions 的路径引用。
- 初始化项目文档：新增 `AGENTS.md`、`CLAUDE.md` 和 `CHANGELOG.md`。
- 仓库结构规整：上游英文 `README.md` 更名为 `README.en_US.md`，保留 `README.zh_CN.md`。
- 新增目录骨架：`docs/`（software-design / hardware-design）、`assets/`（fonts / images / music，各含 `README.md`）、`skills/`。
- 将上游硬件开发指南归位到 `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md`。
- 文档规范：子目录 readme 统一为大写 `README.md`；补充 fork 用户约定（main 只动根 README）。
- 扩展 fork 用户约定：`main` 分支允许修改根目录 `README.md` 和 `assets/docs/`（README 不足以说明项目时存放补充文档与素材）。
- 新增 `assets/docs/` 目录约定：上游 main 只保留空目录 `.gitkeep`，内容文件仅存在于 fork；使用方法规范写入 AGENTS.md「给 fork 用户」约定。
- CI 文档迁移：`docs/software-design/CI.md` 从本分支移除，迁至 `ci/build-firmware` 分支并改名为 `docs/software-design/CI-build-and-release.md`。
- 补充 `main` 分支策略说明：解释 `main` 保持干净的两大原因（与上游同步无冲突 + 多小项目按分支整理）；例外——执意 main 开发需停用 CI 自动同步；提醒 fork 用户默认 action 关闭需手动启用（此条为整个 CI 的通用要求，统一写入 AGENTS.md）。
- 文档拆分：将 `AGENTS.md` 按主题拆为公共文档——新增 `docs/contribution/`（doc-conventions.md、commit-and-pr.md）与 `docs/development/`（build-and-test.md、coding-conventions.md），新增 `docs/fork-guide.md`；`AGENTS.md` 精简为简介 + 项目概述 + 必读文档索引。
- 同步更新索引：`docs/software-design/README.md`、`README.en_US.md` / `README.zh_CN.md` 的 `docs/` 目录说明。
- 参考 cindy 仓库文档组织完善索引：新增 `docs/README.md` 根总索引；AGENTS.md 规则索引按触发场景改写（附触发条件）；`docs/contribution/` 与 `docs/development/` 的 README 补充收录标准。
- 引入社区治理文档（参照 cindy 改写，放仓库根目录）：新增 `CONTRIBUTING.md` / `.zh_CN.md`（贡献指南，针对 ESP-IDF/AI agent/fork 场景改写）、`CODE_OF_CONDUCT.md` / `.zh_CN.md`（贡献者公约）、`SECURITY.md` / `.zh_CN.md`（安全报告流程）、`SUPPORT.md` / `.zh_CN.md`（支持渠道）；AGENTS.md 与 docs/README.md 同步引用。

- 区分应用选择与个人授权扫码页，拉开底部间隔，消息列表显示两行前缀。试用完整汉字 4 bpp 只读映射字库并校验完整性；语音识别权限改为应用身份，缺权限时明确提示。

- 已确认空草稿时跳过反复读盘，写入／账号切换／恢复时使空标记失效，冷读取可取消。底栏拆为状态／红色错误与页码／按键两行，采用同色系蓝灰背景。

- 启动时避免重复设置 SQLite auto_vacuum 和无条件清理 pin 所触发的写事务；增加源码级事务日志失败及恢复测试。

- 严重空间压力下按净回收页数选择 SPIFFS 回收块，计入额外索引重写成本；保留正常空间的磨损均衡与现有磁盘格式。

- 飞书默认使用共享 1 MiB 应用池上的 FlashDB，授权清空后从首次配置开始，不导入旧机器人凭据；SQLite 保留为显式构建选项。必要配置、缓存对象和草稿状态共用串行化的数据池。

- 修复飞书同时返回 content 和 content_v2 时富文本正文重复拼接的问题，跳过不支持的嵌套表示。

- 接入二维码绘制失败时显示错误并每秒重试，仅在绘制成功后标记已显示。

- 恢复语音持久草稿及离线审阅保留；END前回收采集资源，并在压缩队列预算中给TLS保留更多运行时内存。
