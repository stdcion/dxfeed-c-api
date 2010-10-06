/*
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Initial Developer of the Original Code is Devexperts LLC.
 * Portions created by the Initial Developer are Copyright (C) 2010
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 */

#include <Windows.h>
#include <stdio.h>

#include "DXFeed.h"
#include "DXNetwork.h"
#include "DXErrorHandling.h"
#include "DXErrorCodes.h"
#include "DXMemory.h"
#include "Subscription.h"
#include "SymbolCodec.h"
#include "EventSubscription.h"
#include "DataStructures.h"
#include "parser.h"
#include "Logger.h"
#include "DXAlgorithms.h"
#include "ConnectionContextData.h"
#include "DXThreads.h"

BOOL APIENTRY DllMain (HANDLE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call) {
	case DLL_PROCESS_ATTACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
    
    return TRUE;
}

/* -------------------------------------------------------------------------- */
/*
 *	Event subscription helper stuff
 */
/* -------------------------------------------------------------------------- */

typedef enum {
    dx_at_add_subscription,
    dx_at_remove_subscription,
    
    /* add new action types above this line */
    
    dx_at_count
} dx_action_type_t;

dx_message_type_t dx_get_subscription_message_type (dx_action_type_t action_type, dx_subscription_type_t subscr_type) {
    static dx_message_type_t subscr_message_types[dx_at_count][dx_st_count] = {
        { MESSAGE_TICKER_ADD_SUBSCRIPTION, MESSAGE_STREAM_ADD_SUBSCRIPTION, MESSAGE_HISTORY_ADD_SUBSCRIPTION },
        { MESSAGE_TICKER_REMOVE_SUBSCRIPTION, MESSAGE_STREAM_REMOVE_SUBSCRIPTION, MESSAGE_HISTORY_REMOVE_SUBSCRIPTION }
    };
    
    return subscr_message_types[action_type][subscr_type];
}

/* -------------------------------------------------------------------------- */
/*
 *	Auxiliary internal functions
 */
/* -------------------------------------------------------------------------- */

bool dx_subscribe_to (dxf_connection_t connection,
                      dx_const_string_t* symbols, int symbol_count, int event_types, bool unsubscribe) {
	int i = 0;

	dx_byte_t* sub_buffer = NULL;
	dx_int_t out_len = 1000;

	for (; i < symbol_count; ++i){
		dx_event_id_t eid = dx_eid_begin;

		for (; eid < dx_eid_count; ++eid) {
			if (event_types & DX_EVENT_BIT_MASK(eid)) {
			    const dx_event_subscription_param_t* subscr_params = NULL;
			    int j = 0;
			    int param_count = dx_get_event_subscription_params(eid, &subscr_params);
			    
			    for (; j < param_count; ++j) {
			        const dx_event_subscription_param_t* cur_param = subscr_params + j;
			        dx_message_type_t msg_type = dx_get_subscription_message_type(unsubscribe ? dx_at_remove_subscription : dx_at_add_subscription, cur_param->subscription_type);
			        
                    if (dx_create_subscription(msg_type, symbols[i], dx_encode_symbol_name(symbols[i]),
                                               cur_param->record_id,
                                               &sub_buffer, &out_len) != R_SUCCESSFUL) {
                        return false;
                    }

                    if (!dx_send_data(connection, sub_buffer, out_len)) {
                        dx_free(sub_buffer);

                        return false;
                    }

                    dx_free(sub_buffer);
			    }
			}
		}
	}
	
	return true;
}

/* -------------------------------------------------------------------------- */

bool dx_subscribe (dxf_connection_t connection, dx_const_string_t* symbols, int symbols_count, int event_types) {
	return dx_subscribe_to(connection, symbols,symbols_count, event_types, false);
}

/* -------------------------------------------------------------------------- */

bool dx_unsubscribe (dxf_connection_t connection, dx_const_string_t* symbols, int symbols_count, int event_types) {
	return dx_subscribe_to(connection, symbols,symbols_count, event_types, true);
}

/* -------------------------------------------------------------------------- */
/*
 *	Delayed tasks data
 */
/* -------------------------------------------------------------------------- */

typedef struct {
    dxf_connection_t* elements;
    int size;
    int capacity;
    
    pthread_mutex_t guard;
    bool guard_initialized;
} dx_connection_array_t;

static dx_connection_array_t g_connection_queue = {0};

/* -------------------------------------------------------------------------- */

bool dx_queue_connection_for_close (dxf_connection_t connection) {
    bool conn_exists = false;
    int conn_index;
    bool failed = false;
    
    if (!g_connection_queue.guard_initialized) {
        return false;
    }
    
    if (!dx_mutex_lock(&g_connection_queue.guard)) {
        return false;
    }

    DX_ARRAY_SEARCH(g_connection_queue.elements, 0, g_connection_queue.size, connection, DX_NUMERIC_COMPARATOR, false,
                    conn_exists, conn_index);

    if (conn_exists) {
        return dx_mutex_unlock(&g_connection_queue.guard);
    }

    DX_ARRAY_INSERT(g_connection_queue, dxf_connection_t, connection, conn_index, dx_capacity_manager_halfer, failed);

    if (failed) {
        dx_mutex_unlock(&g_connection_queue.guard);
        
        return false;
    }

    return dx_mutex_unlock(&g_connection_queue.guard);
}

/* -------------------------------------------------------------------------- */

void dx_close_queued_connections (void) {
    int i = 0;
    
    if (g_connection_queue.size == 0) {
        return;
    }
    
    if (!dx_mutex_lock(&g_connection_queue.guard)) {
        return;
    }
    
    for (; i < g_connection_queue.size; ++i) {
        /* we don't check the result of the operation because this function
           is called implicitly and the user doesn't expect any resource clean-up
           to take place */
        
        dxf_close_connection(g_connection_queue.elements[i]);
    }
    
    g_connection_queue.size = 0;
    
    dx_mutex_unlock(&g_connection_queue.guard);
}

/* -------------------------------------------------------------------------- */

void dx_init_connection_queue (void) {
    static bool initialized = false;
    
    if (!initialized) {
        initialized = true;
        
        g_connection_queue.guard_initialized = dx_mutex_create(&g_connection_queue.guard);
    }
}

/* -------------------------------------------------------------------------- */

void dx_perform_implicit_tasks (void) {
    dx_init_connection_queue();
    dx_close_queued_connections();
}

/* -------------------------------------------------------------------------- */

ERRORCODE dx_perform_common_actions () {
    dx_perform_implicit_tasks();
    
    if (!dx_pop_last_error()) {
        return DXF_FAILURE;
    }
    
    return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/*
 *	DXFeed API implementation
 */
/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_create_connection (const char* host, dxf_conn_termination_notifier_t notifier,
                                            OUT dxf_connection_t* connection) {
    dx_connection_context_t cc;
    
    dx_perform_common_actions();
    
    if (connection == NULL) {
        return DXF_FAILURE;
    }
    
    if ((*connection = dx_init_connection()) == NULL) {
        return DXF_FAILURE;
    }

    {
        dx_string_t w_host = dx_ansi_to_unicode(host);
        
        if (w_host != NULL) {
            dx_logging_info(L"Connecting to host: %s", w_host);
            dx_free(w_host);
        }
    }
    
    cc.receiver = dx_socket_data_receiver;
    cc.notifier = notifier;
    
    if (!dx_bind_to_host(*connection, host, &cc) ||
        !dx_update_record_description(*connection)) {
        dx_deinit_connection(*connection);
        
        *connection = NULL;
        
        return DXF_FAILURE;
    }
    
    return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_close_connection (dxf_connection_t connection) {
    dx_logging_info(L"Disconnect");
    
    if (!dx_is_thread_master()) {
        return dx_queue_connection_for_close(connection) ? DXF_SUCCESS : DXF_FAILURE;
        
        return DXF_SUCCESS;
    }
    
    return (dx_deinit_connection(connection) ? DXF_SUCCESS : DXF_FAILURE);
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_create_subscription (dxf_connection_t connection, int event_types, OUT dxf_subscription_t* subscription){
	static bool symbol_codec_initialized = false;

    dx_logging_info(L"Create subscription, event types: %x", event_types);

    dx_perform_common_actions();
	
	if (!symbol_codec_initialized) {
		symbol_codec_initialized = true;
		
		if (dx_init_symbol_codec() != R_SUCCESSFUL) {
		    return DXF_FAILURE;
		}
	}
	
	if ((*subscription = dx_create_event_subscription(connection, event_types)) == dx_invalid_subscription) {
	    return DXF_FAILURE;
	}

	return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_remove_subscription (dxf_subscription_t subscription) {
    dxf_connection_t connection;
    int events;
    bool mute_state;

    dx_const_string_t* symbols;
    int symbol_count;

    dx_perform_common_actions();

    if (!dx_get_event_subscription_mute_state(subscription, &mute_state) ||
        (!mute_state && (!dx_get_subscription_connection(subscription, &connection) ||
        !dx_get_event_subscription_event_types(subscription, &events) ||
        !dx_get_event_subscription_symbols(subscription, &symbols, &symbol_count) ||
        !dx_unsubscribe(connection, symbols, symbol_count, events) ||
        !dx_mute_event_subscription(subscription)))) {

        return DXF_FAILURE;
    }

    return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_add_subscription (dxf_subscription_t subscription) {
    dxf_connection_t connection;
    int events; 
    bool mute_state;

    dx_const_string_t* symbols;
    int symbols_count;

    dx_perform_common_actions();

    if (!dx_get_event_subscription_mute_state(subscription, &mute_state) ||
        (mute_state && (!dx_unmute_event_subscription(subscription) ||
        !dx_get_subscription_connection(subscription, &connection) ||
        !dx_get_event_subscription_event_types(subscription, &events) ||
        !dx_get_event_subscription_symbols(subscription, &symbols, &symbols_count) ||
        !dx_subscribe(connection, symbols, symbols_count, events)))) {

        return DXF_FAILURE;
    }

    return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_close_subscription (dxf_subscription_t subscription) {
    dxf_connection_t connection;
    int events;
    bool mute_state;

    dx_const_string_t* symbols;
    int symbol_count;

    dx_perform_common_actions();

    if (!dx_get_event_subscription_mute_state(subscription, &mute_state) ||
        (!mute_state && (!dx_get_subscription_connection(subscription, &connection) ||
        !dx_get_event_subscription_event_types(subscription, &events) ||
        !dx_get_event_subscription_symbols(subscription, &symbols, &symbol_count) ||
        !dx_unsubscribe(connection, symbols, symbol_count, events))) ||
        !dx_close_event_subscription(subscription)) {

        return DXF_FAILURE;
    }

    return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_add_symbol (dxf_subscription_t subscription, dx_const_string_t symbol) {
	dx_int_t events;
	dxf_connection_t connection;
	bool mute_state;
	
    dx_logging_info(L"Adding symbol %s", symbol);

    dx_perform_common_actions();
	
	if (!dx_get_subscription_connection(subscription, &connection) ||
	    !dx_get_event_subscription_event_types(subscription, &events) ||
	    !dx_add_symbols(subscription, &symbol, 1) ||
	    !dx_get_event_subscription_mute_state(subscription, &mute_state) ||
	    (!mute_state && !dx_subscribe(connection, &symbol, 1, events))) {
	    
	    return DXF_FAILURE;
	}
	
	return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_add_symbols (dxf_subscription_t subscription, dx_const_string_t* symbols, int symbols_count) {
    dx_int_t i;

    dx_perform_common_actions();

    if (symbols_count < 0) {
        return DXF_FAILURE; //TODO: set_last_error 
    }

    for ( i = 0; i < symbols_count; ++i) {
        if (dxf_add_symbol(subscription, symbols[i]) == DXF_FAILURE) {
            return DXF_FAILURE;
        }
    }

    return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_remove_symbols (dxf_subscription_t subscription, dx_const_string_t* symbols, int symbol_count) {
    dxf_connection_t connection;
    int events;
    bool mute_state;

    dx_perform_common_actions();

    if (!dx_get_event_subscription_mute_state(subscription, &mute_state) ||
        (!mute_state && (!dx_get_subscription_connection(subscription, &connection) ||
        !dx_get_event_subscription_event_types(subscription, &events) ||
        !dx_unsubscribe(connection, symbols, symbol_count, events))) ||
        !dx_remove_symbols(subscription, symbols, symbol_count)) {

        return DXF_FAILURE;
    }

    return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_get_symbols (dxf_subscription_t subscription, OUT dx_const_string_t* symbols, int* symbols_count){
    dx_perform_common_actions();

    if (!dx_get_event_subscription_symbols(subscription, &symbols, symbols_count)) {
        return DXF_FAILURE;
    }

    return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_set_symbols (dxf_subscription_t subscription, dx_const_string_t* symbols, int symbols_count) {	
    dx_perform_common_actions();

    if (dxf_subscription_clear_symbols(subscription) == DXF_FAILURE ||
        dxf_add_symbols(subscription, symbols, symbols_count) == DXF_FAILURE) {

        return DXF_FAILURE;
    }

    return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_subscription_clear_symbols (dxf_subscription_t subscription) {
    dxf_connection_t connection;
    int events; 
    bool mute_state;

    dx_string_t* symbols;
    int symbol_count;

    dx_perform_common_actions();

    if (!dx_get_event_subscription_symbols(subscription, &symbols, &symbol_count) ||
        !dx_get_event_subscription_mute_state(subscription, &mute_state) ||
        (!mute_state && (!dx_get_subscription_connection(subscription, &connection) ||
        !dx_get_event_subscription_event_types(subscription, &events) ||
        !dx_unsubscribe(connection, symbols, symbol_count, events))) ||
        !dx_remove_symbols(subscription, symbols, symbol_count)) {

        return DXF_FAILURE;
    }

    return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_attach_event_listener (dxf_subscription_t subscription, dx_event_listener_t event_listener) {
    dx_perform_common_actions();
	
	if (!dx_add_listener (subscription, event_listener)) {
		return DXF_FAILURE;
	}
	
	return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_detach_event_listener (dxf_subscription_t subscription, dx_event_listener_t event_listener) {
	dx_perform_common_actions();
	
	if (!dx_remove_listener(subscription, event_listener)) {
		return DXF_FAILURE;
    }
		
	return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_get_subscription_event_types (dxf_subscription_t subscription, OUT int* event_types) {
	dx_perform_common_actions();
	
	if (!dx_get_event_subscription_event_types(subscription, event_types)) {
		return DXF_FAILURE;
	}

	return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_get_last_event (dxf_connection_t connection, int event_type, dx_const_string_t symbol, OUT dx_event_data_t* data) {
    dx_perform_common_actions();
    
    if (!dx_get_last_symbol_event(connection, symbol, event_type, data)) {
        return DXF_FAILURE;
    }

    return DXF_SUCCESS;
}

/* -------------------------------------------------------------------------- */

DXFEED_API ERRORCODE dxf_get_last_error (int* subsystem_id, int* error_code, dx_const_string_t* error_descr) {
    int res = dx_get_last_error(subsystem_id, error_code, error_descr);

    switch (res) {
    case efr_no_error_stored:
        if (subsystem_id != NULL) {
            *subsystem_id = dx_sc_invalid_subsystem;
        }

        if (error_code != NULL) {
            *error_code = DX_INVALID_ERROR_CODE;
        }

        if (error_descr != NULL) {
            *error_descr = L"No error occurred";
        }
    case efr_success:
        return DXF_SUCCESS;
    }

    return DXF_FAILURE;
}