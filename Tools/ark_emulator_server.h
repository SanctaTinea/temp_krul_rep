#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "krul.h"
#include "krul_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARK_EMULATOR_FRAME_SIZE (10U * 1024U)

typedef struct ark_emulator_server ark_emulator_server_t;

/* Creates one stateful ARK Krul v3 emulator. */
ark_emulator_server_t* ark_emulator_server_create(void);

/* Releases the emulator and all lazily allocated memory images. */
void ark_emulator_server_destroy(ark_emulator_server_t* emulator);

/* Returns the production JSON Krul server used to dispatch decoded payloads. */
krul_server_t* ark_emulator_server_krul(ark_emulator_server_t* emulator);

/* Returns the server whose serde codec matches a transport frame magic. */
krul_server_t* ark_emulator_server_krul_for_format(
    ark_emulator_server_t* emulator, krul_transport_format_t format);

#ifdef __cplusplus
}
#endif
