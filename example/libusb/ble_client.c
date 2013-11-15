/*
 * Copyright (C) 2011-2013 by BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. This software may not be used in a commercial product
 *    without an explicit license granted by the copyright holder. 
 *
 * THIS SOFTWARE IS PROVIDED BY MATTHIAS RINGWALD AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

//*****************************************************************************
//
// BLE Client
//
//*****************************************************************************


// NOTE: Supports only a single connection

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <btstack/run_loop.h>
#include <btstack/hci_cmds.h>

#include "config.h"

#include "ble_client.h"
#include "ad_parser.h"

#include "debug.h"
#include "btstack_memory.h"
#include "hci.h"
#include "hci_dump.h"
#include "l2cap.h"
#include "att.h"

// gatt client state
typedef enum {
    W4_ON,
    IDLE,
    //
    START_SCAN,
    W4_SCAN_ACTIVE,
    //
    SCAN_ACTIVE,
    //
    STOP_SCAN,
    W4_SCAN_STOPPED,
    // IDLE,
    W4_CONNECTED,
    CONNECTED,
    //
    DISCONNECT,
    W4_DISCONNECTED
} state_t;


static state_t state = W4_ON;
static linked_list_t le_connections = NULL;


void (*le_central_callback)(le_central_event_t * event);

static void gatt_client_run();


// START Helper Functions - to be sorted
static int l2cap_can_send_conectionless_packet_now(){
    return hci_can_send_packet_now(HCI_ACL_DATA_PACKET);
}

static uint16_t l2cap_max_mtu_for_handle(uint16_t handle){
    return l2cap_max_mtu();
}
// END Helper Functions

static void dummy_notify(le_central_event_t* event){}

void le_central_register_handler(void (*le_callback)(le_central_event_t* event)){
    le_central_callback = dummy_notify;
    if (le_callback != NULL){
        le_central_callback = le_callback;
    } 
}

static void hexdump2(void *data, int size){
    int i;
    for (i=0; i<size;i++){
        printf("%02X ", ((uint8_t *)data)[i]);
    }
    printf("\n");
}


void gatt_client_init(){
    le_connections = NULL;
    state = W4_ON;
}

void le_central_start_scan(){
    if (state != IDLE) return; 
    state = START_SCAN;
    gatt_client_run();
}

void le_central_stop_scan(){
    if (state != SCAN_ACTIVE) return;
    state = STOP_SCAN;
    gatt_client_run();
}

static void le_peripheral_init(le_peripheral_t *context){
    memset(context, 0, sizeof(le_peripheral_t));
    context->state = P_IDLE;
}

void le_central_connect(le_peripheral_t *context, uint8_t addr_type, bd_addr_t addr){
    le_peripheral_init(context);
    context->state = P_W2_CONNECT;
    context->address_type = addr_type;
    memcpy (context->address, addr, 6);
    linked_list_add(&le_connections, (linked_item_t *) context);

    if (state == SCAN_ACTIVE){
        le_central_stop_scan();
    }
    gatt_client_run();
}

static le_peripheral_t * get_peripheral_for_handle(uint16_t handle){
    linked_item_t *it;
    for (it = (linked_item_t *) le_connections; it ; it = it->next){
        le_peripheral_t * peripheral = (le_peripheral_t *) it;
        if (peripheral->handle == handle){
            return peripheral;
        }
    }
    return NULL;
}

static le_peripheral_t * get_peripheral_with_state(peripheral_state_t p_state){
    linked_item_t *it;
    for (it = (linked_item_t *) le_connections; it ; it = it->next){
        le_peripheral_t * peripheral = (le_peripheral_t *) it;
        if (peripheral->state == p_state){
            return peripheral;
        }
    }
    return NULL;
}

static inline le_peripheral_t * get_peripheral_w4_disconnected(){
    return get_peripheral_with_state(P_W4_DISCONNECTED);
}

static inline le_peripheral_t * get_peripheral_w2_connect(){
    return get_peripheral_with_state(P_W2_CONNECT);
}

static inline le_peripheral_t * get_peripheral_w4_connected(){
    return get_peripheral_with_state(P_W4_CONNECTED);
}

static inline le_peripheral_t * get_peripheral_w2_exchange_MTU(){
    return get_peripheral_with_state(P_W2_EXCHANGE_MTU);
}


void le_central_cancel_connect(le_peripheral_t *context){
    if (context->state == P_IDLE) return;
    
    if (context->state == P_W2_CONNECT){
        context->state = P_IDLE;
        return;
    }
    state = DISCONNECT;
}


static void gatt_client_run(){
    if (!hci_can_send_packet_now(HCI_COMMAND_DATA_PACKET)) return;
    if (!l2cap_can_send_conectionless_packet_now()) return;

    le_peripheral_t * peripheral = NULL;
    // hadle peripherals list
    switch(state){
        case IDLE:
            if (! (peripheral = get_peripheral_w2_connect()) ) break;
               
            peripheral->state = P_W4_CONNECTED;
            state = W4_CONNECTED;

            hci_send_cmd(&hci_le_create_connection, 
                 1000,      // scan interval: 625 ms
                 1000,      // scan interval: 625 ms
                 0,         // don't use whitelist
                 0,         // peer address type: public
                 peripheral->address,      // remote bd addr
                 peripheral->address_type, // random or public
                 80,        // conn interval min
                 80,        // conn interval max (3200 * 0.625)
                 0,         // conn latency
                 2000,      // supervision timeout
                 0,         // min ce length
                 1000       // max ce length
            );

            break;

        case START_SCAN:
            state = W4_SCAN_ACTIVE;
            hci_send_cmd(&hci_le_set_scan_enable, 1, 0);
            break;
        case STOP_SCAN:
            state = W4_SCAN_STOPPED;
            hci_send_cmd(&hci_le_set_scan_enable, 0, 0);
            break;
        case CONNECTED:{
            le_peripheral_t * peripheral = get_peripheral_w2_exchange_MTU();
            
            if (!peripheral) break;

            peripheral->state = P_W4_EXCHANGE_MTU;
            uint16_t mtu = l2cap_max_mtu_for_handle(peripheral->handle);
            
            uint8_t request[3];
            request[0] = ATT_EXCHANGE_MTU_REQUEST;
            bt_store_16(request, 1, mtu);
            l2cap_send_connectionless(peripheral->handle, L2CAP_CID_ATTRIBUTE_PROTOCOL, request, sizeof(request));
            break;
        }
        case DISCONNECT:
            peripheral->state = P_W4_DISCONNECTED;
            state = W4_DISCONNECTED;
            hci_send_cmd(&hci_le_create_connection_cancel);
            break;
        default:
            break;
    }
}

static void dump_ad_event(ad_event_t e){
    printf("evt-type %u, addr-type %u, addr %s, rssi %u, length adv %u, data: ", e.event_type, 
            e.address_type, bd_addr_to_str(e.address), e.rssi, e.length); 
    hexdump2( e.data, e.length);                            
}


static void att_packet_handler(uint8_t packet_type, uint16_t handle, uint8_t *packet, uint16_t size){
    if (packet_type != ATT_DATA_PACKET) return;

    switch (packet[0]){
        case ATT_EXCHANGE_MTU_RESPONSE:
        {
            le_peripheral_t * peripheral = get_peripheral_for_handle(handle);
         
            uint16_t remote_rx_mtu = READ_BT_16(packet, 1);
            uint16_t local_rx_mtu = l2cap_max_mtu_for_handle(handle);
            peripheral->mtu = remote_rx_mtu < local_rx_mtu ? remote_rx_mtu : local_rx_mtu;

            le_peripheral_event_t p_connected_event;
            p_connected_event.type = GATT_CONNECTION_COMPLETE;
            p_connected_event.device = peripheral;
            p_connected_event.status = 0;
            (*le_central_callback)((le_central_event_t*)&p_connected_event); 

            peripheral->state = P_CONNECTED; 
            break;
        }
        default:
            break;
    }
}


static void packet_handler (void * connection, uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    switch (packet_type) {
		case HCI_EVENT_PACKET:
			switch (packet[0]) {
				
                case BTSTACK_EVENT_STATE:
					// bt stack activated, get started
					if (packet[2] == HCI_STATE_WORKING) {
                        printf("Working!\n");
                        state = IDLE;
                    }
					break;
                
                case HCI_EVENT_COMMAND_COMPLETE:
                    if (COMMAND_COMPLETE_EVENT(packet, hci_le_set_scan_enable)){
                        switch(state){
                            case W4_SCAN_ACTIVE:
                                state = SCAN_ACTIVE;
                                break;
                            case W4_SCAN_STOPPED:
                                state = IDLE;
                                break;
                            default:
                                break;
                        }
                    }
                    break;

                case HCI_EVENT_LE_META:
                    switch (packet[2]) {
                        case HCI_SUBEVENT_LE_ADVERTISING_REPORT: 
                        if (state != SCAN_ACTIVE) break;
                        {
                            int num_reports = packet[3];
                            int i;
                            int total_data_length = 0;
                            int data_offset = 0;

                            for (i=0; i<num_reports;i++){
                                total_data_length += packet[4+num_reports*8+i];  
                            }

                            for (i=0; i<num_reports;i++){
                                ad_event_t advertisement_event;
                                advertisement_event.type = GATT_ADVERTISEMENT;
                                advertisement_event.event_type = packet[4+i];
                                advertisement_event.address_type = packet[4+num_reports+i];
                                bt_flip_addr(advertisement_event.address, &packet[4+num_reports*2+i*6]);
                                advertisement_event.length = packet[4+num_reports*8+i];
                                advertisement_event.data = &packet[4+num_reports*9+data_offset];
                                data_offset += advertisement_event.length;
                                advertisement_event.rssi = packet[4+num_reports*9+total_data_length + i];
                                
                                (*le_central_callback)((le_central_event_t*)&advertisement_event); 

                                dump_ad_event(advertisement_event);
                            }
                            break;
                        }
                        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:{
                            switch (state){
                                case W4_CONNECTED:{
                                    le_peripheral_t * peripheral = get_peripheral_w4_connected();

                                    if (packet[3] != 0) {
                                        le_peripheral_event_t p_connected_event;
                                        p_connected_event.type = GATT_CONNECTION_COMPLETE;
                                        p_connected_event.device = peripheral;
                                        p_connected_event.status = packet[3];
                                        (*le_central_callback)((le_central_event_t*)&p_connected_event); 
                                        return;
                                    }
                                    
                                    state = CONNECTED;
                                    peripheral->state = P_W2_EXCHANGE_MTU;
                                    
                                    peripheral->handle = READ_BT_16(packet, 4);

                                    printf("Connected: handle 0x%04x!\n", peripheral->handle);
                                    break;
                                }
                                    
                                case W4_DISCONNECTED: {
                                    le_peripheral_t * peripheral = get_peripheral_w4_disconnected();
                                    state = IDLE;
                                    peripheral->state = P_IDLE;

                                    le_peripheral_event_t p_disconnected_event;
                                    p_disconnected_event.type = GATT_CONNECTION_COMPLETE;
                                    p_disconnected_event.device = peripheral;
                                    p_disconnected_event.status = packet[3];

                                    (*le_central_callback)((le_central_event_t*)&p_disconnected_event); 

                                    break;
                                }
                                default:
                                    break;
                            }
                            break;
                        }
                            
                        default:
                            break;
                    }
                    break;

                default:
                    break;
			}
	}

    gatt_client_run();
}


void setup(void){
    /// GET STARTED with BTstack ///
    btstack_memory_init();
    run_loop_init(RUN_LOOP_POSIX);
        
    // use logger: format HCI_DUMP_PACKETLOGGER, HCI_DUMP_BLUEZ or HCI_DUMP_STDOUT
    hci_dump_open("/tmp/hci_dump.pklg", HCI_DUMP_PACKETLOGGER);

    // init HCI
    hci_transport_t    * transport = hci_transport_usb_instance();
    hci_uart_config_t  * config    = NULL;
    bt_control_t       * control   = NULL;
    remote_device_db_t * remote_db = (remote_device_db_t *) &remote_device_db_memory;
        
    hci_init(transport, config, control, remote_db);

    l2cap_init();
    l2cap_register_fixed_channel(att_packet_handler, L2CAP_CID_ATTRIBUTE_PROTOCOL);
    l2cap_register_packet_handler(packet_handler);
}

// main == setup
int main(void)
{
    setup();

    // turn on!
    hci_power_control(HCI_POWER_ON);
    
    // go!
    run_loop_execute(); 
    
    // happy compiler!
    return 0;
}
