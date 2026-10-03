# p910nd 缺陷报告

审查对象：`p910nd.c`（2109 行）、`Makefile`、`aux/p910nd.conf`、`aux/p910nd.init`、`aux/p910nd.spec`、`p910nd.8`、`README.md`、`.github/workflows/CI.yml`
基线提交：`da6685d`　审查日期：2026-10-03　修复日期：2026-10-03
验证环境：Linux 7.0.0-34-generic x86_64，gcc/clang、strace、python3、cppcheck、clang-tidy，非 root

## 状态：16 条缺陷全部已修复并逐条测试通过

`make test`（17 个用例，连跑两轮全绿，约 187 秒）现在**断言的是修复后的行为**：每个用例通过即表示该缺陷不再复现。原始观测值落在 `build/tests/observations/`，本文引用的数字全部来自这些文件。

修复前的证据保留在各条的「修复前」引用块里作为回归对照，修复后的对应观测值列在「修复后实测」。

### 测试语义随代码一起翻转

修复前，用例**通过 = 缺陷复现**；修复后，用例**通过 = 缺陷消失**。方向是随代码一起翻转的（`tests/tools/static_checks.py` 的 `CONFIRMED`/`CLEARED` 也改成了 `OK`/`DEFECT`），所以任何一条回归都会让对应用例立刻变红。

为证明用例不是"永远绿"，`tests/mutation-check.sh` 会把修复逐条还原到源码副本里跑一遍：

```
1/3  PD-05: put the printer read-error exit back out
ok: the case fails without the fix
2/3  PD-09: ask for the world writable lock file again
ok: the case fails without the fix
3/3  PD-07: force the wall clock in the default build
ok: the case fails without the fix
all mutations were detected
```

### 实施过程中修正的三处判断

1. **PD-03 的严重程度被实测下调。** 初版断言认为 inetd 拒绝作业会让客户端收到干净 FIN 并误判成功。实测发现内核规则"带未读数据的 `close()` 发 RST"已经把真实打印作业救了回来（客户端已发数据 → RST），只有尚未发送任何数据时才会得到 FIN。修复保持不变（统一走 `close_connection(0, 1)`），严重程度定为中。
2. **PD-05 / PD-06 的复现手段被实测替换。** 原打算用 pty 制造持续 EIO、用 FIFO 制造"静默后恢复"，实测 pty 首次 EIO 之后持续返回 0、FIFO 因 p910nd 自身 `O_RDWR` 也永不返回 0。改用 `/proc/self/mem`（每次 read 都 EIO）才做出硬错误对照。
3. **PD-08 的默认行为保持不变。** `LOCK_WAIT_TIMEOUT` 默认为 0（无限等待）——用例 A 专门断言"未设置该旋钮时行为与修复前完全一致"，用例 B 才验证启用后的上界行为。

### PD-07 的实现要求：条件编译 + 降级

按要求封装了 `mono_now()`，并做了条件编译与降级：

```c
#if !defined(NO_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
# if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#  if __GLIBC_PREREQ(2, 17)
#   define P910ND_HAVE_MONOTONIC 1
#  endif
# elif !defined(__GLIBC__)
#  define P910ND_HAVE_MONOTONIC 1
# endif
#endif
```

- glibc ≥ 2.17 与 musl/uClibc/BSD/macOS：走 `clock_gettime(CLOCK_MONOTONIC)`，无需额外链接。
- glibc < 2.17（`clock_gettime` 在 librt 里）：自动降级为 `gettimeofday()`；`Makefile` 留了 `#override LIBS += -lrt` 注释行，取消注释即可启用。
- 任何平台都可用 `-DNO_CLOCK_GETTIME=1` 强制降级。
- `mono_now()` 从不失败：`clock_gettime()` 失败时内部退回 `gettimeofday()`。
- 全部 36 处计时点与 2 处 `time(0)` 已改走 `mono_now()`；用例断言"源码中 `gettimeofday(&` 与 `time(0)` 均为 0 处"，并用 `nm` 确认两个构建的符号差异。

## 总表

| ID | 严重程度 | 一句话 | 行为变更 | 状态 |
| --- | --- | --- | --- | --- |
| PD-01 | 严重 | 默认构建下 standalone 模式**完全没有任何打印机互斥** | 是 | 已修复 |
| PD-02 | 高 | 锁目录不存在时守护进程 `exit(1)` 拒绝启动 | 是 | 已修复（方案 ①） |
| PD-03 | 中 | inetd 下取锁失败用干净 FIN 关闭连接 | 是 | 已修复 |
| PD-04 | 高 | `-d` 在 inetd 下不服务连接，并把日志写进客户端流 | 是 | 已修复 |
| PD-05 | 中高 | 双向模式下打印机读错误从不终止作业 | 是 | 已修复 |
| PD-06 | 中低 | 双向模式唯一的结束信号是"连续 20 次 0 字节读" | 是 | 已修复（方案 ①） |
| PD-07 | 中 | 全部超时基于墙钟，回拨使所有超时失效 | 是 | 已修复（条件编译 + 降级） |
| PD-08 | 中 | 作业锁 `F_SETLKW` 无上界 | 是 | 已修复（默认 0 = 不变） |
| PD-09 | 低 | 锁文件以 `0666` 请求 | 是 | 已修复（0644 + O_NOFOLLOW） |
| PD-10 | 中 | `bind()` 全失败后带已关闭 fd 进入 accept | 否 | 已修复 |
| PD-11 | 低 | 节流归一化用 `>` 而非 `>=` | 否 | 已修复 |
| PD-12 | 中低 | 改写 argv[0] 使 init 脚本停不掉进程 | 是 | 已修复（改 init 脚本） |
| PD-13 | 低 | `lock_held` 只写不读 | 否 | 已修复 |
| PD-14 | 低 | 四份文件四个版本号；man page 引用缺失文件 | 否 | 已修复 |
| PD-15 | 低 | `version[]` / `copyright[]` 是可写数组 | 否 | 已修复 |
| PD-16 | 低 | `$(PROG)` 无头文件依赖，CI 只编译不测试 | 否 | 已修复 |

## 静态分析基线（PD-00）

| 工具 | 退出码 | 诊断数 |
| --- | --- | --- |
| `gcc -std=c89 -pedantic -Wall -Wextra -O2` | 0 | 0 |
| `gcc -fanalyzer` | 0 | 0 |
| `cppcheck --enable=warning,style,performance,portability` | 0 | 0 |
| `clang-tidy` | 0 | 0 |
| `gcc -DNO_CLOCK_GETTIME=1 -Werror`（降级路径） | 0 | 0 |

修复前这 16 条缺陷**一条都没有被任何工具发现**；修复后基线仍为 0，用例把"总数必须为 0"也断言了下来。

---

## PD-01 默认构建下没有任何打印机互斥

**缺陷 ID**：PD-01　**严重程度**：严重　**行为变更**：是　**状态**：已修复

**位置**：`p910nd.c:1900-1908`（`server()`）、`Makefile:8-14`、`p910nd.c:662-694`（`get_lock()`）

```c
/* 修复前：整段被守卫编译掉，LOCKFILE_DIR 没有任何构建定义 */
#ifdef	LOCKFILE_DIR
	if (get_lock(lpnumber) == 0)
		exit(1);
#endif
```

**根因**：`LOCKFILE_DIR` 只在用户手动取消 `Makefile` 注释时才有定义，默认构建与 CI 全部 34 个架构都不定义它，`get_lock()` 被预处理器整段删除，`lockfd` 恒为 `-1`。而 `lock_printer_job()` 第一句就是 `if (lockfd < 0) return (1);`——**两个锁都失效，且静默失效**，与其上方四行注释直接矛盾。提交 `d78a7a1`（取消作业子进程上限）与 `a11690e`（忙则排队）都假定锁生效，守卫一失效就变成同一台打印机上的输出交错。

LSP `findReferences get_lock` 佐证：默认预处理下 `get_lock()` **只有一个调用点**（`one_job()`），`server()` 里那个被编译掉了。

> **修复前实测**：默认构建 strace 中锁文件 `openat` = 0、`F_SETLK` = 0；两客户端并发时设备字节流切换 **73 次** / 600000 字节。只多加一个 `-DLOCKFILE_DIR` 变成 1/1 与 **1 次**切换。

**修复方案（已实施）**：删掉 `#ifdef LOCKFILE_DIR` 守卫，`server()` 无条件取实例锁；`get_lock()` 在锁目录缺失时创建它（PD-02）。

**修复后实测**（`make test PD-01`）：默认构建的 strace 中出现
`openat(AT_FDCWD, "/var/lock/subsys/p9100d", O_RDWR|O_CREAT|O_NOFOLLOW, 0644) = -1 EACCES`
——路径被访问了（守卫已消失），非 root 下取不到锁于是干净退出（exit 1）。两客户端并发的设备字节流切换 **1 次** / 600000 字节。

**行为影响**：
- 变更前：默认构建下同一打印机并发作业交错（73 次字节交替）。
- 变更后：并发作业排队（1 次交替），与 man page 和注释一致。
- 影响面：所有 standalone 部署。
- 风险：锁取不到时守护进程拒绝启动而非无锁运行——这是有意的取捨，已在 PD-02 侧解决可用性。

**确认状态**：- [x] 已确认并实施（2026-10-03）

---

## PD-02 锁目录不存在时守护进程拒绝启动

**缺陷 ID**：PD-02　**严重程度**：高　**行为变更**：是　**状态**：已修复（方案 ① `mkdir` 重试）

**位置**：`p910nd.c:662-694`（新增 `make_lock_dir()`）、`p910nd.c:157`

**根因**：`/var/lock/subsys` 是 SUSE 路径，Debian/Ubuntu 通常只有 `/var/lock -> /run/lock` 而无 `subsys`，OpenWrt 同理。`get_lock()` 不 `mkdir`、不回退，任何 `open()` 失败都 `return 0`，`server()` 变成 `exit(1)`。这条路径今天够不到（被 PD-01 的守卫编译掉），但**修好 PD-01 就会立刻变成"缺目录的系统上守护进程彻底起不来"**，而这正是本项目的目标平台。

**修复方案（已实施）**：新增 `make_lock_dir()`，逐级 `mkdir(0755)`，`EEXIST` 视为成功；`get_lock()` 在 `errno == ENOENT` 时调用它并重试一次 `open()`。同时按方案 ① 的要求，锁目录不可创建时仍然致命（不再降级为无锁运行）。

**修复后实测**（`make test PD-02`）：

| 段 | 场景 | 结果 |
| --- | --- | --- |
| A | `-DLOCKFILE_DIR=<case>/lock/deeply/nested`（三层不存在） | 目录被创建，锁文件 mode `644`，守护进程正常服务 4096 字节作业 |
| B | 同一路径第二次启动 | 无 `No such file` 报错 |
| C | `-DLOCKFILE_DIR=/proc/p910nd-deny`（无法创建） | 拒绝启动，日志点名 `p910nd-deny/p9100d` |

**行为影响**：
- 变更前：缺目录 → `exit(1)`；inetd 部署在不可写锁目录下静默拒绝连接。
- 变更后：缺目录自动创建后正常启动；仍无法创建时按既定策略退出。
- 影响面：缺 `/var/lock/subsys` 的发行版与 OpenWrt 类设备。
- 风险：只读根文件系统上 `mkdir` 失败，仍会拒绝启动（但日志会明确指出锁路径）。

**确认状态**：- [x] 已确认并实施（2026-10-03，方案 ①）

---

## PD-03 inetd 下取锁失败用干净 FIN 关闭连接

**缺陷 ID**：PD-03　**严重程度**：中　**行为变更**：是　**状态**：已修复

**位置**：`p910nd.c:1819-1829`（`one_job()`）

**根因**：这条分支靠进程退出让 fd 0 被 `close()`，没走 V3 设计的 `SO_LINGER{1,0}` + RST。`handle_connection()` 对同一语义用了 `close_connection(fd, 1)`。

> **修复前实测**：A1 客户端已发 64 字节 → `rst`（内核"带未读数据 close 发 RST"规则偶然救回）；A2 客户端未发数据 → `fin`（被当作成功）；B 设备缺失 → `rst`。

**修复方案（已实施）**：失败分支改用 `close_connection(0, 1)` 后再 `return`。注意锁失败条件在 PD-02 修复后变成了"锁目录无法创建"（用例改用 `/proc/p910nd-deny` 构造）。

**修复后实测**：A1 `rst`、A2 `rst`、B `rst`——三种情况统一。

**行为影响**：
- 变更前：客户端未发数据时收到 FIN（视为成功）；已发数据时收到 RST（偶然正确）。
- 变更后：统一 RST，与 `handle_connection()` 一致。
- 影响面：inetd 部署下取锁失败的作业；客户端从"不重试"变为"重试"。
- 风险：把原本被内核规则掩盖的行为显式化，依赖 RST 的客户端处理方式可能与 FIN 不同。

**确认状态**：- [x] 已确认并实施（2026-10-03）

---

## PD-04 `-d` 在 inetd 下不服务连接，并把日志写进客户端字节流

**缺陷 ID**：PD-04　**严重程度**：高　**行为变更**：是　**状态**：已修复

**位置**：`p910nd.c:443-460`（新增 `stdout_is_the_client_socket()` + `dolog()`）、`p910nd.c:2170-2178`（`main()`）

**根因**：两个问题叠加。(1) `(x)inetd` 把 socket `dup2` 到 fd 0/1/2，`-d` 置 `log_to_stdout` 后 `dolog()` 走 stdout，即写回客户端；(2) `main()` 里的 `log_to_stdout ||` 让 `-d` 在 inetd 下完全跳过 `one_job()` 去 `server()` 监听，被交付的连接没人读。

> **修复前实测**：A 段设备收到 **0** 字节、客户端 `how=timeout`；B 段第一个客户端收到 **197 字节**日志文本，含 `Connection from 127.0.0.1 port 48190 accepted` / `wrote 1 bytes to printer` / `Finished job: 1/1 bytes sent to printer`。

**修复方案（已实施）**：
- `main()` 去掉 `log_to_stdout ||`，只按 `is_standalone()` 分流；
- 新增 `stdout_is_the_client_socket()`（`fstat` 比对 fd 1 与 fd 0 的 `st_dev`/`st_ino` 且 `S_ISSOCK`），命中时 `dolog()` 强制走 `vsyslog`。

**修复后实测**：

| 段 | 结果 |
| --- | --- |
| A `-d` + inetd | 设备收到 **256** 字节，客户端 `how=fin`（作业真正被服务了） |
| B 第二个客户端触发日志 | 第一个客户端收到 **0** 字节，`client_received_preview` 里没有任何 p910nd 日志 |
| B' stdout 检查 | `client_received=0` |
| C 不带 `-d` | 设备 **256** 字节（无回归） |
| D `-d` 前台 | 日志仍进 stdout（`Connection from` 在日志里） |

**行为影响**：
- 变更前：`-d` + inetd = 不服务作业 + 日志注入客户端流。
- 变更后：正常服务该连接，日志进 syslog；`-d` 手工前台运行行为不变。
- 影响面：仅 `-d` 调试路径。
- 风险：inetd 下调试者需改看 syslog。

**确认状态**：- [x] 已确认并实施（2026-10-03）

---

## PD-05 双向模式下打印机读错误从不终止作业

**缺陷 ID**：PD-05　**严重程度**：中高　**行为变更**：是　**状态**：已修复

**位置**：`p910nd.c:1323-1336`（`copy_stream()` 双向循环）

**根因**：主循环只对 `networkToPrinterBuffer` 检查 `READ_ERR`（1183-1187 旧行号），`printerToNetworkBuffer.err` 全程只当 `WRITE_ERR` 用。于是这个方向的生死完全交给两个不相干的启发式。

> **修复前实测**（客户端发 0 字节并半关闭，只换设备）：`/proc/self/mem`（每次 read 都 EIO）作业 **5.87 s**；`/dev/null`（软"无数据"）作业 **0.34 s**。**打印机 outright 失败比"什么都没读到"还要命。**

**修复方案（已实施）**：在双向循环内补一条与打印方向对称的判断，注释说明了"硬错误比软情况更难处理"这一事实：

```c
			/* PD-05: the same for the printer->network direction. ... */
			if ((printerToNetworkBuffer.err & READ_ERR) && printerToNetworkBuffer.bytes == 0)
				break;
```

**修复后实测**：A 段（EIO）作业 **0.42 s**，日志保留真实原因（2 条 `read: Input/output error`），且**不再**出现 `sent no data, stop reading from printer`；B 段（软 0 读）**0.37 s** 依旧走计数器。静态核实：`printerToNetworkBuffer.err & READ_ERR` 从 0 处变 1 处。

**行为影响**：
- 变更前：打印机硬读错误时作业等满 `PRINTER_REPLY_WINDOW`（出货 60 秒），且被描述为"quiet printer"。
- 变更后：读完已缓冲字节后立即结束，并记录真实错误原因。
- 影响面：仅 `-b`；打印方向判定不变（仍只看 `networkToPrinterBuffer`），已完整打印的作业依然报成功，不会重复打印。
- 风险：把瞬时 EIO 当常态的驱动会提前结束作业并触发客户端重试。

**确认状态**：- [x] 已确认并实施（2026-10-03）

---

## PD-06 双向模式唯一的结束信号是"连续 20 次 0 字节读"

**缺陷 ID**：PD-06　**严重程度**：中低　**行为变更**：是　**状态**：已修复（方案 ① 限定在打印方向完成后）

**位置**：`p910nd.c:1398-1420`

**根因**：`/dev/lpX`、`usblp` 永不返回 EOF，所以 R6 用"连续 0 字节读"当结束标志，注释里点名适用对象是"a device that only ever returns 0 (regular file, /dev/null, **a wedged USB device**)"。无条件应用时，打印机在两个回答块之间停顿也会被当成"没话说了"。

> **修复前实测**：阈值 20 → 0.33 s；阈值 2000 → 5.45 s（证明该计数器是唯一的决定因素）。诚实边界：pty/FIFO 空闲时返回 `EAGAIN` 而非 0，计数器根本不前进，所以 lp/usblp 这类真实设备**不受影响**，残留风险只在"用 0 字节读表示帧结束"的驱动上。

**修复方案（已实施，方案 ①）**：计数器只在 `networkToPrinterBuffer.eof_sent` 时生效；打印方向未完成时把 `zero_reads` 清零重新计数，交给有界的回包窗口做主。

**修复后实测**：

| 段 | 结果 |
| --- | --- |
| A 客户端连接但未半关闭 | 2.5 s 观察窗内计数器**未触发**（日志计数保持基线 1，来自端口探测自身的作业），作业由 `no data transferred for 3 seconds` 结束 |
| B 客户端半关闭后 | 计数器正常触发，作业 **0.36 s** 结束 |
| C 空闲 FIFO | 计数器 0 次触发，客户端完整收到 **54** 字节回程 |

**行为影响**：
- 变更前：0 读满 20 次即结束会话（约 2 秒静默），可能在作业中途。
- 变更后：会话只在打印方向结束后才可能被 0 读收掉；中途由有界窗口兜底。
- 影响面：仅 `-b` + 使用 0 字节读表示帧结束的驱动；真实 lp/usblp 与配 `-b` 打印时无变化。
- 风险：设备真死但一直返回 0 时，会多等一个 `PRINTER_REPLY_WINDOW`（默认 60 秒）——但那本来就是有界的。

**确认状态**：- [x] 已确认并实施（2026-10-03，方案 ①）

---

## PD-07 全部超时基于墙钟

**缺陷 ID**：PD-07　**严重程度**：中　**行为变更**：是　**状态**：已修复（条件编译 + 降级）

**位置**：`p910nd.c:445-520`（新增 `P910ND_HAVE_MONOTONIC` 门控与 `mono_now()`）、全部 36 处计时点、`Makefile:16-24`

**根因**：所有 `now - start` 都取自 `CLOCK_REALTIME`。NTP 校正、手动改表、VM 从挂起恢复都会让差值变负，`>= limit` 恒假，**IDLE_TIMEOUT / SILENT_TIMEOUT / PRINTER_STALL_TIMEOUT / PRINTER_REPLY_WINDOW / SHUTDOWN_GRACE / 永久失败预算全部同时失效**；前跳则相反，超时同时到期并丢弃正在打印的作业。

> **修复前实测**：作业开始 1 秒后把墙钟回拨 1 小时 → 7 秒后客户端**仍连接**，日志里没有任何 idle 超时行；把 `skew` 改回 0 → 立即补发 FIN（总 elapsed 7.227 s）。

**修复方案（已实施）**：见文首「PD-07 的实现要求」的条件编译块。要点：
- `mono_now()` 是程序里唯一读时钟的地方，`clock_gettime()` 失败时内部退回 `gettimeofday()`；
- glibc ≥ 2.17 与 musl/uClibc/BSD/macOS 走单调时钟且**不需要额外链接**；
- glibc < 2.17 自动降级，`Makefile` 里 `#override LIBS += -lrt` 取消注释即可启用；
- `-DNO_CLOCK_GETTIME=1` 任何平台都能强制降级；
- `open_printer_retry()` 的预算参数从 `time_t` 改为 `const struct timeval *`，与其它计时点统一。

**修复后实测**：

| 段 | 结果 |
| --- | --- |
| A 默认（单调） | 时钟回拨后客户端仍在 **3.212 s** 被干净关闭（`how=fin`），日志正常记录 idle 超时 |
| B `-DNO_CLOCK_GETTIME=1` | 回拨后 **8 s 仍未关闭**，观察窗内 idle 超时计数为 **0**；把墙钟恢复后超时立即补发 |
| C 静态 | 源码中 `gettimeofday(&` 0 处、`time(0)` 0 处；`nm` 显示默认构建引用 `clock_gettime`（1）、降级构建不引用（0） |
| PD-00 | 降级路径 `-Werror` 编译 0 诊断 |

**行为影响**：
- 变更前：墙钟被改动时全部超时失效或同时误触发。
- 变更后：超时只受单调时钟影响。
- 影响面：全部超时路径；正常运行（时钟不被动）完全不变。
- 风险：引入 `clock_gettime` 依赖；老 glibc 需 `-lrt`（已用条件编译 + 注释开关处理）。注意 `reap_children_and_exit` 的关机宽限也改为单调计时。

**确认状态**：- [x] 已确认并实施（2026-10-03）

---

## PD-08 作业锁无时间上限

**缺陷 ID**：PD-08　**严重程度**：中　**行为变更**：是　**状态**：已修复（默认 0 = 行为不变）

**位置**：`p910nd.c:192-200`（新增 `LOCK_WAIT_TIMEOUT`）、`p910nd.c:601-660`（`take_lock()`）

**根因**：`a11690e` 把"打印机忙"改成"排队"但队列无上界。`F_SETLKW` 无限等待，只有信号能打断（`EINTR` + `got_term`），而排队的客户端不发任何东西。

> **修复前实测**：`lockhold.py` 占住字节 1 → strace 中两个子进程停在 `F_SETLKW {... l_start=1 ...} <unfinished ...>`，8.11 s 内 0 字节到达打印机，客户端既没收到 FIN 也没收到 RST。

**修复方案（已实施）**：新增 `LOCK_WAIT_TIMEOUT`（秒）。为 0（**默认**）时保持 `F_SETLKW` 无限等待，与修复前逐字节一致；大于 0 时改用 `F_SETLK` 轮询（每秒一次 `sleep_us(1000000)`），超时记 `lock: still busy after N seconds` 并返回 0，由 `handle_connection()` 按 V3 发 RST。

**修复后实测**：

| 段 | 配置 | 结果 |
| --- | --- | --- |
| A | 默认（`LOCK_WAIT_TIMEOUT=0`） | 客户端 `timeout`（8.014 s），0 字节到达，2 次 `F_SETLKW` 停在字节 1 —— **与修复前一致** |
| B | `-DLOCK_WAIT_TIMEOUT=2` | 客户端 **2.004 s** 收到 `rst`，0 字节到达打印机，日志含 `still busy after 2 seconds` 与 `could not take the job lock` |

**行为影响**：
- 变更前：卡死作业永久堵死该打印机。
- 变更后（默认 0）：不变。变更后（启用超时）：超时作业被 RST，客户端立即重试。
- 影响面：启用后故障打印机的客户端开始重试。
- 风险：超时值过小会误伤正常长作业——所以默认关闭。

**确认状态**：- [x] 已确认并实施（2026-10-03，默认值 0）

---

## PD-09 锁文件以 0666 请求

**缺陷 ID**：PD-09　**严重程度**：低　**行为变更**：是　**状态**：已修复（0644 + O_NOFOLLOW）

**位置**：`p910nd.c:679-693`（`get_lock()`）

**根因**：`open(..., 0666)` 的实际权限被 umask 截断，而 `-d` 与 `(x)inetd` 路径完全继承调用方 umask；`open()` 也没有 `O_NOFOLLOW`。

> **修复前实测**：umask 0002 → `664`；umask 000 → **`666`**；umask 022 → `644`（守护化路径内部设了 `umask(022)`，所以正规启动的守护进程本来就不是世界可写——实际暴露需要异常的 umask）。三种权限下非 root 进程都能 `open(O_RDWR)` 并成功 `lockf`。

**修复方案（已实施）**：`open(lockname, O_CREAT | O_RDWR | O_NOFOLLOW, 0644)`。

**修复后实测**：

| 段 | 结果 |
| --- | --- |
| A umask 0002 | `644` |
| A umask 000 | `644`（修复前是 `666`） |
| A umask 022 | `644` |
| B 锁路径是符号链接 | 守护进程拒绝启动，日志点名锁文件；符号链接目标字节数 0（未被当作锁使用） |
| C 静态 | 源码含 `O_CREAT | O_RDWR | O_NOFOLLOW, 0644`；`umask(022)` 仍只有一处且在守护化分支内 |

**行为影响**：
- 变更前：权限由 umask 决定，`umask 0` 下为 `666`。
- 变更后：固定 `0644`，且拒绝符号链接。
- 影响面：仅影响需要跨用户共享该锁文件的部署（正常没有）。
- 风险：若锁文件被故意做成符号链接，守护进程改为失败退出（这是期望行为，但属于策略变化）。

**确认状态**：- [x] 已确认并实施（2026-10-03）

---

## PD-10 `bind()` 全部失败后带着已关闭的 fd 进入 accept

**缺陷 ID**：PD-10　**严重程度**：中　**行为变更**：否　**状态**：已修复

**位置**：`p910nd.c:1936-1988`（`server()`）

**根因**：循环的每条失败路径都 `close(netfd)` 却从不置回 `-1`，循环结束后也没有"是否成功绑定过"的判断，于是全部地址失败时 `accept()` 拿到 `EBADF`。

> **修复前实测**：`bind: Address already in use` 之后紧跟 `accept: Bad file descriptor`；strace 显示 `accept(3, ...) = -1 EBADF`。

**修复方案（已实施）**：四条失败路径都加 `netfd = -1;`（含 `socket()` 失败那条），循环后加 `if (netfd < 0) { dolog(... "no address worked"); exit(1); }`。

**修复后实测**：日志为 `bind: Address already in use` + `cannot listen on 127.0.0.1 port 19100: no address worked`；strace 中**没有任何 `accept(` 调用**；退出码仍为 1。

**行为影响**：无运行期行为变化（启动失败场景），退出码仍为 1，只是日志更准确且不再进入无意义的 accept 循环。

**确认状态**：- [x] 已确认并实施（2026-10-03）

---

## PD-11 节流时间戳归一化用 `>`

**缺陷 ID**：PD-11　**严重程度**：低　**行为变更**：否　**状态**：已修复

**位置**：`p910nd.c:1389-1394`

**根因**：`now.tv_usec == 900000` 且 `PRINTER_READ_PACE_US == 100000` 时和恰好等于 `1000000`，严格 `>` 不触发，`tv_usec` 留在非法值（合法范围 0..999999）且不递增秒。

> **修复前实测**（`pace_probe` 复刻算术）：`900000` 对齐下 `>` 解除了 99 个节拍、`>=` 解除 100 个，差 1 ms；并打印出 `ILLEGAL tv_usec=1000000 survives normalisation`。

**修复方案（已实施）**：`>` 改 `>=`，注释说明理由。

**修复后实测**：`pace_probe` 现在把出货算术与"调用方真正想要的 start+pace"（独立计算）逐个对齐比较，输出 `VERDICT clean`、`out-of-range=0 early=0 late=0`；源码中不再存在严格比较。

**行为影响**：出货配置下最大差异 1 毫秒（一个时钟节拍）；消除了一个非法 `tv_usec`。

**确认状态**：- [x] 已确认并实施（2026-10-03）

---

## PD-12 改写 argv[0] 使 init 脚本停不掉进程

**缺陷 ID**：PD-12　**严重程度**：中低　**行为变更**：是　**状态**：已修复（改 init 脚本，保留改名）

**位置**：`aux/p910nd.init:279-325`（`stop` 分支）、`p910nd.c:2185-2192`（保留未改）

**根因**：`main()` 就地改写 `argv[0]` 的 basename 里的 `n`，`/proc/PID/cmdline` 变成 `p9100d`（man page 当作特性宣传），而 init 脚本用 `killproc -TERM /usr/sbin/p910nd` 按名匹配；`-d` 模式下连 pid 文件都没有。附带：LSP 核实 `progname` 的 10 处引用中 `p910nd.c:2199` 把改名后的指针交给 `openlog()`，所以 syslog ident 也是 `p9100d`。

> **修复前实测**：`pgrep -f '^<path>/p910nd$'` 匹配 **0**，`pgrep -f 'p9100d'` 匹配 **2**。

**修复方案（已实施）**：**不**动 `argv[0]` 改写（那是既有对外行为，man page 有记载）。改 `aux/p910nd.init` 的 `stop`：优先读 `/var/run/p910[0-9]d.pid`，用 `kill -0` 等价手段确认该 pid 仍是我们自己的进程（`grep '^.*p910' /proc/$pid/cmdline`）且文件内容是纯数字，再 `kill -TERM` 并最多等 40 秒，然后 `kill -KILL`；取不到 pid 文件才退回 `killproc`。

**修复后实测**：改名行为保持不变（用例仍断言 `pgrep` 按安装名 0 匹配、按 `p9100d` ≥1 匹配，以证明 init 脚本确实需要另一条路）；init 脚本含 pid 文件查找、纯数字校验、`/proc/$pid/cmdline` 校验、`kill -TERM "$P910ND_PID"` 与 `killproc` 回退；`bash -n` 通过（脚本用 bash 数组语法，`sh -n` 不适用，这是既有属性）。

**行为影响**：
- 变更前：`init.d p910nd stop` 在 SUSE 系上可能杀不掉守护进程。
- 变更后：stop 走 pid 文件，可靠停机。
- 影响面：`stop` / `try-restart` / `force-reload` / `restart`。
- 风险：过期 pid 文件可能误杀无关进程——已加两道校验（纯数字 + `/proc/$pid/cmdline` 含 `p910`）。

**确认状态**：- [x] 已确认并实施（2026-10-03）

---

## PD-13 `lock_held` 只写不读

**缺陷 ID**：PD-13　**严重程度**：低　**行为变更**：否　**状态**：已修复

**位置**：`p910nd.c`（原 360 / 577 / 608 / 624 / 1936）

**根因**：提交 T6 把"退出时删除锁文件"改成"故意不删"，`lock_held` 原本唯一的用途（判断是否由本进程删除锁文件）消失，四处赋值全留下。

> **修复前实测**：`grep -c lock_held` = 5，LSP `findReferences` 确认 1 处声明 + 4 处赋值、**0 处读取**。

**修复方案（已实施）**：删除变量与全部四处赋值；`get_lock()` 的注释改为说明 `lockname` 的用途。

**修复后实测**：`grep -c lock_held p910nd.c` = **0**。

**行为影响**：无（无任何读取点）。

**确认状态**：- [x] 已确认并实施（2026-10-03）

---

## PD-14 文档与实现漂移

**缺陷 ID**：PD-14　**严重程度**：低　**行为变更**：否　**状态**：已修复

**位置**：`p910nd.c:360-361`、`p910nd.8`、`README.md`、`aux/p910nd.spec`、`aux/p910nd.init`

**根因**：`version` 只在 `p910nd.c` 里被 `-v` 使用，四份文档各自手工维护；man page 的 ifilter/ofilter 例子沿用 0.4 时代的历史文件清单；`aux/p910nd.init` 的 RHEL 分支照抄 `status -p` 却去掉了参数。

> **修复前实测**：四份文件四个版本号（`p910nd.c=1.1`、`p910nd.8=1.0`、`README.md=0.97`、`aux/p910nd.spec=0.96`）；`client.pl`、`banner.pl`、`p910nd.sh` 三个文件不存在；12 个构建期旋钮未记录；RHEL 分支 `checkproc() { return status ${1+"$@"}; }` 调用了不存在的命令。

**修复方案（已实施）**：
- 四份文件统一到 **1.1**，并在 `README.md` 写明 `version[]` 是唯一真源；
- `p910nd.8` 删掉三个不存在的文件引用，改用 `lp| -d :P\Ihost\fP:9100` 的标准写法；`p910nd.sh` 改成实际存在的 `p910nd.init` 并说明它按 pid 文件停机；
- 新增 `TUNING AT BUILD TIME` 一节，**21 个旋钮全部记录**（含 `LOCK_WAIT_TIMEOUT`、`PRINTERFILE`、`FAIL_WITH_RST`、`NO_CLOCK_GETTIME` 等）；
- 修 `Descriptors of any number are\nsupported` 的断句；
- 补 `-d` 在 inetd 下日志去向的说明；
- `aux/p910nd.init` 的 RHEL 分支改用 `pidofproc`，无库兜底分支改为诚实的 `rc_failed 4`（unknown）而不是假装进程在跑；
- `aux/p910nd.spec` 版本 1.1 + 1.1 changelog，并注明上游 tarball 停在 0.97、应从 git 检出构建。

**修复后实测**：四份文件一致报 1.1；man page 不再引用缺失文件；21 个旋钮全部有文档；init 脚本无 `return status` 调用。

**行为影响**：无运行期影响。`checkproc` 的修正是脚本行为修正：非 SUSE 系统上 `status` 子命令从"永远成功"变成真实状态（无库兜底时返回 4 = unknown，符合 LSB）。

**确认状态**：- [x] 已确认并实施（2026-10-03）

---

## PD-15 `version[]` / `copyright[]` 是可写数组

**缺陷 ID**：PD-15　**严重程度**：低　**行为变更**：否　**状态**：已修复

**位置**：`p910nd.c:360-361`

**根因**：同文件 342-345 行的 D15 注释刚因同一理由修过 `default_progname`，这两个数组沿用可写存储却无理由。`-Wwrite-strings` 只把字面量 const 化，对已初始化的 `char[]` 无效，编译器不会提醒。

**修复方案（已实施）**：改为 `static const char version[]` / `static const char copyright[]`。

**修复后实测**：`static_checks.py` 报 `version-arrays-const OK`；两处使用点（`usage()`、`show_version()`）都只读，`const` 安全；`gcc -Wwrite-strings -fsyntax-only` 0 诊断。

**行为影响**：无（无任何写入点）。

**确认状态**：- [x] 已确认并实施（2026-10-03）

---

## PD-16 `$(PROG)` 无头文件依赖，CI 只编译不测试

**缺陷 ID**：PD-16　**严重程度**：低　**行为变更**：否　**状态**：已修复

**位置**：`Makefile:37-40,53-59,84-89`、`.github/workflows/CI.yml`

**根因**：`$(PROG): p910nd.c` 只列一个依赖，也没有 `-MMD`。`p910nd.c` 有 26 个 `#include`，改任何一个都不触发重编；因为当前只有一个编译单元，后果被掩盖，但加入第二个 `.c` 文件就会变成难以察觉的错误构建。CI 的 34 个交叉构建只 `make` 然后上传产物。

> **修复前实测**：沙箱构建（加一个真被包含的头文件、改它之后 `make -n`）输出 `make: 'p910nd' is up to date.`；CI 里 2 处 `make`、0 处测试。

**修复方案（已实施）**：
- `override CFLAGS += -MMD -MP` + `-include p910nd.d`（保留 `override` 语义，CI 的命令行 `CFLAGS=` 覆盖仍然有效）；
- 链接规则显式写 `p910nd.c` 而不是 `$^`，否则 `.d` 里的头文件会被当成编译输入生成 `.gch`；
- `clean` 增加 `*.d`；
- CI 新增 `test` job（装 strace/python3/cppcheck/clang-tidy → `make test` → 严格 C89 `-Werror` 构建），`upload-releases` 的 `needs` 加上 `test`，测试不过就不发版。

**修复后实测**：沙箱构建的 `p910nd.d` 里出现 `scratch.h`；`touch scratch.h` 后 `make -n` 计划重编（不再是 "up to date"），且命令行里只有 `p910nd.c`、没有把头文件当输入；`static_checks.py` 四项全 OK。

**行为影响**：
- 变更前：改头文件不触发重编；CI 不做任何检查。
- 变更后：改头文件触发重编；CI 多一个测试与严格构建门禁。
- 影响面：构建与 CI，不影响运行时。
- 风险：CI 现在可能因测试失败而挡住发布——这正是目的，但需要知情。

**确认状态**：- [x] 已确认并实施（2026-10-03）

---

## 行为变更实施结果

全部 16 条已实施。11 条行为变更的最终落地形态：

| # | 缺陷 | 变更前 → 变更后 | 现状 |
| --- | --- | --- | --- |
| 1 | PD-01 严重 | 并发交错（73 次交替）→ 排队（1 次交替） | 已实施 |
| 2 | PD-02 高 | 缺目录 `exit(1)` → 自动创建；仍不可创建则退出 | 已实施（方案 ①） |
| 3 | PD-04 高 | 不服务作业 + 注入 197 字节日志 → 正常服务 + 日志进 syslog | 已实施 |
| 4 | PD-05 中高 | 硬错误等满 60 s（实测 5.87 s）→ 0.42 s 且记录真实原因 | 已实施 |
| 5 | PD-07 中 | 墙钟回拨使全部超时失效 → 只受单调时钟影响 | 已实施（条件编译 + 降级） |
| 6 | PD-03 中 | 未发数据时 FIN / 已发数据时偶然 RST → 统一 RST | 已实施 |
| 7 | PD-06 中低 | 0 读满 20 次即结束（作业中途）→ 仅打印方向结束后生效 | 已实施（方案 ①） |
| 8 | PD-08 中 | 永久排队 → 默认不变，`-DLOCK_WAIT_TIMEOUT=n` 可设上界 | 已实施（默认 0） |
| 9 | PD-09 低 | 权限随 umask（000 时 666）→ 固定 0644 + 拒绝符号链接 | 已实施 |
| 10 | PD-12 中低 | `killproc` 匹配不到 → 按 pid 文件停机（含两道校验） | 已实施（保留改名） |
| 11 | PD-10/11/13/14/15/16 | 无运行期变化 | 已实施 |

## 已核实无问题清单

以下点本次逐条核对过，确认正确，未作改动：

- **固定缓冲尺寸全部够用**：`pidfilename[21]` vs 展开 20、`lockname[25]` vs 24、`lpname[13]` vs 9、`service[16]`、`host[INET6_ADDRSTRLEN]`。
- **全部 `dolog()` 格式串与实参类型逐条核对**，无 `-Wformat` 问题。
- **环形缓冲区不变量自洽**；`totalin`/`totalout` 用无符号差值判断未送达量，跨回绕精确。
- **作业成败判定只看打印方向**，客户端 hangup 不会被误判为失败，不会造成 CUPS 重复打印。
- **`getaddrinfo` 路径无泄漏**，`clientlen` 每次 accept 前重置。
- **fork 前 `fflush(NULL)`** 与子进程 `_exit` 前的 flush 正确。
- **`record_child()` 与 SIGCHLD 阻塞配合正确**，`waitpid()` 只在 handler 内调用。
- **信号处理全部走 `sigaction`**，`SIGTERM`/`SIGINT` 不带 `SA_RESTART`。
- **`take_lock()` 的 `l_pid`** 对 `fcntl` 记录锁是忽略字段，无害。
- **锁文件不 unlink 的决定是对的**（T6），未改动。
- **`printer_open_is_permanent()` 的分类与预算钳制逻辑**正确。

## 回归规则

任何后续改动都必须满足：

```sh
make test                     # 17/17 通过，且约 187 秒内完成
sh tests/mutation-check.sh    # 三个变异必须全部被捕获
make && make CC=clang          # 两者 0 警告
```

静态分析基线（PD-00）已把"诊断总数必须为 0"也纳入断言，新增警告会直接让 CI 失败。
