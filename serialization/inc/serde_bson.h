#pragma once

/**
 * @file serde_bson.h
 * @brief Heap-free BSON implementation of the format-neutral serde API.
 *
 * Decoded nodes borrow the input buffer.  The buffer must remain unchanged
 * until the next call to serde_decode() for the same codec instance.
 */

#include "serde.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup serde_bson BSON codec
 * @ingroup serde
 * @{ */

/** Mutable state of one BSON codec instance. */
typedef struct {
    const uint8_t* input;
    size_t input_size;
    bool initialized;
} serde_bson_codec_t;

/** Initialize a BSON codec instance. */
bool serde_bson_init(serde_bson_codec_t* bson);

/** Return the BSON codec through the common serde interface. */
serde_codec_t serde_bson(serde_bson_codec_t* bson);

/** @} */

#ifdef __cplusplus
}
#endif
