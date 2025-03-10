/*-
 * Copyright(c) <2010-2021>, Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Created 2010 by Keith Wiles @ intel.com */

#include <stdint.h>
#include <time.h>
#include <inttypes.h>
#include <math.h>

#include <pg_delay.h>
#include <rte_lcore.h>
#include <lua_config.h>
#include <rte_net.h>
#include <rte_arp.h>
#include <rte_cycles.h>

#include "pktgen.h"
#include "pktgen-gre.h"
#include "pktgen-tcp.h"
#include "pktgen-ipv4.h"
#include "pktgen-ipv6.h"
#include "pktgen-udp.h"
#include "pktgen-arp.h"
#include "pktgen-vlan.h"
#include "pktgen-cpu.h"
#include "pktgen-display.h"
#include "pktgen-random.h"
#include "pktgen-log.h"
#include "pktgen-gtpu.h"
#include "pktgen-cfg.h"
#include "pktgen-rate.h"

#include <pthread.h>
#include <sched.h>

/* Allocated the pktgen structure for global use */
pktgen_t pktgen;

double
next_poisson_time(double rateParameter)
{
    return -logf(1.0f - ((double)random()) / (double)(RAND_MAX)) / rateParameter;
}

/**
 *
 * pktgen_wire_size - Calculate the wire size of the data to be sent.
 *
 * DESCRIPTION
 * Calculate the number of bytes/bits in a burst of traffic.
 *
 * RETURNS: Number of bytes in a burst of packets.
 *
 * SEE ALSO:
 */

uint64_t
pktgen_wire_size(port_info_t *info)
{
    uint64_t i, size = 0;

    if (pktgen_tst_port_flags(info, SEND_PCAP_PKTS))
        size = info->pcap->pkt_size + PKT_OVERHEAD_SIZE;
    else if (pktgen_tst_port_flags(info, SEND_RATE_PACKETS))
        size = info->seq_pkt[RATE_PKT].pktSize + PKT_OVERHEAD_SIZE;
    else {
        if (unlikely(info->seqCnt > 0)) {
            for (i = 0; i < info->seqCnt; i++)
                size += info->seq_pkt[i].pktSize + PKT_OVERHEAD_SIZE;
            size = size / info->seqCnt; /* Calculate the average sized packet */
        } else
            size = info->seq_pkt[SINGLE_PKT].pktSize + PKT_OVERHEAD_SIZE;
    }
    return size;
}

/**
 *
 * pktgen_packet_rate - Calculate the transmit rate.
 *
 * DESCRIPTION
 * Calculate the number of cycles to wait between sending bursts of traffic.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

void
pktgen_packet_rate(port_info_t *info)
{
    uint64_t wire_size = (pktgen_wire_size(info) * 8);
    uint64_t lk        = (uint64_t)info->link.link_speed * Million;
    uint64_t pps       = (((lk / wire_size) * info->tx_rate) / 100) + ROUND_FACTOR;
    uint64_t cpp       = (pps > 0) ? (pktgen.hz / pps) : pktgen.hz;

    info->tx_pps    = pps;
    info->tx_cycles = ((cpp * info->tx_burst) * get_port_txcnt(pktgen.l2p, info->pid));
}

/**
 *
 * pktgen_fill_pattern - Create the fill pattern in a packet buffer.
 *
 * DESCRIPTION
 * Create a fill pattern based on the arguments for the packet data.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

static __inline__ void
pktgen_fill_pattern(uint8_t *p, uint32_t len, uint32_t type, char *user)
{
    uint32_t i;

    switch (type) {
    case USER_FILL_PATTERN:
        memset(p, 0, len);
        for (i = 0; i < len; i++)
            p[i] = user[i & (USER_PATTERN_SIZE - 1)];
        break;

    case NO_FILL_PATTERN:
        break;

    case ZERO_FILL_PATTERN:
        memset(p, 0, len);
        break;

    default:
    case ABC_FILL_PATTERN: /* Byte wide ASCII pattern */
        for (i = 0; i < len; i++)
            p[i] = "abcdefghijklmnopqrstuvwxyz012345"[i & 0x1f];
        break;
    }
}

/**
 *
 * pktgen_find_matching_ipsrc - Find the matching IP source address
 *
 * DESCRIPTION
 * locate and return the pkt_seq_t pointer to the match IP address.
 *
 * RETURNS: pkt_seq_t  * or NULL
 *
 * SEE ALSO:
 */

pkt_seq_t *
pktgen_find_matching_ipsrc(port_info_t *info, uint32_t addr)
{
    pkt_seq_t *pkt = NULL;
    int i;

    addr = ntohl(addr);

    /* Search the sequence packets for a match */
    for (i = 0; i < info->seqCnt; i++)
        if (addr == info->seq_pkt[i].ip_src_addr.addr.ipv4.s_addr) {
            pkt = &info->seq_pkt[i];
            break;
        }

    /* Now try to match the single packet address */
    if (pkt == NULL)
        if (addr == info->seq_pkt[SINGLE_PKT].ip_src_addr.addr.ipv4.s_addr)
            pkt = &info->seq_pkt[SINGLE_PKT];

    return pkt;
}

/**
 *
 * pktgen_find_matching_ipdst - Find the matching IP destination address
 *
 * DESCRIPTION
 * locate and return the pkt_seq_t pointer to the match IP address.
 *
 * RETURNS: pkt_seq_t  * or NULL
 *
 * SEE ALSO:
 */

pkt_seq_t *
pktgen_find_matching_ipdst(port_info_t *info, uint32_t addr)
{
    pkt_seq_t *pkt = NULL;
    int i;

    addr = ntohl(addr);

    /* Search the sequence packets for a match */
    for (i = 0; i < info->seqCnt; i++)
        if (addr == info->seq_pkt[i].ip_dst_addr.addr.ipv4.s_addr) {
            pkt = &info->seq_pkt[i];
            break;
        }

    /* Now try to match the single packet address */
    if (pkt == NULL)
        if (addr == info->seq_pkt[SINGLE_PKT].ip_dst_addr.addr.ipv4.s_addr)
            pkt = &info->seq_pkt[SINGLE_PKT];

    /* Now try to match the range packet address */
    if (pkt == NULL)
        if (addr == info->seq_pkt[RANGE_PKT].ip_dst_addr.addr.ipv4.s_addr)
            pkt = &info->seq_pkt[RANGE_PKT];

    return pkt;
}

static __inline__ tstamp_t *
pktgen_tstamp_pointer(port_info_t *info, struct rte_mbuf *m, int32_t seq_idx)
{
    tstamp_t *tstamp;
    char *p;

    p = rte_pktmbuf_mtod(m, char *);

    p += sizeof(struct rte_ether_hdr);

    p += (info->seq_pkt[seq_idx].ethType == RTE_ETHER_TYPE_IPV4) ? sizeof(struct rte_ipv4_hdr)
                                                                 : sizeof(struct rte_ipv6_hdr);

    p += (info->seq_pkt[seq_idx].ipProto == PG_IPPROTO_UDP) ? sizeof(struct rte_udp_hdr)
                                                            : sizeof(struct rte_tcp_hdr);

    /* Force pointer to be aligned correctly */
    p = RTE_PTR_ALIGN_CEIL(p, sizeof(uint64_t));

    tstamp = (tstamp_t *)p;

    return tstamp;
}

static inline void
pktgen_tstamp_apply(port_info_t *info __rte_unused, struct rte_mbuf **mbufs, int cnt,
                    int32_t seq_idx)
{
    pkt_seq_t *pkt            = &info->seq_pkt[seq_idx];
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)&pkt->hdr.eth;
    char *l3_hdr              = (char *)&eth[1]; /* Point to l3 hdr location */
    int i;

    for (i = 0; i < cnt; i++) {
        tstamp_t *tstamp;

        tstamp = pktgen_tstamp_pointer(info, mbufs[i], seq_idx);

        tstamp->timestamp = rte_rdtsc_precise();
        tstamp->magic     = TSTAMP_MAGIC;

        /* Construct the UDP header */
        pktgen_udp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV4);

        /* IPv4 Header constructor */
        pktgen_ipv4_ctor(pkt, l3_hdr);
    }
}

static inline void
pktgen_do_tx_tap(port_info_t *info, struct rte_mbuf **mbufs, int cnt)
{
    int i;

    for (i = 0; i < cnt; i++)
        if (write(info->tx_tapfd, rte_pktmbuf_mtod(mbufs[i], char *), mbufs[i]->pkt_len) < 0) {
            pktgen_log_error("Write failed for tx_tap%d", info->pid);
            break;
        }
}

/**
 *
 * pktgen_send_burst - Send a burst of packet as fast as possible.
 *
 * DESCRIPTION
 * Transmit a burst of packets to a given port.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

static __inline__ void
pktgen_send_burst(port_info_t *info, uint16_t qid)
{
    struct mbuf_table *mtab = &info->q[qid].tx_mbufs;
    struct rte_mbuf **pkts;
    struct qstats_s *qstats = &info->qstats[qid];
    uint32_t ret, cnt, tap, rnd, tstamp, i;
    int32_t seq_idx;

    tap = pktgen_tst_port_flags(info, PROCESS_TX_TAP_PKTS);

    if ((cnt = mtab->len) == 0)
        goto special_send;

    mtab->len = 0;
    pkts      = mtab->m_table;

    if (pktgen_tst_port_flags(info, SEND_RANGE_PKTS))
        seq_idx = RANGE_PKT;
    else if (pktgen_tst_port_flags(info, SEND_RATE_PACKETS))
        seq_idx = RATE_PKT;
    else
        seq_idx = SINGLE_PKT;

    rnd = pktgen_tst_port_flags(info, SEND_RANDOM_PKTS);
    tstamp =
        pktgen_tst_port_flags(info, (SEND_LATENCY_PKTS | SEND_RATE_PACKETS | SAMPLING_LATENCIES));

    qstats->txpkts += cnt;
    for (i = 0; i < cnt; i++)
        qstats->txbytes += rte_pktmbuf_data_len(pkts[i]);

    /* Send all of the packets before we can exit this function */
    while (cnt) {
        if (rnd)
            pktgen_rnd_bits_apply(info, pkts, cnt, NULL);

        if (tstamp)
            pktgen_tstamp_apply(info, pkts, cnt, seq_idx);

        ret = rte_eth_tx_burst(info->pid, qid, pkts, cnt);

        if (tap)
            pktgen_do_tx_tap(info, pkts, ret);

        if (cnt - ret)
            info->qstats[qid].stats.tx_failed += (cnt - ret);

        pkts += ret;
        cnt -= ret;
    }

special_send:
    /* Send all of the special packets if any exists */
    mtab = &info->q[qid].special_mbufs;

    if ((cnt = mtab->len) == 0)
        return;

    mtab->len = 0;
    pkts      = mtab->m_table;

    qstats->txpkts += cnt;
    for (i = 0; i < cnt; i++)
        qstats->txbytes += rte_pktmbuf_data_len(pkts[i]);

    /* Send all of the packets before we can exit this function */
    while (cnt) {
        ret = rte_eth_tx_burst(info->pid, qid, pkts, cnt);

        if (tap)
            pktgen_do_tx_tap(info, pkts, ret);

        pkts += ret;
        cnt -= ret;
    }
}

static __inline__ void
pktgen_recv_tstamp(port_info_t *info, struct rte_mbuf **pkts, uint16_t nb_pkts)
{
    uint32_t flags;
    int32_t seq_idx;
    int lid = rte_lcore_id();
    int qid = get_rxque(pktgen.l2p, lid, info->pid);
    int i;
    uint64_t lat, jitter;

    flags = rte_atomic32_read(&info->port_flags);

    if (flags & SEND_RANGE_PKTS)
        seq_idx = RANGE_PKT;
    else if (flags & SEND_RATE_PACKETS)
        seq_idx = RATE_PKT;
    else
        seq_idx = SINGLE_PKT;

    for (i = 0; i < nb_pkts; i++) {

        if (flags & (SEND_LATENCY_PKTS | SEND_RATE_PACKETS | SAMPLING_LATENCIES)) {
            tstamp_t *tstamp;
            tstamp = pktgen_tstamp_pointer(info, pkts[i], seq_idx);

            if (tstamp->magic == TSTAMP_MAGIC) {
                lat = (rte_rdtsc_precise() - tstamp->timestamp);

                if (flags & (SEND_LATENCY_PKTS | SEND_RATE_PACKETS)) {
                    info->avg_latency += lat;
                    if (lat > info->prev_latency)
                        jitter = lat - info->prev_latency;
                    else
                        jitter = info->prev_latency - lat;
                    if (lat > info->max_latency)
                        info->max_latency = lat;
                    if (jitter > info->jitter_threshold_clks)
                        info->jitter_count++;
                    info->prev_latency = lat;
                } else if (flags & (SAMPLING_LATENCIES)) {
                    /* Record latency if it's time for sampling (seperately per lcore) */
                    latsamp_stats_t *stats = &info->latsamp_stats[qid];
                    uint64_t now           = rte_rdtsc_precise();
                    stats->pkt_counter++;
                    if (stats->next == 0 || now >= stats->next) {
                        if (stats->idx < stats->num_samples) {
                            // stats->data[stats->idx] = lat;
                            stats->data[stats->idx] =
                                (lat * Billion) /
                                rte_get_tsc_hz(); /* Do we want to keep it as cycles? */
                            stats->idx++;
                        }

                        /* Calculate next sampling point TODO: Use poisson */
                        if (info->latsamp_type == LATSAMPLER_POISSON) {
                            // TODO: Write poisson
                            double next_possion_time_ns = next_poisson_time(info->latsamp_rate);
                            stats->next                 = now + next_possion_time_ns *
                                                    (double)rte_get_tsc_hz();        // Time based
                            // pktgen_log_warning("core %d, queue %d next poisson time %lf, ms:
                            // %lu", lid, qid, next_possion_time_ns,
                            // stats->next*1000/rte_get_tsc_hz());
                        } else {        // LATSAMPLER_SIMPLE or LATSAMPLER_UNSPEC
                            stats->next =
                                now + rte_get_tsc_hz() / info->latsamp_rate;        // Time based
                            // stats->next = stats->pkt_counter + info->latsamp_rate;		//
                            // Packet count based
                        }
                    }
                }

            } else
                info->magic_errors++;

            info->latency_nb_pkts++;
        }
    }
}

/**
 *
 * pktgen_tx_flush - Flush Tx buffers from ring.
 *
 * DESCRIPTION
 * Flush TX buffers from ring.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

static __inline__ void
pktgen_tx_flush(port_info_t *info, uint16_t qid)
{
    /* Flush any queued pkts to the driver. */
    pktgen_send_burst(info, qid);

    rte_eth_tx_done_cleanup(info->pid, qid, 0);

    pktgen_clr_q_flags(info, qid, DO_TX_FLUSH);
}

/**
 *
 * pktgen_exit_cleanup - Clean up the data and other items
 *
 * DESCRIPTION
 * Clean up the data.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

static __inline__ void
pktgen_exit_cleanup(uint8_t lid)
{
    uint8_t idx;

    for (idx = 0; idx < get_lcore_txcnt(pktgen.l2p, lid); idx++) {
        port_info_t *info;
        uint8_t pid;

        pid = get_tx_pid(pktgen.l2p, lid, idx);
        if ((info = (port_info_t *)get_port_private(pktgen.l2p, pid)) != NULL) {
            uint8_t qid = get_txque(pktgen.l2p, lid, pid);
            pktgen_tx_flush(info, qid);
        }
    }
}

/**
 *
 * pktgen_has_work - Determine if lcore has work to do, if not wait for stop.
 *
 * DESCRIPTION
 * If lcore has work to do then return zero else spin till stopped and return 1.
 *
 * RETURNS: 0 or 1
 *
 * SEE ALSO:
 */

static __inline__ int
pktgen_has_work(void)
{
    if (!get_map(pktgen.l2p, RTE_MAX_ETHPORTS, rte_lcore_id())) {
        pktgen_log_warning("Nothing to do on lcore %d: exiting", rte_lcore_id());
        return 1;
    }
    return 0;
}

/**
 *
 * pktgen_packet_ctor - Construct a complete packet with all headers and data.
 *
 * DESCRIPTION
 * Construct a packet type based on the arguments passed with all headers.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

void
pktgen_packet_ctor(port_info_t *info, int32_t seq_idx, int32_t type)
{
    pkt_seq_t *pkt            = &info->seq_pkt[seq_idx];
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)&pkt->hdr.eth;
    uint32_t flags;
    char *l3_hdr = (char *)&eth[1]; /* Point to l3 hdr location for GRE header */

    /* Fill in the pattern for data space. */
    pktgen_fill_pattern((uint8_t *)&pkt->hdr, (sizeof(pkt_hdr_t) + sizeof(pkt->pad)),
                        info->fill_pattern_type, info->user_pattern);

    flags = rte_atomic32_read(&info->port_flags);

    /* Add GRE header and adjust rte_ether_hdr pointer if requested */
    if (flags & SEND_GRE_IPv4_HEADER)
        l3_hdr = pktgen_gre_hdr_ctor(info, pkt, (greIp_t *)l3_hdr);
    else if (flags & SEND_GRE_ETHER_HEADER)
        l3_hdr = pktgen_gre_ether_hdr_ctor(info, pkt, (greEther_t *)l3_hdr);
    else
        l3_hdr = pktgen_ether_hdr_ctor(info, pkt, eth);

    if (likely(pkt->ethType == RTE_ETHER_TYPE_IPV4)) {
        if (likely(pkt->ipProto == PG_IPPROTO_TCP)) {
            if (pkt->dport != PG_IPPROTO_L4_GTPU_PORT) {
                /* Construct the TCP header */
                pktgen_tcp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV4);

                /* IPv4 Header constructor */
                pktgen_ipv4_ctor(pkt, l3_hdr);
            } else {
                /* Construct the GTP-U header */
                pktgen_gtpu_hdr_ctor(pkt, l3_hdr, pkt->ipProto, GTPu_VERSION | GTPu_PT_FLAG, 0, 0,
                                     0);

                /* Construct the TCP header */
                pktgen_tcp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV4);

                /* IPv4 Header constructor */
                pktgen_ipv4_ctor(pkt, l3_hdr);
            }
        } else if (pkt->ipProto == PG_IPPROTO_UDP) {
            if (flags & SEND_VXLAN_PACKETS) {
                /* Construct the UDP header */
                pkt->dport = VXLAN_PORT_ID;
                pktgen_udp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV4);

                /* IPv4 Header constructor */
                pktgen_ipv4_ctor(pkt, l3_hdr);
            } else if (pkt->dport != PG_IPPROTO_L4_GTPU_PORT) {
                /* Construct the UDP header */
                pktgen_udp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV4);

                /* IPv4 Header constructor */
                pktgen_ipv4_ctor(pkt, l3_hdr);
            } else {
                /* Construct the GTP-U header */
                pktgen_gtpu_hdr_ctor(pkt, l3_hdr, pkt->ipProto, GTPu_VERSION | GTPu_PT_FLAG, 0, 0,
                                     0);

                /* Construct the UDP header */
                pktgen_udp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV4);

                /* IPv4 Header constructor */
                pktgen_ipv4_ctor(pkt, l3_hdr);
            }
        } else if (pkt->ipProto == PG_IPPROTO_ICMP) {
            struct rte_ipv4_hdr *ipv4;
            struct rte_udp_hdr *udp;
            struct rte_icmp_hdr *icmp;
            uint16_t tlen;

            /* Start from Ethernet header */
            ipv4 = (struct rte_ipv4_hdr *)l3_hdr;
            udp  = (struct rte_udp_hdr *)&ipv4[1];

            /* Create the ICMP header */
            ipv4->src_addr = htonl(pkt->ip_src_addr.addr.ipv4.s_addr);
            ipv4->dst_addr = htonl(pkt->ip_dst_addr.addr.ipv4.s_addr);

            tlen               = pkt->pktSize - (pkt->ether_hdr_size + sizeof(struct rte_ipv4_hdr));
            ipv4->total_length = htons(tlen);
            ipv4->next_proto_id = pkt->ipProto;

            icmp            = (struct rte_icmp_hdr *)&udp[1];
            icmp->icmp_code = 0;
            if ((type == -1) || (type == ICMP4_TIMESTAMP)) {
                union icmp_data *data = (union icmp_data *)&udp[1];

                icmp->icmp_type           = ICMP4_TIMESTAMP;
                data->timestamp.ident     = 0x1234;
                data->timestamp.seq       = 0x5678;
                data->timestamp.originate = 0x80004321;
                data->timestamp.receive   = 0;
                data->timestamp.transmit  = 0;
            } else if (type == ICMP4_ECHO) {
                union icmp_data *data = (union icmp_data *)&udp[1];

                icmp->icmp_type  = ICMP4_ECHO;
                data->echo.ident = 0x1234;
                data->echo.seq   = 0x5678;
                data->echo.data  = 0;
            }
            icmp->icmp_cksum = 0;
            /* ICMP4_TIMESTAMP_SIZE */
            tlen             = pkt->pktSize - (pkt->ether_hdr_size + sizeof(struct rte_ipv4_hdr));
            icmp->icmp_cksum = rte_raw_cksum(icmp, tlen);
            if (icmp->icmp_cksum == 0)
                icmp->icmp_cksum = 0xFFFF;

            /* IPv4 Header constructor */
            pktgen_ipv4_ctor(pkt, l3_hdr);
        }
    } else if (pkt->ethType == RTE_ETHER_TYPE_IPV6) {
        if (pkt->ipProto == PG_IPPROTO_TCP) {
            /* Construct the TCP header */
            pktgen_tcp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV6);

            /* IPv6 Header constructor */
            pktgen_ipv6_ctor(pkt, l3_hdr);
        } else if (pkt->ipProto == PG_IPPROTO_UDP) {
            /* Construct the UDP header */
            pktgen_udp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV6);

            /* IPv6 Header constructor */
            pktgen_ipv6_ctor(pkt, l3_hdr);
        }
    } else if (pkt->ethType == RTE_ETHER_TYPE_ARP) {
        /* Start from Ethernet header */
        struct rte_arp_hdr *arp = (struct rte_arp_hdr *)l3_hdr;

        arp->arp_hardware = htons(1);
        arp->arp_protocol = htons(RTE_ETHER_TYPE_IPV4);
        arp->arp_hlen     = RTE_ETHER_ADDR_LEN;
        arp->arp_plen     = 4;

        /* make request/reply operation selectable by user */
        arp->arp_opcode = htons(2);

        rte_ether_addr_copy(&pkt->eth_src_addr, (struct rte_ether_addr *)&arp->arp_data.arp_sha);
        *((uint32_t *)&arp->arp_data.arp_sha) = htonl(pkt->ip_src_addr.addr.ipv4.s_addr);

        rte_ether_addr_copy(&pkt->eth_dst_addr, (struct rte_ether_addr *)&arp->arp_data.arp_tha);
        *((uint32_t *)((void*)&arp->arp_data + offsetof(struct rte_arp_ipv4,arp_tip))) = htonl(pkt->ip_dst_addr.addr.ipv4.s_addr);
    } else
        pktgen_log_error("Unknown EtherType 0x%04x", pkt->ethType);
}

/**
 *
 * pktgen_send_mbuf - Send a single packet to the given port.
 *
 * DESCRIPTION
 * Send a single packet to a given port, but enqueue the packet until we have
 * a given burst count of packets to send.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

void
pktgen_send_mbuf(struct rte_mbuf *m, uint8_t pid, uint16_t qid)
{
    port_info_t *info       = &pktgen.info[pid];
    struct mbuf_table *mtab = &info->q[qid].special_mbufs;

    /* Add packet to the TX list. */
    mtab->m_table[mtab->len++] = m;
}

/**
 *
 * pktgen_packet_type - Examine a packet and return the type of packet
 *
 * DESCRIPTION
 * Examine a packet and return the type of packet.
 * the packet.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

static __inline__ pktType_e
pktgen_packet_type(struct rte_mbuf *m)
{
    pktType_e ret;
    struct rte_ether_hdr *eth;

    eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

    ret = ntohs(eth->ether_type);

    return ret;
}

/**
 *
 * pktgen_packet_classify - Examine a packet and classify it for statistics
 *
 * DESCRIPTION
 * Examine a packet and determine its type along with counting statistics around
 * the packet.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

static void
pktgen_packet_classify(struct rte_mbuf *m, int pid, int qid)
{
    port_info_t *info = &pktgen.info[pid];
    uint32_t plen;
    uint32_t flags;
    pktType_e pType;

    pType = pktgen_packet_type(m);

    plen = rte_pktmbuf_pkt_len(m);

    flags = rte_atomic32_read(&info->port_flags);
    if (unlikely(flags & (PROCESS_INPUT_PKTS | PROCESS_RX_TAP_PKTS))) {
        if (unlikely(flags & PROCESS_RX_TAP_PKTS))
            if (write(info->rx_tapfd, rte_pktmbuf_mtod(m, char *), m->pkt_len) < 0)
                pktgen_log_error("Write failed for rx_tap%d", pid);

        switch ((int)pType) {
        case RTE_ETHER_TYPE_ARP:
            info->qstats[qid].stats.arp_pkts++;
            pktgen_process_arp(m, pid, qid, 0);
            break;
        case RTE_ETHER_TYPE_IPV4:
            info->qstats[qid].stats.ip_pkts++;
            pktgen_process_ping4(m, pid, qid, 0);
            break;
        case RTE_ETHER_TYPE_IPV6:
            info->qstats[qid].stats.ipv6_pkts++;
            pktgen_process_ping6(m, pid, qid, 0);
            break;
        case RTE_ETHER_TYPE_VLAN:
            info->qstats[qid].stats.vlan_pkts++;
            pktgen_process_vlan(m, pid, qid);
            break;
        case UNKNOWN_PACKET: /* FALL THRU */
        default:
            break;
        }
    } else
        /* Count the type of packets found. */
        switch ((int)pType) {
        case RTE_ETHER_TYPE_ARP:
            info->qstats[qid].stats.arp_pkts++;
            break;
        case RTE_ETHER_TYPE_IPV4:
            info->qstats[qid].stats.ip_pkts++;
            break;
        case RTE_ETHER_TYPE_IPV6:
            info->qstats[qid].stats.ipv6_pkts++;
            break;
        case RTE_ETHER_TYPE_VLAN:
            info->qstats[qid].stats.vlan_pkts++;
            break;
        default:
            break;
        }

    plen += pktgen_get_hw_strip_crc();

    /* Count the size of each packet. */
    if (plen == RTE_ETHER_MIN_LEN)
        info->qstats[qid].sizes._64++;
    else if ((plen >= (RTE_ETHER_MIN_LEN + 1)) && (plen <= 127))
        info->qstats[qid].sizes._65_127++;
    else if ((plen >= 128) && (plen <= 255))
        info->qstats[qid].sizes._128_255++;
    else if ((plen >= 256) && (plen <= 511))
        info->qstats[qid].sizes._256_511++;
    else if ((plen >= 512) && (plen <= 1023))
        info->qstats[qid].sizes._512_1023++;
    else if ((plen >= 1024) && (plen <= RTE_ETHER_MAX_LEN))
        info->qstats[qid].sizes._1024_1518++;
    else if (plen < RTE_ETHER_MIN_LEN)
        info->qstats[qid].sizes.runt++;
    else if (plen > RTE_ETHER_MAX_LEN)
        info->qstats[qid].sizes.jumbo++;
    else
        info->qstats[qid].sizes.unknown++;

    /* Process multicast and broadcast packets. */
    if (unlikely(((uint8_t *)m->buf_addr + m->data_off)[0] == 0xFF)) {
        if ((((uint64_t *)m->buf_addr + m->data_off)[0] & 0xFFFFFFFFFFFF0000LL) ==
            0xFFFFFFFFFFFF0000LL)
            info->qstats[qid].sizes.broadcast++;
        else if (((uint8_t *)m->buf_addr + m->data_off)[0] & 1)
            info->qstats[qid].sizes.multicast++;
    }
}

/**
 *
 * pktgen_packet_classify_buld - Classify a set of packets in one call.
 *
 * DESCRIPTION
 * Classify a list of packets and to improve classify performance.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

#define PREFETCH_OFFSET 3
static __inline__ void
pktgen_packet_classify_bulk(struct rte_mbuf **pkts, int nb_rx, int pid, int qid)
{
    int j, i;

    /* Prefetch first packets */
    for (j = 0; j < PREFETCH_OFFSET && j < nb_rx; j++)
        rte_prefetch0(rte_pktmbuf_mtod(pkts[j], void *));

    /* Prefetch and handle already prefetched packets */
    for (i = 0; i < (nb_rx - PREFETCH_OFFSET); i++) {
        rte_prefetch0(rte_pktmbuf_mtod(pkts[j], void *));
        j++;

        pktgen_packet_classify(pkts[i], pid, qid);
    }

    /* Handle remaining prefetched packets */
    for (; i < nb_rx; i++)
        pktgen_packet_classify(pkts[i], pid, qid);
}

/**
 *
 * pktgen_send_special - Send a special packet to the given port.
 *
 * DESCRIPTION
 * Create a special packet in the buffer provided.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

static void
pktgen_send_special(port_info_t *info, uint32_t flags)
{
    uint32_t s;

    /* Send packets attached to the sequence packets. */
    for (s = 0; s < info->seqCnt; s++) {
        if (flags & SEND_GRATUITOUS_ARP)
            pktgen_send_arp(info->pid, GRATUITOUS_ARP, s);
        if (flags & SEND_ARP_REQUEST)
            pktgen_send_arp(info->pid, 0, s);

        if (flags & SEND_PING4_REQUEST)
            pktgen_send_ping4(info->pid, s);
#ifdef INCLUDE_PING6
        if (flags & SEND_PING6_REQUEST)
            pktgen_send_ping6(info->pid, s);
#endif
    }

    /* Send the requests from the Single packet setup. */
    if (flags & SEND_GRATUITOUS_ARP)
        pktgen_send_arp(info->pid, GRATUITOUS_ARP, SINGLE_PKT);
    if (flags & SEND_ARP_REQUEST)
        pktgen_send_arp(info->pid, 0, SINGLE_PKT);

    if (flags & SEND_PING4_REQUEST)
        pktgen_send_ping4(info->pid, SINGLE_PKT);
#ifdef INCLUDE_PING6
    if (flags & SEND_PING6_REQUEST)
        pktgen_send_ping6(info->pid, SINGLE_PKT);
#endif

    pktgen_clr_port_flags(info, SEND_ARP_PING_REQUESTS);
}

typedef struct {
    port_info_t *info;
    uint16_t qid;
} pkt_data_t;

static __inline__ void
pktgen_setup_cb(struct rte_mempool *mp, void *opaque, void *obj, unsigned obj_idx __rte_unused)
{
    pkt_data_t *data     = (pkt_data_t *)opaque;
    struct rte_mbuf *m   = (struct rte_mbuf *)obj;
    union pktgen_data *d = pktgen_data_field(m);
    port_info_t *info;
    pkt_seq_t *pkt;
    uint16_t qid, idx;

    info = data->info;
    qid  = data->qid;

    /* Cleanup the mbuf data as virtio messes with the values */
    rte_pktmbuf_reset(m);

    if (mp == info->q[qid].tx_mp)
        idx = SINGLE_PKT;
    else if (mp == info->q[qid].rate_mp)
        idx = RATE_PKT;
    else if (mp == info->q[qid].range_mp)
        idx = RANGE_PKT;
    else if (mp == info->q[qid].seq_mp) {
        idx = info->seqIdx;

        /* move to the next packet in the sequence. */
        if (unlikely(++info->seqIdx >= info->seqCnt))
            info->seqIdx = 0;
    } else
        return;

    pkt = &info->seq_pkt[idx];

    if (idx == RANGE_PKT)
        pktgen_range_ctor(&info->range, pkt);

    pktgen_packet_ctor(info, idx, -1);

    rte_memcpy((uint8_t *)m->buf_addr + m->data_off, (uint8_t *)&pkt->hdr, MAX_PKT_SIZE);

    m->pkt_len  = pkt->pktSize;
    m->data_len = pkt->pktSize;

    /* Save the information */
    d->pkt_len  = m->pkt_len;
    d->buf_len  = m->buf_len;
    d->data_len = m->data_len;

    switch (pkt->ethType) {
    case RTE_ETHER_TYPE_IPV4:
        if (info->dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM)
            pkt->ol_flags = RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IP_CKSUM;
        break;

    case RTE_ETHER_TYPE_IPV6:
        pkt->ol_flags = RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV6;
        break;

    case RTE_ETHER_TYPE_VLAN:
        if (info->dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_VLAN_INSERT) {
            /* TODO */
        }
        break;
    default:
        break;
    }

    switch (pkt->ipProto) {
    case PG_IPPROTO_UDP:
        if (info->dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM)
            pkt->ol_flags |= RTE_MBUF_F_TX_UDP_CKSUM;
        break;
    case PG_IPPROTO_TCP:
        if (info->dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_TCP_CKSUM)
            pkt->ol_flags |= RTE_MBUF_F_TX_TCP_CKSUM;
        break;
    default:
        break;
    }
    m->ol_flags = pkt->ol_flags;
}

/**
 *
 * pktgen_setup_packets - Setup the default packets to be sent.
 *
 * DESCRIPTION
 * Construct the default set of packets for a given port.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

static __inline__ void
pktgen_setup_packets(port_info_t *info, struct rte_mempool *mp, uint16_t qid)
{
    pkt_data_t pkt_data;

    pktgen_clr_q_flags(info, qid, CLEAR_FAST_ALLOC_FLAG);

    if (mp == info->q[qid].pcap_mp)
        return;

    rte_spinlock_lock(&info->port_lock);

    pkt_data.info = info;
    pkt_data.qid  = qid;

    rte_mempool_obj_iter(mp, pktgen_setup_cb, &pkt_data);
    rte_spinlock_unlock(&info->port_lock);
}

/**
 *
 * pktgen_send_pkts - Send a set of packet buffers to a given port.
 *
 * DESCRIPTION
 * Transmit a set of packets mbufs to a given port.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

static __inline__ void
pktgen_send_pkts(port_info_t *info, uint16_t qid, struct rte_mempool *mp)
{
    uint64_t txCnt;
    uint16_t txLen, cnt;
    struct rte_mbuf **pkts;

    if (!pktgen_tst_port_flags(info, SEND_FOREVER)) {
        txCnt = pkt_atomic64_tx_count(&info->current_tx_count, info->tx_burst);
        if (txCnt == 0) {
            pktgen_clr_port_flags(info, (SENDING_PACKETS | SEND_FOREVER));
            pktgen_send_burst(info, qid);
            return;
        }
    } else
        txCnt = info->tx_burst;

    txLen = info->q[qid].tx_mbufs.len;
    pkts  = &info->q[qid].tx_mbufs.m_table[txLen];

    /* calculate the number of pkts to add that are below tx_burst count */
    cnt = txCnt - txLen;
    if (cnt > info->tx_burst)
        cnt = info->tx_burst;

    info->q[qid].tx_mbufs.len += pg_pktmbuf_alloc_bulk(mp, pkts, cnt);

    pktgen_send_burst(info, qid);
}

/**
 *
 * pktgen_main_transmit - Determine the next packet format to transmit.
 *
 * DESCRIPTION
 * Determine the next packet format to transmit for a given port.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

static void
pktgen_main_transmit(port_info_t *info, uint16_t qid)
{
    struct rte_mempool *mp = NULL;
    uint32_t flags;

    flags = rte_atomic32_read(&info->port_flags);

    /*
     * Transmit ARP/Ping packets if needed
     */
    if ((flags & SEND_ARP_PING_REQUESTS))
        pktgen_send_special(info, flags);

    /* When not transmitting on this port then continue. */
    if (flags & SENDING_PACKETS) {
        mp = info->q[qid].tx_mp;

        if (flags & (SEND_RANGE_PKTS | SEND_PCAP_PKTS | SEND_SEQ_PKTS | SEND_RATE_PACKETS)) {
            if (flags & SEND_RANGE_PKTS)
                mp = info->q[qid].range_mp;
            else if (flags & SEND_SEQ_PKTS)
                mp = info->q[qid].seq_mp;
            else if (flags & SEND_RATE_PACKETS)
                mp = info->q[qid].rate_mp;
            else if (flags & SEND_PCAP_PKTS)
                mp = info->q[qid].pcap_mp;
        }

        if (pktgen_tst_q_flags(info, qid, CLEAR_FAST_ALLOC_FLAG))
            pktgen_setup_packets(info, mp, qid);

        pktgen_send_pkts(info, qid, mp);
    }

    if (pktgen_tst_q_flags(info, qid, DO_TX_FLUSH))
        pktgen_tx_flush(info, qid);
}

/**
 *
 * pktgen_main_receive - Main receive routine for packets of a port.
 *
 * DESCRIPTION
 * Handle the main receive set of packets on a given port plus handle all of the
 * input processing if required.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

static __inline__ void
pktgen_main_receive(port_info_t *info, uint8_t lid, struct rte_mbuf *pkts_burst[], uint16_t nb_pkts)
{
    uint8_t pid;
    uint16_t qid, nb_rx;
    capture_t *capture;
    struct qstats_s *qstats;
    int i;

    pid = info->pid;
    qid = get_rxque(pktgen.l2p, lid, pid);

    qstats = &info->qstats[qid];

    if (pktgen_tst_port_flags(info, STOP_RECEIVING_PACKETS))
        return;

    /*
     * Read packet from RX queues and free the mbufs
     */
    if ((nb_rx = rte_eth_rx_burst(pid, qid, pkts_burst, nb_pkts)) == 0)
        return;

    qstats->rxpkts += nb_rx;
    for (i = 0; i < nb_rx; i++)
        qstats->rxbytes += rte_pktmbuf_data_len(pkts_burst[i]);

    pktgen_recv_tstamp(info, pkts_burst, nb_rx);

    /* packets are not freed in the next call. */
    pktgen_packet_classify_bulk(pkts_burst, nb_rx, pid, qid);

    if (unlikely(info->dump_count > 0))
        pktgen_packet_dump_bulk(pkts_burst, nb_rx, pid);

    if (unlikely(pktgen_tst_port_flags(info, CAPTURE_PKTS))) {
        capture = &pktgen.capture[pktgen.core_info[lid].s.socket_id];
        if (unlikely(capture->port == pid))
            pktgen_packet_capture_bulk(pkts_burst, nb_rx, capture);
    }

    rte_pktmbuf_free_bulk(pkts_burst, nb_rx);
}

static void
port_map_info(uint8_t lid, port_info_t **infos, uint8_t *qids, uint8_t *txcnt, uint8_t *rxcnt,
              const char *msg)
{
    uint8_t idx, pid, cnt = 0;
    uint8_t rx, tx;
    char buf[256];

    rx = get_lcore_rxcnt(pktgen.l2p, lid);
    tx = get_lcore_txcnt(pktgen.l2p, lid);

    if (txcnt && rxcnt) {
        *rxcnt = rx;
        *txcnt = tx;
        cnt    = tx;
    } else if (rxcnt) {
        *rxcnt = rx;
        cnt    = rx;
    } else if (txcnt) {
        *txcnt = tx;
        cnt    = tx;
    }

    snprintf(buf, sizeof(buf), "  %s processing lcore: %3d rx: %2d tx: %2d", msg, lid, rx, tx);

    for (idx = 0; idx < cnt; idx++) {
        if (rxcnt)
            pid = get_rx_pid(pktgen.l2p, lid, idx);
        else
            pid = get_tx_pid(pktgen.l2p, lid, idx);

        if ((infos[idx] = get_port_private(pktgen.l2p, pid)) == NULL)
            rte_panic("Config error: No port %d found on lcore %d\n", pid, lid);

        if (qids)
            qids[idx] = get_txque(pktgen.l2p, lid, pid);
    }

    pktgen_log_info("%s", buf);
}

/**
 *
 * pktgen_main_rxtx_loop - Single thread loop for tx/rx packets
 *
 * DESCRIPTION
 * Handle sending and receiving packets from a given set of ports. This is the
 * main loop or thread started on a single core.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

static void
pktgen_main_rxtx_loop(uint8_t lid)
{
    struct rte_mbuf *pkts_burst[MAX_PKT_RX_BURST];
    port_info_t *infos[RTE_MAX_ETHPORTS];
    uint8_t qids[RTE_MAX_ETHPORTS];
    uint8_t idx, txcnt, rxcnt;
    uint64_t curr_tsc;
    uint64_t tx_next_cycle; /**< Next cycle to send a burst of traffic */
    uint64_t tx_bond_cycle;

    memset(infos, '\0', sizeof(infos));
    memset(qids, '\0', sizeof(qids));

    if (lid == rte_get_main_lcore()) {
        printf("Using %d initial lcore for Rx/Tx\n", lid);
        rte_exit(0, "using initial lcore for port");
    }
    port_map_info(lid, infos, qids, &txcnt, &rxcnt, "RX/TX");

    curr_tsc      = rte_get_tsc_cycles();
    tx_next_cycle = rte_get_tsc_cycles() + infos[0]->tx_cycles;
    tx_bond_cycle = rte_get_tsc_cycles() + rte_get_timer_hz() / 10;

    pg_start_lcore(pktgen.l2p, lid);

    if (rxcnt == 0)
        rte_panic("No ports found for %d lcore\n", lid);

    if (pktgen.verbose)
        pktgen_log_info("For RX found %d port(s) for lcore %d", rxcnt, lid);
    for (idx = 0; idx < rxcnt; idx++) {
        if (infos[idx] == NULL)
            rte_panic("Invalid RX config: port at index %d not found for %d lcore\n", idx, lid);
    }

    if (txcnt == 0)
        rte_panic("No ports found for %d lcore\n", lid);

    if (pktgen.verbose)
        pktgen_log_info("For TX found %d port(s) for lcore %d", txcnt, lid);
    for (idx = 0; idx < txcnt; idx++) {
        if (infos[idx] == NULL)
            rte_panic("Invalid TX config: port at index %d not found for %d lcore\n", idx, lid);
    }

    for (idx = 0; idx < rxcnt; idx++) {
        uint16_t pid = infos[idx]->pid;
        int dev_sock = rte_eth_dev_socket_id(pid);

        if (dev_sock != SOCKET_ID_ANY && dev_sock != (int)rte_socket_id())
            rte_panic(
                "*** port %u on socket ID %u has different socket ID on lcore %u socket ID %d\n",
                pid, rte_eth_dev_socket_id(pid), rte_lcore_id(), rte_socket_id());
    }
    while (pg_lcore_is_running(pktgen.l2p, lid)) {
        for (idx = 0; idx < rxcnt; idx++) /* Read Packets */
            pktgen_main_receive(infos[idx], lid, pkts_burst, infos[idx]->tx_burst);

        curr_tsc = rte_get_tsc_cycles();

        if (infos[0]->tx_cycles == 0) {
            pktgen_get_link_status(infos[0], infos[0]->pid, 0);
            if (infos[0]->link.link_status) {
                pktgen_packet_rate(infos[0]);
                tx_next_cycle = curr_tsc + infos[0]->tx_cycles;
            }
        }

        /* Determine when is the next time to send packets */
        if (curr_tsc >= tx_next_cycle) {
            tx_next_cycle = tx_next_cycle + infos[0]->tx_cycles;

            for (idx = 0; idx < txcnt; idx++) /* Transmit packets */
                pktgen_main_transmit(infos[idx], qids[idx]);
        } else if (curr_tsc >= tx_bond_cycle) {
            tx_bond_cycle = curr_tsc + rte_get_timer_hz() / 10;
            for (idx = 0; idx < txcnt; idx++) { /* Transmit zero pkts for Bonding PMD */
                if (pktgen_tst_port_flags(infos[idx], BONDING_TX_PACKETS))
                    rte_eth_tx_burst(infos[idx]->pid, qids[idx], NULL, 0);
            }
        }
    }

    pktgen_log_debug("Exit %d", lid);

    pktgen_exit_cleanup(lid);
}

/**
 *
 * pktgen_main_tx_loop - Main transmit loop for a core, no receive packet handling
 *
 * DESCRIPTION
 * When Tx and Rx are split across two cores this routing handles the tx packets.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

static void
pktgen_main_tx_loop(uint8_t lid)
{
    uint8_t idx, txcnt;
    port_info_t *infos[RTE_MAX_ETHPORTS];
    uint8_t qids[RTE_MAX_ETHPORTS];
    uint64_t curr_tsc;
    uint64_t tx_next_cycle; /**< Next cycle to send a burst of traffic */
    uint64_t tx_bond_cycle;

    memset(infos, '\0', sizeof(infos));
    memset(qids, '\0', sizeof(qids));

    if (lid == rte_get_main_lcore()) {
        printf("Using %d initial lcore for Rx/Tx\n", lid);
        rte_exit(0, "Invalid initial lcore assigned a port");
    }

    port_map_info(lid, infos, qids, &txcnt, NULL, "TX");

    curr_tsc      = rte_get_tsc_cycles();
    tx_next_cycle = curr_tsc + infos[0]->tx_cycles;
    tx_bond_cycle = curr_tsc + rte_get_timer_hz() / 10;

    pg_start_lcore(pktgen.l2p, lid);

    if (txcnt == 0)
        rte_panic("No ports found for %d lcore\n", lid);

    if (pktgen.verbose)
        pktgen_log_info("For TX found %d port(s) for lcore %d\n", txcnt, lid);

    for (idx = 0; idx < txcnt; idx++) {
        if (infos[idx] == NULL)
            rte_panic("Invalid TX config: port at index %d not found for %d lcore\n", idx, lid);
    }

    for (idx = 0; idx < txcnt; idx++) {
        uint16_t pid = infos[idx]->pid;
        int dev_sock = rte_eth_dev_socket_id(pid);

        if (dev_sock != SOCKET_ID_ANY && dev_sock != (int)rte_socket_id())
            rte_panic("*** port %u on socket ID %u has different socket ID for lcore %u on socket "
                      "ID %d\n",
                      pid, rte_eth_dev_socket_id(pid), rte_lcore_id(), rte_socket_id());
    }

    idx = 0;
    while (pg_lcore_is_running(pktgen.l2p, lid)) {
        curr_tsc = rte_get_tsc_cycles();

        if (infos[0]->tx_cycles == 0) {
            pktgen_get_link_status(infos[0], infos[0]->pid, 0);
            if (infos[0]->link.link_status) { /* wait for link up */
                pktgen_packet_rate(infos[0]);
                tx_next_cycle = curr_tsc + infos[0]->tx_cycles;
            }
            continue;
        }

        /* Determine when is the next time to send packets */
        if (curr_tsc >= tx_next_cycle) {
            tx_next_cycle = tx_next_cycle + infos[0]->tx_cycles;

            for (idx = 0; idx < txcnt; idx++) /* Transmit packets */
                pktgen_main_transmit(infos[idx], qids[idx]);
        } else if (curr_tsc >= tx_bond_cycle) {
            tx_bond_cycle = curr_tsc + rte_get_timer_hz() / 10;
            for (idx = 0; idx < txcnt; idx++) { /* Transmit pkts for Bonding PMD */
                if (pktgen_tst_port_flags(infos[idx], BONDING_TX_PACKETS))
                    rte_eth_tx_burst(infos[idx]->pid, qids[idx], NULL, 0);
            }
        }
    }

    pktgen_log_debug("Exit %d", lid);

    pktgen_exit_cleanup(lid);
}

/**
 *
 * pktgen_main_rx_loop - Handle only the rx packets for a set of ports.
 *
 * DESCRIPTION
 * When Tx and Rx processing is split between two ports this routine handles
 * only the receive packets.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

static void
pktgen_main_rx_loop(uint8_t lid)
{
    struct rte_mbuf *pkts_burst[MAX_PKT_RX_BURST];
    uint8_t idx, rxcnt;
    port_info_t *infos[RTE_MAX_ETHPORTS];

    memset(infos, '\0', sizeof(infos));
    if (lid == rte_get_main_lcore()) {
        printf("Using %d initial lcore for Rx/Tx\n", lid);
        rte_exit(0, "using initial lcore for ports");
    }

    port_map_info(lid, infos, NULL, NULL, &rxcnt, "RX");

    pg_start_lcore(pktgen.l2p, lid);

    if (rxcnt == 0)
        rte_panic("No ports found for %d lcore\n", lid);

    if (pktgen.verbose)
        pktgen_log_info("For RX found %d port(s) for lcore %d", rxcnt, lid);
    for (idx = 0; idx < rxcnt; idx++) {
        if (infos[idx] == NULL)
            rte_panic("Invalid RX config: port at index %d not found for %d lcore\n", idx, lid);
    }

    for (idx = 0; idx < rxcnt; idx++) {
        uint16_t pid = infos[idx]->pid;
        int dev_sock = rte_eth_dev_socket_id(pid);

        if (dev_sock != SOCKET_ID_ANY && dev_sock != (int)rte_socket_id())
            rte_panic(
                "*** port %u on socket ID %u has different socket ID for lcore %u socket ID %d\n",
                pid, rte_eth_dev_socket_id(pid), rte_lcore_id(), rte_socket_id());
    }
    while (pg_lcore_is_running(pktgen.l2p, lid))
        for (idx = 0; idx < rxcnt; idx++) /* Read packet */
            pktgen_main_receive(infos[idx], lid, pkts_burst, infos[idx]->rx_burst);

    pktgen_log_debug("Exit %d", lid);

    pktgen_exit_cleanup(lid);
}

/**
 *
 * pktgen_launch_one_lcore - Launch a single logical core thread.
 *
 * DESCRIPTION
 * Help launch a single thread on one logical core.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

int
pktgen_launch_one_lcore(void *arg __rte_unused)
{
    uint8_t lid = rte_lcore_id();

    if (pktgen_has_work())
        return 0;

    rte_delay_us_sleep((lid + 1) * 10021);

    switch (get_type(pktgen.l2p, lid)) {
    case RX_TYPE:
        pktgen_main_rx_loop(lid);
        break;
    case TX_TYPE:
        pktgen_main_tx_loop(lid);
        break;
    case (RX_TYPE | TX_TYPE):
        pktgen_main_rxtx_loop(lid);
        break;
    }
    return 0;
}

static void
_page_display(void)
{
    static unsigned int counter = 0;

    pktgen_display_set_color("top.spinner");
    scrn_printf(1, 1, "%c", "-\\|/"[(counter++ & 3)]);
    pktgen_display_set_color(NULL);

    if (pktgen.flags & CPU_PAGE_FLAG)
        pktgen_page_cpu();
    else if (pktgen.flags & PCAP_PAGE_FLAG)
        pktgen_page_pcap(pktgen.portNum);
    else if (pktgen.flags & RANGE_PAGE_FLAG)
        pktgen_page_range();
    else if (pktgen.flags & CONFIG_PAGE_FLAG)
        pktgen_page_config();
    else if (pktgen.flags & SEQUENCE_PAGE_FLAG)
        pktgen_page_seq(pktgen.portNum);
    else if (pktgen.flags & RND_BITFIELD_PAGE_FLAG)
        pktgen_page_random_bitfields(pktgen.flags & PRINT_LABELS_FLAG, pktgen.portNum,
                                     pktgen.info[pktgen.portNum].rnd_bitfields);
    else if (pktgen.flags & LOG_PAGE_FLAG)
        pktgen_page_log(pktgen.flags & PRINT_LABELS_FLAG);
    else if (pktgen.flags & LATENCY_PAGE_FLAG)
        pktgen_page_latency();
    else if (pktgen.flags & STATS_PAGE_FLAG)
        pktgen_page_phys_stats(pktgen.portNum);
    else if (pktgen.flags & XSTATS_PAGE_FLAG)
        pktgen_page_xstats(pktgen.portNum);
    else if (pktgen.flags & RATE_PAGE_FLAG)
        pktgen_page_rate();
    else
        pktgen_page_stats();
}

/**
 *
 * pktgen_page_display - Display the correct page based on timer callback.
 *
 * DESCRIPTION
 * When timer is active update or display the correct page of data.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

void
pktgen_page_display(void)
{
    static unsigned int update_display = 1;

    /* Leave if the screen is paused */
    if (scrn_is_paused())
        return;

    scrn_save();

    if (pktgen.flags & UPDATE_DISPLAY_FLAG) {
        pktgen.flags &= ~UPDATE_DISPLAY_FLAG;
        update_display = 1;
    }

    update_display--;
    if (update_display == 0) {
        update_display = UPDATE_DISPLAY_TICK_INTERVAL;
        _page_display();

        if (pktgen.flags & PRINT_LABELS_FLAG)
            pktgen.flags &= ~PRINT_LABELS_FLAG;
    }

    scrn_restore();

    pktgen_print_packet_dump();
}

static void *
_timer_thread(void *arg)
{
    uint64_t process, page, process_timo, page_timo, prev;

    this_scrn = arg;

    process_timo = pktgen.hz;
    page_timo    = UPDATE_DISPLAY_TICK_RATE;

    page = prev = rte_get_tsc_cycles();
    process = page + process_timo;
    page += page_timo;

    pktgen.timer_running = 1;

    while (pktgen.timer_running) {
        uint64_t curr;

        curr = rte_get_tsc_cycles();

        if (curr >= process) {
            process = curr + process_timo;
            pktgen_process_stats((double)(curr - prev) / process_timo);
            prev = curr;
        }

        if (curr >= page) {
            page = curr + page_timo;
            pktgen_page_display();
        }

        rte_pause();
    }
    return NULL;
}

/**
 *
 * rte_timer_setup - Set up the timer callback routines.
 *
 * DESCRIPTION
 * Setup the two timers to be used for display and calculating statistics.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

void
rte_timer_setup(void)
{
    rte_cpuset_t cpuset_data;
    rte_cpuset_t *cpuset = &cpuset_data;
    pthread_t tid;

    CPU_ZERO(cpuset);

    pthread_create(&tid, NULL, _timer_thread, this_scrn);

    CPU_SET(rte_get_main_lcore(), cpuset);
    pthread_setaffinity_np(tid, sizeof(cpuset), cpuset);
}
