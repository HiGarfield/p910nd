# p910nd 缺陷验证脚手架

`cases/` 里的用例分两类：`DEFECT_REPORT.md` 中已报告缺陷的回归断言，以及后续审查陆续发现的源码、构建、CI、脚手架和文档缺陷。**用例通过 = 该缺陷不复现**（修复后语义；修复前是反过来）。用例清单以本文件下方的索引为准（由 `t-pd-32` 校验，PD-65 校验本节描述）；`DEFECT_REPORT.md` 只记录首轮与次轮的 16 + 12 条，不是全部用例。观测原始值写入 `build/tests/observations/<ID>.txt`，报告只引用不臆造。

```sh
make test                          # 全部用例（见下方用例索引，由 t-pd-32 校验）
make test-case CASE=PD-01          # 单条（ID 大小写皆可，可写 pd-01 或 t-pd-01）
sh tests/run-all.sh PD-01          # 等价的直接调用；make test PD-01 不成立（PD-63）
sh tests/cases/t-pd-05-*.sh        # 直接跑单个用例（变体会自动补建）
sh tests/mutation-check.sh         # 灵敏度自检：还原修复，用例必须失败
```

## 铁律

* **`p910nd.c` 永不被修改。** 需要换常量的用例构建源码副本（`mktemp -d` 到 `build/tests/`），用 `tools/patch_source.py` 打测试档补丁，编译完立刻删除副本。补丁对每个 needle 断言命中次数，匹配不上就报错退出，不会静默产出"看起来像出货版"的二进制。
* `P910ND_SRC=<file>` 让所有变体改从该文件构建。`tests/mutation-check.sh` 用它把修复还原到副本里验证用例会红。`p910nd.c` 本身始终不动。
* 打过补丁的变体把 `BASEPORT` 移到 `TEST_PORT`（默认 19100，可用 `TEST_PORT=<port>` 覆盖），并按比例缩小墙钟超时（30s→3s、60s→6s、120s→6s、120s→12s）。**超时之间的比例保持不变**，只有绝对时长变。
* 用例不需要 root。锁文件放在 `build/tests/` 下，守护进程一律 `-d` 启动（跳过守护化块因而不写 pid 文件）。
* 守护进程用 `setsid sh -c 'echo $$ > pidfile; exec "$@"'` 启动——`setsid` 本身会 fork，`$!` 拿到的是短命的 setsid 进程而不是守护进程，因此让被 exec 的进程自报 PID。清理按进程组信号。
* `daemon_start` 把 stdin 接到 `/dev/null`：p910nd 用 `getsockname(0)` 判断"是守护进程还是一次性服务"，继承来的 socket（IDE / CI runner 常见）会静默把它变成一次性服务。

## 构建产物

| 变体 | 构建方式 | 用途 |
| --- | --- | --- |
| `p910nd-baseline` | `make`，即出货配置 | PD-01（锁路径必须被访问）、PD-16 |
| `test` | 打补丁的副本 + `-DLOCKFILE_DIR=<tmp>` | 大部分运行期用例 |
| `test-lockdir` | 同上 | PD-01 并发对照、PD-04 |
| `test-lockdir-missing` | 锁目录指向不存在的路径 | （PD-02 修复后已不用，见 PD-03） |
| `test-<name>` | 用例自建，名字含 `test` 即走打补丁路径 | 各用例的专用变体 |

`build/tests/faketime.so` 是 `LD_PRELOAD` 垫片（`tools/faketime_preload.c`），通过一个控制文件驱动墙钟（`skew=` / `abs=` / `abs_step_us=`），只有 PD-07 用。`tools/stream_transitions.py` 数设备字节流的交替次数，PD-01 用。

## 用例索引

| 用例 | 缺陷 | 手法 |
| --- | --- | --- |
| `t-pd-00-static-analysis-baseline.sh` | 基线 | gcc c89 / `-fanalyzer` / cppcheck / clang-tidy / 降级路径各 0 诊断 |
| `t-pd-01-lock-mutex.sh` | 互斥缺失 | 出货构建 strace 必须出现锁路径；两客户端并发必须串行 |
| `t-pd-02-lock-dir-missing.sh` | 锁目录缺失 | 三层不存在的目录要被创建；不可创建的目录要被拒绝并点名 |
| `t-pd-03-inetd-lock-refused.sh` | 拒绝路径信号 | `tools/inetd_sim.py`：三种场景都必须 RST |
| `t-pd-04-dash-d-inetd-log.sh` | `-d` 语义 | 作业必须被服务；客户端必须收不到任何日志 |
| `t-pd-05-bidir-read-error.sh` | 打印机读错误 | `/proc/self/mem` 持续 EIO 必须快速结束并记录真实原因 |
| `t-pd-06-bidir-zero-read-eof.sh` | 0 读计数器 | 打印方向未完成时计数器不得触发 |
| `t-pd-07-wallclock-timeout.sh` | 墙钟依赖 | `faketime.so` 回拨 1 小时；另跑一遍降级构建对照 |
| `t-pd-08-job-lock-hol.sh` | 锁等待上界 | 默认 0 保持无限等待；`-DLOCK_WAIT_TIMEOUT=2` 必须超时并 RST |
| `t-pd-09-lockfile-mode.sh` | 锁文件权限 | 三种 umask 都必须 0644；符号链接必须被拒 |
| `t-pd-10-bind-fallthrough.sh` | bind 失败 | 必须报 "no address worked" 且不调用 accept |
| `t-pd-11-pace-normalize.sh` | 归一化 | `tools/pace_probe.c` 比对出货算术与理想释放时刻 |
| `t-pd-12-init-stop-match.sh` | init 停机 | 改名行为保留；init 脚本必须走 pid 文件并校验 |
| `t-pd-13-lock-held-dead.sh` | 死变量 | `lock_held` 出现次数必须为 0 |
| `t-pd-14-doc-drift.sh` | 文档漂移 | 四份文件版本一致 + 旋钮全覆盖 + init 无幻影命令 |
| `t-pd-15-version-const.sh` | 可写数组 | `version`/`copyright` 必须是 `const` |
| `t-pd-16-makefile-deps.sh` | 构建与 CI | 沙箱构建证明头文件触发重编；CI 必须跑测试 |
| `t-pd-17-instance-lock-failfast.sh` | 第二实例挂起 | 第二个实例必须立刻 exit(1) 并点名"already running"；前一个退出后新的能接管 |
| `t-pd-18-printer-wait-bounded.sh` | 等打印机无上界 | FIFO 无读者（ENXIO）时作业必须在 OPEN_PRINTER_MAX_WAIT 内放弃；之后打印机仍可用（锁没有卡住） |
| `t-pd-19-job-children-capped.sh` | 无界 fork | `MAX_JOB_CHILDREN=2` 时最多两个作业子进程且无人被拒；对照构建会 fork 六个 |
| `t-pd-20-bidir-silent-timeout.sh` | 双向无 SILENT_TIMEOUT | `-b` 下客户端中途暂停 6s 必须被容忍（8192 字节全到）；不发数据的客户端仍按短超时释放 |
| `t-pd-21-poll-only.sh` | 等待接口可回退 | 代码/文档零遗留旧接口字样；三处 `poll()` 与 `<poll.h>` 必须在位，二进制符号级校验 |
| `t-pd-22-close-drain.sh` | 关闭路径排空 | strace 证明收尾 `read()` 等待并读走客户端在途字节，而不是立刻 EAGAIN |
| `t-pd-23-first-retry-log.sh` | 首条失败日志 | 单调时钟起点为 5 秒时，首次打开失败仍以 LOG_ERR 记录 |
| `t-pd-24-child-table-signal-mask.sh` | 子进程表并发 | 关机路径只能在阻塞 SIGCHLD 的辅助函数里遍历 `child_pids` |
| `t-pd-25-printer-reply-reset.sh` | 回程 RST | loopback RST 必须让 `readBuffer()` 标记该方向 EOF |
| `t-pd-26-read-error-flag.sh` | 错误标志被清 | 网络写失败只清 WRITE_ERR；探针断言 READ_ERR 仍在 |
| `t-pd-27-accept-transient.sh` | 临时 accept 错误 | 注入一次 ENOSR 后，下一个客户端仍被完整服务 |
| `t-pd-28-inetd-libwrap.sh` | inetd 访问控制 | `hosts_ctl` 垫片记录调用；一次性模式必须做 libwrap 检查 |
| `t-pd-29-printer-number-range.sh` | 编号越界 | `3` 立即打印 Usage，`0` 仍能启动监听 |
| `t-pd-30-retry-throttle.sh` | 节流日志文本 | 确定性时钟数出 1 条正式告警与 27 条被节流的调试行 |
| `t-pd-31-ci-deps.sh` | CI 依赖缺失 | CI 必须声明 libwrap0-dev，且 USE_LIBWRAP 变体能编译 |
| `t-pd-32-readme-index.sh` | 文档索引漂移 | 每个 case 文件都必须出现在本索引中，且索引行数相等 |
| `t-pd-33-ignore-build-artifacts.sh` | 构建产物未忽略 | 独立检出里 `make` 之后 `git status` 必须干净 |
| `t-pd-34-argument-check.sh` | 参数校验不完整 | 非数字、多位、多余参数都必须打印 Usage；`0` 仍能启动 |
| `t-pd-35-multi-address.sh` | 只监听首个地址 | 通配监听下 IPv4 与 IPv6 客户端都必须完成作业 |
| `t-pd-36-flush-result.sh` | flush 结果被丢弃 | 探针断言 sink 丢弃字节被上报，三处调用都消费返回值 |
| `t-pd-37-standalone-detect.sh` | inetd 误判 | `getsockname(0)` 失败时仍须服务 fd 0 上已交付的连接 |
| `t-pd-38-child-cap-oom.sh` | OOM 子进程漏计 | OOM 时子进程仍计入 `inflight_children`，回收时无条件递减 |
| `t-pd-39-printer-template.sh` | 设备模板未校验 | 非单个 `%c` 的模板启动时拒绝；路径缓冲固定并检查截断 |
| `t-pd-40-clock-base.sh` | 时钟基准混用 | 单调钟临时失败后三帧采样必须保持同一基准并继续推进 |
| `t-pd-41-pollnval-exit.sh` | 监听 fd 失效空转 | POLLNVAL 垫片下守护进程须自行停机并说明原因 |
| `t-pd-42-skip-is-failure.sh` | 跳过用例假绿 | 端口被占用导致跳过时整套测试必须非零退出并说明原因 |
| `t-pd-43-flush-sink-count.sh` | 环形缓冲重复计数 | 跨尾部的丢弃缓冲区须按实际消耗字节上报 |
| `t-pd-44-ci-action-pins.sh` | CI Action 可变引用 | 全部 `uses:` 须固定到已发布版本标签且步骤不缺失 |
| `t-pd-45-spec-file-modes.sh` | RPM 文件权限 | spec 的配置与 man 页须为 0644，仅程序为 0755 |
| `t-pd-46-case-id-width.sh` | 用例编号位数截断 | 两位正则会把 PD-100 折叠成 PD-10；共享枚举须支持任意位数 |
| `t-pd-47-inetd-sim-clock.sh` | inetd 模拟器墙钟超时 | 墙钟回拨一小时，读写/回收两个超时仍须按时触发 |
| `t-pd-48-client-timeout-status.sh` | 客户端超时被判成功 | 无确认的超时/OS 错误须非零退出，干净 FIN 仍为零 |
| `t-pd-49-init-status.sh` | init status 误判 | 运行中守护进程的 argv[0] 已改名，status 须据 pid 文件识别 |
| `t-pd-50-lock-wait-sigterm.sh` | 关闭时锁等待挂死 | got_term 已置位时 F_SETLKW 等待须立即放弃 |
| `t-pd-51-devnode-clock.sh` | pty 设备墙钟期限 | 墙钟回拨后 pty master 仍须按时关闭 |
| `t-pd-52-lockhold-clock.sh` | 锁持有者墙钟期限 | 墙钟回拨后 byte-1 作业锁仍须按时释放 |
| `t-pd-53-kill-stray-scope.sh` | 清理函数误杀进程 | 仅提及构建路径的无关进程须存活，构建树内可执行文件仍被回收 |
| `t-pd-54-client-complete-mode.sh` | 完整回复上报 none | 收满期望字节时须报 complete，短回复仍报 timeout |
| `t-pd-55-static-check-not-vacuous.sh` | 静态检查永不失败 | 强制覆盖锁目录须报 DEFECT，且无双 OK 分支 |
| `t-pd-56-libwrap-reject-rst.sh` | 拒绝连接误报成功 | 独立模式 libwrap 拒绝须以 RST 结束，允许时仍为 FIN |
| `t-pd-57-client-read-timeout.sh` | 读超时被放大 | 0.1 秒读超时须在 0.6 秒内结束，1.4 秒仍生效 |
| `t-pd-58-case-timeout-budget.sh` | 静态分析假超时 | 用例可声明更大墙钟预算，默认预算不得被放宽 |
| `t-pd-59-init-status-no-pidfile.sh` | pid 文件丢失误判 | 无 pid 文件时仍须按 argv[0] 识别存活守护进程 |
| `t-pd-60-inetd-sim-complete.sh` | 完整回复误报超时 | 收满期望字节须报 complete，短回复仍报 timeout |
| `t-pd-61-report-current-state.sh` | 缺陷报告现状描述漂移 | 报告中的时钟、缓冲与用例数描述须与源码一致 |
| `t-pd-62-devnode-repl-clock.sh` | repl 事件墙钟时间戳 | 墙钟回拨后 t= 仍须为递增的单调经过时长 |
| `t-pd-63-single-case-command.sh` | 单用例命令失效 | `make test-case CASE=` 须只跑指定用例，旧写法被拒 |
| `t-pd-64-inetd-sim-exit-status.sh` | 模拟器失败仍返回 0 | RST/超时须非零退出，FIN/完整回复仍为零 |
| `t-pd-65-harness-readme-current.sh` | 脚手架文档漂移 | 用例范围与 TEST_PORT 覆盖须与实现一致 |
| `t-pd-66-inetd-diagnostic-output.sh` | inetd 诊断注入打印流 | `-Z`/`-v` 下客户端须收到 0 字节，命令行输出保持 |
| `t-pd-67-mono-now-clock-failure.sh` | 双时钟失败返回未定义值 | 两个钟都失败时须回退到零值，墙钟/单调钟正常路径不变 |
| `t-pd-68-init-stop-no-pidfile.sh` | stop 漏停无 pid 文件进程 | status 能发现的守护进程 stop 必须同样能停止 |
| `t-pd-69-init-proc-scan-race.sh` | /proc 扫描竞态报错 | 进程在扫描间退出时 stderr 须为空且有效项仍被发现 |
| `t-pd-70-inetd-sim-read-timeout.sh` | 模拟器读超时被放大 | 0.1 秒须在 0.4 秒内结束，1.2 秒仍生效并记录耗时 |
| `t-pd-71-early-syslog-init.sh` | 早期诊断丢失 syslog 身份 | inetd 下无效参数与 -v 须先 `openlog(LOG_LPR\|LOG_PID)` 再 `vsyslog` |
| `t-pd-72-harness-monotonic-wait.sh` | 测试框架墙钟等待挂起 | 墙钟回拨后两个有界等待须正常结束，正向路径仍成功 |
| `t-pd-73-float-assert-numeric.sh` | 浮点断言空值假绿 | 空值/非数值操作数须被拒绝，合法浮点比较行为不变 |
| `t-pd-74-runall-signal-exit.sh` | 驱动忽略取消信号 | SIGTERM 须以 143 立即退出，正常退出仍为 0 |
| `t-pd-75-variant-stamp-content.sh` | 源码改动不触发重建 | 同路径内容变化须重建共享变体，未变化则复用缓存 |
| `t-pd-76-init-self-invoke-quoting.sh` | init 递归调用未引号 | 含空格路径下 restart 仍须执行 stop 与 start |
| `t-pd-77-duplicate-case-id.sh` | 同编号用例被静默丢弃 | 重复编号须报失败且不执行，单一文件仍正常通过 |
| `t-pd-78-init-path-quoting.sh` | init 二进制/配置路径拆词 | 含空格路径须通过存在性检查、加载配置并启动二进制 |
| `t-pd-79-daemon-stop-pgid-reuse.sh` | PGID 复用误杀无关进程 | 回收的进程组不得被信号，真实守护进程仍须正常停止 |
| `t-pd-80-jobmax-ceiling.sh` | 报告 B-2 只修一半 | 作业绝对上限须真实生效，默认关闭语义与文档一致 |
| `t-pd-81-stall-budget-reset.sh` | 报告 B-3 无回归保护 | 两处预算复位与实测日志须受守护，删一处即变红 |
| `t-pd-82-child-sigmask-restore.sh` | 报告 B-4 无回归保护 | 作业子进程 `SigBlk` 不得阻塞 SIGCHLD，删除恢复调用即变红 |
| `t-pd-83-client-out-error.sh` | client.py 回复文件写入失败 | `--out` 不可写时仍须输出 RESULT、返回 2 且不泄漏连接 |
| `t-pd-84-repl-broken-pipe.sh` | repl 谎报写入并抛异常 | 读者消失时须只记录真实写入、输出结束事件且不泄漏 fd |
| `t-pd-85-inetd-target-missing.sh` | 服务未启动却报成功 | 无效目标须拒绝启动、诊断不注入客户端流、`exit=127` 判失败 |
| `t-pd-86-init-return-command.sh` | init 辅助函数从不执行包装命令 | `checkproc`/`start_daemon`/`rc_check` 须真正调用并传递返回码 |
| `t-pd-87-spec-arch-pin.sh` | RPM spec 排除非 x86 架构 | 不得硬编码架构列表，CI 交叉编译矩阵须全部可用 |
| `t-pd-88-client-bad-send.sh` | client.py 非法发送参数抛异常 | `\u` 越界与非数值 `--send-byte` 须输出 bad-send 结果并返回 2 |
| `t-pd-89-inetd-sim-bad-byte.sh` | 模拟器字节参数解析遗漏 | 非法 `--send-byte` 须输出 bad-send-byte 结果并返回 2 |

`t-pd-13` 起的静态类用例通过 `tools/static_checks.py` 判定，它的判定词是 `OK`（属性成立）/ `DEFECT`（缺陷仍在）。

## 环境要求

必需：`gcc`（或 `$CC_BIN`）、`python3`、`strace`、`setsid`、`timeout`、`make`、`libwrap0-dev`（提供 `tcpd.h` 与 `libwrap.so`，PD-28 / PD-31 用）。
可选：`cppcheck`、`clang-tidy` 与 `clang`（PD-00 会记录是否跳过）、`nm`（PD-07、PD-21 用）。
