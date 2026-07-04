#pragma once

#include <stddef.h>
#include <stdint.h>

#define XINXANG_SEI_UUID_SIZE 16

static const uint8_t xinxang_sei_uuid[XINXANG_SEI_UUID_SIZE] = {
	0x05, 0x17, 0x0D, 0xB4, 0xE9, 0xAB, 0x44, 0xB0, 0xB4, 0x1C, 0xCC, 0x8E, 0x4F, 0xE2, 0xF9, 0xC6,
};

size_t xinxang_sei_build_payload(uint8_t *dst, size_t dst_size, int64_t fsts_ms, const char *client_name);

