/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "tls/extensions/s2n_client_key_share.h"
#include "tls/extensions/s2n_key_share.h"
#include "tls/s2n_security_policies.h"

#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_safety.h"

/**
 * Specified in https://tools.ietf.org/html/rfc8446#section-4.2.8
 * "The "key_share" extension contains the endpoint's cryptographic parameters."
 *
 * Structure:
 * Extension type (2 bytes)
 * Extension data size (2 bytes)
 * Client shares size (2 bytes)
 * Client shares:
 *      Named group (2 bytes)
 *      Key share size (2 bytes)
 *      Key share (variable size)
 *
 * This extension only modifies the connection's client ecc_evp_params. It does
 * not make any decisions about which set of params to use.
 *
 * The server will NOT alert when processing a client extension that violates the RFC.
 * So the server will accept:
 * - Multiple key shares for the same named group. The server will accept the first
 *   key share for the group and ignore any duplicates.
 * - Key shares for named groups not in the client's supported_groups extension.
 **/

static int s2n_client_key_share_send(struct s2n_connection *conn, struct s2n_stuffer *out);
static int s2n_client_key_share_recv(struct s2n_connection *conn, struct s2n_stuffer *extension);

const s2n_extension_type s2n_client_key_share_extension = {
    .iana_value = TLS_EXTENSION_KEY_SHARE,
    .is_response = false,
    .send = s2n_client_key_share_send,
    .recv = s2n_client_key_share_recv,
    .should_send = s2n_extension_send_if_tls13_connection,
    .if_missing = s2n_extension_noop_if_missing,
};

static int s2n_ecdhe_supported_curves_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    notnull_check(conn);
    notnull_check(conn->config);

    const struct s2n_ecc_preferences *ecc_pref = NULL;
    GUARD(s2n_connection_get_ecc_preferences(conn, &ecc_pref));
    notnull_check(ecc_pref);

    const struct s2n_ecc_named_curve *named_curve = NULL;
    struct s2n_ecc_evp_params *ecc_evp_params = NULL;

    for (uint32_t i = 0; i < ecc_pref->count; i++) {
        ecc_evp_params = &conn->secure.client_ecc_evp_params[i];
        named_curve = ecc_pref->ecc_curves[i];

        ecc_evp_params->negotiated_curve = named_curve;
        ecc_evp_params->evp_pkey = NULL;
        GUARD(s2n_ecdhe_parameters_send(ecc_evp_params, out));
    }

    return S2N_SUCCESS;
}

static int s2n_client_key_share_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    struct s2n_stuffer_reservation shares_size;
    GUARD(s2n_stuffer_reserve_uint16(out, &shares_size));

    GUARD(s2n_ecdhe_supported_curves_send(conn, out));

    GUARD(s2n_stuffer_write_vector_size(shares_size));

    return S2N_SUCCESS;
}

static int s2n_client_key_share_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    if (!s2n_is_tls13_enabled()) {
        return S2N_SUCCESS;
    }

    notnull_check(conn);
    notnull_check(extension);

    const struct s2n_ecc_preferences *ecc_pref = NULL;
    GUARD(s2n_connection_get_ecc_preferences(conn, &ecc_pref));
    notnull_check(ecc_pref);

    uint16_t key_shares_size;
    GUARD(s2n_stuffer_read_uint16(extension, &key_shares_size));
    S2N_ERROR_IF(s2n_stuffer_data_available(extension) < key_shares_size, S2N_ERR_BAD_MESSAGE);

    const struct s2n_ecc_named_curve *supported_curve;
    struct s2n_blob point_blob;
    uint16_t named_group, share_size;
    uint32_t supported_curve_index;

    /* Whether a match was found */
    uint8_t match = 0;

    /* bytes_processed is declared as a uint32_t to avoid integer overflow in later calculations */
    uint32_t bytes_processed = 0;

    while (bytes_processed < key_shares_size) {
        GUARD(s2n_stuffer_read_uint16(extension, &named_group));
        GUARD(s2n_stuffer_read_uint16(extension, &share_size));

        S2N_ERROR_IF(s2n_stuffer_data_available(extension) < share_size, S2N_ERR_BAD_MESSAGE);
        bytes_processed += share_size + S2N_SIZE_OF_NAMED_GROUP + S2N_SIZE_OF_KEY_SHARE_SIZE;

        supported_curve = NULL;
        for (uint32_t i = 0; i < ecc_pref->count; i++) {
            if (named_group == ecc_pref->ecc_curves[i]->iana_id) {
                supported_curve_index = i;
                supported_curve = ecc_pref->ecc_curves[i];
                break;
            }
        }

        /* Ignore unsupported curves */
        if (!supported_curve) {
            GUARD(s2n_stuffer_skip_read(extension, share_size));
            continue;
        }

        /* Ignore curves that we've already received material for */
        if (conn->secure.client_ecc_evp_params[supported_curve_index].negotiated_curve) {
            GUARD(s2n_stuffer_skip_read(extension, share_size));
            continue;
        }

        /* Ignore curves with unexpected share sizes */
        if (supported_curve->share_size != share_size) {
            GUARD(s2n_stuffer_skip_read(extension, share_size));
            continue;
        }

        GUARD(s2n_ecc_evp_read_params_point(extension, share_size, &point_blob));

        conn->secure.client_ecc_evp_params[supported_curve_index].negotiated_curve = supported_curve;
        if (s2n_ecc_evp_parse_params_point(&point_blob, &conn->secure.client_ecc_evp_params[supported_curve_index]) < 0) {
            /* Ignore curves with points we can't parse */
            conn->secure.client_ecc_evp_params[supported_curve_index].negotiated_curve = NULL;
            GUARD(s2n_ecc_evp_params_free(&conn->secure.client_ecc_evp_params[supported_curve_index]));
        } else {
            match = 1;
        }
    }

    /* If there was no matching key share then we received an empty key share extension
     * or we didn't match a keyshare with a supported group. We should send a retry. */
    if (match == 0) {
        GUARD(s2n_set_hello_retry_required(conn));
    }

    return S2N_SUCCESS;
}

/* Old-style extension functions -- remove after extensions refactor is complete */

uint32_t s2n_extensions_client_key_share_size(struct s2n_connection *conn)
{
    notnull_check(conn);

    const struct s2n_ecc_preferences *ecc_pref = NULL;
    GUARD(s2n_connection_get_ecc_preferences(conn, &ecc_pref));
    notnull_check(ecc_pref);

    uint32_t s2n_client_key_share_extension_size = S2N_SIZE_OF_EXTENSION_TYPE
            + S2N_SIZE_OF_EXTENSION_DATA_SIZE
            + S2N_SIZE_OF_CLIENT_SHARES_SIZE;

    for (uint32_t i = 0; i < ecc_pref->count ; i++) {
        s2n_client_key_share_extension_size += S2N_SIZE_OF_KEY_SHARE_SIZE + S2N_SIZE_OF_NAMED_GROUP;
        s2n_client_key_share_extension_size += ecc_pref->ecc_curves[i]->share_size; 
    }

    return s2n_client_key_share_extension_size;
}

int s2n_extensions_client_key_share_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    return s2n_extension_send(&s2n_client_key_share_extension, conn, out);
}

int s2n_extensions_client_key_share_recv(struct s2n_connection *conn, struct s2n_stuffer *extension)
{
    return s2n_extension_recv(&s2n_client_key_share_extension, conn, extension);
}
