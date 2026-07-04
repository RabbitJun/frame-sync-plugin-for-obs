#pragma once

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xinxang_sei_config {
	bool enabled;
	bool only_rtmp_output;
	const char *client_name;
};

void xinxang_sei_inject(obs_output_t *output, struct encoder_packet *pkt, struct encoder_packet_time *pkt_time,
			void *param);

#ifdef __cplusplus
}
#endif

