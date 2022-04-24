/* packet-quic.h
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/**
 * Metadata for a STREAM frame.
 * https://tools.ietf.org/html/draft-ietf-quic-transport-23#section-19.8
 */
typedef struct _quic_stream_info {
    guint64     stream_id;      /**< 62-bit Stream ID. */
    guint64     stream_offset;  /**< 62-bit stream offset. */
    guint32     offset;         /**< Offset within the stream (different for reassembled data). */
    struct quic_info_data *quic_info;    /**< Opaque data structure to find the QUIC session. */
    gboolean    from_server;
} quic_stream_info;

/*
 * Although the QUIC SCID/DCID length field can store at most 255, v1 limits the
 * CID length to 20.
 */
#define QUIC_MAX_CID_LENGTH  20

typedef struct quic_cid {
    guint8      len;
    guint8      cid[QUIC_MAX_CID_LENGTH];
} quic_cid_t;

struct tcp_multisegment_pdu {
	guint32 seq;
	guint32 nxtpdu;
	guint32 first_frame;            /* The frame where this MSP was created (used as key in reassembly tables). */
	guint32 last_frame;
	nstime_t last_frame_time;
	guint32 first_frame_with_seq;   /* The frame that contains the first frame that matches 'seq'
					   (same as 'first_frame', larger than 'first_frame' for OoO segments) */
	guint32 flags;
#define MSP_FLAGS_REASSEMBLE_ENTIRE_SEGMENT	0x00000001
/* Whether this MSP is finished and no more segments can be added. */
#define MSP_FLAGS_GOT_ALL_SEGMENTS		0x00000002
/* Whether the first segment of this MSP was not yet seen. */
#define MSP_FLAGS_MISSING_FIRST_SEGMENT		0x00000004
};

/**
 * Obtain Stream Type from a Stream ID.
 * https://tools.ietf.org/html/draft-ietf-quic-transport-23#section-2.1
 */
#define QUIC_STREAM_TYPE(stream_id) ((stream_id) & 3U)
#define QUIC_STREAM_CLIENT_BIDI 0
#define QUIC_STREAM_SERVER_BIDI 1
#define QUIC_STREAM_CLIENT_UNI  2
#define QUIC_STREAM_SERVER_UNI  3

