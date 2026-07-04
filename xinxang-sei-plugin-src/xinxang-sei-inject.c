#include "xinxang-sei-inject.h"

#include "xinxang-sei-internal.h"

#include <obs-module.h>
#include <util/platform.h>

#include <time.h>
#include <string.h>

static inline void write_u32le(uint8_t *dst, uint32_t v)
{
	dst[0] = (uint8_t)(v & 0xFF);
	dst[1] = (uint8_t)((v >> 8) & 0xFF);
	dst[2] = (uint8_t)((v >> 16) & 0xFF);
	dst[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline void write_u64le(uint8_t *dst, uint64_t v)
{
	dst[0] = (uint8_t)(v & 0xFF);
	dst[1] = (uint8_t)((v >> 8) & 0xFF);
	dst[2] = (uint8_t)((v >> 16) & 0xFF);
	dst[3] = (uint8_t)((v >> 24) & 0xFF);
	dst[4] = (uint8_t)((v >> 32) & 0xFF);
	dst[5] = (uint8_t)((v >> 40) & 0xFF);
	dst[6] = (uint8_t)((v >> 48) & 0xFF);
	dst[7] = (uint8_t)((v >> 56) & 0xFF);
}

size_t xinxang_sei_build_payload(uint8_t *dst, size_t dst_size, int64_t fsts_ms, const char *client_name)
{
	if (!dst || !client_name)
		return 0;

	const size_t name_len_raw = strlen(client_name);
	const size_t name_len = name_len_raw > 256 ? 256 : name_len_raw;

	const uint32_t fsts_block_size = 4 + 8;                    // flag + int64
	const uint32_t fscn_block_size = 4 + (uint32_t)name_len + 1; // flag + name + '\0'

	const size_t payload_size = XINXANG_SEI_UUID_SIZE + 4 + fsts_block_size + 4 + fscn_block_size;
	if (payload_size > dst_size)
		return 0;

	size_t off = 0;
	memcpy(dst + off, xinxang_sei_uuid, XINXANG_SEI_UUID_SIZE);
	off += XINXANG_SEI_UUID_SIZE;

	// FSTS
	write_u32le(dst + off, fsts_block_size);
	off += 4;
	memcpy(dst + off, "FSTS", 4);
	off += 4;
	write_u64le(dst + off, (uint64_t)fsts_ms);
	off += 8;

	// FSCN
	write_u32le(dst + off, fscn_block_size);
	off += 4;
	memcpy(dst + off, "FSCN", 4);
	off += 4;
	memcpy(dst + off, client_name, name_len);
	off += name_len;
	dst[off++] = 0;

	return off;
}

static int64_t cts_to_epoch_ms(uint64_t cts_ns)
{
	struct timespec ts;
	if (!os_nstime_to_timespec(cts_ns, &ts))
		return -1;

	const int64_t sec = (int64_t)ts.tv_sec;
	const int64_t nsec = (int64_t)ts.tv_nsec;
	return sec * 1000 + nsec / 1000000;
}

static const uint8_t nal_start[4] = {0, 0, 0, 1};

static size_t write_sei_type_and_size(uint8_t *dst, size_t dst_size, uint32_t payload_type, uint32_t payload_size)
{
	size_t off = 0;

	while (payload_type >= 0xFF) {
		if (off + 1 > dst_size)
			return 0;
		dst[off++] = 0xFF;
		payload_type -= 0xFF;
	}
	if (off + 1 > dst_size)
		return 0;
	dst[off++] = (uint8_t)payload_type;

	while (payload_size >= 0xFF) {
		if (off + 1 > dst_size)
			return 0;
		dst[off++] = 0xFF;
		payload_size -= 0xFF;
	}
	if (off + 1 > dst_size)
		return 0;
	dst[off++] = (uint8_t)payload_size;

	return off;
}

static size_t rbsp_to_ebsp(uint8_t *dst, size_t dst_size, const uint8_t *rbsp, size_t rbsp_size)
{
	size_t off = 0;
	int zero_count = 0;

	for (size_t i = 0; i < rbsp_size; i++) {
		const uint8_t b = rbsp[i];

		if (zero_count >= 2 && b <= 0x03) {
			if (off + 1 > dst_size)
				return 0;
			dst[off++] = 0x03;
			zero_count = 0;
		}

		if (off + 1 > dst_size)
			return 0;
		dst[off++] = b;

		if (b == 0x00)
			zero_count++;
		else
			zero_count = 0;
	}

	return off;
}

void xinxang_sei_inject(obs_output_t *output, struct encoder_packet *pkt, struct encoder_packet_time *pkt_time,
			void *param)
{
	if (!pkt || pkt->type != OBS_ENCODER_VIDEO)
		return;
	if (!pkt_time)
		return;

	const struct xinxang_sei_config *cfg = (const struct xinxang_sei_config *)param;
	if (!cfg || !cfg->enabled)
		return;

	if (cfg->only_rtmp_output) {
		const char *out_id = output ? obs_output_get_id(output) : NULL;
		if (!out_id || strcmp(out_id, "rtmp_output") != 0)
			return;
	}

	const char *client_name = cfg->client_name;
	if (!client_name || !*client_name)
		return;

	const char *codec = obs_encoder_get_codec(pkt->encoder);
	const bool avc = codec && strcmp(codec, "h264") == 0;
	const bool hevc = codec && strcmp(codec, "hevc") == 0;
	if (!avc && !hevc)
		return;

	const int64_t fsts_ms = cts_to_epoch_ms(pkt_time->cts);
	if (fsts_ms < 0)
		return;

	uint8_t payload[512];
	const size_t payload_size = xinxang_sei_build_payload(payload, sizeof(payload), fsts_ms, client_name);
	if (!payload_size)
		return;

	/* Build SEI RBSP for user_data_unregistered (payload type 5) */
	uint8_t rbsp[1024];
	size_t rbsp_off = 0;

	const size_t hdr = write_sei_type_and_size(rbsp, sizeof(rbsp), 5, (uint32_t)payload_size);
	if (!hdr)
		return;
	rbsp_off += hdr;

	if (rbsp_off + payload_size + 1 > sizeof(rbsp))
		return;
	memcpy(rbsp + rbsp_off, payload, payload_size);
	rbsp_off += payload_size;

	/* rbsp_trailing_bits */
	rbsp[rbsp_off++] = 0x80;

	uint8_t ebsp[2048];
	const size_t ebsp_size = rbsp_to_ebsp(ebsp, sizeof(ebsp), rbsp, rbsp_off);
	if (!ebsp_size)
		return;

#ifdef ENABLE_HEVC
	uint8_t hevc_nal_header[2];
	if (hevc) {
		size_t nal_header_index_start = 4;
		if (memcmp(pkt->data, nal_start + 1, 3) == 0) {
			nal_header_index_start = 3;
		} else if (memcmp(pkt->data, nal_start, 4) == 0) {
			nal_header_index_start = 4;
		} else {
			return;
		}
		hevc_nal_header[0] = pkt->data[nal_header_index_start];
		hevc_nal_header[1] = pkt->data[nal_header_index_start + 1];
	}
#endif

	struct encoder_packet backup = *pkt;

	long ref = 1;
	size_t extra = 0;
	if (avc) {
		extra = 4 /* start code */ + 1 /* nal header */ + ebsp_size;
#ifdef ENABLE_HEVC
	} else if (hevc) {
		extra = 3 /* start code */ + 2 /* nal header */ + ebsp_size;
#endif
	} else {
		return;
	}

	const size_t total = sizeof(ref) + pkt->size + extra;
	uint8_t *buf = bmalloc(total);
	if (!buf)
		return;

	size_t off = 0;
	memcpy(buf + off, &ref, sizeof(ref));
	off += sizeof(ref);

	if (avc) {
		memcpy(buf + off, nal_start, 4);
		off += 4;
		buf[off++] = 0x06; /* SEI NAL unit type */
		memcpy(buf + off, ebsp, ebsp_size);
		off += ebsp_size;
		memcpy(buf + off, pkt->data, pkt->size);
		off += pkt->size;
#ifdef ENABLE_HEVC
	} else if (hevc) {
		memcpy(buf + off, nal_start + 1, 3);
		off += 3;
		const uint8_t prefix_sei_nal_type = 39;
		hevc_nal_header[0] = (prefix_sei_nal_type << 1) | (0x01 & hevc_nal_header[0]);
		memcpy(buf + off, hevc_nal_header, 2);
		off += 2;
		memcpy(buf + off, ebsp, ebsp_size);
		off += ebsp_size;
		memcpy(buf + off, pkt->data, pkt->size);
		off += pkt->size;
#endif
	}

	obs_encoder_packet_release(pkt);

	*pkt = backup;
	pkt->data = buf + sizeof(ref);
	pkt->size = off - sizeof(ref);
}

