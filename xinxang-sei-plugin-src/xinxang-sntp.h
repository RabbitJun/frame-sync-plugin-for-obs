#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SNTP 校时（与播放端 xinxang-sync、sinsam 用同一时间参考）。
 *
 * 为什么推流端也要校时：
 *   SEI 里的 FSTS 是「推流端机器时钟」写下的绝对 Unix 毫秒。
 *   1) 多台 OBS 各推一路时，机器之间的时钟差会**直接**变成画面错位；
 *   2) 即便单机推流，若本机时钟偏 E 毫秒，播放端要么多等 E 毫秒（推流端快），
 *      要么缓冲被吃光、抖动直接穿透（推流端慢）——同步等于失效。
 *   Windows 自带的 w32time 精度很差（实测本机偏 4.2 秒），不能指望它。
 *
 * 关键约束：**FSTS 不能跳变**。播放端有 200ms 的时间戳回退保护，
 * FSTS 突然跳一下会让那一批帧被全部丢弃（表现为整段卡顿）。
 * 因此除首次校时外，所有修正都按有限速率**缓变**过去。
 */

/* 启动后台校时线程。server 如 "ntp.tencent.com"，interval_s 为重同步间隔。 */
bool xinxang_sntp_start(const char *server, int interval_s);

/* 停止后台线程（进程退出/插件卸载时调用）。 */
void xinxang_sntp_stop(void);

/* 当前生效的时钟偏移（毫秒）。FSTS = 本机时钟 + 该值。 */
int64_t xinxang_sntp_offset_ms(void);

/* 把本机 Unix 毫秒校正为参考时间轴的毫秒（写 FSTS 用这个）。 */
int64_t xinxang_sntp_correct_ms(int64_t local_ms);

/* 状态查询：offset = 当前生效偏移，target = 最近一次测得的偏移，
 * rtt = 最近一次往返时延，age_ms = 距上次成功校时多久。
 * 从未成功校时返回 false。 */
bool xinxang_sntp_status(int64_t *offset, int64_t *target, int64_t *rtt, int64_t *age_ms);

/* 立即触发一次校时（供 UI「立即校时」按钮用）。 */
void xinxang_sntp_sync_now(void);

/* 阻塞查询一次（供测试用）。成功返回 true 并填 offset/rtt。 */
bool xinxang_sntp_query_once(const char *server, int64_t *out_offset, int64_t *out_rtt);

#ifdef __cplusplus
}
#endif
