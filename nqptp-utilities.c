/*
 * This file is part of the nqptp distribution (https://github.com/mikebrady/nqptp).
 * Copyright (c) 2021 Mike Brady.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial licensing is also available.
 */

#include "nqptp-utilities.h"
#include <errno.h>
#include <fcntl.h>            // fcntl etc.
#include <ifaddrs.h>          // getifaddrs
#include <linux/if_packet.h>  // sockaddr_ll
#include <linux/net_tstamp.h> // SOF_TIMESTAMPING_TX_HARDWARE and friends
#include <netdb.h>            // getaddrinfo etc.
#include <stdio.h>            // snprintf
#include <stdlib.h>           // malloc, free
#include <string.h>           // memset strcpy, etc.

#include "debug.h"

#ifndef SO_TIMESTAMPING
#define SO_TIMESTAMPING 37
#define SCM_TIMESTAMPING SO_TIMESTAMPING
#endif
#ifndef SO_TIMESTAMPNS
#define SO_TIMESTAMPNS 35
#endif
#ifndef SIOCGSTAMPNS
#define SIOCGSTAMPNS 0x8907
#endif
#ifndef SIOCSHWTSTAMP
#define SIOCSHWTSTAMP 0x89b0
#endif

void open_sockets_at_port(uint16_t port, sockets_open_bundle *sockets_open_stuff) {
  // open up sockets for UDP ports 319 and 320

  struct addrinfo hints, *info, *p;
  int ret;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;

  char portstr[20];
  snprintf(portstr, 20, "%d", port);

  ret = getaddrinfo(NULL, portstr, &hints, &info);
  if (ret) {
    die("getifaddrs: %s", gai_strerror(ret));
  }

  for (p = info; p; p = p->ai_next) {
    ret = 0;
    int fd = socket(p->ai_family, p->ai_socktype, IPPROTO_UDP);
    int yes = 1;

    // Handle socket open failures if protocol unavailable (or IPV6 not handled)
    if (fd != -1) {
#ifdef IPV6_V6ONLY
      // some systems don't support v4 access on v6 sockets, but some do.
      // since we need to account for two sockets we might as well
      // always.
      if (p->ai_family == AF_INET6) {
        ret |= setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
      }
#endif

      if (!ret)
        ret = bind(fd, p->ai_addr, p->ai_addrlen);

      int so_timestamping_flags = SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_TX_SOFTWARE |
                                  SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RX_SOFTWARE |
                                  SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
      // int so_timestamping_flags =  SOF_TIMESTAMPING_RX_SOFTWARE ;

      if (ret == 0)
        ret = setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &so_timestamping_flags,
                         sizeof(so_timestamping_flags));

      int flags = fcntl(fd, F_GETFL);
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);

      // one of the address families will fail on some systems that
      // report its availability. do not complain.

      if (ret) {
        die("unable to listen on %s port %d. The error is: \"%s\". Daemon must run as root. Or is "
            "a "
            "separate PTP daemon running?",
            p->ai_family == AF_INET6 ? "IPv6" : "IPv4", port, strerror(errno));
      } else {

        debug(2, "listening on %s port %d.", p->ai_family == AF_INET6 ? "IPv6" : "IPv4", port);
        sockets_open_stuff->sockets[sockets_open_stuff->sockets_open].number = fd;
        sockets_open_stuff->sockets[sockets_open_stuff->sockets_open].port = port;
        sockets_open_stuff->sockets_open++;
      }
    }
  }

  freeaddrinfo(info);
}

void debug_print_buffer(int level, char *buf, size_t buf_len) {
  // printf("Received %u bytes in a packet from %s:%d\n", buf_len, inet_ntoa(si_other.sin_addr),
  // ntohs(si_other.sin_port));
  char *obf =
      malloc(buf_len * 4 + 1); // to be on the safe side -- 4 characters on average for each byte
  if (obf != NULL) {
    char *obfp = obf;
    unsigned int obfc;
    for (obfc = 0; obfc < buf_len; obfc++) {
      snprintf(obfp, 3, "%02X", buf[obfc]);
      obfp += 2;
      if (obfc != buf_len - 1) {
        if (obfc % 32 == 31) {
          snprintf(obfp, 5, " || ");
          obfp += 4;
        } else if (obfc % 16 == 15) {
          snprintf(obfp, 4, " | ");
          obfp += 3;
        } else if (obfc % 4 == 3) {
          snprintf(obfp, 2, " ");
          obfp += 1;
        }
      }
    };
    *obfp = 0;
    switch (buf[0]) {

    case 0x10:
      debug(level, "SYNC: \"%s\".", obf);
      break;
    case 0x18:
      debug(level, "FLUP: \"%s\".", obf);
      break;
    case 0x19:
      debug(level, "DRSP: \"%s\".", obf);
      break;
    case 0x1B:
      debug(level, "ANNC: \"%s\".", obf);
      break;
    case 0x1C:
      debug(level, "SGNL: \"%s\".", obf);
      break;
    default:
      debug(1, "XXXX  \"%s\".", obf); // output this at level 1
      break;
    }
    free(obf);
  }
}

uint64_t get_self_clock_id() {
  // make up a clock ID based on an interfaces' MAC
  char local_clock_id[8];
  int len = 0;
  struct ifaddrs *ifaddr = NULL;
  struct ifaddrs *ifa = NULL;
  int status;
  if ((status = getifaddrs(&ifaddr) == -1)) {
    die("getifaddrs: %s", gai_strerror(status));
  } else {
    int found = 0;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
      if ((ifa->ifa_addr) && (ifa->ifa_addr->sa_family == AF_PACKET)) {
        struct sockaddr_ll *s = (struct sockaddr_ll *)ifa->ifa_addr;
        if ((strcmp(ifa->ifa_name, "lo") != 0) && (found == 0)) {
          len = s->sll_halen;
          memcpy(local_clock_id, &s->sll_addr, len);
          found = 1;
        }
      }
    }
    freeifaddrs(ifaddr);
  }
  // if the length of the MAC address is 6 we need to doctor it a little
  // See Section 7.5.2.2.2 IEEE EUI-64 clockIdentity values, NOTE 2

  if (len == 6) { // i.e. an EUI-48 MAC Address
    local_clock_id[7] = local_clock_id[5];
    local_clock_id[6] = local_clock_id[4];
    local_clock_id[5] = local_clock_id[3];
    local_clock_id[3] = 0xFF;
    local_clock_id[4] = 0xFE;
  }
  // it's in Network Byte Order!
  uint64_t result;
  memcpy(&result, local_clock_id, sizeof(result));
  // debug(1,"local_clock_id: %" PRIx64 ".", result);
  return result;
}

/*
void send_delay_req_message(int socket_number, SOCKADDR *from_sock_addr, uint16_t seqno) {
  struct ptp_delay_req_message m;
  memset(&m, 0, sizeof(m));
  m.header.transportSpecificAndMessageID = 0x11; // Table 19, pp 125, 1 byte field
  m.header.reservedAndVersionPTP = 0x02;         // 1 byte field
  m.header.messageLength = htons(44);
  m.header.flags = htons(0x608);
  m.header.sourcePortID = htons(1);
  m.header.controlOtherMessage = 5; // 1 byte field
  m.header.sequenceId = htons(seqno);
  m.header.logMessagePeriod = 0x7f; // Table 24, pp 128
  uint64_t sid = get_self_clock_id();
  memcpy(&m.header.clockIdentity, &sid, sizeof(uint64_t));
  struct msghdr header;
  struct iovec io;
  memset(&header, 0, sizeof(header));
  memset(&io, 0, sizeof(io));
  header.msg_name = from_sock_addr;
  header.msg_namelen = sizeof(SOCKADDR);
  header.msg_iov = &io;
  header.msg_iov->iov_base = &m;
  header.msg_iov->iov_len = sizeof(m);
  header.msg_iovlen = 1;
  uint64_t transmission_time = get_time_now(); // in case nothing better works
  if ((sendmsg(socket_number, &header, 0)) == -1) {
    debug(1, "Error in sendmsg [errno = %d]", errno);
  } else {
    debug_print_buffer(1, (char *)&m, sizeof(m));
  }
}
*/
