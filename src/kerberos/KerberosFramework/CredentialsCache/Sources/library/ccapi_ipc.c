/*
 * $Header$
 *
 * Copyright 2006 Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * Export of this software from the United States of America may
 * require a specific license from the United States Government.
 * It is the responsibility of any person or organization contemplating
 * export to obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 */

#include "ccapi_ipc.h"
#include "ccapi_os_ipc.h"

/* ------------------------------------------------------------------------ */

static cc_int32 _cci_ipc_send (enum cci_msg_id_t  in_request_name,
                               cc_int32           in_launch_server,
                               cci_identifier_t   in_identifier,
                               cci_stream_t       in_request_data,
                               cci_stream_t      *out_reply_data)
{
    cc_int32 err = ccNoError;
    cci_stream_t request = NULL;
    cci_stream_t reply = NULL;
    cc_int32 reply_error = 0;
    
    if (!in_identifier) { err = cci_check_error (ccErrBadParam); }
    /* in_request_data may be NULL */
    /* out_reply_data may be NULL */
    
    if (!err) {
        err = cci_message_new_request_header (&request,
                                              in_request_name,
                                              in_identifier);
    }
    
    if (!err && in_request_data) {
        err = cci_stream_write (request, 
                                cci_stream_data (in_request_data), 
                                cci_stream_size (in_request_data));
    }
    
    if (!err) {
        err = cci_os_ipc (in_launch_server, request, &reply);

        if (!err && cci_stream_size (reply) > 0) {
            err = cci_message_read_reply_header (reply, &reply_error);
        }
    }
    
    if (!err && reply_error) { 
        err = reply_error;
    }
    
    if (!err && out_reply_data) {
        *out_reply_data = reply;
        reply = NULL; /* take ownership */
    }
    
    cci_stream_release (request);
    cci_stream_release (reply);
    
    return cci_check_error (err);
}

/* ------------------------------------------------------------------------ */

cc_int32 cci_ipc_send (enum cci_msg_id_t  in_request_name,
                       cci_identifier_t   in_identifier,
                       cci_stream_t       in_request_data,
                       cci_stream_t      *out_reply_data)
{
    return cci_check_error (_cci_ipc_send (in_request_name, 1,
                                           in_identifier,
                                           in_request_data,
                                           out_reply_data));
}

/* ------------------------------------------------------------------------ */

cc_int32 cci_ipc_send_no_launch (enum cci_msg_id_t  in_request_name,
                                 cci_identifier_t   in_identifier,
                                 cci_stream_t       in_request_data,
                                 cci_stream_t      *out_reply_data)
{
    return cci_check_error (_cci_ipc_send (in_request_name, 0,
                                           in_identifier,
                                           in_request_data,
                                           out_reply_data));
}
