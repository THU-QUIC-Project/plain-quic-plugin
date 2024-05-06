/* packet-quic.c
 * Routines for QUIC (IETF) dissection
 * Copyright 2017, Alexis La Goutte <alexis.lagoutte at gmail dot com>
 * Copyright 2018 Peter Wu <peter@lekensteyn.nl>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * See https://quicwg.org
 * RFC9000 QUIC: A UDP-Based Multiplexed and Secure Transport
 * RFC9001 Using TLS to Secure QUIC
 * RFC8889 Version-Independent Properties of QUIC
 * https://tools.ietf.org/html/draft-ietf-quic-version-negotiation-03
 *
 * Extension:
 * https://tools.ietf.org/html/draft-ferrieuxhamchaoui-quic-lossbits-03
 * https://datatracker.ietf.org/doc/html/draft-ietf-quic-datagram-06
 * https://tools.ietf.org/html/draft-huitema-quic-ts-02
 * https://tools.ietf.org/html/draft-iyengar-quic-delayed-ack-00
 * https://tools.ietf.org/html/draft-deconinck-quic-multipath-06
 *
 * Currently supported QUIC version(s): draft-21, draft-22, draft-23, draft-24,
 * draft-25, draft-26, draft-27, draft-28, draft-29, draft-30, draft-31, draft-32,
 * draft-33
 * For a table of supported QUIC versions per Wireshark version, see
 * https://github.com/quicwg/base-drafts/wiki/Tools#wireshark
 *
 * Decryption is supported via TLS 1.3 secrets in the "TLS Key Log File",
 * configured either at the TLS Protocol preferences, or embedded in a pcapng
 * file. Sample captures and secrets can be found at:
 * https://gitlab.com/wireshark/wireshark/-/issues/13881
 *
 * Limitations:
 * - STREAM offsets larger than 32-bit are unsupported.
 * - STREAM with sizes larger than 32 bit are unsupported. STREAM sizes can be
 *   up to 62 bit in QUIC, but the TVB and reassembly API is limited to 32 bit.
 * - Out-of-order and overlapping STREAM frame data is not handled.
 * - "Follow QUIC Stream" doesn't work with STREAM IDs larger than 32 bit
 */
#include <config.h>

#include <epan/conversation.h>
#include <epan/tvbuff.h>
#include <epan/packet.h>
#include <epan/expert.h>
#include <epan/proto_data.h>
#include <epan/to_str.h>
#include <epan/reassemble.h>
#include <epan/prefs.h>
#include <wsutil/pint.h>

#include <epan/tap.h>
#include <epan/follow.h>
#include <epan/addr_resolv.h>

#include "packet-plain-quic.h"

/* Prototypes */
void proto_reg_handoff_quic_plain(void);
void proto_register_quic_plain(void);

static int quic_plain_follow_tap = -1;

/* Initialize the protocol and registered fields */
static int proto_quic_plain = -1;
static int hf_quic_connection_number = -1;
static int hf_quic_packet_length = -1;
static int hf_quic_header_form = -1;
static int hf_quic_long_packet_type = -1;
static int hf_quic_long_reserved = -1;
static int hf_quic_packet_number_length = -1;
static int hf_quic_dcid = -1;
static int hf_quic_scid = -1;
static int hf_quic_dcil = -1;
static int hf_quic_scil = -1;
static int hf_quic_token_length = -1;
static int hf_quic_token = -1;
static int hf_quic_length = -1;
static int hf_quic_packet_number = -1;
static int hf_quic_version = -1;
static int hf_quic_supported_version = -1;
static int hf_quic_vn_unused = -1;
static int hf_quic_short = -1;
static int hf_quic_fixed_bit = -1;
static int hf_quic_spin_bit = -1;
static int hf_quic_short_reserved = -1;
static int hf_quic_q_bit = -1;
static int hf_quic_l_bit = -1;
static int hf_quic_key_phase = -1;
static int hf_quic_payload = -1;
static int hf_quic_protected_payload = -1;
static int hf_quic_remaining_payload = -1;
static int hf_quic_odcil = -1;
static int hf_quic_odcid = -1;
static int hf_quic_retry_token = -1;
static int hf_quic_retry_integrity_tag = -1;

static int hf_quic_frame = -1;
static int hf_quic_frame_type = -1;

static int hf_quic_padding_length = -1;
static int hf_quic_ack_largest_acknowledged = -1;
static int hf_quic_ack_ack_delay = -1;
static int hf_quic_ack_ack_range_count = -1;
static int hf_quic_ack_first_ack_range = -1;
static int hf_quic_ack_gap = -1;
static int hf_quic_ack_ack_range = -1;
static int hf_quic_ack_ect0_count = -1;
static int hf_quic_ack_ect1_count = -1;
static int hf_quic_ack_ecn_ce_count = -1;
static int hf_quic_rsts_stream_id = -1;
static int hf_quic_rsts_application_error_code = -1;
static int hf_quic_rsts_final_size = -1;
static int hf_quic_ss_stream_id = -1;
static int hf_quic_ss_application_error_code = -1;
static int hf_quic_crypto_offset = -1;
static int hf_quic_crypto_length = -1;
static int hf_quic_crypto_crypto_data = -1;
static int hf_quic_nt_length = -1;
static int hf_quic_nt_token = -1;
static int hf_quic_stream_fin = -1;
static int hf_quic_stream_len = -1;
static int hf_quic_stream_off = -1;
static int hf_quic_stream_stream_id = -1;
static int hf_quic_stream_initiator = -1;
static int hf_quic_stream_direction = -1;
static int hf_quic_stream_offset = -1;
static int hf_quic_stream_length = -1;
static int hf_quic_stream_data = -1;
static int hf_quic_md_maximum_data = -1;
static int hf_quic_msd_stream_id = -1;
static int hf_quic_msd_maximum_stream_data = -1;
static int hf_quic_ms_max_streams = -1;
static int hf_quic_db_stream_data_limit = -1;
static int hf_quic_sdb_stream_id = -1;
static int hf_quic_sdb_stream_data_limit = -1;
static int hf_quic_sb_stream_limit = -1;
static int hf_quic_nci_retire_prior_to = -1;
static int hf_quic_nci_sequence = -1;
static int hf_quic_nci_connection_id_length = -1;
static int hf_quic_nci_connection_id = -1;
static int hf_quic_nci_stateless_reset_token = -1;
static int hf_quic_rci_sequence = -1;
static int hf_quic_path_challenge_data = -1;
static int hf_quic_path_response_data = -1;
static int hf_quic_cc_error_code = -1;
static int hf_quic_cc_error_code_app = -1;
static int hf_quic_cc_frame_type = -1;
static int hf_quic_cc_reason_phrase_length = -1;
static int hf_quic_cc_reason_phrase = -1;
static int hf_quic_dg_length = -1;
static int hf_quic_dg = -1;
static int hf_quic_af_sequence_number = -1;
static int hf_quic_af_packet_tolerance = -1;
static int hf_quic_af_update_max_ack_delay = -1;
static int hf_quic_ts = -1;
static int hf_quic_reassembled_in = -1;
static int hf_quic_reassembled_length = -1;
static int hf_quic_reassembled_data = -1;
static int hf_quic_fragments = -1;
static int hf_quic_fragment = -1;
static int hf_quic_fragment_overlap = -1;
static int hf_quic_fragment_overlap_conflict = -1;
static int hf_quic_fragment_multiple_tails = -1;
static int hf_quic_fragment_too_long_fragment = -1;
static int hf_quic_fragment_error = -1;
static int hf_quic_fragment_count = -1;
static int hf_quic_mp_add_address_first_byte	= -1;
static int hf_quic_mp_add_address_reserved = -1;
static int hf_quic_mp_add_address_port_present = -1;
static int hf_quic_mp_add_address_ip_version = -1;
static int hf_quic_mp_add_address_id = -1;
static int hf_quic_mp_add_address_sq_number = -1;
static int hf_quic_mp_add_address_interface_type = -1;
static int hf_quic_mp_add_address_ip_address = -1;
static int hf_quic_mp_add_address_ip_address_v6 = -1;
static int hf_quic_mp_add_address_port = -1;
static int hf_quic_mp_uniflow_id = -1;
static int hf_quic_mp_receiving_uniflows = -1;
static int hf_quic_mp_active_sending_uniflows = -1;
static int hf_quic_mp_add_local_address_id = -1;
static int hf_quic_mp_uniflow_info_section = -1;
static int hf_quic_mp_receiving_uniflow_info_section = -1;
static int hf_quic_mp_active_sending_uniflows_info_section = -1;

static expert_field ei_quic_connection_unknown = EI_INIT;
static expert_field ei_quic_ft_unknown = EI_INIT;
static expert_field ei_quic_decryption_failed = EI_INIT;
static expert_field ei_quic_protocol_violation = EI_INIT;
static expert_field ei_quic_bad_retry = EI_INIT;
static expert_field ei_quic_coalesced_padding_data = EI_INIT;

static gint ett_quic = -1;
static gint ett_quic_short_header = -1;
static gint ett_quic_connection_info = -1;
static gint ett_quic_ft = -1;
static gint ett_quic_ftflags = -1;
static gint ett_quic_ftid = -1;
static gint ett_quic_fragments = -1;
static gint ett_quic_fragment = -1;

static dissector_handle_t quic_handle;
static dissector_handle_t tls13_handshake_handle;

static dissector_table_t quic_proto_dissector_table;

/* Fields for showing reassembly results for fragments of QUIC stream data. */
static const fragment_items quic_stream_fragment_items = {
    &ett_quic_fragment,
    &ett_quic_fragments,
    &hf_quic_fragments,
    &hf_quic_fragment,
    &hf_quic_fragment_overlap,
    &hf_quic_fragment_overlap_conflict,
    &hf_quic_fragment_multiple_tails,
    &hf_quic_fragment_too_long_fragment,
    &hf_quic_fragment_error,
    &hf_quic_fragment_count,
    &hf_quic_reassembled_in,
    &hf_quic_reassembled_length,
    &hf_quic_reassembled_data,
    "Fragments"
};

/*
 * PROTECTED PAYLOAD DECRYPTION (done in first pass)
 *
 * Long packet types always use a single cipher depending on packet type.
 * Short packet types always use 1-RTT secrets for packet protection (pp).
 *
 * Considerations:
 * - QUIC packets might appear out-of-order (short packets before handshake
 *   message is captured), lost or retransmitted/duplicated.
 * - During live capture, keys might not be immediately be available. 1-RTT
 *   client keys will be ready while client proceses Server Hello (Handshake).
 *   1-RTT server keys will be ready while server creates Handshake message in
 *   response to Initial Handshake.
 * - So delay cipher creation until first short packet is received.
 *
 * Required input from TLS dissector: TLS-Exporter 0-RTT/1-RTT secrets and
 * cipher/hash algorithms.
 *
 * QUIC payload decryption requires proper reconstruction of the packet number
 * which requires proper header decryption. The different states are:
 *
 *  Packet type             Packet number space     Secrets
 *  Long: Initial           Initial                 Initial secrets
 *  Long: Handshake         Handshake               Handshake
 *  Long: 0-RTT             0/1-RTT (appdata)       0-RTT
 *  Short header            0/1-RTT (appdata)       1-RTT (KP0 / KP1)
 *
 * Important to note is that Short Header decryption requires TWO ciphers (one
 * for each key phase), but that header protection uses only KP0. Total state
 * needed for each peer (client and server):
 * - 3 packet number spaces: Initial, Handshake, 0/1-RTT (appdata).
 * - 4 header protection ciphers: initial, 0-RTT, HS, 1-RTT.
 * - 5 payload protection ciphers: initial, 0-RTT, HS, 1-RTT (KP0), 1-RTT (KP1).
 */

/* Loss bits feature: https://tools.ietf.org/html/draft-ferrieuxhamchaoui-quic-lossbits-03
   "The use of the loss bits is negotiated using a transport parameter.
    [..]
    When loss_bits parameter is present, the peer is allowed to use
    reserved bits in the short packet header as loss bits if the peer
    sends loss_bits=1.
    When loss_bits is set to 1, the sender will use reserved bits as loss
    bits if the peer includes the loss_bits transport parameter.
    [..]
    Unlike the reserved (R) bits, the loss (Q and L) bits are not
    protected.  When sending loss bits has been negotiated, the first
    byte of the header protection mask used to protect short packet
    headers has its five most significant bits masked out instead of
    three.
*/

typedef struct quic_decrypt_result {
    const guchar   *error;      /**< Error message or NULL for success. */
    const guint8   *data;       /**< Decrypted result on success (file-scoped). */
    guint           data_len;   /**< Size of decrypted data. */
} quic_decrypt_result_t;

/** QUIC decryption context. */

/** Singly-linked list of Connection IDs. */
typedef struct quic_cid_item quic_cid_item_t;
struct quic_cid_item {
    struct quic_cid_item   *next;
    quic_cid_t              data;
};

/**
 * Per-STREAM state, identified by QUIC Stream ID.
 *
 * Assume that every QUIC Short Header packet has no STREAM frames that overlap
 * each other in the same QUIC packet (identified by "frame_num"). Thus, the
 * Stream ID and offset uniquely identifies the STREAM Frame info in per packet.
 */
typedef struct _quic_stream_state {
    guint64         stream_id;
    wmem_tree_t    *multisegment_pdus;
    void           *subdissector_private;
} quic_stream_state;

/**
 * Data used to allow "Follow QUIC Stream" functionality
 */
typedef struct _quic_follow_stream {
    guint32         num;
    guint64         stream_id;
} quic_follow_stream;


typedef struct quic_follow_tap_data {
    tvbuff_t *tvb;
    guint64  stream_id;
    gboolean from_server;
} quic_follow_tap_data_t;

/**
 * State for a single QUIC connection, identified by one or more Destination
 * Connection IDs (DCID).
 */
typedef struct quic_info_data {
    guint32         number;         /** Similar to "udp.stream", but for identifying QUIC connections across migrations. */
    guint32         version;
    address         server_address;
    guint16         server_port;
    gboolean        skip_decryption : 1; /**< Set to 1 if no keys are available. */
    gboolean        client_dcid_set : 1; /**< Set to 1 if client_dcid_initial is set. */
    gboolean        client_loss_bits_recv : 1; /**< The client is able to read loss bits info */
    gboolean        client_loss_bits_send : 1; /**< The client wants to send loss bits info */
    gboolean        server_loss_bits_recv : 1; /**< The server is able to read loss bits info */
    gboolean        server_loss_bits_send : 1; /**< The server wants to send loss bits info */
    int             hash_algo;      /**< Libgcrypt hash algorithm for key derivation. */
    int             cipher_algo;    /**< Cipher algorithm for packet number and packet encryption. */
    int             cipher_mode;    /**< Cipher mode for packet encryption. */
    guint64         max_client_pkn[3];  /**< Packet number spaces for Initial, Handshake and appdata. */
    guint64         max_server_pkn[3];
    quic_cid_item_t client_cids;    /**< SCID of client from first Initial Packet. */
    quic_cid_item_t server_cids;    /**< SCID of server from first Retry/Handshake. */
    quic_cid_t      client_dcid_initial;    /**< DCID from Initial Packet. */
    dissector_handle_t app_handle;  /**< Application protocol handle (NULL if unknown). */
    wmem_map_t     *client_streams; /**< Map from Stream ID -> STREAM info (guint64 -> quic_stream_state), sent by the client. */
    wmem_map_t     *server_streams; /**< Map from Stream ID -> STREAM info (guint64 -> quic_stream_state), sent by the server. */
    wmem_list_t    *streams_list;   /**< Ordered list of QUIC Stream ID in this connection (both directions). Used by "Follow QUIC Stream" functionality */
    wmem_map_t     *streams_map;    /**< Map pinfo->num --> First stream in that frame (guint -> quic_follow_stream). Used by "Follow QUIC Stream" functionality */
} quic_info_data_t;

/** Per-packet information about QUIC, populated on the first pass. */
struct quic_packet_info {
    struct quic_packet_info *next;
    guint64                 packet_number;  /**< Reconstructed full packet number. */
    quic_decrypt_result_t   decryption;
    guint8                  pkn_len;        /**< Length of PKN (1/2/3/4) or unknown (0). */
    guint8                  first_byte;     /**< Decrypted flag byte, valid only if pkn_len is non-zero. */
    gboolean                retry_integrity_failure : 1;
    gboolean                retry_integrity_success : 1;
};
typedef struct quic_packet_info quic_packet_info_t;

/** A UDP datagram contains one or more QUIC packets. */
typedef struct quic_datagram {
    quic_info_data_t       *conn;
    quic_packet_info_t      first_packet;
    gboolean                from_server : 1;
} quic_datagram;

/**
 * Maps CID (quic_cid_t *) to a QUIC Connection (quic_info_data_t *).
 * This assumes that the CIDs are not shared between two different connections
 * (potentially with different versions) as that would break dissection.
 *
 * These mappings are authorative. For example, Initial.SCID is stored in
 * quic_client_connections while Retry.SCID is stored in
 * quic_server_connections. Retry.DCID should normally correspond to an entry in
 * quic_client_connections.
 */
static wmem_map_t *quic_client_connections, *quic_server_connections;
static wmem_map_t *quic_initial_connections;    /* Initial.DCID -> connection */
static wmem_list_t *quic_connections;   /* All unique connections. */
static guint32 quic_cid_lengths;        /* Bitmap of CID lengths. */
static guint quic_connections_count;

static struct tcp_multisegment_pdu *
pdu_store_sequencenumber_of_next_pdu(packet_info *pinfo, guint32 seq, guint32 nxtpdu, wmem_tree_t *multisegment_pdus)
{
    struct tcp_multisegment_pdu *msp;

    msp=wmem_new(wmem_file_scope(), struct tcp_multisegment_pdu);
    msp->nxtpdu=nxtpdu;
    msp->seq=seq;
    msp->first_frame=pinfo->num;
    msp->first_frame_with_seq=pinfo->num;
    msp->last_frame=pinfo->num;
    msp->last_frame_time=pinfo->abs_ts;
    msp->flags=0;
    wmem_tree_insert32(multisegment_pdus, seq, (void *)msp);
    /*ws_warning("pdu_store_sequencenumber_of_next_pdu: seq %u", seq);*/
    return msp;
}

/* Returns the QUIC draft version or 0 if not applicable. */
static inline guint8 quic_draft_version(guint32 version) {
    /* IETF Draft versions */
    if ((version >> 8) == 0xff0000) {
       return (guint8) version;
    }
    /* Facebook mvfst, based on draft -22. */
    if (version == 0xfaceb001) {
        return 22;
    }
    /* Facebook mvfst, based on draft -27. */
    if (version == 0xfaceb002 || version == 0xfaceb00e) {
        return 27;
    }
    /* GQUIC Q050, T050 and T051: they are not really based on any drafts,
     * but we must return a sensible value */
    if (version == 0x51303530 ||
        version == 0x54303530 ||
        version == 0x54303531) {
        return 27;
    }
    /* https://tools.ietf.org/html/draft-ietf-quic-transport-32#section-15
       "Versions that follow the pattern 0x?a?a?a?a are reserved for use in
       forcing version negotiation to be exercised"
       It is tricky to return a correct draft version: such number is primarly
       used to select a proper salt (which depends on the version itself), but
       we don't have a real version here! Let's hope that we need to handle
       only latest drafts... */
    if ((version & 0x0F0F0F0F) == 0x0a0a0a0a) {
        return 29;
    }
    /* QUIC (final?) constants for v1 are defined in draft-33 */
    if (version == 0x00000001) {
        return 33;
    }
    return 0;
}

static inline gboolean is_quic_draft_max(guint32 version, guint8 max_version) {
    guint8 draft_version = quic_draft_version(version);
    return draft_version && draft_version <= max_version;
}

const range_string quic_version_vals[] = {
    { 0x00000000, 0x00000000, "Version Negotiation" },
    { 0x00000001, 0x00000001, "1" },
    { 0x45474700, 0x454747ff, "Quant" },
    { 0x50435130, 0x50435131, "Picoquic internal" },
    { 0x50524f58, 0x50524f58, "Proxied QUIC (PROX)" },
    /* Versions QXXX < Q050 are dissected by Wireshark as GQUIC and not as QUIC.
       Nonetheless, some implementations report these values in "Version Negotiation"
       packets, so decode these fields */
    { 0x51303433, 0x51303433, "Google Q043" },
    { 0x51303434, 0x51303434, "Google Q044" },
    { 0x51303436, 0x51303436, "Google Q046" },
    { 0x51303530, 0x51303530, "Google Q050" },
    { 0x51474f00, 0x51474fff, "QGO (QUIC GO)" },
    { 0x54303530, 0x54303530, "Google T050" },
    { 0x54303531, 0x54303531, "Google T051" },
    { 0x91c17000, 0x91c170ff, "Quicly" },
    { 0xabcd0000, 0xabcd000f, "MsQuic" },
    { 0xf0f0f0f0, 0xf0f0f0ff, "ETH Zürich (Measurability experiments)" },
    { 0xf0f0f1f0, 0xf0f0f1ff, "Telecom Italia (Measurability experiments)" },
    { 0xf123f0c0, 0xf123f0cf, "MozQuic" },
    { 0xfaceb001, 0xfaceb001, "Facebook mvfst (draft-22)" },
    { 0xfaceb002, 0xfaceb002, "Facebook mvfst (draft-27)" },
    { 0xfaceb003, 0xfaceb00d, "Facebook mvfst" },
    { 0xfaceb00e, 0xfaceb00e, "Facebook mvfst (Experimental)" },
    { 0xfaceb00f, 0xfaceb00f, "Facebook mvfst" },
    { 0xff000004, 0xff000004, "draft-04" },
    { 0xff000005, 0xff000005, "draft-05" },
    { 0xff000006, 0xff000006, "draft-06" },
    { 0xff000007, 0xff000007, "draft-07" },
    { 0xff000008, 0xff000008, "draft-08" },
    { 0xff000009, 0xff000009, "draft-09" },
    { 0xff00000a, 0xff00000a, "draft-10" },
    { 0xff00000b, 0xff00000b, "draft-11" },
    { 0xff00000c, 0xff00000c, "draft-12" },
    { 0xff00000d, 0xff00000d, "draft-13" },
    { 0xff00000e, 0xff00000e, "draft-14" },
    { 0xff00000f, 0xff00000f, "draft-15" },
    { 0xff000010, 0xff000010, "draft-16" },
    { 0xff000011, 0xff000011, "draft-17" },
    { 0xff000012, 0xff000012, "draft-18" },
    { 0xff000013, 0xff000013, "draft-19" },
    { 0xff000014, 0xff000014, "draft-20" },
    { 0xff000015, 0xff000015, "draft-21" },
    { 0xff000016, 0xff000016, "draft-22" },
    { 0xff000017, 0xff000017, "draft-23" },
    { 0xff000018, 0xff000018, "draft-24" },
    { 0xff000019, 0xff000019, "draft-25" },
    { 0xff00001a, 0xff00001a, "draft-26" },
    { 0xff00001b, 0xff00001b, "draft-27" },
    { 0xff00001c, 0xff00001c, "draft-28" },
    { 0xff00001d, 0xff00001d, "draft-29" },
    { 0xff00001e, 0xff00001e, "draft-30" },
    { 0xff00001f, 0xff00001f, "draft-31" },
    { 0xff000020, 0xff000020, "draft-32" },
    { 0xff000021, 0xff000021, "draft-33" },
    { 0xff000022, 0xff000022, "draft-34" },
    { 0, 0, NULL }
};

static const value_string quic_short_long_header_vals[] = {
    { 0, "Short Header" },
    { 1, "Long Header" },
    { 0, NULL }
};

#define SH_KP       0x04

#define QUIC_LPT_INITIAL    0x0
#define QUIC_LPT_0RTT       0x1
#define QUIC_LPT_HANDSHAKE  0x2
#define QUIC_LPT_RETRY      0x3
#define QUIC_LPT_VER_NEG    0xfe    /* Version Negotiation packets don't have any real packet type */
#define QUIC_SHORT_PACKET   0xff    /* dummy value that is definitely not LPT */

static const value_string quic_long_packet_type_vals[] = {
    { QUIC_LPT_INITIAL, "Initial" },
    { QUIC_LPT_RETRY, "Retry" },
    { QUIC_LPT_HANDSHAKE, "Handshake" },
    { QUIC_LPT_0RTT, "0-RTT" },
    /* Version Negotiation packets never use this mapping, so no need to add QUIC_LPT_VER_NEG */
    { 0, NULL }
};

/* https://github.com/quicwg/base-drafts/wiki/Temporary-IANA-Registry#quic-frame-types */
#define FT_PADDING                  0x00
#define FT_PING                     0x01
#define FT_ACK                      0x02
#define FT_ACK_ECN                  0x03
#define FT_RESET_STREAM             0x04
#define FT_STOP_SENDING             0x05
#define FT_CRYPTO                   0x06
#define FT_NEW_TOKEN                0x07
#define FT_STREAM_8                 0x08
#define FT_STREAM_9                 0x09
#define FT_STREAM_A                 0x0a
#define FT_STREAM_B                 0x0b
#define FT_STREAM_C                 0x0c
#define FT_STREAM_D                 0x0d
#define FT_STREAM_E                 0x0e
#define FT_STREAM_F                 0x0f
#define FT_MAX_DATA                 0x10
#define FT_MAX_STREAM_DATA          0x11
#define FT_MAX_STREAMS_BIDI         0x12
#define FT_MAX_STREAMS_UNI          0x13
#define FT_DATA_BLOCKED             0x14
#define FT_STREAM_DATA_BLOCKED      0x15
#define FT_STREAMS_BLOCKED_BIDI     0x16
#define FT_STREAMS_BLOCKED_UNI      0x17
#define FT_NEW_CONNECTION_ID        0x18
#define FT_RETIRE_CONNECTION_ID     0x19
#define FT_PATH_CHALLENGE           0x1a
#define FT_PATH_RESPONSE            0x1b
#define FT_CONNECTION_CLOSE_TPT     0x1c
#define FT_CONNECTION_CLOSE_APP     0x1d
#define FT_HANDSHAKE_DONE           0x1e
#define FT_DATAGRAM                 0x30
#define FT_MP_NEW_CONNECTION_ID     0x40
#define FT_MP_RETIRE_CONNECTION_ID  0x41
#define FT_MP_ACK                   0x42
#define FT_MP_ACK_ECN               0x43
#define FT_ADD_ADDRESS              0x44
#define FT_REMOVE_ADDRESS           0x45
#define FT_UNIFLOWS                 0x46
#define FT_DATAGRAM_LENGTH          0x31
#define FT_ACK_FREQUENCY            0xAF
#define FT_TIME_STAMP               0x02F5

static const range_string quic_frame_type_vals[] = {
    { 0x00, 0x00,   "PADDING" },
    { 0x01, 0x01,   "PING" },
    { 0x02, 0x03,   "ACK" },
    { 0x04, 0x04,   "RESET_STREAM" },
    { 0x05, 0x05,   "STOP_SENDING" },
    { 0x06, 0x06,   "CRYPTO" },
    { 0x07, 0x07,   "NEW_TOKEN" },
    { 0x08, 0x0f,   "STREAM" },
    { 0x10, 0x10,   "MAX_DATA" },
    { 0x11, 0x11,   "MAX_STREAM_DATA" },
    { 0x12, 0x12,   "MAX_STREAMS (BIDI)" },
    { 0x13, 0x13,   "MAX_STREAMS (UNI)" },
    { 0x14, 0x14,   "DATA_BLOCKED" },
    { 0x15, 0x15,   "STREAM_DATA_BLOCKED" },
    { 0x16, 0x16,   "STREAMS_BLOCKED (BIDI)" },
    { 0x16, 0x17,   "STREAMS_BLOCKED (UNI)" },
    { 0x18, 0x18,   "NEW_CONNECTION_ID" },
    { 0x19, 0x19,   "RETIRE_CONNECTION_ID" },
    { 0x1a, 0x1a,   "PATH_CHALLENGE" },
    { 0x1b, 0x1b,   "PATH_RESPONSE" },
    { 0x1c, 0x1c,   "CONNECTION_CLOSE (Transport)" },
    { 0x1d, 0x1d,   "CONNECTION_CLOSE (Application)" },
    { 0x1e, 0x1e,   "HANDSHAKE_DONE" },
    { 0x30, 0x31,   "DATAGRAM" },
    { 0x40, 0x40,   "MP_NEW_CONNECTION_ID" },
    { 0x41, 0x41,   "MP_RETIRE_CONNECTION_ID" },
    { 0x42, 0x43,   "MP_ACK" },
    { 0x44, 0x44,   "ADD_ADDRESS" },
    { 0x45, 0x45,   "REMOVE_ADDRESS" },
    { 0x46, 0x46,   "UNIFLOWS" },
    { 0xaf, 0xaf,   "ACK_FREQUENCY" },
    { 0x02f5, 0x02f5, "TIME_STAMP" },
    { 0,    0,        NULL },
};


/* >= draft-08 */
#define FTFLAGS_STREAM_FIN 0x01
#define FTFLAGS_STREAM_LEN 0x02
#define FTFLAGS_STREAM_OFF 0x04

#define FTFLAGS_STREAM_INITIATOR 0x01
#define FTFLAGS_STREAM_DIRECTION 0x02

static const range_string quic_transport_error_code_vals[] = {
    /* 0x00 - 0x3f Assigned via Standards Action or IESG Review policies. */
    { 0x0000, 0x0000, "NO_ERROR" },
    { 0x0001, 0x0001, "INTERNAL_ERROR" },
    { 0x0002, 0x0002, "CONNECTION_REFUSED" },
    { 0x0003, 0x0003, "FLOW_CONTROL_ERROR" },
    { 0x0004, 0x0004, "STREAM_ID_ERROR" },
    { 0x0005, 0x0005, "STREAM_STATE_ERROR" },
    { 0x0006, 0x0006, "FINAL_SIZE_ERROR" },
    { 0x0007, 0x0007, "FRAME_ENCODING_ERROR" },
    { 0x0008, 0x0008, "TRANSPORT_PARAMETER_ERROR" },
    { 0x0009, 0x0009, "CONNECTION_ID_LIMIT_ERROR" },
    { 0x000a, 0x000a, "PROTOCOL_VIOLATION" },
    { 0x000b, 0x000b, "INVALID_TOKEN" },
    { 0x000c, 0x000c, "APPLICATION_ERROR" },
    { 0x000d, 0x000d, "CRYPTO_BUFFER_EXCEEDED" },
    { 0x000e, 0x000e, "KEY_UPDATE_ERROR" },
    { 0x000f, 0x000f, "AEAD_LIMIT_REACHED" },
    { 0x0010, 0x0010, "NO_VIABLE_PATH" },
    { 0x0100, 0x01ff, "CRYPTO_ERROR" },
    /* 0x40 - 0x3fff Assigned via Specification Required policy. */
    { 0x53F8, 0x53F8, "VERSION_NEGOTIATION_ERROR" },
    { 0, 0, NULL }
};

static const value_string quic_packet_number_lengths[] = {
    { 0, "1 bytes" },
    { 1, "2 bytes" },
    { 2, "3 bytes" },
    { 3, "4 bytes" },
    { 0, NULL }
};

static const val64_string quic_frame_id_initiator[] = {
    { 0, "Client-initiated" },
    { 1, "Server-initiated" },
    { 0, NULL }
};

static const val64_string quic_frame_id_direction[] = {
    { 0, "Bidirectional" },
    { 1, "Unidirectional" },
    { 0, NULL }
};

static void
quic_extract_header(tvbuff_t *tvb, guint8 *long_packet_type, guint32 *version,
                    quic_cid_t *dcid, quic_cid_t *scid);

static void
quic_streams_add(packet_info *pinfo, quic_info_data_t *quic_info, guint64 stream_id);

/* Inspired from ngtcp2 */
static guint64 quic_pkt_adjust_pkt_num(guint64 max_pkt_num, guint64 pkt_num,
                                   size_t n) {
  guint64 k = max_pkt_num == G_MAXUINT64 ? max_pkt_num : max_pkt_num + 1;
  guint64 u = k & ~((G_GUINT64_CONSTANT(1) << n) - 1);
  guint64 a = u | pkt_num;
  guint64 b = (u + (G_GUINT64_CONSTANT(1) << n)) | pkt_num;
  guint64 a1 = k < a ? a - k : k - a;
  guint64 b1 = k < b ? b - k : k - b;

  if (a1 < b1) {
    return a;
  }
  return b;
}

/**
 * Given a header protection cipher, a buffer and the packet number offset,
 * return the unmasked first byte and packet number.
 * If the loss bits feature is enabled, the protected bits in the first byte
 * are fewer than usual: 3 instead of 5 (on short headers only)
 */
static gboolean
quic_decrypt_header(tvbuff_t *tvb, guint pn_offset,
                    guint8 *first_byte, guint32 *pn)
{
    // https://tools.ietf.org/html/draft-ietf-quic-tls-22#section-5.4.1
    guint8 packet0 = tvb_get_guint8(tvb, 0);
    guint pkn_len = (packet0 & 0x03) + 1;

    guint8 pkn_bytes[4];
    tvb_memcpy(tvb, pkn_bytes, pn_offset, pkn_len);
    guint32 pkt_pkn = 0;
    for (guint i = 0; i < pkn_len; i++) {
        pkt_pkn |= (pkn_bytes[i]) << (8 * (pkn_len - 1 - i));
    }
    *first_byte = packet0;
    *pn = pkt_pkn;
    return TRUE;
}

/**
 * Retrieve the maximum valid packet number space for a peer.
 */
static guint64 *
quic_max_packet_number(quic_info_data_t *quic_info, gboolean from_server, guint8 first_byte)
{
    int pkn_space;
    if ((first_byte & 0x80) && (first_byte & 0x30) >> 4 == QUIC_LPT_INITIAL) {
        // Long header, Initial
        pkn_space = 0;
    } else if ((first_byte & 0x80) && (first_byte & 0x30) >> 4 == QUIC_LPT_HANDSHAKE) {
        // Long header, Handshake
        pkn_space = 1;
    } else {
        // Long header (0-RTT) or Short Header (1-RTT appdata).
        pkn_space = 2;
    }
    if (from_server) {
        return &quic_info->max_server_pkn[pkn_space];
    } else {
        return &quic_info->max_client_pkn[pkn_space];
    }
}

/**
 * Calculate the full packet number and store it for later use.
 */
static void
quic_set_full_packet_number(quic_info_data_t *quic_info, quic_packet_info_t *quic_packet,
                            gboolean from_server, guint8 first_byte, guint32 pkn32)
{
    guint       pkn_len = (first_byte & 3) + 1;
    guint64     pkn_full;
    guint64     max_pn = *quic_max_packet_number(quic_info, from_server, first_byte);

    /* Sequential first pass, try to reconstruct full packet number. */
    pkn_full = quic_pkt_adjust_pkt_num(max_pn, pkn32, 8 * pkn_len);
    quic_packet->pkn_len = pkn_len;
    quic_packet->packet_number = pkn_full;
}

static const char *
cid_to_string(const quic_cid_t *cid)
{
    if (cid->len == 0) {
        return "(none)";
    }
    char *str = (char *)wmem_alloc0(wmem_packet_scope(), 2 * cid->len + 1);
    bytes_to_hexstr(str, cid->cid, cid->len);
    return str;
}

/* QUIC Connection tracking. {{{ */
static guint
quic_connection_hash(gconstpointer key)
{
    const quic_cid_t *cid = (const quic_cid_t *)key;

    return wmem_strong_hash((const guint8 *)cid, sizeof(quic_cid_t) - sizeof(cid->cid) + cid->len);
}

static gboolean
quic_connection_equal(gconstpointer a, gconstpointer b)
{
    const quic_cid_t *cid1 = (const quic_cid_t *)a;
    const quic_cid_t *cid2 = (const quic_cid_t *)b;

    return cid1->len == cid2->len && !memcmp(cid1->cid, cid2->cid, cid1->len);
}

static gboolean
quic_cids_has_match(const quic_cid_item_t *items, const quic_cid_t *raw_cid)
{
    while (items) {
        const quic_cid_t *cid = &items->data;
        // "raw_cid" potentially has some trailing data that is not part of the
        // actual CID, so accept any prefix match against "cid".
        // Note that this explicitly matches an empty CID.
        if (raw_cid->len >= cid->len && !memcmp(raw_cid->cid, cid->cid, cid->len)) {
            return TRUE;
        }
        items = items->next;
    }
    return FALSE;
}

static void
quic_cids_insert(quic_cid_t *cid, quic_info_data_t *conn, gboolean from_server)
{
    wmem_map_t *connections = from_server ? quic_server_connections : quic_client_connections;
    // Replace any previous CID key with the new one.
    wmem_map_remove(connections, cid);
    wmem_map_insert(connections, cid, conn);
    G_STATIC_ASSERT(QUIC_MAX_CID_LENGTH <= 8 * sizeof(quic_cid_lengths));
    quic_cid_lengths |= (1ULL << cid->len);
}

static inline gboolean
quic_cids_is_known_length(const quic_cid_t *cid)
{
    return (quic_cid_lengths & (1ULL << cid->len)) != 0;
}

/**
 * Returns the QUIC connection for the current UDP stream. This may return NULL
 * after connection migration if the new UDP association was not properly linked
 * via a match based on the Connection ID.
 */
static quic_info_data_t *
quic_connection_from_conv(packet_info *pinfo)
{
    conversation_t *conv = find_conversation_pinfo(pinfo, 0);
    if (conv) {
        return (quic_info_data_t *)conversation_get_proto_data(conv, proto_quic_plain);
    }
    return NULL;
}

/**
 * Tries to lookup a matching connection (Connection ID is optional).
 * If connection is found, "from_server" is set accordingly.
 */
static quic_info_data_t *
quic_connection_find_dcid(packet_info *pinfo, const quic_cid_t *dcid, gboolean *from_server)
{
    /* https://tools.ietf.org/html/draft-ietf-quic-transport-22#section-5.2
     *
     * "If the packet has a Destination Connection ID corresponding to an
     * existing connection, QUIC processes that packet accordingly."
     * "If the Destination Connection ID is zero length and the packet matches
     * the address/port tuple of a connection where the host did not require
     * connection IDs, QUIC processes the packet as part of that connection."
     */
    quic_info_data_t *conn = NULL;
    gboolean check_ports = FALSE;

    if (dcid && dcid->len > 0) {
        // Optimization: avoid lookup for invalid CIDs.
        if (!quic_cids_is_known_length(dcid)) {
            return NULL;
        }
        conn = (quic_info_data_t *) wmem_map_lookup(quic_client_connections, dcid);
        if (conn) {
            // DCID recognized by client, so it was from server.
            *from_server = TRUE;
            // On collision (both client and server choose the same CID), check
            // the port to learn about the side.
            // This is required for supporting draft -10 which has a single CID.
            check_ports = !!wmem_map_lookup(quic_server_connections, dcid);
        } else {
            conn = (quic_info_data_t *) wmem_map_lookup(quic_server_connections, dcid);
            if (conn) {
                // DCID recognized by server, so it was from client.
                *from_server = FALSE;
            }
        }
    } else {
        conn = quic_connection_from_conv(pinfo);
        if (conn) {
            check_ports = TRUE;
        }
    }

    if (check_ports) {
        *from_server = conn->server_port == pinfo->srcport &&
                addresses_equal(&conn->server_address, &pinfo->src);
    }

    return conn;
}

/**
 * Try to find a QUIC connection based on DCID. For short header packets, DCID
 * will be modified in order to find the actual length.
 * DCID can be empty, in that case a connection is looked up by address only.
 */
static quic_info_data_t *
quic_connection_find(packet_info *pinfo, guint8 long_packet_type,
                     quic_cid_t *dcid, gboolean *from_server)
{
    gboolean is_long_packet = long_packet_type != QUIC_SHORT_PACKET;
    quic_info_data_t *conn = NULL;

    if (long_packet_type == QUIC_LPT_0RTT && dcid->len > 0) {
        // The 0-RTT packet always matches the SCID/DCID of the Client Initial
        conn = (quic_info_data_t *) wmem_map_lookup(quic_initial_connections, dcid);
        *from_server = FALSE;
    } else {
        // Find a connection for Handshake, Version Negotiation and Server Initial packets by
        // matching their DCID against the SCIDs of the original Initial packets
        // from the peer. For Client Initial packets, match DCID of the first
        // Client Initial (these may contain ACK frames).
        conn = quic_connection_find_dcid(pinfo, dcid, from_server);
        if (long_packet_type == QUIC_LPT_INITIAL && conn && !*from_server && dcid->len > 0 &&
            memcmp(dcid, &conn->client_dcid_initial, sizeof(quic_cid_t)) &&
            !quic_cids_has_match(&conn->server_cids, dcid)) {
            // If the Initial Packet is from the client, it must either match
            // the DCID from the first Client Initial, or the DCID that was
            // assigned by the server. Otherwise this must be considered a fresh
            // Client Initial, for example after the Version Negotiation packet,
            // and the connection must be cleared to avoid decryption failure.
            conn = NULL;
        }
    }

    if (!is_long_packet && !conn) {
        // For short packets, first try to find a match based on the address.
        conn = quic_connection_find_dcid(pinfo, NULL, from_server);
        if (conn) {
            if ((*from_server && !quic_cids_has_match(&conn->client_cids, dcid)) ||
                (!*from_server && !quic_cids_has_match(&conn->server_cids, dcid))) {
                // Connection does not match packet.
                conn = NULL;
            }
        }

        // No match found so far, potentially connection migration. Length of
        // actual DCID is unknown, so just keep decrementing until found.
        while (!conn && dcid->len > 1) {
            dcid->len--;
            if (quic_cids_is_known_length(dcid)) {
                conn = quic_connection_find_dcid(pinfo, dcid, from_server);
            }
        }
        if (!conn) {
            // No match found, truncate DCID (not really needed, but this
            // ensures that debug prints clearly show that DCID is invalid).
            dcid->len = 0;
        }
    }
    return conn;
}

/** Create a new QUIC Connection based on a Client Initial packet. */
static quic_info_data_t *
quic_connection_create(packet_info *pinfo, guint32 version)
{
    quic_info_data_t *conn = NULL;

    conn = wmem_new0(wmem_file_scope(), quic_info_data_t);
    wmem_list_append(quic_connections, conn);
    conn->number = quic_connections_count++;
    conn->version = version;
    copy_address_wmem(wmem_file_scope(), &conn->server_address, &pinfo->dst);
    conn->server_port = pinfo->destport;

    // For faster lookups without having to check DCID
    conversation_t *conv = find_or_create_conversation(pinfo);
    conversation_add_proto_data(conv, proto_quic_plain, conn);

    return conn;
}

/** Update client/server connection identifiers, assuming the information is
 * from the Client Initial. */
static void
quic_connection_update_initial(quic_info_data_t *conn, const quic_cid_t *scid, const quic_cid_t *dcid)
{
    // Key connection by Client CID (if provided).
    if (scid->len) {
        memcpy(&conn->client_cids.data, scid, sizeof(quic_cid_t));
        quic_cids_insert(&conn->client_cids.data, conn, FALSE);
    }
    if (dcid->len > 0) {
        // According to the spec, the Initial Packet DCID MUST be at least 8
        // bytes, but non-conforming implementations could exist.
        memcpy(&conn->client_dcid_initial, dcid, sizeof(quic_cid_t));
        wmem_map_insert(quic_initial_connections, &conn->client_dcid_initial, conn);
        conn->client_dcid_set = TRUE;
    }
}

/**
 * Use the new CID as additional identifier for the specified connection and
 * remember it for connection tracking.
 */
static void
quic_connection_add_cid(quic_info_data_t *conn, const quic_cid_t *new_cid, gboolean from_server)
{
    DISSECTOR_ASSERT(new_cid->len > 0);
    quic_cid_item_t *items = from_server ? &conn->server_cids : &conn->client_cids;

    if (quic_cids_has_match(items, new_cid)) {
        // CID is already known for this connection.
        return;
    }

    // Insert new CID right after the first known CID (the very first CID cannot
    // be overwritten since it might be used as key somewhere else).
    quic_cid_item_t *new_item = wmem_new0(wmem_file_scope(), quic_cid_item_t);
    new_item->data = *new_cid;
    new_item->next = items->next;
    items->next = new_item;

    quic_cids_insert(&new_item->data, conn, from_server);
}

/** Create or update a connection. */
static void
quic_connection_create_or_update(quic_info_data_t **conn_p,
                                 packet_info *pinfo, guint32 long_packet_type,
                                 guint32 version, const quic_cid_t *scid,
                                 const quic_cid_t *dcid, gboolean from_server)
{
    quic_info_data_t *conn = *conn_p;

    switch (long_packet_type) {
    case QUIC_LPT_INITIAL:
        if (!from_server) {
            if (!conn) {
                // The first Initial Packet from the client creates a new connection.
                *conn_p = quic_connection_create(pinfo, version);
                quic_connection_update_initial(*conn_p, scid, dcid);
            } else if (!conn->client_dcid_set && dcid->len) {
                // If this client Initial Packet responds to a Retry Packet,
                // then remember the new client SCID and initial DCID for the
                // new Initial cipher and clear the first server CID such that
                // the next server Initial Packet can link the connection with
                // that new SCID.
                quic_connection_update_initial(conn, scid, dcid);
                wmem_map_remove(quic_server_connections, &conn->server_cids.data);
                memset(&conn->server_cids, 0, sizeof(quic_cid_t));
            }
            break;
        }
        /* fallthrough */
    case QUIC_LPT_RETRY:
    case QUIC_LPT_HANDSHAKE:
        // Remember CID from first server Retry/Handshake packet
        // (or from the first server Initial packet, since draft -13).
        if (from_server && conn) {
            if (long_packet_type == QUIC_LPT_RETRY) {
                // Retry Packet: the next Initial Packet from the
                // client should start a new cryptographic handshake. Erase the
                // current "Initial DCID" such that the next client Initial
                // packet populates the new value.
                wmem_map_remove(quic_initial_connections, &conn->client_dcid_initial);
                memset(&conn->client_dcid_initial, 0, sizeof(quic_cid_t));
                conn->client_dcid_set = FALSE;
            }
            if (conn->server_cids.data.len == 0 && scid->len) {
                memcpy(&conn->server_cids.data, scid, sizeof(quic_cid_t));
                quic_cids_insert(&conn->server_cids.data, conn, TRUE);
            }
        }
        break;
    }
}

/* QUIC Connection tracking. }}} */

/* QUIC Streams tracking and reassembly. {{{ */
static reassembly_table quic_reassembly_table;

/** Perform sequence analysis for STREAM frames. */
static quic_stream_state *
quic_get_stream_state(packet_info *pinfo, quic_info_data_t *quic_info, gboolean from_server, guint64 stream_id)
{
    wmem_map_t **streams_p = from_server ? &quic_info->server_streams : &quic_info->client_streams;
    wmem_map_t *streams = *streams_p;
    quic_stream_state *stream = NULL;

    if (PINFO_FD_VISITED(pinfo)) {
        DISSECTOR_ASSERT(streams);
        stream = (quic_stream_state *)wmem_map_lookup(streams, &stream_id);
        DISSECTOR_ASSERT(stream);
        return stream;
    }

    // Initialize per-connection and per-stream state.
    if (!streams) {
        streams = wmem_map_new(wmem_file_scope(), wmem_int64_hash, g_int64_equal);
        *streams_p = streams;
    } else {
        stream = (quic_stream_state *)wmem_map_lookup(streams, &stream_id);
    }
    if (!stream) {
        stream = wmem_new0(wmem_file_scope(), quic_stream_state);
        stream->stream_id = stream_id;
        stream->multisegment_pdus = wmem_tree_new(wmem_file_scope());
        wmem_map_insert(streams, &stream->stream_id, stream);
    }
    return stream;
}

static void
process_quic_stream(tvbuff_t *tvb, int offset, packet_info *pinfo, proto_tree *tree,
                    quic_info_data_t *quic_info, quic_stream_info *stream_info)
{
    if (quic_info->app_handle) {
        tvbuff_t *next_tvb = tvb_new_subset_remaining(tvb, offset);
        // Traverse the STREAM frame tree.
        proto_tree *top_tree = proto_tree_get_parent_tree(tree);
        top_tree = proto_tree_get_parent_tree(top_tree);
        call_dissector_with_data(quic_info->app_handle, next_tvb, pinfo, top_tree, stream_info);
    }
}

/**
 * Reassemble stream data within a STREAM frame.
 */
static void
desegment_quic_stream(tvbuff_t *tvb, int offset, int length, packet_info *pinfo,
                      proto_tree *tree, quic_info_data_t *quic_info,
                      quic_stream_info *stream_info,
                      quic_stream_state *stream)
{
    fragment_head *fh;
    int last_fragment_len;
    gboolean must_desegment;
    gboolean called_dissector;
    int another_pdu_follows;
    int deseg_offset;
    struct tcp_multisegment_pdu *msp;
    guint32 seq = (guint32)stream_info->stream_offset;
    const guint32 nxtseq = seq + (guint32)length;
    guint32 reassembly_id = 0;

    // XXX fix the tvb accessors below such that no new tvb is needed.
    tvb = tvb_new_subset_length(tvb, 0, offset + length);

again:
    fh = NULL;
    last_fragment_len = 0;
    must_desegment = FALSE;
    called_dissector = FALSE;
    another_pdu_follows = 0;
    msp = NULL;

    /*
     * Initialize these to assume no desegmentation.
     * If that's not the case, these will be set appropriately
     * by the subdissector.
     */
    pinfo->desegment_offset = 0;
    pinfo->desegment_len = 0;

    /*
     * Initialize this to assume that this segment will just be
     * added to the middle of a desegmented chunk of data, so
     * that we should show it all as data.
     * If that's not the case, it will be set appropriately.
     */
    deseg_offset = offset;

    /* Have we seen this PDU before (and is it the start of a multi-
     * segment PDU)?
     */
    if ((msp = (struct tcp_multisegment_pdu *)wmem_tree_lookup32(stream->multisegment_pdus, seq)) &&
            nxtseq <= msp->nxtpdu) {
        // TODO show expert info for retransmission? Additional checks may be
        // necessary here to tell a retransmission apart from other (normal?)
        // conditions. See also similar code in packet-tcp.c.
#if 0
        proto_tree_add_debug_text(tree, "TODO retransmission expert info frame %d stream_id=%" G_GINT64_MODIFIER "u offset=%d visited=%d reassembly_id=0x%08x",
                pinfo->num, stream->stream_id, offset, PINFO_FD_VISITED(pinfo), reassembly_id);
#endif
        return;
    }
    /* Else, find the most previous PDU starting before this sequence number */
    if (!msp && seq > 0) {
        msp = (struct tcp_multisegment_pdu *)wmem_tree_lookup32_le(stream->multisegment_pdus, seq-1);
        /* Unless if we already fully reassembled the msp that covers seq-1
         * and seq is beyond the end of that msp. In that case this segment
         * will be the start of a new msp.
         */
        if (msp && (msp->flags & MSP_FLAGS_GOT_ALL_SEGMENTS) &&
            seq >= msp->nxtpdu) {
            msp = NULL;
        }
    }

    {
        // A single stream can contain multiple fragments (e.g. for HTTP/3
        // HEADERS and DATA frames). Let's hope that a single stream within a
        // QUIC packet does not contain multiple partial fragments, that would
        // result in a reassembly ID collision here. If that collision becomes
        // an issue, we would have to replace "msp->first_frame" with a new
        // field in "msp" that is initialized with "stream_info->stream_offset".
#if 0
        guint64 reassembly_id_data[2];
        reassembly_id_data[0] = stream_info->stream_id;
        reassembly_id_data[1] = msp ? msp->first_frame : pinfo->num;
        reassembly_id = wmem_strong_hash((const guint8 *)&reassembly_id_data, sizeof(reassembly_id_data));
#else
        // XXX for debug (visibility) purposes, do not use a hash but concatenate
        reassembly_id = ((msp ? msp->first_frame : pinfo->num) << 16) | (guint32)stream_info->stream_id;
#endif
    }

    if (msp && msp->seq <= seq && msp->nxtpdu > seq) {
        int len;

        if (!PINFO_FD_VISITED(pinfo)) {
            msp->last_frame=pinfo->num;
            msp->last_frame_time=pinfo->abs_ts;
        }

        /* OK, this PDU was found, which means the segment continues
         * a higher-level PDU and that we must desegment it.
         */
        if (msp->flags & MSP_FLAGS_REASSEMBLE_ENTIRE_SEGMENT) {
            /* The dissector asked for the entire segment */
            len = tvb_captured_length_remaining(tvb, offset);
        } else {
            len = MIN(nxtseq, msp->nxtpdu) - seq;
        }
        last_fragment_len = len;

        fh = fragment_add(&quic_reassembly_table, tvb, offset,
                          pinfo, reassembly_id, NULL,
                          seq - msp->seq, len,
                          nxtseq < msp->nxtpdu);
        if (fh) {
            msp->flags |= MSP_FLAGS_GOT_ALL_SEGMENTS;
        }
        if (!PINFO_FD_VISITED(pinfo)
        && msp->flags & MSP_FLAGS_REASSEMBLE_ENTIRE_SEGMENT) {
            msp->flags &= (~MSP_FLAGS_REASSEMBLE_ENTIRE_SEGMENT);

            /* If we consumed the entire segment there is no
             * other pdu starting anywhere inside this segment.
             * So update nxtpdu to point at least to the start
             * of the next segment.
             * (If the subdissector asks for even more data we
             * will advance nxtpdu even further later down in
             * the code.)
             */
            msp->nxtpdu = nxtseq;
        }

        if( (msp->nxtpdu < nxtseq)
        &&  (msp->nxtpdu >= seq)
        &&  (len > 0)) {
            another_pdu_follows=msp->nxtpdu - seq;
        }
    } else {
        /* This segment was not found in our table, so it doesn't
         * contain a continuation of a higher-level PDU.
         * Call the normal subdissector.
         */

        stream_info->offset = seq;
        process_quic_stream(tvb, offset, pinfo, tree, quic_info, stream_info);
        called_dissector = TRUE;

        /* Did the subdissector ask us to desegment some more data
         * before it could handle the packet?
         * If so we have to create some structures in our table but
         * this is something we only do the first time we see this
         * packet.
         */
        if (pinfo->desegment_len) {
            if (!PINFO_FD_VISITED(pinfo)) {
                must_desegment = TRUE;
                if (msp)
                    msp->flags &= ~MSP_FLAGS_GOT_ALL_SEGMENTS;
            }

            /*
             * Set "deseg_offset" to the offset in "tvb"
             * of the first byte of data that the
             * subdissector didn't process.
             */
            deseg_offset = offset + pinfo->desegment_offset;
        }

        /* Either no desegmentation is necessary, or this is
         * segment contains the beginning but not the end of
         * a higher-level PDU and thus isn't completely
         * desegmented.
         */
        fh = NULL;
    }

    /* is it completely desegmented? */
    if (fh) {
        /*
         * Yes, we think it is.
         * We only call subdissector for the last segment.
         * Note that the last segment may include more than what
         * we needed.
         */
        if (fh->reassembled_in == pinfo->num) {
            /*
             * OK, this is the last segment.
             * Let's call the subdissector with the desegmented data.
             */

            tvbuff_t *next_tvb = tvb_new_chain(tvb, fh->tvb_data);
            add_new_data_source(pinfo, next_tvb, "Reassembled QUIC");
            stream_info->offset = seq;
            process_quic_stream(next_tvb, 0, pinfo, tree, quic_info, stream_info);
            called_dissector = TRUE;

            int old_len = (int)(tvb_reported_length(next_tvb) - last_fragment_len);
            if (pinfo->desegment_len &&
                pinfo->desegment_offset <= old_len) {
                /*
                 * "desegment_len" isn't 0, so it needs more
                 * data for something - and "desegment_offset"
                 * is before "old_len", so it needs more data
                 * to dissect the stuff we thought was
                 * completely desegmented (as opposed to the
                 * stuff at the beginning being completely
                 * desegmented, but the stuff at the end
                 * being a new higher-level PDU that also
                 * needs desegmentation).
                 */
                fragment_set_partial_reassembly(&quic_reassembly_table,
                                                pinfo, reassembly_id, NULL);

                /* Update msp->nxtpdu to point to the new next
                 * pdu boundary.
                 */
                if (pinfo->desegment_len == DESEGMENT_ONE_MORE_SEGMENT) {
                    /* We want reassembly of at least one
                     * more segment so set the nxtpdu
                     * boundary to one byte into the next
                     * segment.
                     * This means that the next segment
                     * will complete reassembly even if it
                     * is only one single byte in length.
                     * If this is an OoO segment, then increment the MSP end.
                     */
                    msp->nxtpdu = MAX(seq + tvb_reported_length_remaining(tvb, offset), msp->nxtpdu) + 1;
                    msp->flags |= MSP_FLAGS_REASSEMBLE_ENTIRE_SEGMENT;
#if 0
                } else if (pinfo->desegment_len == DESEGMENT_UNTIL_FIN) {
                    tcpd->fwd->flags |= TCP_FLOW_REASSEMBLE_UNTIL_FIN;
#endif
                } else {
                    if (seq + last_fragment_len >= msp->nxtpdu) {
                        /* This is the segment (overlapping) the end of the MSP. */
                        msp->nxtpdu = seq + last_fragment_len + pinfo->desegment_len;
                    } else {
                        /* This is a segment before the end of the MSP, so it
                         * must be an out-of-order segmented that completed the
                         * MSP. The requested additional data is relative to
                         * that end.
                         */
                        msp->nxtpdu += pinfo->desegment_len;
                    }
                }

                /* Since we need at least some more data
                 * there can be no pdu following in the
                 * tail of this segment.
                 */
                another_pdu_follows = 0;
                offset += last_fragment_len;
                seq += last_fragment_len;
                if (tvb_captured_length_remaining(tvb, offset) > 0)
                    goto again;
            } else {
                proto_item *frag_tree_item;
                proto_tree *parent_tree = proto_tree_get_parent(tree);
                show_fragment_tree(fh, &quic_stream_fragment_items,
                        parent_tree, pinfo, next_tvb, &frag_tree_item);
                // TODO move tree item if needed.

                if(pinfo->desegment_len) {
                    if (!PINFO_FD_VISITED(pinfo)) {
                        must_desegment = TRUE;
                        if (msp)
                            msp->flags &= ~MSP_FLAGS_GOT_ALL_SEGMENTS;
                    }
                    /* See packet-tcp.h for details about this. */
                    deseg_offset = fh->datalen - pinfo->desegment_offset;
                    deseg_offset = tvb_reported_length(tvb) - deseg_offset;
                }
            }
        }
    }

    if (must_desegment && !PINFO_FD_VISITED(pinfo)) {
        // TODO handle DESEGMENT_UNTIL_FIN if needed, maybe use the FIN bit?

        guint32 deseg_seq = seq + (deseg_offset - offset);

        if (((nxtseq - deseg_seq) <= 1024*1024)
            && (!PINFO_FD_VISITED(pinfo))) {
            if(pinfo->desegment_len == DESEGMENT_ONE_MORE_SEGMENT) {
                /* The subdissector asked to reassemble using the
                 * entire next segment.
                 * Just ask reassembly for one more byte
                 * but set this msp flag so we can pick it up
                 * above.
                 */
                msp = pdu_store_sequencenumber_of_next_pdu(pinfo, deseg_seq,
                    nxtseq+1, stream->multisegment_pdus);
                msp->flags |= MSP_FLAGS_REASSEMBLE_ENTIRE_SEGMENT;
            } else {
                msp = pdu_store_sequencenumber_of_next_pdu(pinfo,
                    deseg_seq, nxtseq+pinfo->desegment_len, stream->multisegment_pdus);
            }

            /* add this segment as the first one for this new pdu */
            fragment_add(&quic_reassembly_table, tvb, deseg_offset,
                         pinfo, reassembly_id, NULL,
                         0, nxtseq - deseg_seq,
                         nxtseq < msp->nxtpdu);
        }
    }

    if (!called_dissector || pinfo->desegment_len != 0) {
        if (fh != NULL && fh->reassembled_in != 0 &&
            !(fh->flags & FD_PARTIAL_REASSEMBLY)) {
            /*
             * We know what frame this PDU is reassembled in;
             * let the user know.
             */
            proto_item *item = proto_tree_add_uint(tree, hf_quic_reassembled_in, tvb, 0,
                                                   0, fh->reassembled_in);
            proto_item_set_generated(item);
        }
    }
    pinfo->can_desegment = 0;
    pinfo->desegment_offset = 0;
    pinfo->desegment_len = 0;

    if (another_pdu_follows) {
        /* there was another pdu following this one. */
        pinfo->can_desegment = 2;
        offset += another_pdu_follows;
        seq += another_pdu_follows;
        goto again;
    }
}

static void
dissect_quic_stream_payload(tvbuff_t *tvb, int offset, int length, packet_info *pinfo,
                            proto_tree *tree, quic_info_data_t *quic_info,
                            quic_stream_info *stream_info,
                            quic_stream_state *stream)
{
    /* QUIC application data is most likely not properly dissected when
     * reassembly is not enabled. Therefore we do not even offer "desegment"
     * preference to disable reassembly.
     */

    pinfo->can_desegment = 2;
    desegment_quic_stream(tvb, offset, length, pinfo, tree, quic_info, stream_info, stream);
}
/* QUIC Streams tracking and reassembly. }}} */

static void
quic_stream_add_proto_data(packet_info *pinfo, quic_stream_info *stream_info, void *proto_data)
{
    quic_stream_state *stream = quic_get_stream_state(pinfo, stream_info->quic_info, stream_info->from_server, stream_info->stream_id);
    stream->subdissector_private = proto_data;
}

static void *quic_stream_get_proto_data(packet_info *pinfo, quic_stream_info *stream_info)
{
    quic_stream_state *stream = quic_get_stream_state(pinfo, stream_info->quic_info, stream_info->from_server, stream_info->stream_id);
    return stream->subdissector_private;
}

static int
dissect_quic_frame_type(tvbuff_t *tvb, packet_info *pinfo, proto_tree *quic_tree, guint offset, quic_info_data_t *quic_info, gboolean from_server)
{
    proto_item *ti_ft, *ti_ftflags, *ti_ftid, *ti;
    proto_tree *ft_tree, *ftflags_tree, *ftid_tree;
    guint64 frame_type;
    gint32 lenft;
    guint   orig_offset = offset;

    ti_ft = proto_tree_add_item(quic_tree, hf_quic_frame, tvb, offset, 1, ENC_NA);
    ft_tree = proto_item_add_subtree(ti_ft, ett_quic_ft);

    ti_ftflags = proto_tree_add_item_ret_varint(ft_tree, hf_quic_frame_type, tvb, offset, -1, ENC_VARINT_QUIC, &frame_type, &lenft);
    proto_item_set_text(ti_ft, "%s", rval_to_str_const((guint32)frame_type, quic_frame_type_vals, "Unknown"));
    offset += lenft;

    switch(frame_type){
        case FT_PADDING:{
            guint32 pad_len;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", PADDING");

            int end, tvb_len;
            int new_offset = offset;

            /* Get the length remaining */
            tvb_len = tvb_captured_length(tvb);
            end = new_offset + tvb_reported_length_remaining(tvb, offset);

            if (end >= tvb_len)
                end = tvb_len;

            while (new_offset < end) {
                guint8 tempch = tvb_get_guint8(tvb, offset);

                if (tempch != '\0')
                    break;
                new_offset++;
            }

            /* A padding frame consists of a single zero octet, but for brevity
             * sake let's combine multiple zeroes into a single field. */
            pad_len = 1 + new_offset - offset;
            ti = proto_tree_add_uint(ft_tree, hf_quic_padding_length, tvb, offset, 0, pad_len);
            proto_item_set_generated(ti);
            proto_item_append_text(ti_ft, " Length: %u", pad_len);
            offset += pad_len - 1;
        }
        break;
        case FT_PING:{
            col_append_fstr(pinfo->cinfo, COL_INFO, ", PING");
        }
        break;
        case FT_ACK:
        case FT_ACK_ECN:
        case FT_MP_ACK:
        case FT_MP_ACK_ECN:{
            guint64 ack_range_count;
            gint32 lenvar;

            switch(frame_type){
                case FT_ACK:
                    col_append_fstr(pinfo->cinfo, COL_INFO, ", ACK");
                break;
                case FT_ACK_ECN:
                    col_append_fstr(pinfo->cinfo, COL_INFO, ", ACK_ECN");
                break;
                case FT_MP_ACK:
                    col_append_fstr(pinfo->cinfo, COL_INFO, ", MP_ACK");
                    proto_tree_add_item_ret_varint(ft_tree, hf_quic_mp_uniflow_id, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &lenvar);
                    offset += lenvar;
                break;
                case FT_MP_ACK_ECN:
                    col_append_fstr(pinfo->cinfo, COL_INFO, ", MP_ACK_ECN");
                    proto_tree_add_item_ret_varint(ft_tree, hf_quic_mp_uniflow_id, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &lenvar);
                    offset += lenvar;
                break;
            }

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_ack_largest_acknowledged, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &lenvar);
            offset += lenvar;

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_ack_ack_delay, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &lenvar);
            offset += lenvar;

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_ack_ack_range_count, tvb, offset, -1, ENC_VARINT_QUIC, &ack_range_count, &lenvar);
            offset += lenvar;

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_ack_first_ack_range, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &lenvar);
            offset += lenvar;

            /* ACK Ranges - Repeated "Ack Range Count" */
            while (ack_range_count) {

                /* Gap To Next Block */
                proto_tree_add_item_ret_varint(ft_tree, hf_quic_ack_gap, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &lenvar);
                offset += lenvar;

                proto_tree_add_item_ret_varint(ft_tree, hf_quic_ack_ack_range, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &lenvar);
                offset += lenvar;

                ack_range_count--;
            }

            /* ECN Counts. */
            if (frame_type == FT_ACK_ECN) {
                proto_tree_add_item_ret_varint(ft_tree, hf_quic_ack_ect0_count, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &lenvar);
                offset += lenvar;

                proto_tree_add_item_ret_varint(ft_tree, hf_quic_ack_ect1_count, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &lenvar);
                offset += lenvar;

                proto_tree_add_item_ret_varint(ft_tree, hf_quic_ack_ecn_ce_count, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &lenvar);
                offset += lenvar;
            }
        }
        break;
        case FT_RESET_STREAM:{
            guint64 stream_id, error_code;
            gint32 len_streamid = 0, len_finalsize = 0, len_error_code = 0;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", RS");

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_rsts_stream_id, tvb, offset, -1, ENC_VARINT_QUIC, &stream_id, &len_streamid);
            offset += len_streamid;

            proto_item_append_text(ti_ft, " id=%" G_GINT64_MODIFIER "u", stream_id);
            col_append_fstr(pinfo->cinfo, COL_INFO, "(%" G_GINT64_MODIFIER "u)", stream_id);

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_rsts_application_error_code, tvb, offset, -1, ENC_VARINT_QUIC, &error_code, &len_error_code);
            offset += len_error_code;

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_rsts_final_size, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &len_finalsize);
            offset += len_finalsize;

            proto_item_append_text(ti_ft, " Error code: %#" G_GINT64_MODIFIER "x", error_code);
        }
        break;
        case FT_STOP_SENDING:{
            gint32 len_streamid;
            guint64 stream_id, error_code;
            gint32 len_error_code = 0;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", SS");

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_ss_stream_id, tvb, offset, -1, ENC_VARINT_QUIC, &stream_id, &len_streamid);
            offset += len_streamid;

            proto_item_append_text(ti_ft, " id=%" G_GINT64_MODIFIER "u", stream_id);
            col_append_fstr(pinfo->cinfo, COL_INFO, "(%" G_GINT64_MODIFIER "u)", stream_id);

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_ss_application_error_code, tvb, offset, -1, ENC_VARINT_QUIC, &error_code, &len_error_code);
            offset += len_error_code;

            proto_item_append_text(ti_ft, " Error code: %#" G_GINT64_MODIFIER "x", error_code);
        }
        break;
        case FT_CRYPTO: {
            guint64 crypto_offset, crypto_length;
            gint32 lenvar;
            col_append_fstr(pinfo->cinfo, COL_INFO, ", CRYPTO");
            proto_tree_add_item_ret_varint(ft_tree, hf_quic_crypto_offset, tvb, offset, -1, ENC_VARINT_QUIC, &crypto_offset, &lenvar);
            offset += lenvar;
            proto_tree_add_item_ret_varint(ft_tree, hf_quic_crypto_length, tvb, offset, -1, ENC_VARINT_QUIC, &crypto_length, &lenvar);
            offset += lenvar;
            proto_tree_add_item(ft_tree, hf_quic_crypto_crypto_data, tvb, offset, (guint32)crypto_length, ENC_NA);
            {
                tvbuff_t *next_tvb = tvb_new_subset_length(tvb, offset, (int)crypto_length);
                col_set_writable(pinfo->cinfo, -1, FALSE);
                /*
                 * Dissect TLS handshake record. The Client/Server Hello (CH/SH)
                 * are contained in the Initial Packet. 0-RTT keys are ready
                 * after CH. HS + 1-RTT keys are ready after SH.
                 * (Note: keys captured from the client might become available
                 * after capturing the packets due to processing delay.)
                 * These keys will be loaded in the first HS/0-RTT/1-RTT msg.
                 */
                call_dissector_with_data(tls13_handshake_handle, next_tvb, pinfo, ft_tree, GUINT_TO_POINTER(crypto_offset));
                col_set_writable(pinfo->cinfo, -1, TRUE);
            }
            offset += (guint32)crypto_length;
        }
        break;
        case FT_NEW_TOKEN: {
            guint64 token_length;
            gint32 lenvar;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", NT");

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_nt_length, tvb, offset, -1, ENC_VARINT_QUIC, &token_length, &lenvar);
            offset += lenvar;

            proto_tree_add_item(ft_tree, hf_quic_nt_token, tvb, offset, (guint32)token_length, ENC_NA);
            offset += (guint32)token_length;
        }
        break;
        case FT_STREAM_8:
        case FT_STREAM_9:
        case FT_STREAM_A:
        case FT_STREAM_B:
        case FT_STREAM_C:
        case FT_STREAM_D:
        case FT_STREAM_E:
        case FT_STREAM_F: {
            guint64 stream_id, stream_offset = 0, length;
            gint32 lenvar;

            offset -= 1;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", STREAM");

            ftflags_tree = proto_item_add_subtree(ti_ftflags, ett_quic_ftflags);
            proto_tree_add_item(ftflags_tree, hf_quic_stream_fin, tvb, offset, 1, ENC_NA);
            proto_tree_add_item(ftflags_tree, hf_quic_stream_len, tvb, offset, 1, ENC_NA);
            proto_tree_add_item(ftflags_tree, hf_quic_stream_off, tvb, offset, 1, ENC_NA);
            offset += 1;

            ti_ftid = proto_tree_add_item_ret_varint(ft_tree, hf_quic_stream_stream_id, tvb, offset, -1, ENC_VARINT_QUIC, &stream_id, &lenvar);
            ftid_tree = proto_item_add_subtree(ti_ftid, ett_quic_ftid);
            proto_tree_add_item_ret_varint(ftid_tree, hf_quic_stream_initiator, tvb, offset, -1, ENC_VARINT_QUIC, NULL, NULL);
            proto_tree_add_item_ret_varint(ftid_tree, hf_quic_stream_direction, tvb, offset, -1, ENC_VARINT_QUIC, NULL, NULL);
            offset += lenvar;

            proto_item_append_text(ti_ft, " id=%" G_GINT64_MODIFIER "u", stream_id);
            col_append_fstr(pinfo->cinfo, COL_INFO, "(%" G_GINT64_MODIFIER "u)", stream_id);

            proto_item_append_text(ti_ft, " fin=%d", !!(frame_type & FTFLAGS_STREAM_FIN));

            if (!PINFO_FD_VISITED(pinfo)) {
                quic_streams_add(pinfo, quic_info, stream_id);
            }

            if (frame_type & FTFLAGS_STREAM_OFF) {
                proto_tree_add_item_ret_varint(ft_tree, hf_quic_stream_offset, tvb, offset, -1, ENC_VARINT_QUIC, &stream_offset, &lenvar);
                offset += lenvar;
            }
            proto_item_append_text(ti_ft, " off=%" G_GINT64_MODIFIER "u", stream_offset);

            if (frame_type & FTFLAGS_STREAM_LEN) {
                proto_tree_add_item_ret_varint(ft_tree, hf_quic_stream_length, tvb, offset, -1, ENC_VARINT_QUIC, &length, &lenvar);
                offset += lenvar;
            } else {
                length = tvb_reported_length_remaining(tvb, offset);
            }
            proto_item_append_text(ti_ft, " len=%" G_GINT64_MODIFIER "u dir=%s origin=%s", length,
                                   val64_to_str_const(!!(stream_id & FTFLAGS_STREAM_DIRECTION), quic_frame_id_direction, "unknown"),
                                   val64_to_str_const(!!(stream_id & FTFLAGS_STREAM_INITIATOR), quic_frame_id_initiator, "unknown"));

            proto_tree_add_item(ft_tree, hf_quic_stream_data, tvb, offset, (int)length, ENC_NA);
            if (have_tap_listener(quic_plain_follow_tap)) {
                quic_follow_tap_data_t *follow_data = wmem_new0(wmem_packet_scope(), quic_follow_tap_data_t);

                follow_data->tvb = tvb_new_subset_remaining(tvb, offset);
                follow_data->stream_id = stream_id;

                tap_queue_packet(quic_plain_follow_tap, pinfo, follow_data);
            }
            quic_stream_state *stream = quic_get_stream_state(pinfo, quic_info, from_server, stream_id);
            quic_stream_info stream_info = {
                .stream_id = stream_id,
                .stream_offset = stream_offset,
                .quic_info = quic_info,
                .from_server = from_server,
            };
            dissect_quic_stream_payload(tvb, offset, (int)length, pinfo, ft_tree, quic_info, &stream_info, stream);
            offset += (int)length;
        }
        break;
        case FT_MAX_DATA:{
            gint32 len_maximumdata;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", MD");

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_md_maximum_data, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &len_maximumdata);
            offset += len_maximumdata;
        }
        break;
        case FT_MAX_STREAM_DATA:{
            gint32 len_streamid, len_maximumstreamdata;
            guint64 stream_id;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", MSD");

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_msd_stream_id, tvb, offset, -1, ENC_VARINT_QUIC, &stream_id, &len_streamid);
            offset += len_streamid;

            proto_item_append_text(ti_ft, " id=%" G_GINT64_MODIFIER "u", stream_id);
            col_append_fstr(pinfo->cinfo, COL_INFO, "(%" G_GINT64_MODIFIER "u)", stream_id);

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_msd_maximum_stream_data, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &len_maximumstreamdata);
            offset += len_maximumstreamdata;
        }
        break;
        case FT_MAX_STREAMS_BIDI:
        case FT_MAX_STREAMS_UNI:{
            gint32 len_streamid;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", MS");

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_ms_max_streams, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &len_streamid);
            offset += len_streamid;
        }
        break;
        case FT_DATA_BLOCKED:{
            gint32 len_offset;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", DB");

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_db_stream_data_limit, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &len_offset);
            offset += len_offset;
        }
        break;
        case FT_STREAM_DATA_BLOCKED:{
            gint32 len_streamid, len_offset;
            guint64 stream_id;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", SDB");

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_sdb_stream_id, tvb, offset, -1, ENC_VARINT_QUIC, &stream_id, &len_streamid);
            offset += len_streamid;

            proto_item_append_text(ti_ft, " id=%" G_GINT64_MODIFIER "u", stream_id);
            col_append_fstr(pinfo->cinfo, COL_INFO, "(%" G_GINT64_MODIFIER "u)", stream_id);

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_sdb_stream_data_limit, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &len_offset);
            offset += len_offset;
        }
        break;
        case FT_STREAMS_BLOCKED_BIDI:
        case FT_STREAMS_BLOCKED_UNI:{
            gint32 len_streamid;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", SB");

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_sb_stream_limit, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &len_streamid);
            offset += len_streamid;
        }
        break;
        case FT_NEW_CONNECTION_ID:
        case FT_MP_NEW_CONNECTION_ID:{
            gint32 len_sequence;
            gint32 len_retire_prior_to;
            gint32 nci_length;
            gint32 lenvar = 0;
            gboolean valid_cid = FALSE;

            switch(frame_type){
                case FT_NEW_CONNECTION_ID:
                    col_append_fstr(pinfo->cinfo, COL_INFO, ", NCI");
                 break;
                case FT_MP_NEW_CONNECTION_ID:
                    col_append_fstr(pinfo->cinfo, COL_INFO, ", MP_NCI");
                    proto_tree_add_item_ret_varint(ft_tree, hf_quic_mp_uniflow_id, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &lenvar);
                    offset += lenvar;
                 break;
            }

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_nci_sequence, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &len_sequence);
            offset += len_sequence;

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_nci_retire_prior_to, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &len_retire_prior_to);
            offset += len_retire_prior_to;

            ti = proto_tree_add_item_ret_uint(ft_tree, hf_quic_nci_connection_id_length, tvb, offset, 1, ENC_BIG_ENDIAN, &nci_length);
            offset++;

            valid_cid = nci_length >= 1 && nci_length <= QUIC_MAX_CID_LENGTH;
            if (!valid_cid) {
                expert_add_info_format(pinfo, ti, &ei_quic_protocol_violation,
                            "Connection ID Length must be between 1 and %d bytes", QUIC_MAX_CID_LENGTH);
            }

            proto_tree_add_item(ft_tree, hf_quic_nci_connection_id, tvb, offset, nci_length, ENC_NA);
            if (valid_cid && quic_info) {
                quic_cid_t cid = {.len=0};
                tvb_memcpy(tvb, cid.cid, offset, nci_length);
                cid.len = nci_length;
                quic_connection_add_cid(quic_info, &cid, from_server);
            }
            offset += nci_length;

            proto_tree_add_item(ft_tree, hf_quic_nci_stateless_reset_token, tvb, offset, 16, ENC_NA);
            offset += 16;
        }
        break;
        case FT_RETIRE_CONNECTION_ID:
        case FT_MP_RETIRE_CONNECTION_ID:{
            gint32 len_sequence;
            gint32 lenvar;

            switch(frame_type){
                case FT_RETIRE_CONNECTION_ID:
                    col_append_fstr(pinfo->cinfo, COL_INFO, ", RC");
                break;
                case FT_MP_RETIRE_CONNECTION_ID:
                    col_append_fstr(pinfo->cinfo, COL_INFO, ", MP_RC");
                    proto_tree_add_item_ret_varint(ft_tree, hf_quic_mp_uniflow_id, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &lenvar);
                    offset += lenvar;
                break;
            }

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_rci_sequence, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &len_sequence);
            offset += len_sequence;
        }
        break;
        case FT_PATH_CHALLENGE:{
            col_append_fstr(pinfo->cinfo, COL_INFO, ", PC");

            proto_tree_add_item(ft_tree, hf_quic_path_challenge_data, tvb, offset, 8, ENC_NA);
            offset += 8;
        }
        break;
        case FT_PATH_RESPONSE:{
            col_append_fstr(pinfo->cinfo, COL_INFO, ", PR");

            proto_tree_add_item(ft_tree, hf_quic_path_response_data, tvb, offset, 8, ENC_NA);
            offset += 8;
        }
        break;
        case FT_CONNECTION_CLOSE_TPT:
        case FT_CONNECTION_CLOSE_APP:{
            gint32 len_reasonphrase, len_frametype, len_error_code;
            guint64 len_reason = 0;
            guint64 error_code;
            const char *tls_alert = NULL;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", CC");

            if (frame_type == FT_CONNECTION_CLOSE_TPT) {
                proto_tree_add_item_ret_varint(ft_tree, hf_quic_cc_error_code, tvb, offset, -1, ENC_VARINT_QUIC, &error_code, &len_error_code);
                offset += len_error_code;

                proto_tree_add_item_ret_varint(ft_tree, hf_quic_cc_frame_type, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &len_frametype);
                offset += len_frametype;
            } else { /* FT_CONNECTION_CLOSE_APP) */
                proto_tree_add_item_ret_varint(ft_tree, hf_quic_cc_error_code_app, tvb, offset, -1, ENC_VARINT_QUIC, &error_code, &len_error_code);
                offset += len_error_code;
            }


            proto_tree_add_item_ret_varint(ft_tree, hf_quic_cc_reason_phrase_length, tvb, offset, -1, ENC_VARINT_QUIC, &len_reason, &len_reasonphrase);
            offset += len_reasonphrase;

            proto_tree_add_item(ft_tree, hf_quic_cc_reason_phrase, tvb, offset, (guint32)len_reason, ENC_ASCII|ENC_NA);
            offset += (guint32)len_reason;

            // Transport Error codes higher than 0x3fff are for Private Use.
            if (frame_type == FT_CONNECTION_CLOSE_TPT && error_code <= 0x3fff) {
                proto_item_append_text(ti_ft, " Error code: %s", rval_to_str((guint32)error_code, quic_transport_error_code_vals, "Unknown (%d)"));
            } else {
                proto_item_append_text(ti_ft, " Error code: %#" G_GINT64_MODIFIER "x", error_code);
            }
            if (tls_alert) {
                proto_item_append_text(ti_ft, " (%s)", tls_alert);
            }
        }
        break;
        case FT_HANDSHAKE_DONE:
            col_append_fstr(pinfo->cinfo, COL_INFO, ", DONE");
        break;
        case FT_DATAGRAM:
        case FT_DATAGRAM_LENGTH:{
            gint32 dg_length;
            guint64 length;
            col_append_fstr(pinfo->cinfo, COL_INFO, ", DG");
            if (frame_type == FT_DATAGRAM_LENGTH) {

                proto_tree_add_item_ret_varint(ft_tree, hf_quic_dg_length, tvb, offset, -1, ENC_VARINT_QUIC, &length, &dg_length);
                offset += dg_length;
            } else {
                length = (guint32) tvb_reported_length_remaining(tvb, offset);
            }
            proto_tree_add_item(ft_tree, hf_quic_dg, tvb, offset, (guint32)length, ENC_NA);
            offset += (guint32)length;
        }
        break;
        case FT_ACK_FREQUENCY:{
            gint32 length;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", ACK_FREQ");
            proto_tree_add_item_ret_varint(ft_tree, hf_quic_af_sequence_number, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &length);
            offset += (guint32)length;

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_af_packet_tolerance, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &length);
            offset += (guint32)length;

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_af_update_max_ack_delay, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &length);
            offset += (guint32)length;
        }
        break;
        case FT_TIME_STAMP:{
            gint32 length;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", TS");
            proto_tree_add_item_ret_varint(ft_tree, hf_quic_ts, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &length);
            offset += (guint32)length;

        }
        break;
        case FT_ADD_ADDRESS:{
            gint32 length;
            guint64 config_bits;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", ADD_ADDRESS");

            static int * const config_fields[] = {
                &hf_quic_mp_add_address_reserved,
                &hf_quic_mp_add_address_port_present,
                &hf_quic_mp_add_address_ip_version,
                NULL
            };

            proto_tree_add_bitmask_ret_uint64(ft_tree, tvb, offset, hf_quic_mp_add_address_first_byte, ett_quic, config_fields, ENC_BIG_ENDIAN, &config_bits);
            offset += 1;

            proto_tree_add_item(ft_tree, hf_quic_mp_add_address_id, tvb, offset, 1, ENC_NA);
            offset += 1;

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_mp_add_address_sq_number, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &length);
            offset += (guint32)length;

            proto_tree_add_item(ft_tree, hf_quic_mp_add_address_interface_type, tvb, offset, 1, ENC_NA);
            offset += 1;

            if ((config_bits & 0x06) == 0x06) {
                ws_in6_addr addr;
                tvb_get_ipv6(tvb, offset, &addr);
                proto_tree_add_ipv6(ft_tree, hf_quic_mp_add_address_ip_address_v6, tvb, offset, 16, &addr);
                offset += 16;
            } else {
                guint32 ip_config = tvb_get_ipv4(tvb, offset);
                proto_tree_add_ipv4(ft_tree, hf_quic_mp_add_address_ip_address, tvb, offset, 4, ip_config);
                offset += 4;
            }

            if ((config_bits & 0x10 ) == 0x10) {
                proto_tree_add_item(ft_tree, hf_quic_mp_add_address_port, tvb, offset, 2, ENC_NA);
                offset += 2;
            }
        }
        break;
        case FT_REMOVE_ADDRESS:{
            gint32 length;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", REMOVE_ADDRESS");

            proto_tree_add_item(ft_tree, hf_quic_mp_add_address_id, tvb, offset, 1, ENC_NA);
            offset += 1;

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_mp_add_address_sq_number, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &length);
            offset += (guint32)length;
        }
        break;
        case FT_UNIFLOWS:{
            gint32 length;
            gint32 len_receiving_uniflows;
            gint32 len_active_sending_uniflows;
            gint32 len_uniflow_id;

            guint64 ret_receiving_uniflows;
            guint64 ret_active_sending_uniflows;

            col_append_fstr(pinfo->cinfo, COL_INFO, ", UNIFLOWS");

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_mp_add_address_sq_number, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &length);
            offset += (guint32)length;

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_mp_receiving_uniflows, tvb, offset, -1, ENC_VARINT_QUIC, &ret_receiving_uniflows, &len_receiving_uniflows);
            offset += (guint32)len_receiving_uniflows;

            proto_tree_add_item_ret_varint(ft_tree, hf_quic_mp_active_sending_uniflows, tvb, offset, -1, ENC_VARINT_QUIC, &ret_active_sending_uniflows, &len_active_sending_uniflows);
            offset += (guint32)len_active_sending_uniflows;

            proto_item *receiving_uniflows_ft;
            proto_tree *receiving_uniflows_tree;

            receiving_uniflows_ft = proto_tree_add_item(ft_tree, hf_quic_mp_receiving_uniflow_info_section , tvb, offset, 1, ENC_NA);
            receiving_uniflows_tree = proto_item_add_subtree(receiving_uniflows_ft, ett_quic_ft);

            for (guint64 i = 0; i < ret_receiving_uniflows; i++) {
                proto_item *item_ft;
                proto_tree *item_tree;

                item_ft = proto_tree_add_item(receiving_uniflows_tree, hf_quic_mp_uniflow_info_section, tvb, offset, 1, ENC_NA);
                item_tree = proto_item_add_subtree(item_ft, ett_quic_ft);

                len_uniflow_id = 0;

                proto_tree_add_item_ret_varint(item_tree, hf_quic_mp_uniflow_id, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &len_uniflow_id);
                offset += (guint32)len_uniflow_id;

                proto_tree_add_item(item_tree, hf_quic_mp_add_local_address_id , tvb, offset, 1, ENC_NA);
                offset += 1;
            }

            proto_item *active_sending_uniflows_ft;
            proto_tree *active_sending_uniflows_tree;

            active_sending_uniflows_ft = proto_tree_add_item(ft_tree, hf_quic_mp_active_sending_uniflows_info_section, tvb, offset, 1, ENC_NA);
            active_sending_uniflows_tree = proto_item_add_subtree(active_sending_uniflows_ft, ett_quic_ft);

            for (guint64 i = 0; i < ret_active_sending_uniflows; i++) {
                proto_item *item_ft;
                proto_tree *item_tree;

                item_ft = proto_tree_add_item(active_sending_uniflows_tree, hf_quic_mp_uniflow_info_section, tvb, offset, 1, ENC_NA);
                item_tree = proto_item_add_subtree(item_ft, ett_quic_ft);

                len_uniflow_id = 0;

                proto_tree_add_item_ret_varint(item_tree, hf_quic_mp_uniflow_id, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &len_uniflow_id);
                offset += (guint32)len_uniflow_id;

                proto_tree_add_item(item_tree, hf_quic_mp_add_local_address_id , tvb, offset, 1, ENC_NA);
                offset += 1;
            }
        }
        break;
        default:
            expert_add_info_format(pinfo, ti_ft, &ei_quic_ft_unknown, "Unknown Frame Type %#" G_GINT64_MODIFIER "x", frame_type);
        break;
    }

    proto_item_set_len(ti_ft, offset - orig_offset);

    return offset;
}

// static gboolean
// quic_hp_cipher_init(quic_hp_cipher *hp_cipher, int hash_algo, guint8 key_length, guint8 *secret);
// static gboolean
// quic_pp_cipher_init(quic_pp_cipher *pp_cipher, int hash_algo, guint8 key_length, guint8 *secret);


/**
 * Given a QUIC message (header + non-empty payload), the actual packet number,
 * try to decrypt it using the PP cipher.
 * As the header points to the original buffer with an encrypted packet number,
 * the (encrypted) packet number length is also included.
 *
 * The actual packet number must be constructed according to
 * https://tools.ietf.org/html/draft-ietf-quic-transport-22#section-12.3
 */
static void
quic_decrypt_message(tvbuff_t *head, guint header_length, guint pkn_len, quic_decrypt_result_t *result)
{
    guint8         *buffer;
    guint           buffer_length;

    DISSECTOR_ASSERT(pkn_len < header_length);
    DISSECTOR_ASSERT(1 <= pkn_len && pkn_len <= 4);

    /* Input is "header || ciphertext (buffer) || auth tag (16 bytes)" */
    buffer_length = tvb_captured_length_remaining(head, header_length);

    buffer = (guint8 *)tvb_memdup(wmem_file_scope(), head, header_length, buffer_length);

    result->error = NULL;
    result->data = buffer;
    result->data_len = buffer_length;
}

/**
 * Process (protected) payload, adding the encrypted payload to the tree. If
 * decryption is possible, frame dissection is also attempted.
 *
 * The given offset must correspond to the end of the QUIC header and begin of
 * the (protected) payload. Dissected frames are appended to "tree" and expert
 * info is attached to "ti" (the field with the encrypted payload).
 */
static void
quic_process_payload(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, proto_item *ti, guint offset,
                     quic_info_data_t *quic_info, quic_packet_info_t *quic_packet, gboolean from_server,
                     guint pkn_len)
{
    quic_decrypt_result_t *decryption = &quic_packet->decryption;

    /*
     * If no decryption error has occurred yet, try decryption on the first
     * pass and store the result for later use.
     */
    if (!PINFO_FD_VISITED(pinfo)) {
        if (!quic_packet->decryption.error) {
            quic_decrypt_message(tvb, offset, pkn_len, &quic_packet->decryption);
        }
    }

    if (decryption->error) {
        expert_add_info_format(pinfo, ti, &ei_quic_decryption_failed,
                               "Decryption failed: %s", decryption->error);
    } else if (decryption->data_len) {
        tvbuff_t *decrypted_tvb = tvb_new_child_real_data(tvb, decryption->data,
                decryption->data_len, decryption->data_len);
        add_new_data_source(pinfo, decrypted_tvb, "Decrypted QUIC");

        guint decrypted_offset = 0;
        while (tvb_reported_length_remaining(decrypted_tvb, decrypted_offset) > 0) {
            decrypted_offset = dissect_quic_frame_type(decrypted_tvb, pinfo, tree, decrypted_offset, quic_info, from_server);
        }
    } else if (quic_info->skip_decryption) {
        expert_add_info_format(pinfo, ti, &ei_quic_decryption_failed,
                               "Decryption skipped because keys are not available.");
    }
}

static void
quic_verify_retry_token(tvbuff_t *tvb, quic_packet_info_t *quic_packet, const quic_cid_t *odcid, guint32 version)
{
    quic_packet->retry_integrity_success = TRUE;
}


static void
quic_add_connection(packet_info *pinfo, const quic_cid_t *cid)
{
    quic_datagram *dgram_info;

    dgram_info = (quic_datagram *)p_get_proto_data(wmem_file_scope(), pinfo, proto_quic_plain, 0);
    if (dgram_info && dgram_info->conn) {
        quic_connection_add_cid(dgram_info->conn, cid, dgram_info->from_server);
    }
}

static void
quic_add_loss_bits(packet_info *pinfo, guint64 value)
{
    quic_datagram *dgram_info;
    quic_info_data_t *conn;

    dgram_info = (quic_datagram *)p_get_proto_data(wmem_file_scope(), pinfo, proto_quic_plain, 0);
    if (dgram_info && dgram_info->conn) {
        conn = dgram_info->conn;
        if (dgram_info->from_server) {
            conn->server_loss_bits_recv = TRUE;
            if (value == 1) {
                conn->server_loss_bits_send = TRUE;
            }
        } else {
            conn->client_loss_bits_recv = TRUE;
            if (value == 1) {
                conn->client_loss_bits_send = TRUE;
            }
        }
    }
}

static void
quic_add_connection_info(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, quic_info_data_t *conn)
{
    proto_tree         *ctree;
    proto_item         *pi;

    ctree = proto_tree_add_subtree(tree, tvb, 0, 0, ett_quic_connection_info, NULL, "QUIC Connection information");
    if (!conn) {
        expert_add_info(pinfo, ctree, &ei_quic_connection_unknown);
        return;
    }

    pi = proto_tree_add_uint(ctree, hf_quic_connection_number, tvb, 0, 0, conn->number);
    proto_item_set_generated(pi);
#if 0
    proto_tree_add_debug_text(ctree, "Client CID: %s", cid_to_string(&conn->client_cids.data));
    proto_tree_add_debug_text(ctree, "Server CID: %s", cid_to_string(&conn->server_cids.data));
    // Note: for Retry, this value has been cleared before.
    proto_tree_add_debug_text(ctree, "InitialCID: %s", cid_to_string(&conn->client_dcid_initial));
#endif
}

/**
 * Dissects the common part after the first byte for packets using the Long
 * Header form.
 */
static int
dissect_quic_long_header_common(tvbuff_t *tvb, packet_info *pinfo, proto_tree *quic_tree,
                                guint offset, const quic_packet_info_t *quic_packet _U_,
                                guint32 *version_out, quic_cid_t *dcid, quic_cid_t *scid)
{
    guint32     version;
    guint32     dcil, scil;
    proto_item  *ti;

    version = tvb_get_ntohl(tvb, offset);

    if (version_out) {
        *version_out = version;
    }

    ti = proto_tree_add_item(quic_tree, hf_quic_version, tvb, offset, 4, ENC_BIG_ENDIAN);
    if ((version & 0x0F0F0F0F) == 0x0a0a0a0a) {
        proto_item_append_text(ti, " (Forcing Version Negotiation)");
    }
    offset += 4;

    proto_tree_add_item_ret_uint(quic_tree, hf_quic_dcil, tvb, offset, 1, ENC_BIG_ENDIAN, &dcil);
    offset++;
    if (dcil) {
        proto_tree_add_item(quic_tree, hf_quic_dcid, tvb, offset, dcil, ENC_NA);
        // TODO expert info on CID mismatch with connection
        if (dcil <= QUIC_MAX_CID_LENGTH) {
            tvb_memcpy(tvb, dcid->cid, offset, dcil);
            dcid->len = dcil;
        }
        offset += dcil;
    }

    proto_tree_add_item_ret_uint(quic_tree, hf_quic_scil, tvb, offset, 1, ENC_BIG_ENDIAN, &scil);
    offset++;
    if (scil) {
        proto_tree_add_item(quic_tree, hf_quic_scid, tvb, offset, scil, ENC_NA);
        // TODO expert info on CID mismatch with connection
        if (scil <= QUIC_MAX_CID_LENGTH) {
            tvb_memcpy(tvb, scid->cid, offset, scil);
            scid->len = scil;
        }
        offset += scil;
    }

    if (dcid->len > 0) {
        col_append_fstr(pinfo->cinfo, COL_INFO, ", DCID=%s", cid_to_string(dcid));
    }
    if (scid->len > 0) {
        col_append_fstr(pinfo->cinfo, COL_INFO, ", SCID=%s", cid_to_string(scid));
    }
    return offset;
}

/* Retry Packet dissection */
static int
dissect_quic_retry_packet(tvbuff_t *tvb, packet_info *pinfo, proto_tree *quic_tree,
                          quic_datagram *dgram_info _U_, quic_packet_info_t *quic_packet,
                          const quic_cid_t *odcid)
{
    guint       offset = 0;
    guint32     version;
    quic_cid_t  dcid = {.len=0}, scid = {.len=0};
    guint32     odcil = 0;
    guint       retry_token_len;
    proto_item *ti;

    proto_tree_add_item(quic_tree, hf_quic_long_packet_type, tvb, offset, 1, ENC_NA);
    offset += 1;
    col_set_str(pinfo->cinfo, COL_INFO, "Retry");

    offset = dissect_quic_long_header_common(tvb, pinfo, quic_tree, offset, quic_packet, &version, &dcid, &scid);

    if (is_quic_draft_max(version, 24)) {
        proto_tree_add_item_ret_uint(quic_tree, hf_quic_odcil, tvb, offset, 1, ENC_NA, &odcil);
        offset++;
        proto_tree_add_item(quic_tree, hf_quic_odcid, tvb, offset, odcil, ENC_NA);
        offset += odcil;
    }

    retry_token_len = tvb_reported_length_remaining(tvb, offset);
    // Remove length of Retry Integrity Tag
    if (!is_quic_draft_max(version, 24) && retry_token_len >= 16) {
        retry_token_len -= 16;
    }
    proto_tree_add_item(quic_tree, hf_quic_retry_token, tvb, offset, retry_token_len, ENC_NA);
    offset += retry_token_len;

    if (!is_quic_draft_max(version, 24)) {
        // Verify the Retry Integrity Tag according to
        // https://tools.ietf.org/html/draft-ietf-quic-tls-25#section-5.8
        ti = proto_tree_add_item(quic_tree, hf_quic_retry_integrity_tag, tvb, offset, 16, ENC_NA);
        if (!PINFO_FD_VISITED(pinfo) && odcid) {
            // Skip validation if the Initial Packet is unknown, for example due
            // to packet loss in the capture file.
            quic_verify_retry_token(tvb, quic_packet, odcid, version);
        }
        if (quic_packet->retry_integrity_failure) {
            expert_add_info(pinfo, ti, &ei_quic_bad_retry);
        } else if (!quic_packet->retry_integrity_success) {
            expert_add_info_format(pinfo, ti, &ei_quic_bad_retry,
                    "Cannot verify Retry Packet due to unknown ODCID");
        } else {
            proto_item_append_text(ti, " [verified]");
        }
        (void)odcid;
        expert_add_info_format(pinfo, ti, &ei_quic_bad_retry,
                "Libgcrypt >= 1.6.0 is required for Retry Packet verification");
        offset += 16;
    }

    return offset;
}

static int
dissect_quic_long_header(tvbuff_t *tvb, packet_info *pinfo, proto_tree *quic_tree,
                         quic_datagram *dgram_info, quic_packet_info_t *quic_packet)
{
    guint offset = 0;
    guint8 long_packet_type;
    guint32 version;
    quic_cid_t  dcid = {.len=0}, scid = {.len=0};
    gint32 len_token_length;
    guint64 token_length;
    gint32 len_payload_length;
    guint64 payload_length;
    guint8  first_byte = 0;
    quic_info_data_t *conn = dgram_info->conn;

    const gboolean from_server = dgram_info->from_server;
    // quic_ciphers *ciphers = NULL;
    proto_item *ti;


    quic_extract_header(tvb, &long_packet_type, &version, &dcid, &scid);

#define DIGEST_MIN_SIZE 32  /* SHA256 */
#define DIGEST_MAX_SIZE 48  /* SHA384 */
        const gchar *error = NULL;
        if (!error) {
            guint32 pkn32 = 0;
            // PKN is after type(1) + version(4) + DCIL+DCID + SCIL+SCID
            guint pn_offset = 1 + 4 + 1 + dcid.len + 1 + scid.len;
            if (long_packet_type == QUIC_LPT_INITIAL) {
                pn_offset += tvb_get_varint(tvb, pn_offset, 8, &token_length, ENC_VARINT_QUIC);
                pn_offset += (guint)token_length;
            }
            pn_offset += tvb_get_varint(tvb, pn_offset, 8, &payload_length, ENC_VARINT_QUIC);

            // Assume failure unless proven otherwise.
            error = "Header deprotection failed";
                if (quic_decrypt_header(tvb, pn_offset, &first_byte, &pkn32)) {
                    error = NULL;
                }
            if (!error) {
                quic_set_full_packet_number(conn, quic_packet, from_server, first_byte, pkn32);
                quic_packet->first_byte = first_byte;
            }
        }
        if (error) {
            quic_packet->decryption.error = wmem_strdup(wmem_file_scope(), error);
        }

    proto_tree_add_item(quic_tree, hf_quic_fixed_bit, tvb, offset, 1, ENC_NA);
    proto_tree_add_item(quic_tree, hf_quic_long_packet_type, tvb, offset, 1, ENC_NA);
    if (quic_packet->pkn_len) {
        proto_tree_add_uint(quic_tree, hf_quic_long_reserved, tvb, offset, 1, first_byte);
        proto_tree_add_uint(quic_tree, hf_quic_packet_number_length, tvb, offset, 1, first_byte);
    }
    offset += 1;
    col_set_str(pinfo->cinfo, COL_INFO, val_to_str_const(long_packet_type, quic_long_packet_type_vals, "Long Header"));

    offset = dissect_quic_long_header_common(tvb, pinfo, quic_tree, offset, quic_packet, NULL, &dcid, &scid);

    if (long_packet_type == QUIC_LPT_INITIAL) {
        proto_tree_add_item_ret_varint(quic_tree, hf_quic_token_length, tvb, offset, -1, ENC_VARINT_QUIC, &token_length, &len_token_length);
        offset += len_token_length;

        if (token_length) {
            proto_tree_add_item(quic_tree, hf_quic_token, tvb, offset, (guint32)token_length, ENC_NA);
            offset += (guint)token_length;
        }
    }

    proto_tree_add_item_ret_varint(quic_tree, hf_quic_length, tvb, offset, -1, ENC_VARINT_QUIC, &payload_length, &len_payload_length);
    offset += len_payload_length;

    if (quic_packet->decryption.error) {
        expert_add_info_format(pinfo, quic_tree, &ei_quic_decryption_failed,
                               "Failed to create decryption context: %s", quic_packet->decryption.error);
        return offset;
    }
    if (!conn || quic_packet->pkn_len == 0) {
        expert_add_info_format(pinfo, quic_tree, &ei_quic_decryption_failed, "Failed to decrypt packet number");
        return offset;
    }

    proto_tree_add_uint64(quic_tree, hf_quic_packet_number, tvb, offset, quic_packet->pkn_len, quic_packet->packet_number);
    offset += quic_packet->pkn_len;
    col_append_fstr(pinfo->cinfo, COL_INFO, ", PKN: %" G_GINT64_MODIFIER "u", quic_packet->packet_number);

    /* Payload */
    ti = proto_tree_add_item(quic_tree, hf_quic_payload, tvb, offset, -1, ENC_NA);

    if (conn) {
        quic_process_payload(tvb, pinfo, quic_tree, ti, offset,
                             conn, quic_packet, from_server, quic_packet->pkn_len);
    }
    if (!PINFO_FD_VISITED(pinfo) && !quic_packet->decryption.error) {
        // Packet number is verified to be valid, remember it.
        *quic_max_packet_number(conn, from_server, first_byte) = quic_packet->packet_number;
    }
    offset += tvb_reported_length_remaining(tvb, offset);

    return offset;
}

/* Check if "loss bits" feature has been negotiated */
static gboolean
quic_loss_bits_negotiated(quic_info_data_t *conn, gboolean from_server)
{
    if (from_server) {
        return conn->client_loss_bits_recv && conn->server_loss_bits_send;
    } else {
        return conn->server_loss_bits_recv && conn->client_loss_bits_send;
    }
}

static int
dissect_quic_short_header(tvbuff_t *tvb, packet_info *pinfo, proto_tree *quic_tree,
                          quic_datagram *dgram_info, quic_packet_info_t *quic_packet)
{
    guint offset = 0;
    quic_cid_t dcid = {.len=0};
    guint8  first_byte = 0;
    gboolean    key_phase = FALSE;
    proto_item *ti;
    quic_info_data_t *conn = dgram_info->conn;
    const gboolean from_server = dgram_info->from_server;
    gboolean loss_bits_negotiated = FALSE;

    proto_item *pi = proto_tree_add_item(quic_tree, hf_quic_short, tvb, 0, -1, ENC_NA);
    proto_tree *hdr_tree = proto_item_add_subtree(pi, ett_quic_short_header);
    proto_tree_add_item(hdr_tree, hf_quic_header_form, tvb, 0, 1, ENC_NA);

    if (conn) {
       dcid.len = from_server ? conn->client_cids.data.len : conn->server_cids.data.len;
       loss_bits_negotiated = quic_loss_bits_negotiated(conn, from_server);
    }

    if (!PINFO_FD_VISITED(pinfo) && conn) {
        const gchar *error = NULL;
        guint32 pkn32 = 0;
        if (quic_decrypt_header(tvb, 1 + dcid.len, &first_byte, &pkn32)) {
            quic_set_full_packet_number(conn, quic_packet, from_server, first_byte, pkn32);
            quic_packet->first_byte = first_byte;
        }
        if (error) {
            quic_packet->decryption.error = wmem_strdup(wmem_file_scope(), error);
        }
    } else if (conn && quic_packet->pkn_len) {
        first_byte = quic_packet->first_byte;
    }

    proto_tree_add_item(hdr_tree, hf_quic_fixed_bit, tvb, offset, 1, ENC_NA);
    proto_tree_add_item(hdr_tree, hf_quic_spin_bit, tvb, offset, 1, ENC_NA);
    /* Q and L bits are not protected by HP cipher */
    if (loss_bits_negotiated) {
        proto_tree_add_item(hdr_tree, hf_quic_q_bit, tvb, offset, 1, ENC_NA);
        proto_tree_add_item(hdr_tree, hf_quic_l_bit, tvb, offset, 1, ENC_NA);
    }
    if (quic_packet->pkn_len) {
        key_phase = (first_byte & SH_KP) != 0;
        /* No room for reserved bits with "loss bits" feature is enable */
        if (!loss_bits_negotiated) {
            proto_tree_add_uint(hdr_tree, hf_quic_short_reserved, tvb, offset, 1, first_byte);
        }
        proto_tree_add_boolean(hdr_tree, hf_quic_key_phase, tvb, offset, 1, key_phase<<2);
        proto_tree_add_uint(hdr_tree, hf_quic_packet_number_length, tvb, offset, 1, first_byte);
    }
    offset += 1;

    col_clear(pinfo->cinfo, COL_INFO);
    col_append_fstr(pinfo->cinfo, COL_INFO, "Protected Payload (KP%u)", key_phase);

    /* Connection ID */
    if (dcid.len > 0) {
        proto_tree_add_item(hdr_tree, hf_quic_dcid, tvb, offset, dcid.len, ENC_NA);
        tvb_memcpy(tvb, dcid.cid, offset, dcid.len);
        offset += dcid.len;
        const char *dcid_str = cid_to_string(&dcid);
        col_append_fstr(pinfo->cinfo, COL_INFO, ", DCID=%s", dcid_str);
        proto_item_append_text(pi, " DCID=%s", dcid_str);
    }


    if (quic_packet->decryption.error) {
        expert_add_info_format(pinfo, quic_tree, &ei_quic_decryption_failed,
                               "Failed to create decryption context: %s", quic_packet->decryption.error);
        return offset;
    }
    if (!conn || conn->skip_decryption || quic_packet->pkn_len == 0) {
        return offset;
    }

    /* Packet Number */
    proto_tree_add_uint64(hdr_tree, hf_quic_packet_number, tvb, offset, quic_packet->pkn_len, quic_packet->packet_number);
    offset += quic_packet->pkn_len;
    col_append_fstr(pinfo->cinfo, COL_INFO, ", PKN: %" G_GINT64_MODIFIER "u", quic_packet->packet_number);
    proto_item_append_text(pi, " PKN=%" G_GINT64_MODIFIER "u", quic_packet->packet_number);

    /* Protected Payload */
    ti = proto_tree_add_item(hdr_tree, hf_quic_protected_payload, tvb, offset, -1, ENC_NA);

    if (conn) {
        quic_process_payload(tvb, pinfo, quic_tree, ti, offset,
                             conn, quic_packet, from_server, quic_packet->pkn_len);
        if (!PINFO_FD_VISITED(pinfo) && !quic_packet->decryption.error) {
            // Packet number is verified to be valid, remember it.
            *quic_max_packet_number(conn, from_server, first_byte) = quic_packet->packet_number;
        }
    }
    offset += tvb_reported_length_remaining(tvb, offset);

    return offset;
}

static void
quic_proto_tree_add_version(tvbuff_t *tvb, proto_tree *tree, int hfindex, guint offset)
{
    guint32 version;
    proto_item *ti;

    ti = proto_tree_add_item_ret_uint(tree, hfindex, tvb, offset, 4, ENC_BIG_ENDIAN, &version);
    if ((version & 0x0F0F0F0F) == 0x0a0a0a0a) {
        proto_item_append_text(ti, " (GREASE)");
    }
}

static int
dissect_quic_version_negotiation(tvbuff_t *tvb, packet_info *pinfo, proto_tree *quic_tree, const quic_packet_info_t *quic_packet)
{
    guint       offset = 0;
    quic_cid_t  dcid = {.len=0}, scid = {.len=0};

    col_set_str(pinfo->cinfo, COL_INFO, "Version Negotiation");

    proto_tree_add_item(quic_tree, hf_quic_vn_unused, tvb, offset, 1, ENC_NA);
    offset += 1;

    offset = dissect_quic_long_header_common(tvb, pinfo, quic_tree, offset, quic_packet, NULL, &dcid, &scid);

    /* Supported Version */
    while(tvb_reported_length_remaining(tvb, offset) > 0){
        quic_proto_tree_add_version(tvb, quic_tree, hf_quic_supported_version, offset);
        offset += 4;
    }

    return offset;
}

static tvbuff_t *
quic_get_message_tvb(tvbuff_t *tvb, const guint offset)
{
    guint64 token_length;
    guint64 payload_length;
    guint8 packet_type = tvb_get_guint8(tvb, offset);
    guint8 long_packet_type = (packet_type & 0x30) >> 4;
    // Retry and VN packets cannot be coalesced (clarified in draft -14).
    if ((packet_type & 0x80) && long_packet_type != QUIC_LPT_RETRY) {
        // long header form, check version
        guint version = tvb_get_ntohl(tvb, offset + 1);
        // If this is not a VN packet but a valid long form, extract a subset.
        // TODO check for valid QUIC versions as future versions might change the format.
        if (version != 0) {
            guint length = 5;   // flag (1 byte) + version (4 bytes)
            length += 1 + tvb_get_guint8(tvb, offset + length); // DCID
            length += 1 + tvb_get_guint8(tvb, offset + length); // SCID
            if (long_packet_type == QUIC_LPT_INITIAL) {
                length += tvb_get_varint(tvb, offset + length, 8, &token_length, ENC_VARINT_QUIC);
                length += (guint)token_length;
            }
            length += tvb_get_varint(tvb, offset + length, 8, &payload_length, ENC_VARINT_QUIC);
            length += (guint)payload_length;
            if (payload_length <= G_MAXINT32 && length < (guint)tvb_reported_length_remaining(tvb, offset)) {
                return tvb_new_subset_length(tvb, offset, length);
            }
        }
    }

    // short header form, VN or unknown message, return remaining data.
    return tvb_new_subset_remaining(tvb, offset);
}

/**
 * Extracts necessary information from header to find any existing connection.
 * There are two special values for "long_packet_type":
 *  * QUIC_SHORT_PACKET for short header packets;
 *  * QUIC_LPT_VER_NEG for Version Negotiation packets.
 * DCID and SCID are not modified unless available. For short header packets,
 * DCID length is unknown, so the caller should truncate it as needed.
 */
static void
quic_extract_header(tvbuff_t *tvb, guint8 *long_packet_type, guint32 *version,
                    quic_cid_t *dcid, quic_cid_t *scid)
{
    guint offset = 0;

    guint8 packet_type = tvb_get_guint8(tvb, offset);
    gboolean is_long_header = packet_type & 0x80;
    if (is_long_header) {
        // long header form
        *long_packet_type = (packet_type & 0x30) >> 4;
    } else {
        // short header form, store dummy value that is not a long packet type.
        *long_packet_type = QUIC_SHORT_PACKET;
    }
    offset++;

    *version = tvb_get_ntohl(tvb, offset);

    if (is_long_header) {
        /* VN packets don't have any real packet type field, even if they have
           a long header: use a dummy value */
        if (*version == 0x00000000)
            *long_packet_type = QUIC_LPT_VER_NEG;

        // skip version
        offset += 4;

        // read DCID and SCID (both are prefixed by a length byte).
        guint8 dcil = tvb_get_guint8(tvb, offset);
        offset++;

        if (dcil && dcil <= QUIC_MAX_CID_LENGTH) {
            tvb_memcpy(tvb, dcid->cid, offset, dcil);
            dcid->len = dcil;
        }
        offset += dcil;

        guint8 scil = tvb_get_guint8(tvb, offset);
        offset++;
        if (scil && scil <= QUIC_MAX_CID_LENGTH) {
            tvb_memcpy(tvb, scid->cid, offset, scil);
            scid->len = scil;
        }
    } else {
        // Definitely not draft -10, set version to dummy value.
        *version = 0;
        // For short headers, the DCID length is unknown and could be 0 or
        // anything from 1 to 20 bytes. Copy the maximum possible and let the
        // consumer truncate it as necessary.
        tvb_memcpy(tvb, dcid->cid, offset, QUIC_MAX_CID_LENGTH);
        dcid->len = QUIC_MAX_CID_LENGTH;
    }
}

/**
 * Sanity check on (coalasced) packet.
 * https://tools.ietf.org/html/draft-ietf-quic-transport-32#section-12.2
 * "Senders MUST NOT coalesce QUIC packets with different connection IDs
 *  into a single UDP datagram"
 * For the first packet of the datagram, we simply save the DCID for later usage (no real check).
 * For any subsequent packets, we control if DCID is valid.
 */
static gboolean
check_dcid_on_coalesced_packet(tvbuff_t *tvb, const quic_datagram *dgram_info,
                               gboolean is_first_packet, quic_cid_t *first_packet_dcid)
{
    guint offset = 0;
    guint8 first_byte, dcid_len;
    quic_cid_t dcid = {.len=0};

    first_byte = tvb_get_guint8(tvb, offset);
    offset++;
    if (first_byte & 0x80) {
        offset += 4; /* Skip version */
        dcid_len = tvb_get_guint8(tvb, offset);
        offset++;
        if (dcid_len && dcid_len <= QUIC_MAX_CID_LENGTH) {
            dcid.len = dcid_len;
            tvb_memcpy(tvb, dcid.cid, offset, dcid.len);
        }
    } else {
        quic_info_data_t *conn = dgram_info->conn;
        gboolean from_server = dgram_info->from_server;
        if (conn) {
            dcid.len = from_server ? conn->client_cids.data.len : conn->server_cids.data.len;
            if (dcid.len) {
                tvb_memcpy(tvb, dcid.cid, offset, dcid.len);
            }
        } else {
            /* If we don't have a valid quic_info_data_t structure for this flow,
               we can't really validate the CID. */
            return TRUE;
        }
    }

    if (is_first_packet) {
        *first_packet_dcid = dcid;
        return TRUE; /* Nothing to check */
    }

    return quic_connection_equal(&dcid, first_packet_dcid);
}

static int
dissect_quic(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree,
        void *data _U_)
{
    proto_item *quic_ti, *ti;
    proto_tree *quic_tree;
    guint       offset = 0;
    quic_datagram *dgram_info = NULL;
    quic_packet_info_t *quic_packet = NULL;
    quic_cid_t  real_retry_odcid = {.len=0}, *retry_odcid = NULL;
    quic_cid_t  first_packet_dcid = {.len=0}; /* DCID of the first packet of the datagram */

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "QUIC");

    if (PINFO_FD_VISITED(pinfo)) {
        dgram_info = (quic_datagram *)p_get_proto_data(wmem_file_scope(), pinfo, proto_quic_plain, 0);
    }
    if (!dgram_info) {
        dgram_info = wmem_new0(wmem_file_scope(), quic_datagram);
        p_add_proto_data(wmem_file_scope(), pinfo, proto_quic_plain, 0, dgram_info);
    }

    quic_ti = proto_tree_add_item(tree, proto_quic_plain, tvb, 0, -1, ENC_NA);
    quic_tree = proto_item_add_subtree(quic_ti, ett_quic);

    if (!PINFO_FD_VISITED(pinfo)) {
        guint8      long_packet_type;
        guint32     version;
        quic_cid_t  dcid = {.len=0}, scid = {.len=0};
        gboolean    from_server = FALSE;
        quic_info_data_t *conn;

        quic_extract_header(tvb, &long_packet_type, &version, &dcid, &scid);
        conn = quic_connection_find(pinfo, long_packet_type, &dcid, &from_server);
        if (conn && long_packet_type == QUIC_LPT_RETRY && conn->client_dcid_set) {
            // Save the original client DCID before erasure.
            real_retry_odcid = conn->client_dcid_initial;
            retry_odcid = &real_retry_odcid;
        }
        quic_connection_create_or_update(&conn, pinfo, long_packet_type, version, &scid, &dcid, from_server);
        dgram_info->conn = conn;
        dgram_info->from_server = from_server;
#if 0
        proto_tree_add_debug_text(quic_tree, "Connection: %d %p DCID=%s SCID=%s from_server:%d", pinfo->num, dgram_info->conn, cid_to_string(&dcid), cid_to_string(&scid), dgram_info->from_server);
    } else {
        proto_tree_add_debug_text(quic_tree, "Connection: %d %p from_server:%d", pinfo->num, dgram_info->conn, dgram_info->from_server);
#endif
    }

    quic_add_connection_info(tvb, pinfo, quic_tree, dgram_info->conn);

    do {
        if (!quic_packet) {
            quic_packet = &dgram_info->first_packet;
        } else if (!PINFO_FD_VISITED(pinfo)) {
            quic_packet->next = wmem_new0(wmem_file_scope(), quic_packet_info_t);
            quic_packet = quic_packet->next;
        } else {
            quic_packet = quic_packet->next;
            DISSECTOR_ASSERT(quic_packet);
        }

        /* Ensure that coalesced QUIC packets end up separated. */
        if (offset > 0) {
            quic_ti = proto_tree_add_item(tree, proto_quic_plain, tvb, offset, -1, ENC_NA);
            quic_tree = proto_item_add_subtree(quic_ti, ett_quic);
        }

        tvbuff_t *next_tvb = quic_get_message_tvb(tvb, offset);

        if (!check_dcid_on_coalesced_packet(next_tvb, dgram_info, offset == 0, &first_packet_dcid)) {
            /* Coalesced packet with unexpected CID; it probably is some kind
               of unencrypted padding data added after the valid QUIC payload */
            expert_add_info_format(pinfo, quic_tree, &ei_quic_coalesced_padding_data,
                                   "(Random) padding data appended to the datagram");
            break;
        }

        proto_item_set_len(quic_ti, tvb_reported_length(next_tvb));
        ti = proto_tree_add_uint(quic_tree, hf_quic_packet_length, next_tvb, 0, 0, tvb_reported_length(next_tvb));
        proto_item_set_generated(ti);
        guint new_offset = 0;
        guint8 first_byte = tvb_get_guint8(next_tvb, 0);
        if (first_byte & 0x80) {
            guint8 long_packet_type = (first_byte & 0x30) >> 4;
            proto_tree_add_item(quic_tree, hf_quic_header_form, next_tvb, 0, 1, ENC_NA);
            guint32 version = tvb_get_ntohl(next_tvb, 1);
            if (version == 0) {
                offset += dissect_quic_version_negotiation(next_tvb, pinfo, quic_tree, quic_packet);
                break;
            }
            if (long_packet_type == QUIC_LPT_RETRY) {
                new_offset = dissect_quic_retry_packet(next_tvb, pinfo, quic_tree, dgram_info, quic_packet, retry_odcid);
            } else {
                new_offset = dissect_quic_long_header(next_tvb, pinfo, quic_tree, dgram_info, quic_packet);
            }
        } else { /* Note that the "Fixed" bit might have been greased,
                    so 0x00 is a perfectly valid value as first_byte */
            new_offset = dissect_quic_short_header(next_tvb, pinfo, quic_tree, dgram_info, quic_packet);
        }
        if (tvb_reported_length_remaining(next_tvb, new_offset)) {
            // should usually not be present unless decryption is not possible.
            proto_tree_add_item(quic_tree, hf_quic_remaining_payload, next_tvb, new_offset, -1, ENC_NA);
        }
        offset += tvb_reported_length(next_tvb);
    } while (tvb_reported_length_remaining(tvb, offset));

    return offset;
}

static gboolean
dissect_quic_short_header_heur(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
    // If this capture does not contain QUIC, skip the more expensive checks.
    if (quic_cid_lengths == 0) {
        return FALSE;
    }

    // Is this a SH packet after connection migration? SH (since draft -22):
    // Flag (1) + DCID (1-20) + PKN (1/2/4) + encrypted payload (>= 16).
    if (tvb_captured_length(tvb) < 1 + 1 + 1 + 16) {
        return FALSE;
    }

    // DCID length is unknown, so extract the maximum and look for a match.
    quic_cid_t dcid = {.len = MIN(QUIC_MAX_CID_LENGTH, tvb_captured_length(tvb) - 1 - 1 - 16)};
    tvb_memcpy(tvb, dcid.cid, 1, dcid.len);
    gboolean from_server;
    if (!quic_connection_find(pinfo, QUIC_SHORT_PACKET, &dcid, &from_server)) {
        return FALSE;
    }

    conversation_t *conversation = find_or_create_conversation(pinfo);
    conversation_set_dissector(conversation, quic_handle);
    dissect_quic(tvb, pinfo, tree, NULL);
    return TRUE;
}

static gboolean dissect_quic_heur(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    /*
     * Since draft -22:
     * Flag (1 byte) + Version (4 bytes) +
     * Length (1 byte) + Destination Connection ID (0..255) +
     * Length (1 byte) + Source Connection ID (0..255) +
     * Payload length (1/2/4/8) + Packet number (1/2/4 bytes) + Payload.
     * (absolute minimum: 9 + payload)
     * (for Version Negotiation, payload len + PKN + payload is replaced by
     * Supported Version (multiple of 4 bytes.)
     */
    conversation_t *conversation = NULL;
    int offset = 0;
    guint8 flags, dcid, scid;
    guint32 version;
    gboolean is_quic = FALSE;

    /* Verify packet size  (Flag (1 byte) + Connection ID (8 bytes) + Version (4 bytes)) */
    if (tvb_captured_length(tvb) < 13)
    {
        return FALSE;
    }

    flags = tvb_get_guint8(tvb, offset);
    /* Check if long Packet is set */
    if((flags & 0x80) == 0) {
        // Perhaps this is a short header, check it.
        return dissect_quic_short_header_heur(tvb, pinfo, tree);
    }
    offset += 1;

    // check for draft QUIC version (for draft -11 and newer)
    version = tvb_get_ntohl(tvb, offset);
    is_quic = (quic_draft_version(version) >= 11);
    if (!is_quic) {
        return FALSE;
    }

    /* Version check on packet forcing version negotiation is quite weak:
       try hardenig it checking packets type, too */
    if ((version & 0x0F0F0F0F) == 0x0a0a0a0a &&
        (flags & 0x30) != 0x00) { /* Initial Packet */
            return FALSE;
    }

    /* Check that CIDs lengths are valid */
    offset += 4;
    dcid = tvb_get_guint8(tvb, offset);
    if (dcid > QUIC_MAX_CID_LENGTH) {
        return FALSE;
    }
    offset += 1 + dcid;
    if (offset >= (int)tvb_captured_length(tvb)) {
        return FALSE;
    }
    scid = tvb_get_guint8(tvb, offset);
    if (scid > QUIC_MAX_CID_LENGTH) {
        return FALSE;
    }

    /* Ok! */
    conversation = find_or_create_conversation(pinfo);
    conversation_set_dissector(conversation, quic_handle);
    dissect_quic(tvb, pinfo, tree, data);

    return TRUE;
}


/** Initialize QUIC dissection state for a new capture file. */
static void
quic_init(void)
{
    quic_connections = wmem_list_new(wmem_file_scope());
    quic_connections_count = 0;
    quic_initial_connections = wmem_map_new(wmem_file_scope(), quic_connection_hash, quic_connection_equal);
    quic_client_connections = wmem_map_new(wmem_file_scope(), quic_connection_hash, quic_connection_equal);
    quic_server_connections = wmem_map_new(wmem_file_scope(), quic_connection_hash, quic_connection_equal);
    quic_cid_lengths = 0;
}

/** Release QUIC dissection state on closing a capture file. */
static void
quic_cleanup(void)
{
    quic_initial_connections = NULL;
    quic_client_connections = NULL;
    quic_server_connections = NULL;
}

/* Follow QUIC Stream functionality {{{ */
static void
quic_streams_add(packet_info *pinfo, quic_info_data_t *quic_info, guint64 stream_id)
{
    /* List: ordered list of Stream IDs in this connection */
    if (!quic_info->streams_list) {
        quic_info->streams_list = wmem_list_new(wmem_file_scope());
    }
    if (!wmem_list_find(quic_info->streams_list, GUINT_TO_POINTER(stream_id))) {
        wmem_list_insert_sorted(quic_info->streams_list, GUINT_TO_POINTER(stream_id),
                                wmem_compare_uint);
    }

    /* Map: first Stream ID for each UDP payload */
    quic_follow_stream *stream;
    if (!quic_info->streams_map) {
        quic_info->streams_map = wmem_map_new(wmem_file_scope(), g_direct_hash, g_direct_equal);
    }
    stream = wmem_map_lookup(quic_info->streams_map, GUINT_TO_POINTER(pinfo->num));
    if (!stream) {
        stream = wmem_new0(wmem_file_scope(), quic_follow_stream);
        stream->num = pinfo->num;
        stream->stream_id = stream_id;
        wmem_map_insert(quic_info->streams_map, GUINT_TO_POINTER(stream->num), stream);
    }
}


static quic_info_data_t *
get_conn_by_number(guint conn_number)
{
    quic_info_data_t *conn;
    wmem_list_frame_t *elem;

    elem = wmem_list_head(quic_connections);
    while (elem) {
        conn = (quic_info_data_t *)wmem_list_frame_data(elem);
        if (conn->number == conn_number)
            return conn;
        elem = wmem_list_frame_next(elem);
    }
    return NULL;
}

static gboolean
quic_get_stream_id_le(guint streamid, guint sub_stream_id, guint *sub_stream_id_out)
{
    quic_info_data_t *quic_info;
    wmem_list_frame_t *curr_entry;
    guint prev_stream_id;

    quic_info = get_conn_by_number(streamid);
    if (!quic_info) {
        return FALSE;
    }
    if (!quic_info->streams_list) {
        return FALSE;
    }

    prev_stream_id = G_MAXUINT32;
    curr_entry = wmem_list_head(quic_info->streams_list);
    while (curr_entry) {
        if (GPOINTER_TO_UINT(wmem_list_frame_data(curr_entry)) > sub_stream_id &&
            prev_stream_id != G_MAXUINT32) {
            *sub_stream_id_out = (guint)prev_stream_id;
            return TRUE;
        }
        prev_stream_id = GPOINTER_TO_UINT(wmem_list_frame_data(curr_entry));
        curr_entry = wmem_list_frame_next(curr_entry);
    }

    if (prev_stream_id != G_MAXUINT32) {
        *sub_stream_id_out = prev_stream_id;
        return TRUE;
    }

    return FALSE;
}

static gboolean
quic_get_stream_id_ge(guint streamid, guint sub_stream_id, guint *sub_stream_id_out)
{
    quic_info_data_t *quic_info;
    wmem_list_frame_t *curr_entry;

    quic_info = get_conn_by_number(streamid);
    if (!quic_info) {
        return FALSE;
    }
    if (!quic_info->streams_list) {
        return FALSE;
    }

    curr_entry = wmem_list_head(quic_info->streams_list);
    while (curr_entry) {
        if (GPOINTER_TO_UINT(wmem_list_frame_data(curr_entry)) >= sub_stream_id) {
            /* StreamIDs are 64 bits long in QUIC, but "Follow Stream" generic code uses guint variables */
            *sub_stream_id_out = GPOINTER_TO_UINT(wmem_list_frame_data(curr_entry));
            return TRUE;
        }
        curr_entry = wmem_list_frame_next(curr_entry);
    }

    return FALSE;
}

static gboolean
quic_get_sub_stream_id(guint streamid, guint sub_stream_id, gboolean le, guint *sub_stream_id_out)
{
    if (le) {
        return quic_get_stream_id_le(streamid, sub_stream_id, sub_stream_id_out);
    } else {
        return quic_get_stream_id_ge(streamid, sub_stream_id, sub_stream_id_out);
    }
}

static gchar *
quic_follow_conv_filter(epan_dissect_t *edt _U_, packet_info *pinfo, guint *stream, guint *sub_stream)
{
    if (((pinfo->net_src.type == AT_IPv4 && pinfo->net_dst.type == AT_IPv4) ||
        (pinfo->net_src.type == AT_IPv6 && pinfo->net_dst.type == AT_IPv6))) {
        gboolean from_server;
        quic_info_data_t *conn = quic_connection_find_dcid(pinfo, NULL, &from_server);
        if (!conn) {
            return NULL;
        }

        /* First Stream ID in the selected packet */
        quic_follow_stream *s;
        if (conn->streams_map) {
	    s = wmem_map_lookup(conn->streams_map, GUINT_TO_POINTER(pinfo->num));
            if (s) {
                *stream = conn->number;
                *sub_stream = (guint)s->stream_id;
                return g_strdup_printf("quic_plain.connection.number eq %u and quic.stream.stream_id eq %u", conn->number, *sub_stream);
            }
        }
    }

    return NULL;
}

static gchar *
quic_follow_index_filter(guint stream, guint sub_stream)
{
    return g_strdup_printf("quic_plain.connection.number eq %u and quic.stream.stream_id eq %u", stream, sub_stream);
}

static gchar *
quic_follow_address_filter(address *src_addr _U_, address *dst_addr _U_, int src_port _U_, int dst_port _U_)
{
    // This appears to be solely used for tshark. Let's not support matching by
    // IP addresses and UDP ports for now since that fails after connection
    // migration. If necessary, use udp_follow_address_filter.
    return NULL;
}

static tap_packet_status
follow_quic_tap_listener(void *tapdata, packet_info *pinfo, epan_dissect_t *edt _U_, const void *data, tap_flags_t flags _U_)
{
    follow_record_t *follow_record;
    follow_info_t *follow_info = (follow_info_t *)tapdata;
    const quic_follow_tap_data_t *follow_data = (const quic_follow_tap_data_t *)data;

    if (follow_info->substream_id != SUBSTREAM_UNUSED &&
        follow_info->substream_id != follow_data->stream_id) {
        return TAP_PACKET_DONT_REDRAW;
    }

    follow_record = g_new(follow_record_t, 1);

    // XXX: Ideally, we should also deal with stream retransmission
    // and out of order packets in a similar manner to the TCP dissector,
    // using the offset, plus ACKs and other information.
    follow_record->data = g_byte_array_sized_new(tvb_captured_length(follow_data->tvb));
    follow_record->data = g_byte_array_append(follow_record->data, tvb_get_ptr(follow_data->tvb, 0, -1), tvb_captured_length(follow_data->tvb));
    follow_record->packet_num = pinfo->fd->num;
    follow_record->abs_ts = pinfo->fd->abs_ts;

    /* This sets the address and port information the first time this
     * stream is tapped. It will no longer be true after migration, but
     * as it seems it's only used for display, using the initial values
     * is the best we can do.
     */

    if (follow_data->from_server) {
        follow_record->is_server = TRUE;
        if (follow_info->client_port == 0) {
            follow_info->server_port = pinfo->srcport;
            copy_address(&follow_info->server_ip, &pinfo->src);
            follow_info->client_port = pinfo->destport;
            copy_address(&follow_info->client_ip, &pinfo->dst);
        }
    } else {
        follow_record->is_server = FALSE;
        if (follow_info->client_port == 0) {
            follow_info->client_port = pinfo->srcport;
            copy_address(&follow_info->client_ip, &pinfo->src);
            follow_info->server_port = pinfo->destport;
            copy_address(&follow_info->server_ip, &pinfo->dst);
        }
    }

    follow_info->bytes_written[follow_record->is_server] += follow_record->data->len;

    follow_info->payload = g_list_prepend(follow_info->payload, follow_record);
    return TAP_PACKET_DONT_REDRAW;
}

static guint32
get_quic_connections_count(void)
{
    return quic_connections_count;
}
/* Follow QUIC Stream functionality }}} */

void
proto_register_quic_plain(void)
{
    expert_module_t *expert_quic;

    static hf_register_info hf[] = {
        { &hf_quic_connection_number,
          { "Connection Number", "quic_plain.connection.number",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Connection identifier within this capture file", HFILL }
        },

        { &hf_quic_packet_length,
          { "Packet Length", "quic_plain.packet_length",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Size of the QUIC packet", HFILL }
        },

        { &hf_quic_header_form,
          { "Header Form", "quic_plain.header_form",
            FT_UINT8, BASE_DEC, VALS(quic_short_long_header_vals), 0x80,
            "The most significant bit (0x80) of the first octet is set to 1 for long headers and 0 for short headers.", HFILL }
        },

        { &hf_quic_long_packet_type,
          { "Packet Type", "quic_plain.long.packet_type",
            FT_UINT8, BASE_DEC, VALS(quic_long_packet_type_vals), 0x30,
            "Long Header Packet Type", HFILL }
        },
        { &hf_quic_long_reserved,
          { "Reserved", "quic_plain.long.reserved",
            FT_UINT8, BASE_DEC, NULL, 0x0c,
            "Reserved bits (protected using header protection)", HFILL }
        },
        { &hf_quic_packet_number_length,
          { "Packet Number Length", "quic_plain.packet_number_length",
            FT_UINT8, BASE_DEC, VALS(quic_packet_number_lengths), 0x03,
            "Packet Number field length (protected using header protection)", HFILL }
        },
        { &hf_quic_dcid,
          { "Destination Connection ID", "quic_plain.dcid",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_scid,
          { "Source Connection ID", "quic_plain.scid",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_dcil,
          { "Destination Connection ID Length", "quic_plain.dcil",
            FT_UINT8, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_scil,
          { "Source Connection ID Length", "quic_plain.scil",
            FT_UINT8, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_token_length,
          { "Token Length", "quic_plain.token_length",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_token,
          { "Token", "quic_plain.token",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_length,
          { "Length", "quic_plain.length",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Length of Packet Number and Payload fields", HFILL }
        },

        { &hf_quic_packet_number,
          { "Packet Number", "quic_plain.packet_number",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Decoded packet number", HFILL }
        },
        { &hf_quic_version,
          { "Version", "quic_plain.version",
            FT_UINT32, BASE_RANGE_STRING | BASE_HEX, RVALS(quic_version_vals), 0x0,
            NULL, HFILL }
        },
        { &hf_quic_supported_version,
          { "Supported Version", "quic_plain.supported_version",
            FT_UINT32, BASE_RANGE_STRING | BASE_HEX, RVALS(quic_version_vals), 0x0,
            NULL, HFILL }
        },
        { &hf_quic_vn_unused,
          { "Unused", "quic_plain.vn.unused",
            FT_UINT8, BASE_HEX, NULL, 0x7F,
            NULL, HFILL }
        },
        { &hf_quic_short,
          { "QUIC Short Header", "quic_plain.short",
            FT_NONE, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_fixed_bit,
          { "Fixed Bit", "quic_plain.fixed_bit",
            FT_BOOLEAN, 8, NULL, 0x40,
            "Must be 1", HFILL }
        },
        { &hf_quic_spin_bit,
          { "Spin Bit", "quic_plain.spin_bit",
            FT_BOOLEAN, 8, NULL, 0x20,
            "Latency Spin Bit", HFILL }
        },
        { &hf_quic_mp_add_address_first_byte,
          { "Config", "quic_plain.mp_first_byte",
            FT_UINT8, BASE_HEX, NULL, 0,
            NULL, HFILL }
        },
        { &hf_quic_mp_add_address_reserved,
          { "Reserved", "quic_plain.mp_reserved_bit",
            FT_UINT8, BASE_DEC, NULL, 0xE0,
            NULL, HFILL }
        },
        { &hf_quic_mp_add_address_port_present,
          { "Port presence", "quic_plain.port_presence_bit",
            FT_BOOLEAN, 8, NULL, 0x10,
            "Must be 1", HFILL }
        },
        { &hf_quic_mp_add_address_ip_version,
          { "IP Version", "quic_plain.ip_version",
            FT_UINT8, BASE_DEC, NULL, 0x0f,
            NULL, HFILL }
        },
       { &hf_quic_mp_add_address_id,
          { "Address ID", "quic_plain.mp_address_id",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
       { &hf_quic_mp_add_address_sq_number,
          { "Sequence Number", "quic_plain.mp_sequence_number",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
       { &hf_quic_mp_add_address_interface_type,
          { "Interface Type", "quic_plain.mp_interface_type",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
       { &hf_quic_mp_add_address_ip_address,
          { "IP Address", "quic_plain.mp_ip_address",
            FT_IPv4, BASE_NONE,
            NULL, 0x0, NULL, HFILL }
        },
       { &hf_quic_mp_add_address_ip_address_v6,
          { "IP Address", "quic_plain.mp_ip_address_v6",
            FT_IPv6, BASE_NONE,
            NULL, 0x0, NULL, HFILL }
        },
        { &hf_quic_mp_add_address_port,
          { "Port", "quic_plain.mp_port",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
       { &hf_quic_mp_uniflow_id,
          { "Uniflow ID", "quic_plain.mp_uniflow_id",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
       { &hf_quic_mp_receiving_uniflows,
          { "Receiving uniflows", "quic_plain.mp_receiving_uniflows",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
       { &hf_quic_mp_active_sending_uniflows,
          { "Active sending uniflows", "quic_plain.mp_act_send_uf",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
       { &hf_quic_mp_receiving_uniflow_info_section,
          { "Receiving uniflows", "quic_plain.mp_receiving_uniflows_section",
            FT_NONE, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
       { &hf_quic_mp_active_sending_uniflows_info_section,
          { "Active sending uniflows", "quic_plain.mp_act_send_uf_section",
            FT_NONE, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
       { &hf_quic_mp_uniflow_info_section,
          { "Uniflow Info Section", "quic_plain.mp_uniflow_info_section",
            FT_NONE, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
       { &hf_quic_mp_add_local_address_id ,
          { "Local address id", "quic_plain.mp_add_local_address_id",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_short_reserved,
          { "Reserved", "quic_plain.short.reserved",
            FT_UINT8, BASE_DEC, NULL, 0x18,
            "Reserved bits (protected using header protection)", HFILL }
        },
        { &hf_quic_q_bit,
          { "Square Signal Bit (Q)", "quic_plain.q_bit",
            FT_BOOLEAN, 8, NULL, 0x10,
            "Square Signal Bit (used to measure and locate the source of packet loss)", HFILL }
        },
        { &hf_quic_l_bit,
          { "Loss Event Bit (L)", "quic_plain.l_bit",
            FT_BOOLEAN, 8, NULL, 0x08,
            "Loss Event Bit (used to measure and locate the source of packet loss)",  HFILL }
        },
        { &hf_quic_key_phase,
          { "Key Phase Bit", "quic_plain.key_phase",
            FT_BOOLEAN, 8, NULL, SH_KP,
            "Selects the packet protection keys to use (protected using header protection)", HFILL }
        },

        { &hf_quic_payload,
          { "Payload", "quic_plain.payload",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            "(Encrypted) payload of a packet", HFILL }
        },
        { &hf_quic_protected_payload,
          { "Protected Payload", "quic_plain.protected_payload",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            "1-RTT protected payload", HFILL }
        },
        { &hf_quic_remaining_payload,
          { "Remaining Payload", "quic_plain.remaining_payload",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            "Remaining payload in a packet (possibly PKN followed by encrypted payload)", HFILL }
        },

        { &hf_quic_odcil,
          { "Original Destination Connection ID Length", "quic_plain.odcil",
            FT_UINT8, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_odcid,
          { "Original Destination Connection ID", "quic_plain.odcid",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_retry_token,
          { "Retry Token", "quic_plain.retry_token",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_retry_integrity_tag,
          { "Retry Integrity Tag", "quic_plain.retry_integrity_tag",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },

        { &hf_quic_frame,
          { "Frame", "quic_plain.frame",
            FT_NONE, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_frame_type,
          { "Frame Type", "quic_plain.frame_type",
            FT_UINT64, BASE_RANGE_STRING | BASE_HEX, RVALS(quic_frame_type_vals), 0x0,
            NULL, HFILL }
        },

        /* PADDING */
        { &hf_quic_padding_length,
          { "Padding Length", "quic_plain.padding_length",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        /* ACK */
        { &hf_quic_ack_largest_acknowledged,
          { "Largest Acknowledged", "quic_plain.ack.largest_acknowledged",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Largest packet number the peer is acknowledging in this packet", HFILL }
        },
        { &hf_quic_ack_ack_delay,
          { "ACK Delay", "quic_plain.ack.ack_delay",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Time from when the largest acknowledged packet, as indicated in the Largest Acknowledged field, was received by this peer to when this ACK was sent", HFILL }
        },
        { &hf_quic_ack_ack_range_count,
          { "ACK Range Count", "quic_plain.ack.ack_range_count",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Number of Gap and ACK Range fields in the frame", HFILL }
        },
        { &hf_quic_ack_first_ack_range,
          { "First ACK Range", "quic_plain.ack.first_ack_range",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Number of contiguous packets preceding the Largest Acknowledged that are being acknowledged", HFILL }
        },
        { &hf_quic_ack_gap,
          { "Gap", "quic_plain.ack.gap",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Number of contiguous unacknowledged packets preceding the packet number one lower than the smallest in the preceding ACK Range", HFILL }
        },
        { &hf_quic_ack_ack_range,
          { "ACK Range", "quic_plain.ack.ack_range",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Number of contiguous acknowledged packets preceding the largest packet number, as determined by the preceding Gap", HFILL }
        },
        { &hf_quic_ack_ect0_count,
          { "ECT(0) Count", "quic_plain.ack.ect0_count",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Total number of packets received with the ECT(0) codepoint", HFILL }
        },
        { &hf_quic_ack_ect1_count,
          { "ECT(1) Count", "quic_plain.ack.ect1_count",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Total number of packets received with the ECT(1) codepoint", HFILL }
        },
        { &hf_quic_ack_ecn_ce_count,
          { "ECN-CE Count", "quic_plain.ack.ecn_ce_count",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Total number of packets received with the CE codepoint", HFILL }
        },
        /* RESET_STREAM */
        { &hf_quic_rsts_stream_id,
            { "Stream ID", "quic_plain.rsts.stream_id",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Stream ID of the stream being terminated", HFILL }
        },
        { &hf_quic_rsts_application_error_code,
            { "Application Error code", "quic_plain.rsts.application_error_code",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Indicates why the stream is being closed", HFILL }
        },
        { &hf_quic_rsts_final_size,
            { "Final Size", "quic_plain.rsts.final_size",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "The final size of the stream by the RESET_STREAM sender (in bytes)", HFILL }
        },
        /* STOP_SENDING */
        { &hf_quic_ss_stream_id,
            { "Stream ID", "quic_plain.ss.stream_id",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Stream ID of the stream being ignored", HFILL }
        },
        { &hf_quic_ss_application_error_code,
            { "Application Error code", "quic_plain.ss.application_error_code",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Indicates why the sender is ignoring the stream", HFILL }
        },
        /* CRYPTO */
        { &hf_quic_crypto_offset,
            { "Offset", "quic_plain.crypto.offset",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Byte offset into the stream", HFILL }
        },
        { &hf_quic_crypto_length,
            { "Length", "quic_plain.crypto.length",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Length of the Crypto Data field", HFILL }
        },
        { &hf_quic_crypto_crypto_data,
            { "Crypto Data", "quic_plain.crypto.crypto_data",
              FT_NONE, BASE_NONE, NULL, 0x0,
              "The cryptographic message data", HFILL }
        },
        /* NEW_TOKEN */
        { &hf_quic_nt_length,
            { "(Token) Length", "quic_plain.nt.length",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Specifying the length of the token", HFILL }
        },
        { &hf_quic_nt_token,
            { "Token", "quic_plain.nt.token",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              "An opaque blob that the client may use with a future Initial packet", HFILL }
        },
        /* STREAM */
        { &hf_quic_stream_fin,
          { "Fin", "quic_plain.stream.fin",
            FT_BOOLEAN, 8, NULL, FTFLAGS_STREAM_FIN,
            NULL, HFILL }
        },
        { &hf_quic_stream_len,
          { "Len(gth)", "quic_plain.stream.len",
            FT_BOOLEAN, 8, NULL, FTFLAGS_STREAM_LEN,
            NULL, HFILL }
        },
        { &hf_quic_stream_off,
          { "Off(set)", "quic_plain.stream.off",
            FT_BOOLEAN, 8, NULL, FTFLAGS_STREAM_OFF,
            NULL, HFILL }
        },
        { &hf_quic_stream_stream_id,
          { "Stream ID", "quic_plain.stream.stream_id",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_stream_initiator,
          { "Stream initiator", "quic_plain.stream.initiator",
            FT_UINT64, BASE_DEC | BASE_VAL64_STRING, VALS64(quic_frame_id_initiator), FTFLAGS_STREAM_INITIATOR,
            NULL, HFILL }
        },
        { &hf_quic_stream_direction,
          { "Stream direction", "quic_plain.stream.direction",
            FT_UINT64, BASE_DEC | BASE_VAL64_STRING, VALS64(quic_frame_id_direction), FTFLAGS_STREAM_DIRECTION,
            NULL, HFILL }
        },
        { &hf_quic_stream_offset,
          { "Offset", "quic_plain.stream.offset",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_stream_length,
          { "Length", "quic_plain.stream.length",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_stream_data,
          { "Stream Data", "quic_plain.stream_data",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },

        /* MAX_DATA */
        { &hf_quic_md_maximum_data,
            { "Maximum Data", "quic_plain.md.maximum_data",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Indicating the maximum amount of data that can be sent on the entire connection, in units of 1024 octets", HFILL }
        },
        /* MAX_STREAM_DATA */
        { &hf_quic_msd_stream_id,
            { "Stream ID", "quic_plain.msd.stream_id",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "The stream ID of the stream that is affected", HFILL }
        },
        { &hf_quic_msd_maximum_stream_data,
            { "Maximum Stream Data", "quic_plain.msd.maximum_stream_data",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Indicating the maximum amount of data that can be sent on the identified stream, in units of octets", HFILL }
        },
        /* MAX_STREAMS */
        { &hf_quic_ms_max_streams,
            { "Max Streams", "quic_plain.ms.max_streams",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "A count of the cumulative number of streams of the corresponding type that can be opened over the lifetime of the connection", HFILL }
        },
        /* DATA_BLOCKED */
        { &hf_quic_db_stream_data_limit,
            { "Stream Data Limit", "quic_plain.sb.stream_data_limit",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Indicating the connection-level limit at which the blocking occurred", HFILL }
        },
        /* STREAM_DATA_BLOCKED */
        { &hf_quic_sdb_stream_id,
            { "Stream ID", "quic_plain.sdb.stream_id",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Indicating the stream which is flow control blocked", HFILL }
        },
        { &hf_quic_sdb_stream_data_limit,
            { "Stream Data Limit", "quic_plain.sb.stream_data_limit",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Indicating the offset of the stream at which the blocking occurred", HFILL }
        },
        /* STREAMS_BLOCKED */
        { &hf_quic_sb_stream_limit,
            { "Stream Limit", "quic_plain.sib.stream_limit",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Indicating the stream limit at the time the frame was sent", HFILL }
        },
        /* NEW_CONNECTION_ID */
        { &hf_quic_nci_retire_prior_to,
            { "Retire Prior To", "quic_plain.nci.retire_prior_to",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "A variable-length integer indicating which connection IDs should be retired", HFILL }
        },
        { &hf_quic_nci_sequence,
            { "Sequence", "quic_plain.nci.sequence",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Increases by 1 for each connection ID that is provided by the server", HFILL }
        },
        { &hf_quic_nci_connection_id_length,
            { "Connection ID Length", "quic_plain.nci.connection_id.length",
              FT_UINT8, BASE_DEC, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_quic_nci_connection_id,
            { "Connection ID", "quic_plain.nci.connection_id",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_quic_nci_stateless_reset_token,
            { "Stateless Reset Token", "quic_plain.stateless_reset_token",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        /* RETIRE_CONNECTION_ID */
        { &hf_quic_rci_sequence,
            { "Sequence", "quic_plain.rci.sequence",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "The sequence number of the connection ID being retired", HFILL }
        },
        /* PATH_CHALLENGE */
        { &hf_quic_path_challenge_data,
          { "Data", "quic_plain.path_challenge.data",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            "Arbitrary data that must be matched by a PATH_RESPONSE frame", HFILL }
        },
        /* PATH_RESPONSE */
        { &hf_quic_path_response_data,
          { "Data", "quic_plain.path_response.data",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            "Arbitrary data that must match a PATH_CHALLENGE frame", HFILL }
        },
        /* CONNECTION_CLOSE */
        { &hf_quic_cc_error_code,
            { "Error code", "quic_plain.cc.error_code",
              FT_UINT64, BASE_DEC|BASE_RANGE_STRING, RVALS(quic_transport_error_code_vals), 0x0,
              "Indicates the reason for closing this connection", HFILL }
        },
        { &hf_quic_cc_error_code_app,
            { "Application Error code", "quic_plain.cc.error_code.app",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Indicates the reason for closing this application", HFILL }
        },
        { &hf_quic_cc_frame_type,
            { "Frame Type", "quic_plain.cc.frame_type",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "The type of frame that triggered the error", HFILL }
        },
        { &hf_quic_cc_reason_phrase_length,
            { "Reason phrase Length", "quic_plain.cc.reason_phrase.length",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Specifying the length of the reason phrase", HFILL }
        },
        { &hf_quic_cc_reason_phrase,
            { "Reason phrase", "quic_plain.cc.reason_phrase",
              FT_STRING, BASE_NONE, NULL, 0x0,
              "A human-readable explanation for why the connection was closed", HFILL }
        },
        /* DATAGRAM */
        { &hf_quic_dg_length,
            { "Datagram Length", "quic_plain.dg.length",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Specifying the length of the the datagram in bytes", HFILL }
        },
        { &hf_quic_dg,
            { "Datagram", "quic_plain.dg",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              "The bytes of the datagram to be delivered", HFILL }
        },
        /* ACK-FREQUENCY */
        { &hf_quic_af_sequence_number,
            { "Sequence Number", "quic_plain.af.sequence_number",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Sequence number assigned to the ACK-FREQUENCY frame by the sender to allow receivers to ignore obsolete frames", HFILL }
        },
        { &hf_quic_af_packet_tolerance,
            { "Packet Tolerance", "quic_plain.af.packet_tolerance",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Representing the maximum number of ack-eliciting packets after which the receiver sends an acknowledgement", HFILL }
        },
        { &hf_quic_af_update_max_ack_delay,
            { "Update Max Ack Delay", "quic_plain.af.update_max_ack_delay",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Representing an update to the peer's 'max_ack_delay' transport parameter", HFILL }
        },
        { &hf_quic_ts,
            { "Time Stamp", "quic_plain.ts",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              NULL, HFILL }
        },

        /* Fields for QUIC Stream data reassembly. */
        { &hf_quic_fragment_overlap,
          { "Fragment overlap", "quic_plain.fragment.overlap",
            FT_BOOLEAN, BASE_NONE, NULL, 0x0,
            "Fragment overlaps with other fragments", HFILL }
        },
        { &hf_quic_fragment_overlap_conflict,
          { "Conflicting data in fragment overlap", "quic_plain.fragment.overlap.conflict",
            FT_BOOLEAN, BASE_NONE, NULL, 0x0,
            "Overlapping fragments contained conflicting data", HFILL }
        },
        { &hf_quic_fragment_multiple_tails,
          { "Multiple tail fragments found", "quic_plain.fragment.multipletails",
            FT_BOOLEAN, BASE_NONE, NULL, 0x0,
            "Several tails were found when reassembling the pdu", HFILL }
        },
        { &hf_quic_fragment_too_long_fragment,
          { "Fragment too long", "quic_plain.fragment.toolongfragment",
            FT_BOOLEAN, BASE_NONE, NULL, 0x0,
            "Fragment contained data past end of the pdu", HFILL }
        },
        { &hf_quic_fragment_error,
          { "Reassembling error", "quic_plain.fragment.error",
            FT_FRAMENUM, BASE_NONE, NULL, 0x0,
            "Reassembling error due to illegal fragments", HFILL }
        },
        { &hf_quic_fragment_count,
          { "Fragment count", "quic_plain.fragment.count",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_fragment,
          { "QUIC STREAM Data Fragment", "quic_plain.fragment",
            FT_FRAMENUM, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_fragments,
          { "Reassembled QUIC STREAM Data Fragments", "quic_plain.fragments",
            FT_NONE, BASE_NONE, NULL, 0x0,
            "QUIC STREAM Data Fragments", HFILL }
        },
        { &hf_quic_reassembled_in,
          { "Reassembled PDU in frame", "quic_plain.reassembled_in",
            FT_FRAMENUM, BASE_NONE, NULL, 0x0,
            "The PDU that doesn't end in this fragment is reassembled in this frame", HFILL }
        },
        { &hf_quic_reassembled_length,
          { "Reassembled QUIC STREAM Data length", "quic_plain.reassembled.length",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "The total length of the reassembled payload", HFILL }
        },
        { &hf_quic_reassembled_data,
          { "Reassembled QUIC STREAM Data", "quic_plain.reassembled.data",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            "The reassembled payload", HFILL }
        },
    };

    static gint *ett[] = {
        &ett_quic,
        &ett_quic_short_header,
        &ett_quic_connection_info,
        &ett_quic_ft,
        &ett_quic_ftflags,
        &ett_quic_ftid,
        &ett_quic_fragments,
        &ett_quic_fragment,
    };

    static ei_register_info ei[] = {
        { &ei_quic_connection_unknown,
          { "quic_plain.connection.unknown", PI_PROTOCOL, PI_NOTE,
            "Unknown QUIC connection. Missing Initial Packet or migrated connection?", EXPFILL }
        },
        { &ei_quic_ft_unknown,
          { "quic_plain.ft.unknown", PI_UNDECODED, PI_NOTE,
            "Unknown Frame Type", EXPFILL }
        },
        { &ei_quic_decryption_failed,
          { "quic_plain.decryption_failed", PI_DECRYPTION, PI_WARN,
            "Failed to decrypt handshake", EXPFILL }
        },
        { &ei_quic_protocol_violation,
          { "quic_plain.protocol_violation", PI_PROTOCOL, PI_WARN,
            "Invalid data according to the protocol", EXPFILL }
        },
        { &ei_quic_bad_retry,
          { "quic_plain.bad_retry", PI_PROTOCOL, PI_WARN,
            "Retry Integrity Tag verification failure", EXPFILL }
        },
        { &ei_quic_coalesced_padding_data,
          { "quic_plain.coalesced_padding_data", PI_PROTOCOL, PI_NOTE,
            "Coalesced Padding Data", EXPFILL }
        },
    };

    proto_quic_plain = proto_register_protocol("QUIC Plain", "QUIC Plain", "quic_plain");

    proto_register_field_array(proto_quic_plain, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));

    expert_quic = expert_register_protocol(proto_quic_plain);
    expert_register_field_array(expert_quic, ei, array_length(ei));

    quic_handle = register_dissector("quic_plain", dissect_quic, proto_quic_plain);

    register_init_routine(quic_init);
    register_cleanup_routine(quic_cleanup);

    register_follow_stream(proto_quic_plain, "quic_plain_follow", quic_follow_conv_filter, quic_follow_index_filter, quic_follow_address_filter,
                           udp_port_to_display, follow_quic_tap_listener, get_quic_connections_count,
                           quic_get_sub_stream_id);

    // TODO implement custom reassembly functions that uses the QUIC Connection
    // ID instead of address and port numbers.
    reassembly_table_register(&quic_reassembly_table,
                              &addresses_ports_reassembly_table_functions);

    /*
     * Application protocol. QUIC with TLS uses ALPN.
     * https://tools.ietf.org/html/draft-ietf-quic-transport-23#section-7
     * This could in theory be an arbitrary octet string with embedded NUL
     * bytes, but in practice these do not exist yet.
     */
    quic_proto_dissector_table = register_dissector_table("quic_plain.proto", "QUIC Protocol", proto_quic_plain, FT_STRING, FALSE);
}

void
proto_reg_handoff_quic_plain(void)
{
    tls13_handshake_handle = find_dissector("tls13-handshake");
    dissector_add_uint_with_preference("udp.port", 0, quic_handle);
    heur_dissector_add("udp", dissect_quic_heur, "QUIC_Plain", "quic_plain", proto_quic_plain, HEURISTIC_ENABLE);
    quic_plain_follow_tap = register_tap("quic_plain_follow");
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
