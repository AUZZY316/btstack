/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4; tab-width: 4 -*-  */
/*
 * bnep_test.c
 * Copyright (C) 2014 BlueKitchen GmbH
 * based on panu_demo implemented by Ole Reinhardt <ole.reinhardt@kernelconcepts.de>
 */

#include "btstack-config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>


#include <btstack/hci_cmds.h>
#include <btstack/run_loop.h>
#include <btstack/sdp_util.h>

#include "hci.h"
#include "btstack_memory.h"
#include "hci_dump.h"
#include "l2cap.h"
#include "pan.h"

// prototypes
 void show_usage();

// Configuration for PTS
static bd_addr_t pts_addr = {0x00,0x1b,0xDC,0x07,0x32,0xEF};    

// state
static uint16_t bnep_protocol_id    = 0x000f;
static uint16_t bnep_l2cap_psm      = 0;
static uint32_t bnep_remote_uuid    = 0;
static uint16_t bnep_version        = 0;
static uint16_t bnep_cid            = 0;

static uint8_t network_buffer[BNEP_MTU_MIN];
static size_t  network_buffer_len = 0;

/** Testig User Interface **/
void show_usage(){

    printf("\n--- Bluetooth BNEP Test Console ---\n");
    printf("Local UUID %04x, remote UUID %04x\n", 0, 0);
    printf("---\n");
    printf("p - connect to PTS\n");
    printf("e - send general Ethernet packet\n");
    printf("c - send compressed Ethernet packet\n");
    printf("s - send source only compressed Ethernet packet\n");
    printf("d - send destination only compressed Ethernet packet\n");
    printf("f - set network filter\n");
    printf("m - set multicast network filter\n");
    printf("---\n");
    printf("Ctrl-c - exit\n");
    printf("---\n");
}

int  stdin_process(struct data_source *ds){
    char buffer;
    read(ds->fd, &buffer, 1);

    switch (buffer){
        case 'p':
            break;
        case 'e':
            break;
        case 'c':
            break;
        case 's':
            break;
        case 'd':
            break;
        case 'f':
            break;
        case 'm':
            break;
        default:
            show_usage();
            break;

    }
    return 0;
}

/*************** PANU client routines *********************/

static void packet_handler (void * connection, uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    uint8_t   event;
    bd_addr_t event_addr;
    uint16_t  uuid_source;
    uint16_t  uuid_dest;
    uint16_t  mtu;    
  
    switch (packet_type) {
		case HCI_EVENT_PACKET:
            event = packet[0];
            switch (event) {            
                case BTSTACK_EVENT_STATE:
                    /* BT Stack activated, get started */ 
                    if (packet[2] == HCI_STATE_WORKING) {
                        printf("BNEP Test ready\n");
                    }
                    break;

                case HCI_EVENT_COMMAND_COMPLETE:
					if (COMMAND_COMPLETE_EVENT(packet, hci_read_bd_addr)){
                        bt_flip_addr(event_addr, &packet[6]);
                        printf("BD-ADDR: %s\n", bd_addr_to_str(event_addr));
                        break;
                    }
                    break;

                case HCI_EVENT_USER_CONFIRMATION_REQUEST:
                    // inform about user confirmation request
                    printf("SSP User Confirmation Request with numeric value '%06u'\n", READ_BT_32(packet, 8));
                    printf("SSP User Confirmation Auto accept\n");
                    break;
					
				case BNEP_EVENT_OPEN_CHANNEL_COMPLETE:
                    if (packet[2]) {
                        printf("BNEP channel open failed, status %02x\n", packet[2]);
                    } else {
                        // data: event(8), len(8), status (8), bnep source uuid (16), bnep destination uuid (16), remote_address (48)
                        uuid_source = READ_BT_16(packet, 3);
                        uuid_dest   = READ_BT_16(packet, 5);
                        mtu         = READ_BT_16(packet, 7);
                        bnep_cid    = channel;
                        //bt_flip_addr(event_addr, &packet[9]); 
                        memcpy(&event_addr, &packet[9], sizeof(bd_addr_t));
                        printf("BNEP connection open succeeded to %s source UUID 0x%04x dest UUID: 0x%04x, max frame size %u\n", bd_addr_to_str(event_addr), uuid_source, uuid_dest, mtu);
                    }
					break;
                    
                case BNEP_EVENT_CHANNEL_TIMEOUT:
                    printf("BNEP channel timeout! Channel will be closed\n");
                    break;
                    
                case BNEP_EVENT_CHANNEL_CLOSED:
                    printf("BNEP channel closed\n");
                    break;

                case BNEP_EVENT_READY_TO_SEND:
                    /* Check for parked network packets and send it out now */
                    if (network_buffer_len > 0) {
                        bnep_send(bnep_cid, network_buffer, network_buffer_len);
                        network_buffer_len = 0;
                    }
                    break;
                    
                default:
                    break;
            }
            break;
        case BNEP_DATA_PACKET:
            // show received packet on console
            printf("BNEP packet received\n");
            printf("Dst Addr: %s\n", bd_addr_to_str(&packet[0]));
            printf("Src Addr: %s\n", bd_addr_to_str(&packet[6]));
            printf("Net Type: %04x\n", READ_NET_16(packet, 12));
            // ignore the next 60 bytes
            hexdumpf(&packet[74], size - 74);
            break;            
            
        default:
            break;
    }
}

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){
    
    /* Initialize L2CAP */
    l2cap_init();
    l2cap_register_packet_handler(packet_handler);

    /* Initialise BNEP */
    bnep_init();
    bnep_register_packet_handler(packet_handler);
    bnep_register_service(NULL, BNEP_UUID_PANU, 1691);  /* Minimum L2CAP MTU for bnep is 1691 bytes */

    /* Turn on the device */
    hci_power_control(HCI_POWER_ON);

    /* Start mainloop */
    run_loop_execute(); 
    return 0;
}