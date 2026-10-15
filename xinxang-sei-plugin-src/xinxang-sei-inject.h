#pragma once

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xinxang_sei_config {
	bool enabled;
	bool only_rtmp_output;
	/* 固定缓冲：编码线程只读这份副本，保存配置时不再换掉指针。 */
	char client_name[257];
};

void xinxang_sei_inject(obs_output_t *output, struct encoder_packet *pkt, struct encoder_packet_time *pkt_time,
			void *param);

#ifdef __cplusplus
}
#endif

