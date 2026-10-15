/*
 * SNTP 校时实现（跨平台：Winsock / POSIX）。
 * 设计要点见 xinxang-sntp.h 顶部注释——核心是「缓变」，绝不让 FSTS 跳变。
 */

#include "xinxang-sntp.h"

/* obs-module.h 带进 util/base.h（blog / LOG_INFO / LOG_WARNING） */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdlib.h>
#include <string.h>
#include <time.h> /* timespec_get / TIME_UTC（真实纪元时间源） */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET xxsock;
#define XX_CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int xxsock;
#define XX_CLOSESOCK close
#define INVALID_SOCKET (-1)
#endif

#define NTP_UNIX_DELTA 2208988800LL
#define NTP_PORT "123"
#define SNTP_TIMEOUT_MS 3000
/* 缓变速率：每 50ms 最多挪 5ms —— 即每秒最多 100ms。
   25fps 下每帧 FSTS 变化 ≤ 4ms，远低于播放端 200ms 的回退保护阈值。 */
#define SLEW_TICK_MS 50
#define SLEW_STEP_MS 5

static volatile bool sntp_running = false;
static volatile bool sntp_need_query = false;
/* OBS 32.x 已移除 os_thread_*，直接用 pthread（Windows 上走 w32-pthreads） */
static pthread_t sntp_thread_id;
static pthread_mutex_t sntp_lock;

/* 以下四个量都用锁保护（校时间隔很长，锁竞争可忽略） */
static int64_t sntp_applied_offset = 0; /* 当前生效 */
static int64_t sntp_target_offset = 0;  /* 最近测得 */
static int64_t sntp_last_rtt = 0;
static int64_t sntp_last_sync_ms = 0;
static int sntp_interval_s = 600;
static char sntp_server[256] = "ntp.tencent.com";

static int64_t now_ms(void)
{
	/* 注意：OBS 的 os_gettime_ns() 在 Windows 上是 QueryPerformanceCounter
	 * 换算的「开机以来」单调时间，不是 Unix 纪元！SNTP 的时间戳必须是
	 * 真实纪元毫秒，否则 offset = 纪元 - 开机时间 ≈ 1.79e12，会被合理性
	 * 检查拒收。这里用 C11 timespec_get(TIME_UTC) 取真实纪元。 */
	struct timespec ts;
	if (timespec_get(&ts, TIME_UTC) == TIME_UTC)
		return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	return 0;
}

static int64_t net_last_error(void)
{
#ifdef _WIN32
	return (int64_t)WSAGetLastError();
#else
	return (int64_t)errno;
#endif
}

static void lock(void)
{
	pthread_mutex_lock(&sntp_lock);
}
static void unlock(void)
{
	pthread_mutex_unlock(&sntp_lock);
}

/* ---- NTP 时间戳 <-> Unix 毫秒 ---- */
static void ms_to_ntp(int64_t ms, uint32_t *sec, uint32_t *frac)
{
	*sec = (uint32_t)(ms / 1000 + NTP_UNIX_DELTA);
	const int64_t rem = ms % 1000;
	*frac = (uint32_t)((rem * (int64_t)4294967296LL) / 1000);
}

static int64_t ntp_to_ms(uint32_t sec, uint32_t frac)
{
	return ((int64_t)sec - NTP_UNIX_DELTA) * 1000 + ((int64_t)frac * 1000) / (int64_t)4294967296LL;
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static bool net_init(void)
{
#ifdef _WIN32
	WSADATA wsa;
	return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
	return true;
#endif
}

static void net_cleanup(void)
{
#ifdef _WIN32
	WSACleanup();
#endif
}

/* 单次 SNTP 查询。返回 offset/rtt（毫秒）。 */
bool xinxang_sntp_query_once(const char *server, int64_t *out_offset, int64_t *out_rtt)
{
	if (!server || !*server || !net_init())
		return false;

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	struct addrinfo *res = NULL;
	if (getaddrinfo(server, NTP_PORT, &hints, &res) != 0 || !res) {
		net_cleanup();
		return false;
	}

	bool ok = false;
	for (struct addrinfo *ai = res; ai && !ok; ai = ai->ai_next) {
		xxsock s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == INVALID_SOCKET) {
			blog(LOG_WARNING, "[xinxang-sei] SNTP socket() 失败: %lld", net_last_error());
			continue;
		}

		/* 接收超时：Windows 用毫秒 DWORD，POSIX 用 timeval */
#ifdef _WIN32
		DWORD tv = SNTP_TIMEOUT_MS;
#else
		struct timeval tv;
		tv.tv_sec = SNTP_TIMEOUT_MS / 1000;
		tv.tv_usec = (SNTP_TIMEOUT_MS % 1000) * 1000;
#endif
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

		uint8_t pkt[48];
		memset(pkt, 0, sizeof(pkt));
		pkt[0] = 0x1B; /* LI=0, VN=3, Mode=3(client) */

		const int64_t t1 = now_ms();
		uint32_t sec1, frac1;
		ms_to_ntp(t1, &sec1, &frac1);
		put_be32(pkt + 40, sec1);
		put_be32(pkt + 44, frac1);

		if (sendto(s, (const char *)pkt, 48, 0, ai->ai_addr, (int)ai->ai_addrlen) >= 0) {
			uint8_t resp[48];
			int n = (int)recvfrom(s, (char *)resp, sizeof(resp), 0, NULL, NULL);
			const int64_t t4 = now_ms();
			if (n >= 48) {
				const uint8_t li = resp[0] >> 6;
				const uint8_t mode = resp[0] & 0x07;
				if (li != 3 && (mode == 4 || mode == 5)) {
					const int64_t t2 = ntp_to_ms(be32(resp + 32), be32(resp + 36));
					const int64_t t3 = ntp_to_ms(be32(resp + 40), be32(resp + 44));
					const int64_t offset = ((t2 - t1) + (t3 - t4)) / 2;
					const int64_t rtt = (t4 - t1) - (t3 - t2);
					/* 合理性检查：RTT 0~10s，偏移不超过 24 小时 */
					if (rtt >= 0 && rtt < 10000 && offset > -86400000LL && offset < 86400000LL) {
						if (out_offset)
							*out_offset = offset;
						if (out_rtt)
							*out_rtt = rtt;
						ok = true;
					} else {
						blog(LOG_WARNING,
						     "[xinxang-sei] SNTP 应答异常被拒：offset=%lldms rtt=%lldms（t1=%lld t2=%lld t3=%lld t4=%lld）",
						     (long long)offset, (long long)rtt, (long long)t1,
						     (long long)t2, (long long)t3, (long long)t4);
					}
				} else {
					blog(LOG_WARNING, "[xinxang-sei] SNTP 应答被拒：LI=%u Mode=%u（非服务器应答或未同步）",
					     (unsigned)li, (unsigned)mode);
				}
			} else {
				blog(LOG_WARNING, "[xinxang-sei] SNTP 接收失败/超时: 收到 %d 字节，errno %lld", n,
				     net_last_error());
			}
		} else {
			blog(LOG_WARNING, "[xinxang-sei] SNTP sendto 失败: %lld", net_last_error());
		}
		XX_CLOSESOCK(s);
	}

	freeaddrinfo(res);
	net_cleanup();
	return ok;
}

/* ---- 后台线程：周期查询 + 缓变 ---- */
static void *sntp_thread(void *unused)
{
	(void)unused;

	bool first_sync_done = false;
	int backoff_s = 5;
	int64_t next_query_ms = 0;

	while (sntp_running) {
		/* 1) 需要查询就查一次 */
		if (sntp_need_query || now_ms() >= next_query_ms) {
			sntp_need_query = false;
			int64_t off = 0, rtt = 0;
			char server[256];
			lock();
			strncpy(server, sntp_server, sizeof(server) - 1);
			server[sizeof(server) - 1] = 0;
			unlock();

			if (xinxang_sntp_query_once(server, &off, &rtt)) {
				lock();
				sntp_target_offset = off;
				sntp_last_rtt = rtt;
				sntp_last_sync_ms = now_ms();
				if (!first_sync_done) {
					/* 首次：直接采用（发生在插件加载时，尚未推流，跳变无副作用） */
					sntp_applied_offset = off;
					first_sync_done = true;
					unlock();
					blog(LOG_INFO, "[xinxang-sei] SNTP 首次校时成功：%s 偏移 %lldms（RTT %lldms），已立即采用",
					     server, (long long)off, (long long)rtt);
				} else {
					const int64_t delta = off - sntp_applied_offset;
					unlock();
					if (delta != 0)
						blog(LOG_INFO,
						     "[xinxang-sei] SNTP 校时：目标偏移 %lldms，与当前的差 %lldms，开始缓变（≤%dms/秒）",
						     (long long)off, (long long)delta, SLEW_STEP_MS * (1000 / SLEW_TICK_MS));
				}
				backoff_s = 5;
				int interval = 600;
				lock();
				interval = sntp_interval_s;
				unlock();
				next_query_ms = now_ms() + (int64_t)interval * 1000;
			} else {
				blog(LOG_WARNING, "[xinxang-sei] SNTP 校时失败（%s），%d 秒后重试；未校时期间 FSTS 用本机时钟",
				     server, backoff_s);
				next_query_ms = now_ms() + (int64_t)backoff_s * 1000;
				backoff_s = backoff_s * 2 > 60 ? 60 : backoff_s * 2;
			}
		}

		/* 2) 缓变：每 50ms 最多挪 5ms */
		lock();
		if (sntp_applied_offset != sntp_target_offset) {
			const int64_t diff = sntp_target_offset - sntp_applied_offset;
			const int64_t step = diff > 0 ? (diff < SLEW_STEP_MS ? diff : SLEW_STEP_MS)
					     : (diff > -SLEW_STEP_MS ? diff : -SLEW_STEP_MS);
			sntp_applied_offset += step;
		}
		unlock();

		os_sleep_ms(SLEW_TICK_MS);
	}

	return NULL;
}

bool xinxang_sntp_start(const char *server, int interval_s)
{
	if (sntp_running)
		return true;

	if (server && *server) {
		strncpy(sntp_server, server, sizeof(sntp_server) - 1);
		sntp_server[sizeof(sntp_server) - 1] = 0;
	}
	if (interval_s >= 10)
		sntp_interval_s = interval_s;

	pthread_mutex_init(&sntp_lock, NULL);
	sntp_running = true;
	sntp_need_query = true; /* 启动即查一次 */

	if (pthread_create(&sntp_thread_id, NULL, sntp_thread, NULL) != 0) {
		sntp_running = false;
		pthread_mutex_destroy(&sntp_lock);
		return false;
	}
	blog(LOG_INFO, "[xinxang-sei] SNTP 校时已启动：服务器 %s，间隔 %d 秒", sntp_server, sntp_interval_s);
	return true;
}

void xinxang_sntp_stop(void)
{
	if (!sntp_running)
		return;
	sntp_running = false;
	pthread_join(sntp_thread_id, NULL);
	pthread_mutex_destroy(&sntp_lock);
}

int64_t xinxang_sntp_offset_ms(void)
{
	if (!sntp_running)
		return 0;
	lock();
	const int64_t v = sntp_applied_offset;
	unlock();
	return v;
}

int64_t xinxang_sntp_correct_ms(int64_t local_ms)
{
	return local_ms + xinxang_sntp_offset_ms();
}

bool xinxang_sntp_status(int64_t *offset, int64_t *target, int64_t *rtt, int64_t *age_ms)
{
	if (!sntp_running)
		return false;
	lock();
	if (sntp_last_sync_ms == 0) {
		unlock();
		return false;
	}
	if (offset)
		*offset = sntp_applied_offset;
	if (target)
		*target = sntp_target_offset;
	if (rtt)
		*rtt = sntp_last_rtt;
	if (age_ms)
		*age_ms = now_ms() - sntp_last_sync_ms;
	unlock();
	return true;
}

void xinxang_sntp_sync_now(void)
{
	sntp_need_query = true;
}
