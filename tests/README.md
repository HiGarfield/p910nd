# p910nd 缺陷验证脚手架

`cases/` 里每个用例验证 `DEFECT_REPORT.md` 中的一条缺陷。**用例通过 = 该缺陷不复现**（修复后语义；修复前是反过来）。观测原始值写入 `build/tests/observations/<ID>.txt`，报告只引用不臆造。

```sh
make test                          # 全部 17 个用例，约 187 秒
make test PD-01                    # 单条（ID 大小写皆可，可写 pd-01 或 t-pd-01）
sh tests/cases/t-pd-05-*.sh        # 直接跑单个用例（变体会自动补建）
sh tests/mutation-check.sh         # 灵敏度自检：还原修复，用例必须失败
```

## 铁律

* **`p910nd.c` 永不被修改。** 需要换常量的用例构建源码副本（`mktemp -d` 到 `build/tests/`），用 `tools/patch_source.py` 打测试档补丁，编译完立刻删除副本。补丁对每个 needle 断言命中次数，匹配不上就报错退出，不会静默产出"看起来像出货版"的二进制。
* `P910ND_SRC=<file>` 让所有变体改从该文件构建。`tests/mutation-check.sh` 用它把修复还原到副本里验证用例会红。`p910nd.c` 本身始终不动。
* 打过补丁的变体把 `BASEPORT` 移到 19100，并按比例缩小墙钟超时（30s→3s、60s→6s、120s→6s、120s→12s）。**超时之间的比例保持不变**，只有绝对时长变。
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

`t-pd-13` 起的静态类用例通过 `tools/static_checks.py` 判定，它的判定词是 `OK`（属性成立）/ `DEFECT`（缺陷仍在）。

## 环境要求

必需：`gcc`（或 `$CC_BIN`）、`python3`、`strace`、`setsid`、`timeout`、`make`。
可选：`cppcheck`、`clang-tidy`（PD-00 会记录是否跳过）、`nm`（PD-07 用）。
