/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>


#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"
#include "sr_nat.h"

/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 *
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance* sr)
{
    /* REQUIRES */
    assert(sr);

    /* Initialize cache and cache cleanup thread */
    sr_arpcache_init(&(sr->cache));

    /* NAT */
    if (sr->nat_mode) {
         printf ("Nat is enabled... \n");
   	 sr_nat_init(&(sr->nat));
    }
    pthread_attr_init(&(sr->attr));
    pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_t thread;

    pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);
    
    /* Add initialization code here! */

} /* -- sr_init -- */

/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance* sr,
        uint8_t * packet/* lent */,
        unsigned int len,
        char* interface/* lent */)
{
    /* REQUIRES */
    assert(sr);
    assert(packet);
    assert(interface);

    /* Get ethernet header */
    sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *) packet;

    /* Check for mininum length requirement */
    if (check_min_len (len, ETH_HDR)) {
        printf("Ethernet header does not satisfy mininum length requirement \n");
        return;
    }

    uint16_t ethtype = ethertype((uint8_t *)eth_hdr);

    if (ethtype == ethertype_ip){  
        sr_iphandler(sr, packet, len, interface);
    } else if (ethtype == ethertype_arp){
        sr_arphandler(sr, packet, len, interface);
    }

} /* end sr_ForwardPacket */

void sr_arphandler (struct sr_instance* sr,
        uint8_t * packet/* lent */,
        unsigned int len,
        char* interface/* lent */)
{
    assert(sr);
    assert(packet);
    assert(interface);

    /* Get ARP header */
    sr_arp_hdr_t *arp_hdr = get_arp_hdr (packet);
    /* Get Ethernet header */
    sr_ethernet_hdr_t *eth_hdr = get_eth_hdr(packet);

    /* Check mininum length */
    if (check_min_len (len, ARP_PACKET)) {
        printf("ARP packet does not satisfy mininum length requirement \n");
        return;
    }

    /* If target interface is not NULL, the packet is for one of the interfaces in our router */
    struct sr_if *target_iface = get_router_interface (arp_hdr->ar_tip, sr);
    if (target_iface) {
        if (ntohs(arp_hdr->ar_op) == arp_op_request) {
            printf("Received ARP Request!\n");
            /* Create reply packet to send back to sender */
            int packet_len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t);
            uint8_t *arp_reply = malloc(packet_len);

            /* Create Ethernet header */
            create_ethernet_header (eth_hdr, arp_reply, sr_get_interface(sr, interface)->addr, eth_hdr->ether_shost, htons(ethertype_arp)); 
            /* Create ARP header */
            create_arp_header (arp_hdr, arp_reply, target_iface); 

            /* Send out ARP reply */
            sr_send_packet(sr, arp_reply, packet_len, target_iface->name);
            printf("Sent an ARP reply packet\n");
            free(arp_reply);
            return;
    
        } else if (ntohs(arp_hdr -> ar_op) == arp_op_reply) {
            /* Cache ARP packet and go through the request queue to send out outstanding packets */
            printf("Received ARP reply!\n");
            send_arp_req (arp_hdr, &(sr->cache), sr);
            return;
        }
    } else {
        printf ("Dropping packet! ARP packet is not targeted at our router.");
        return; 
    }
}

void sr_iphandler (struct sr_instance* sr,
        uint8_t * packet,
        unsigned int len,
        char* interface) 
{
    assert(sr);
    assert(packet);
    assert(interface);

    /* Get Ethernet header */
    sr_ethernet_hdr_t* eth_hdr = get_eth_hdr(packet);

    /* Get IP header */
    sr_ip_hdr_t * ip_hdr = get_ip_hdr(packet);

    /* ARP Cache */
    struct sr_arpcache *sr_cache = &sr->cache;

    /* Get IP protocol */
    uint8_t ip_p = ip_hdr->ip_p;

    /* */
    struct sr_if *target_iface = get_router_interface (ip_hdr->ip_dst, sr);
    struct sr_rt *dst_lpm = sr_routing_lpm (sr, ip_hdr->ip_dst);

    /* Check for mininum length  */
    if (check_min_len (len, IP_PACKET)) {
        printf("IP packet does not satisfy mininum length requirement \n");
        return;
    }

    /* Verify checksum */
    if (verify_ip_checksum (ip_hdr)) {
        printf("IP Header checksum fails\n");
        return;
    } 

    /* If time exceeded, send out time exceeded message */
    if (decrement_and_recalculate (ip_hdr)){
        printf("TTL of IP is 0. Time exceeded. \n");
        int packet_len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t11_hdr_t);
        uint8_t *new_packet = malloc(packet_len);

        /* Create ethernet header */
        create_ethernet_header (eth_hdr, new_packet, sr_get_interface(sr, interface)->addr, eth_hdr->ether_shost, htons(ethertype_ip)); 

        /* Create IP header */
        create_ip_header (ip_hdr, new_packet, sr_get_interface(sr, interface)->ip, ip_hdr->ip_src); 

        /* Create ICMP Header */
        create_icmp_type3_header (ip_hdr, new_packet, time_exceeded_type, time_exceeded_code); 

        /* Send time exceeded ICMP packet */
        struct sr_arpentry * arp_entry = sr_arpcache_lookup (sr_cache, ip_hdr->ip_src);
        if (arp_entry) {
            sr_send_packet (sr, new_packet, packet_len, interface);
        } else {
            struct sr_arpreq * req = sr_arpcache_queuereq(sr_cache, ip_hdr->ip_src, new_packet, packet_len, interface);
            handle_arpreq(req, sr);
        }

        free (new_packet);

        return;
    }

    /* If there is no match in routing table and the packet is not for one of the interfaces, send ICMP net unreachable */
    if (target_iface == NULL && dst_lpm == NULL) {
        int packet_len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
        uint8_t *new_packet = malloc(packet_len);

        /* Create ethernet header */
        create_ethernet_header (eth_hdr, new_packet, eth_hdr->ether_dhost, eth_hdr->ether_shost, htons(ethertype_ip));

        /* Create ip header */
        create_ip_header (ip_hdr, new_packet, sr_get_interface(sr, interface)->ip, ip_hdr->ip_src);

        /* Create icmp header */
        create_icmp_type3_header (ip_hdr, new_packet, dest_net_unreachable_type, dest_net_unreachable_code);

        /* Look up routing table for rt entry that is mapped to the source of received packet */
        struct sr_rt *src_lpm = sr_routing_lpm(sr, ip_hdr->ip_src);

        /* Send ICMP net unreachable message */
        send_icmp_type3_msg (new_packet, src_lpm, sr_cache, sr, interface, packet_len); 

        free (new_packet);
    } else {
        if (sr->nat_mode) {
            if (sr_nat_is_interface_internal(interface)) {
                /* Packet is for the router or the internal interface */
                if (target_iface || sr_nat_is_interface_internal(dst_lpm->interface)) {
                    /* Get ICMP header */
                    sr_icmp_hdr_t *icmp_hdr = get_icmp_hdr (packet);

                    if (ip_p == ip_protocol_icmp) {
                        /* Check for mininum length  */
                        if (check_min_len (len, ICMP_PACKET)) {
                            printf("IP packet does not satisfy mininum length requirement \n");
                            return;
                        }

                        /* Check ICMP checksum */
                        if (verify_icmp_checksum (icmp_hdr, ICMP_PACKET, len)) {
                            printf("ICMP packet fails checksum \n");
                            return;
                        } 

                        if (is_icmp_echo_request (icmp_hdr)) {
                            printf ("Sending ICMP echo reply. \n");
			                 send_echo_reply (sr, packet, len, interface);
                        } else {
                            printf("Unknown ICMP type \n");
                            return;
                        }
                    /* Port unreachable */
                    } else if (ip_p == ip_protocol_tcp) {
                        int packet_len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t);

                        uint8_t *new_packet = malloc(len);

                        /* Create ethernet header */
                        create_ethernet_header (eth_hdr, new_packet, sr_get_interface(sr, interface)->addr, eth_hdr->ether_shost, htons(ethertype_ip));

                        /* Create ip header */
                        create_ip_header (ip_hdr, new_packet, target_iface->ip, ip_hdr->ip_src);

                        /* Create icmp header */
                        create_icmp_type3_header (ip_hdr, new_packet, port_unreachable_type, port_unreachable_code);

                        /* Send ICMP port unreachable message */
                        sr_send_packet (sr, new_packet, packet_len, interface);

                        free(new_packet);
                        return; 
                    } else {
                        printf("Received ICMP packet of unknown type\n");
                        return; 
                    }

                /* Packet is for the external interface */
                } else {
                    if (ip_p == ip_protocol_icmp) {
                        printf("Protocol is ICMP\n");
                        /* Get ICMP header */
                        sr_icmp_hdr_t *icmp_hdr = get_icmp_hdr (packet);

                         /* Check for mininum length  */
                        if (check_min_len (len, ICMP_PACKET)) {
                            printf("IP packet does not satisfy mininum length requirement \n");
                            return;
                        }

                        /* Check ICMP checksum */
                        if (verify_icmp_checksum (icmp_hdr, ICMP_PACKET, len)) {
                            printf("ICMP packet fails checksum \n");
                            return;
                        } 

                        struct sr_nat_mapping *nat_lookup = sr_nat_lookup_internal(&(sr->nat), ip_hdr->ip_src, icmp_hdr->icmp_id, nat_mapping_icmp);
                        if (nat_lookup == NULL) {
                        	nat_lookup = sr_nat_insert_mapping(&(sr->nat), ip_hdr->ip_src, icmp_hdr->icmp_id, nat_mapping_icmp); 
				nat_lookup->ip_ext = sr_get_interface(sr, dst_lpm->interface)->ip;
                       		nat_lookup->aux_ext = sr_nat_generate_icmp_identifier(&(sr->nat));
			}

                        nat_lookup->last_updated = time(NULL);
                        icmp_hdr->icmp_id = nat_lookup->aux_ext;
                        ip_hdr->ip_src = nat_lookup->ip_ext;
                        ip_hdr->ip_sum = 0;
                        icmp_hdr->icmp_sum = 0;
                        ip_hdr->ip_sum = cksum(ip_hdr, sizeof(sr_ip_hdr_t));
                        icmp_hdr->icmp_sum = cksum(icmp_hdr, len - sizeof(sr_ethernet_hdr_t) - sizeof(sr_ip_hdr_t)); 
		            } else if (ip_p == ip_protocol_tcp) {
                        sr_tcp_hdr_t *tcp_hdr = (sr_tcp_hdr_t *) (packet + sizeof (sr_ethernet_hdr_t) + sizeof(sr_tcp_hdr_t)); 
                        struct sr_nat_mapping *nat_lookup = sr_nat_lookup_internal(&(sr->nat), ip_hdr->ip_src, ntohs(tcp_hdr->src_port), nat_mapping_tcp);

                        if (ntohs(tcp_hdr->ctrl_bits) & TCP_SYN) {
			    /* Outbound sync with no prior mapping */
                            if (nat_lookup == NULL) {
				printf ("Outbound sync with no existing mapping \n");
                                pthread_mutex_lock(&((sr->nat).lock));
                                nat_lookup = sr_nat_insert_mapping(&(sr->nat), ip_hdr->ip_src, ntohs(tcp_hdr->src_port), nat_mapping_tcp);
                                nat_lookup->ip_ext = sr_get_interface(sr, dst_lpm->interface)->ip;
                                nat_lookup->aux_ext = sr_nat_generate_tcp_port(&(sr->nat));
                                struct sr_nat_connection *initialConn = malloc(sizeof(struct sr_nat_connection));

                                /* Fill in first connection information. */
                                initialConn->tcp_conn_state = OUTBOUND_SYN;
                                initialConn->last_updated = time(NULL);
                                initialConn->inboundSyn = NULL;
                                initialConn->external.ip_addr = ip_hdr->ip_dst;
                                initialConn->external.port_num = tcp_hdr->dst_port;

                                /* Add to the list of connections */
                                initialConn->next = nat_lookup->conns;
                                nat_lookup->conns = initialConn;

                                pthread_mutex_unlock(&((sr->nat).lock));
                            /* Outbound sync with prior mapping */
                            } else {
				printf ("Outbound sync with existing mapping \n");
                                pthread_mutex_lock(&((sr->nat).lock));
                                struct sr_nat_connection *connection = sr_nat_lookup_connection(nat_lookup, ip_hdr->ip_dst, tcp_hdr->dst_port);

                                if (connection == NULL)
                                {
				    printf ("Connection doesn't exist. Creating a new one \n");
                                    /* Connection does not exist. Create it. */
                                    connection = malloc(sizeof(struct sr_nat_connection));
                                    assert(connection);

                                    /* Fill in connection information. */
                                    connection->tcp_conn_state = OUTBOUND_SYN;
                                    connection->external.ip_addr = ip_hdr->ip_dst;
                                    connection->external.port_num = tcp_hdr->dst_port;

                                    /* Add to the list of connections. */
                                    connection->next = nat_lookup->conns;
                                    nat_lookup->conns = connection;
                                } else {
                                    switch (connection->tcp_conn_state) {
                                        case TIME_WAIT:
					    printf ("[TRANS] WAIT -> OUTBOUND SYN \n");
					    /* Give client opportunity to reopen the connection. */
                                            connection->tcp_conn_state = OUTBOUND_SYN;
                                            break; 
                                        case SYN_RCVD:
					    printf ("[TRANS] PENDING -> CONNECTED \n");
                                            connection->tcp_conn_state = ESTABLISHED;
                                            /* Retry of inbound SYN. Silently drop. */
                                            if (connection->inboundSyn) {free(connection->inboundSyn);}
                                            break;
                                        default:
                                            break;
                                    }      
                                }

                                connection->last_updated = time(NULL);
                                pthread_mutex_unlock(&((sr->nat).lock));
                            }
                        /* Outbound FIN detected. Put connection into TIME_WAIT state. */
                        } else if (ntohs(tcp_hdr->ctrl_bits) & TCP_FIN_M){
			    printf ("Outbound FIN detected. Put connection into TIME_WAIT state \n");
                            /* Outbound FIN detected. Put connection into TIME_WAIT state. */
                            pthread_mutex_lock(&((sr->nat).lock));
                            struct sr_nat_connection *connection = sr_nat_lookup_connection(nat_lookup, ip_hdr->ip_dst, tcp_hdr->dst_port);
                            if (connection)
                            {
				printf ("THERE IS CONNECTION! \n");
                                connection->tcp_conn_state = TIME_WAIT;
                                connection->last_updated = time(NULL);
                            }
                            pthread_mutex_unlock(&((sr->nat).lock));

                        } else if (nat_lookup == NULL) {
                            printf("[Dropping] Outbound non-SYN TCP packet tries to travel without mapping.\n");
                            return;
                        }

                        nat_lookup->last_updated = time(NULL);
                        ip_hdr->ip_src = nat_lookup->ip_ext;
                        tcp_hdr->src_port = htons(nat_lookup->aux_ext);
                        ip_hdr->ip_sum = 0;
                        ip_hdr->ip_sum = cksum(ip_hdr, sizeof(sr_ip_hdr_t));
                        tcp_hdr->tcp_sum = tcp_cksum(ip_hdr, tcp_hdr, len);
                        print_nat_mapping (nat_lookup);

		    }

                    /* check routing table, and perform LPM */ 
                    /* Look up routing table for the rt entry that is mapped to the destination of received packet */
                    if (dst_lpm) {
                        struct sr_if *out_iface = sr_get_interface(sr, dst_lpm->interface);
                        /* If there is a match, check ARP cache */
                        struct sr_arpentry * arp_entry = sr_arpcache_lookup (sr_cache, dst_lpm->gw.s_addr); 
                        /* If there is a match in our ARP cache, send frame to next hop */
                        if (arp_entry){
                           
                            memcpy(eth_hdr->ether_shost, out_iface->addr, sizeof(uint8_t)*ETHER_ADDR_LEN);
                            memcpy(eth_hdr->ether_dhost, arp_entry->mac, sizeof(unsigned char)*ETHER_ADDR_LEN);
                            sr_send_packet (sr, packet, len, out_iface->name); 
                            return;

                        } else {
                           /* printf("There is no match in our ARP cache\n");*/
                            /* If there is no match in our ARP cache, send ARP request. */
                            struct sr_arpreq * req = sr_arpcache_queuereq(sr_cache, ip_hdr->ip_dst, packet, len, out_iface->name);
                            handle_arpreq(req, sr);
                            return;
                        }
                    }
                }
            } else {          
                if (target_iface) {
                    if (ip_p == ip_protocol_icmp) {
                        printf("[NAT](EX->IN) ICMP\n");
                        /* Get ICMP header */
                        sr_icmp_hdr_t *icmp_hdr = get_icmp_hdr (packet);

                        /* Check for mininum length  */
                        if (check_min_len (len, ICMP_PACKET)) {
                            printf("IP packet does not satisfy mininum length requirement \n");
                            return;
                        }

                        /* Check ICMP checksum */
                        if (verify_icmp_checksum (icmp_hdr, ICMP_PACKET, len)) {
                            printf("ICMP packet fails checksum \n");
                            return;
                        } 

                        struct sr_nat_mapping *nat_lookup = sr_nat_lookup_external(&(sr->nat), icmp_hdr->icmp_id, nat_mapping_icmp); 
		
			            if (nat_lookup != NULL) {
                            if (is_icmp_echo_reply(icmp_hdr)) {
                                ip_hdr->ip_dst = nat_lookup->ip_int;
                                icmp_hdr->icmp_id= nat_lookup->aux_int;
                                nat_lookup->last_updated = time(NULL);
                				icmp_hdr->icmp_sum = 0;
                   				ip_hdr->ip_sum = 0;
                                ip_hdr->ip_sum = cksum(ip_hdr, sizeof(sr_ip_hdr_t));
                                icmp_hdr->icmp_sum = cksum(icmp_hdr, len - sizeof(sr_ethernet_hdr_t) - sizeof(sr_ip_hdr_t));             
                            }
                        } else {
                            printf ("[NAT] nat mapping does not exist. Dropping the packet \n");
                            return; 
                        }
                    } else if (ip_p == ip_protocol_tcp) {
                        sr_tcp_hdr_t *tcp_hdr = (sr_tcp_hdr_t *) (packet + sizeof (sr_ethernet_hdr_t) + sizeof(sr_tcp_hdr_t)); 
			struct sr_nat_mapping *nat_lookup = sr_nat_lookup_external(&(sr->nat), ntohs(tcp_hdr->dst_port), nat_mapping_tcp);
                        assert (nat_lookup != NULL);
			if (nat_lookup == NULL) {
                            printf ("[Dropping packet] Mapping doesn't exit \n");
                            return; 
                        }
                        if ((ntohs(tcp_hdr->ctrl_bits) & TCP_SYN)) {
                            /* Potential simultaneous open */
                            pthread_mutex_lock(&((sr->nat).lock));

                            struct sr_nat_connection *connection = sr_nat_lookup_connection(nat_lookup, ip_hdr->ip_src, tcp_hdr->src_port);
                            
                            if (connection == NULL)
                            {
			       printf ("Potential simultaneous open \n");  
                               /* Potential simultaneous open. */
                               connection = malloc(sizeof(struct sr_nat_connection));
                               assert(connection);
                               
                               /* Fill in connection information. */
                               connection->tcp_conn_state = SYN_RCVD;
                               connection->inboundSyn = malloc(len);
                               memcpy(connection->inboundSyn, ip_hdr, len);
                               connection->external.ip_addr = ip_hdr->ip_src;
                               connection->external.port_num = tcp_hdr->src_port;
                               
                               /* Add to the list of connections. */
                               connection->next = nat_lookup->conns;
                               nat_lookup->conns = connection;
                               
                               return;
                            } else {
                                switch (connection->tcp_conn_state) {
                                    case SYN_RCVD:
                                       /* Retry of inbound SYN. Silently drop. */
                                        return;
                                    case OUTBOUND_SYN:
                                        /* Connection UP! */
                                        connection->tcp_conn_state = ESTABLISHED;
                                        break;
                                    default:
                                        break;
                                }
                            }
                            connection->last_updated = time(NULL);
                            pthread_mutex_unlock(&((sr->nat).lock));

                        } else if (ntohs(tcp_hdr->ctrl_bits) & TCP_FIN_M) {
                            /* Inbound FIN detected. Put connection into TIME_WAIT state. */
                            pthread_mutex_lock(&((sr->nat).lock));
                            struct sr_nat_connection *connection = sr_nat_lookup_connection(nat_lookup, ip_hdr->ip_src, tcp_hdr->src_port);
                            if (connection)
                            {
                                connection->tcp_conn_state = TIME_WAIT;
                            }
                            connection->last_updated = time(NULL);
                            pthread_mutex_unlock(&((sr->nat).lock));
                        }

                        nat_lookup->last_updated = time(NULL);
                        ip_hdr->ip_dst = nat_lookup->ip_int;
                        tcp_hdr->dst_port = htons(nat_lookup->aux_int);
                        ip_hdr->ip_sum = 0; 
                        ip_hdr->ip_sum = cksum(ip_hdr, sizeof(sr_ip_hdr_t));
                        tcp_hdr->tcp_sum = tcp_cksum(ip_hdr, tcp_hdr, len);
                    }

                    struct sr_rt *dst_lpm = sr_routing_lpm (sr, ip_hdr->ip_dst);
                    if (dst_lpm) {
                        struct sr_if *out_iface = sr_get_interface(sr, dst_lpm->interface);
                        /* If there is a match, check ARP cache */
                        struct sr_arpentry * arp_entry = sr_arpcache_lookup (sr_cache, dst_lpm->gw.s_addr); 
                        /* If there is a match in our ARP cache, send frame to next hop */
                        if (arp_entry){
                        
                            memcpy(eth_hdr->ether_shost, out_iface->addr, sizeof(uint8_t)*ETHER_ADDR_LEN);
                            memcpy(eth_hdr->ether_dhost, arp_entry->mac, sizeof(unsigned char)*ETHER_ADDR_LEN);
                            sr_send_packet (sr, packet, len, out_iface->name); 
                            return;

                        } else {
                            /* If there is no match in our ARP cache, send ARP request. */
                            struct sr_arpreq * req = sr_arpcache_queuereq(sr_cache, ip_hdr->ip_dst, packet, len, out_iface->name);
                            handle_arpreq(req, sr);
                            return;
                        }
                    } 
                } else {
                    if (!sr_nat_is_interface_internal(dst_lpm->interface)) {
                        /* Look up routing table for the rt entry that is mapped to the destination of received packet */
                        printf("It is not for an internal interface or router. Dropping the packet \n"); 
                        return; 
                    }
                }
            }
        } else {
            route_packet (sr, packet, len, interface); 
        }

    }  
}

/* Get Ethernet header */
sr_ethernet_hdr_t * get_eth_hdr (uint8_t* packet) {
    return (sr_ethernet_hdr_t *) packet; 
}

/* Get ARP header */
sr_arp_hdr_t * get_arp_hdr (uint8_t* packet) {
    return (sr_arp_hdr_t *) (packet + sizeof(sr_ethernet_hdr_t));
}

/* Get IP header */
sr_ip_hdr_t *get_ip_hdr (uint8_t *packet) {
    return (sr_ip_hdr_t *) (packet + sizeof(sr_ethernet_hdr_t));
}

/* Get ICMP header */
sr_icmp_hdr_t *get_icmp_hdr (uint8_t *packet) {
    return (sr_icmp_hdr_t*)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
}

/* Verify IP checksum */
int verify_ip_checksum (sr_ip_hdr_t *ip_hdr) {
    uint16_t original_cksum = ip_hdr->ip_sum;
    memset(&(ip_hdr->ip_sum), 0, sizeof(uint16_t));
    uint16_t received_cksum = cksum(ip_hdr, sizeof(sr_ip_hdr_t));
    if (original_cksum != received_cksum){
        return 1;
    }
    return 0; 
}

/* Verify ICMP checksum */
int verify_icmp_checksum (sr_icmp_hdr_t *icmp_hdr, int type, int len) {
    if (type == ICMP_PACKET) {
        uint16_t original_cksum = icmp_hdr->icmp_sum;
        memset(&(icmp_hdr->icmp_sum), 0, sizeof(uint16_t));
        uint16_t received_cksum = cksum(icmp_hdr, len - sizeof(sr_ethernet_hdr_t) - sizeof(sr_ip_hdr_t));
        if(original_cksum != received_cksum){
            return 1;
        }
        icmp_hdr->icmp_sum = received_cksum;
    } 
    return 0; 
}

/* Decrement TTL and calculate new checksum */
int decrement_and_recalculate (sr_ip_hdr_t *ip_hdr) { 
    printf ("Decrementing TTL now. Th current value is %d \n", ip_hdr->ip_ttl);
    ip_hdr->ip_ttl--;
    if (ip_hdr->ip_ttl <= 0){
        return 1;
    } else {
        memset(&(ip_hdr->ip_sum), 0, sizeof(uint16_t));
        ip_hdr->ip_sum = cksum(ip_hdr, sizeof(sr_ip_hdr_t));
    }
    return 0;
}

/* Create ethernet header */
void create_ethernet_header (sr_ethernet_hdr_t * eth_hdr, uint8_t * new_packet, uint8_t *src_eth_addr, uint8_t *dest_eth_addr, uint16_t ether_type) {
    sr_ethernet_hdr_t *new_eth_hdr = (sr_ethernet_hdr_t *) new_packet;
    memcpy(new_eth_hdr->ether_shost, src_eth_addr, sizeof(uint8_t)*ETHER_ADDR_LEN);
    memcpy(new_eth_hdr->ether_dhost, dest_eth_addr, sizeof(uint8_t)*ETHER_ADDR_LEN);
    new_eth_hdr->ether_type = ether_type;
} 

 /* Create ARP header */
void create_arp_header (sr_arp_hdr_t* arp_hdr, uint8_t* new_packet, struct sr_if *src_iface) {
    sr_arp_hdr_t *new_arp_hdr = (sr_arp_hdr_t *)(new_packet + sizeof(sr_ethernet_hdr_t));
    new_arp_hdr->ar_hrd = arp_hdr->ar_hrd;
    new_arp_hdr->ar_pro = arp_hdr->ar_pro;
    new_arp_hdr->ar_hln = arp_hdr->ar_hln;
    new_arp_hdr->ar_pln = arp_hdr->ar_pln;
    new_arp_hdr->ar_op =  htons(arp_op_reply);

    /* Switch sender and receiver hardware address and IP address */
    memcpy(new_arp_hdr->ar_sha, src_iface->addr, sizeof(unsigned char)*ETHER_ADDR_LEN);
    new_arp_hdr->ar_sip =  src_iface->ip; 
    memcpy(new_arp_hdr->ar_tha, arp_hdr->ar_sha, sizeof(unsigned char)*ETHER_ADDR_LEN);
    new_arp_hdr->ar_tip = arp_hdr->ar_sip;
}

/* Create IP header for ICMP */
void create_ip_header (sr_ip_hdr_t *ip_hdr, uint8_t* new_packet, uint32_t ip_src, uint32_t ip_dst) {
    sr_ip_hdr_t *new_ip_hdr = (sr_ip_hdr_t *)(new_packet + sizeof(sr_ethernet_hdr_t));
    new_ip_hdr->ip_v = 4;
    new_ip_hdr->ip_hl = sizeof(sr_ip_hdr_t)/4;
    new_ip_hdr->ip_tos = 0;
    new_ip_hdr->ip_len = htons(sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
    new_ip_hdr->ip_id = htons(0);
    new_ip_hdr->ip_off = htons(IP_DF);
    new_ip_hdr->ip_ttl = 64;
    new_ip_hdr->ip_dst = ip_dst;
    new_ip_hdr->ip_p = ip_protocol_icmp;
    new_ip_hdr->ip_src = ip_src;
    new_ip_hdr->ip_sum = 0;
    new_ip_hdr->ip_sum = cksum(new_ip_hdr, sizeof(sr_ip_hdr_t));
}


/* Create ICMP type 3 header */
void create_icmp_type3_header (sr_ip_hdr_t *ip_hdr, uint8_t* new_packet, uint8_t type, unsigned int code) {
    sr_icmp_t3_hdr_t *new_icmp_header = (sr_icmp_t3_hdr_t *)(new_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
    new_icmp_header->icmp_type = type;
    new_icmp_header->icmp_code = code;
    new_icmp_header->unused = 0;
    new_icmp_header->next_mtu = 0;
    new_icmp_header->icmp_sum = 0;
    memcpy(new_icmp_header->data, ip_hdr, ICMP_DATA_SIZE);
    new_icmp_header->icmp_sum = cksum(new_icmp_header, sizeof(sr_icmp_t3_hdr_t));
}

void send_echo_reply (struct sr_instance* sr, uint8_t * packet, unsigned int len, char* interface) {
    /* Get Ethernet header */
    sr_ethernet_hdr_t* eth_hdr = get_eth_hdr(packet);

    /* Get IP header */
    sr_ip_hdr_t* ip_hdr = get_ip_hdr(packet);

    /* Get ICMP header */
    sr_icmp_hdr_t* icmp_hdr = get_icmp_hdr (packet);

    /* ARP Cache */
    struct sr_arpcache *sr_cache = &sr->cache;

    /* Modify ethernet header */
    memcpy(eth_hdr->ether_dhost, eth_hdr->ether_shost, sizeof(uint8_t)*ETHER_ADDR_LEN);
    memcpy(eth_hdr->ether_shost, sr_get_interface(sr, interface)->addr, sizeof(uint8_t)*ETHER_ADDR_LEN);

    /* Modify IP header */ 
    uint32_t src_ip = ip_hdr->ip_src;
    ip_hdr->ip_src = ip_hdr->ip_dst;
    ip_hdr->ip_dst = src_ip;
    memset(&(ip_hdr->ip_sum), 0, sizeof(uint16_t));
    ip_hdr->ip_sum = cksum(ip_hdr, sizeof(sr_ip_hdr_t));

    /* Modify ICMP header  */
    icmp_hdr->icmp_type = echo_reply_type;
    icmp_hdr->icmp_code = echo_reply_code;
    memset(&(icmp_hdr->icmp_sum), 0, sizeof(uint16_t));
    icmp_hdr->icmp_sum = cksum(icmp_hdr, len - sizeof(sr_ethernet_hdr_t) - sizeof(sr_ip_hdr_t));
    
    struct sr_arpentry * arp_entry = sr_arpcache_lookup (sr_cache, ip_hdr->ip_dst);
    if (arp_entry) {
    	sr_send_packet (sr, packet, len, interface);
    } else {
        struct sr_arpreq * req = sr_arpcache_queuereq(sr_cache, ip_hdr->ip_dst, packet, len, interface);
        handle_arpreq(req, sr);
    }
}

/* Send ARP request */
void send_arp_req (sr_arp_hdr_t *arp_hdr, struct sr_arpcache *cache, struct sr_instance* sr) {
    struct sr_arpreq *req = sr_arpcache_insert(cache, arp_hdr->ar_sha, arp_hdr->ar_sip);
    if (req){
        /* Sending out outstanding packets for a request */
        struct sr_packet *req_packet = req->packets;
        while (req_packet) {
            sr_ethernet_hdr_t *req_eth_hdr = (sr_ethernet_hdr_t *) req_packet->buf;
            memcpy(req_eth_hdr->ether_dhost, arp_hdr->ar_sha, sizeof(unsigned char)*ETHER_ADDR_LEN);
            memcpy(req_eth_hdr->ether_shost, sr_get_interface(sr, req_packet->iface)->addr, sizeof(unsigned char)*ETHER_ADDR_LEN);
            sr_send_packet(sr, req_packet->buf, req_packet->len, req_packet->iface);
            req_packet = req_packet->next;
        }
    }
    sr_arpreq_destroy(cache, req);
}

/* Send ICMP type 3 message after performing longest prefix match */
void send_icmp_type3_msg(uint8_t * new_packet, struct sr_rt *src_lpm, struct sr_arpcache *sr_cache, struct sr_instance* sr, char* interface, unsigned int len)  {
    if (src_lpm){
        printf("Found the match in routing table\n");
        struct sr_arpentry *entry = sr_arpcache_lookup(sr_cache, src_lpm->gw.s_addr);
        if (entry){
            printf("Found the ARP entry in the cache\n");
            struct sr_if *out_iface = sr_get_interface(sr, src_lpm->interface);

            /* Modify ethernet header */
            sr_ethernet_hdr_t *new_eth_hdr = (sr_ethernet_hdr_t *) new_packet;
            memcpy(new_eth_hdr->ether_dhost, entry->mac, sizeof(uint8_t)*ETHER_ADDR_LEN);
            memcpy(new_eth_hdr->ether_shost, out_iface->addr, sizeof(uint8_t)*ETHER_ADDR_LEN);

            /* Modify ip header */
            sr_ip_hdr_t *new_ip_hdr = (sr_ip_hdr_t *) (new_packet + sizeof (sr_ethernet_hdr_t));
            new_ip_hdr->ip_src = sr_get_interface(sr, interface)->ip;
            new_ip_hdr->ip_sum = 0;
            new_ip_hdr->ip_sum = cksum(new_ip_hdr, sizeof(sr_ip_hdr_t));

            sr_send_packet(sr, new_packet, len, out_iface->name);
            free(entry);
        } else {
             /* If there is no match in our ARP cache, send ARP request. */
            struct sr_arpreq *req = sr_arpcache_queuereq(sr_cache, src_lpm->gw.s_addr, new_packet, len, src_lpm->interface);
            handle_arpreq(req, sr);
        }    
    }
}

/* Check for mininum length requirement for respective header type */
int check_min_len (unsigned int len, int type) {
    int min_len = 0;
    switch (type) {
        case ETH_HDR:
            min_len = sizeof (sr_ethernet_hdr_t);
            break; 
        case ARP_PACKET:
            min_len = sizeof (sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t);
            break; 
        case IP_PACKET:
            min_len = sizeof (sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t);
            break; 
        case ICMP_PACKET:
            min_len = sizeof (sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof (sr_icmp_hdr_t);
            break; 
        case ICMP_TYPE3_PACKET:
            min_len = sizeof (sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof (sr_icmp_t3_hdr_t);
            break; 
    }
    /* Check for mininum length requirement */
    if (len < min_len) {
        printf( "The ethernet header does not satisfy mininum length \n");
        return 1;
    }
    return 0;
}

/* Return an interface if the targer IP belongs to our router */
struct sr_if* get_router_interface (uint32_t ip, struct sr_instance* sr) {
    struct sr_if* curr_iface = sr->if_list;
    while (curr_iface){
        /* Check if the packet is targeted towards the current router interface */
        if (curr_iface->ip == ip){
            printf("Packet is for me \n");
            /* The packet is targeted towards the current router */
            return curr_iface;
        }
        curr_iface = curr_iface->next;
    }
    return NULL;
} 

int is_icmp_echo_reply(sr_icmp_hdr_t *icmp_hdr) {
    return (icmp_hdr->icmp_type == 0 && icmp_hdr->icmp_code == 0) ? 1 : 0;
}

int is_icmp_echo_request(sr_icmp_hdr_t *icmp_hdr) {
    return (icmp_hdr->icmp_type == 8 && icmp_hdr->icmp_code == 0) ? 1 : 0;
}

void route_packet (struct sr_instance* sr,
        uint8_t * packet,
        unsigned int len,
        char* interface) 
{
    assert(sr);
    assert(packet);
    assert(interface);

    /* Get Ethernet header */
    sr_ethernet_hdr_t* eth_hdr = get_eth_hdr(packet);

    /* Get IP header */
    sr_ip_hdr_t * ip_hdr = get_ip_hdr(packet);

    /* Get ICMP header */
    sr_icmp_hdr_t *icmp_hdr = get_icmp_hdr (packet);

    /* ARP Cache */
    struct sr_arpcache *sr_cache = &sr->cache;

    /* If target interface is not NULL, the packet is for one of the interfaces in our router */
    struct sr_if *target_iface = get_router_interface (ip_hdr->ip_dst, sr);
    if (target_iface) {
        uint8_t ip_p = ip_protocol((uint8_t *)ip_hdr); 
        /* Check if the ip protocol is of type ICMP */
        if (ip_p == ip_protocol_icmp) {
            /* Check for mininum length  */
            if (check_min_len (len, ICMP_PACKET)) {
                printf("IP packet does not satisfy mininum length requirement \n");
                return;
            }

            /* Check ICMP checksum */
            if (verify_icmp_checksum (icmp_hdr, ICMP_PACKET, len)) {
                printf("ICMP packet fails checksum \n");
                return;
            } 

            if (is_icmp_echo_request (icmp_hdr)) {
                send_echo_reply (sr, packet, len, interface);
                return;
            } else {
                printf("Unknown ICMP type \n");
                return;
            }
        /* If it is TCP / UDP, send ICMP port unreachable */
        } else if (ip_p == ip_protocol_udp || ip_p == ip_protocol_tcp) {
            int packet_len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
            uint8_t *new_packet = malloc(len);

            /* Create ethernet header */
            create_ethernet_header (eth_hdr, new_packet, sr_get_interface(sr, interface)->addr, eth_hdr->ether_shost, htons(ethertype_ip));

            /* Create ip header */
            create_ip_header (ip_hdr, new_packet, target_iface->ip, ip_hdr->ip_src);

            /* Create icmp header */
            create_icmp_type3_header (ip_hdr, new_packet, port_unreachable_type, port_unreachable_code);

            /* Send ICMP port unreachable message */
            sr_send_packet (sr, new_packet, packet_len, interface);
          
            free(new_packet);
            return; 
        }
    /* Not for me*/ 
    } else {
        /* check routing table, and perform LPM */ 
        /* Look up routing table for the rt entry that is mapped to the destination of received packet */
        struct sr_rt* dst_lpm = sr_routing_lpm (sr, ip_hdr->ip_dst); 
        if (dst_lpm) {
            struct sr_if *out_iface = sr_get_interface(sr, dst_lpm->interface);
            /* If there is a match, check ARP cache */
            struct sr_arpentry * arp_entry = sr_arpcache_lookup (sr_cache, dst_lpm->gw.s_addr); 
            /* If there is a match in our ARP cache, send frame to next hop */
            if (arp_entry){
                printf("There is a match in the ARP cache\n");
                memcpy(eth_hdr->ether_shost, out_iface->addr, sizeof(uint8_t)*ETHER_ADDR_LEN);
                memcpy(eth_hdr->ether_dhost, arp_entry->mac, sizeof(unsigned char)*ETHER_ADDR_LEN);
                sr_send_packet (sr, packet, len, out_iface->name); 
                return;

            } else {
                printf("There is no match in our ARP cache\n");
                /* If there is no match in our ARP cache, send ARP request. */
                struct sr_arpreq * req = sr_arpcache_queuereq(sr_cache, ip_hdr->ip_dst, packet, len, out_iface->name);
                handle_arpreq(req, sr);
                return;
            }
        }
    }
}

