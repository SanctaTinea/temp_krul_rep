#pragma once

/**
 * @file serde_cbor.h
 * @brief Heap-free CBOR codec with numeric map keys for the serde API.
 *
 * Decoded nodes borrow the input buffer.  The writer deliberately emits
 * indefinite-length maps and arrays; scalar values use their shortest normal
 * representation except that f32 is always encoded as IEEE-754 binary32.
 */

#include "serde.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t* input;
    size_t input_size;
    bool initialized;
} serde_cbor_codec_t;

bool serde_cbor_init(serde_cbor_codec_t* cbor);
serde_codec_t serde_cbor(serde_cbor_codec_t* cbor);

#ifdef __cplusplus
}
#endif
