#include <sys/types.h>
#include <stdio.h>
#include <assert.h>
#include "responder.c"
#include <stdarg.h>
#include <net/if_arp.h>
#include <netinet/udp.h>

#define DEFAULT_SRC_MAC (u_char*)"\x46\x45\x44\x43\x42\x41"
#define DEFAULT_DST_MAC (u_char*)"\x36\x35\x34\x33\x32\x31"
#define DEFAULT_DST_IP (uint32_t*)"\xc0\xa8\x00\x02"
#define DEFAULT_SRC_IP (uint32_t*)"\xc0\xa8\x53\x47"

int tests_ok;
int tests_not_ok;

static int current_test = 0;

unsigned char test_result_buffer[ETH_FRAME_LEN];
ssize_t test_result_len;

void test_log(const char * format, ...) {
   char buf[4096];
   size_t l;
   time_t t;
   struct tm tm;
   va_list va;
   va_start(va, format);
   t = time(NULL);
   localtime_r(&t, &tm);
   l = strftime(buf, sizeof buf, "[%Y-%m-%d %H:%M:%S]: ", &tm);
   vsnprintf(buf+l, sizeof(buf)-l, format, va);
   printf("%s\n", buf);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
   // makes sure we are not called wrong
   assert(sockfd>0);
   assert(buf != NULL);
   assert(len>0);
   assert(flags==0);
   assert(len<sizeof(test_result_buffer));
   memcpy(test_result_buffer, buf, len);
   test_result_len = len;
   return len;
}

void test_sanitize(void) {
  memset(test_result_buffer,0,sizeof test_result_buffer);
  test_result_len = 0;
}

ssize_t build_eth_header(const u_char *dmac, const u_char *smac, unsigned short vlan, unsigned short protocol, u_char *bytes) {
   unsigned short val;
   struct ether_header eh;

   memcpy(eh.ether_dhost, dmac, ETH_ALEN);
   memcpy(eh.ether_shost, smac, ETH_ALEN);
#ifdef HAS_VLAN
   eh.ether_type = htons(ETH_P_8021Q);
   val = htons(vlan);
   memcpy(bytes+ETH_HLEN, &val, 2);
   val = htons(protocol);
   memcpy(bytes+ETH_HLEN+2, &val, 2);
#else
   eh.ether_type = htons(protocol);
#endif
   memcpy(bytes, &eh, ETH_HLEN);
   return ETH_HLEN + ETH_O_VLAN;
}

// ensure that the response comes from dest->src
int assert_eth_header(const u_char *bytes, unsigned short vlan, unsigned short protocol) {
   struct ether_header eh;
   memcpy(&eh, bytes, sizeof eh);

   // check macs
   if (memcmp(eh.ether_shost, DEFAULT_DST_MAC, ETH_ALEN)) {
      test_log("Wrong source MAC in packet");
      return 1;
   }
   if (memcmp(eh.ether_dhost, DEFAULT_SRC_MAC, ETH_ALEN)) {
      test_log("Wrong destination MAC in packet");
      return 1;
   }
#ifdef HAS_VLAN
   if (eh.ether_type != htons(ETH_P_8021Q) ||
       *(uint16_t*)(bytes + ETH_HLEN) != htons(vlan) ||
       *(uint16_t*)(bytes + ETH_HLEN + 2) != htons(protocol))
#else 
   if (eh.ether_type != htons(protocol)) 
#endif
   {
      test_log("Wrong protocol number in packet");
      return 1;
   }
   return 0;
}

void build_ip_header(uint32_t src_ip, uint32_t dst_ip, unsigned short data_len, unsigned short protocol, u_char *bytes) {
   struct iphdr iph;
   iph.version = 4;
   iph.ihl = 5;
   iph.tos = 0;
   iph.tot_len = htons(data_len + sizeof(iph));
   iph.id = 0x4545;
   iph.frag_off = 0;
   iph.ttl = 64;
   iph.protocol = protocol; 
   iph.check = 0;
   iph.saddr = src_ip;
   iph.daddr = dst_ip;

   // checksum
   ip_checksum(&iph, sizeof(iph), &iph.check);
   memcpy(bytes + ETH_HLEN + ETH_O_VLAN, &iph, sizeof(iph));
}

void build_arp_pak(const u_char *dstmac, const u_char *srcmac, short arpop, uint32_t dstip, uint32_t srcip, u_char *bytes) {
   struct arphdr ah;
   build_eth_header(dstmac, srcmac, 42, ETH_P_ARP, bytes);
   ah.ar_hrd = htons(ARPHRD_ETHER);
   ah.ar_pro = htons(ETH_P_IP);
   ah.ar_hln = ETH_ALEN;
   ah.ar_pln = 4; // ip address size
   ah.ar_op = htons(arpop);
   memcpy(bytes + ETH_HLEN + ETH_O_VLAN, &ah, sizeof ah);
   memcpy(bytes + ETH_HLEN + ETH_O_VLAN + sizeof(ah), srcmac, ETH_ALEN);
   memcpy(bytes + ETH_HLEN + ETH_O_VLAN + sizeof(ah) + ETH_ALEN, &srcip, 4);
   memcpy(bytes + ETH_HLEN + ETH_O_VLAN + sizeof(ah) + ETH_ALEN + 4, dstmac, ETH_ALEN);
   memcpy(bytes + ETH_HLEN + ETH_O_VLAN + sizeof(ah) + ETH_ALEN*2 + 4, &dstip, 4);
}

int assert_arp(u_char *bytes) {
   struct arphdr ah;
   if (assert_eth_header(bytes, 42, ETH_P_ARP)) return 1;
   memcpy(&ah, bytes + ETH_HLEN + ETH_O_VLAN, sizeof ah);
   if (ah.ar_hrd != htons(ARPHRD_ETHER)) { test_log("invalid hwaddr in response"); return 1; }
   if (ah.ar_pro != htons(ETH_P_IP)) { test_log("invalid protocol in response"); return 1; }
   if (ah.ar_hln != ETH_ALEN) { test_log("invalid hwaddr length in response"); return 1; }
   if (ah.ar_pln != 4) { test_log("invalid protocol length in response"); return 1; }
   if (ah.ar_op != htons(ARPOP_REPLY)) { test_log("response is not reply"); return 1; }
   // make sure it was intended for us.
   if (memcmp(bytes + ETH_HLEN + ETH_O_VLAN + sizeof(ah), DEFAULT_DST_MAC, ETH_ALEN)) { test_log("wrong source mac in reply"); return 1; }
   if (memcmp(bytes + ETH_HLEN + ETH_O_VLAN + sizeof(ah) + ETH_ALEN, DEFAULT_DST_IP, 4)) { test_log("wrong source ip in reply"); return 1; }
   if (memcmp(bytes + ETH_HLEN + ETH_O_VLAN + sizeof(ah) + ETH_ALEN + 4, DEFAULT_SRC_MAC, ETH_ALEN)) { test_log("wrong destination mac in reply"); return 1; }
   if (memcmp(bytes + ETH_HLEN + ETH_O_VLAN + sizeof(ah) + ETH_ALEN*2 + 4, DEFAULT_SRC_IP, 4)) { test_log("wrong destination ip in reply, %08x != %08x", *(uint32_t*)(bytes + ETH_HLEN + ETH_O_VLAN + sizeof(ah) + ETH_ALEN*2 + 4), *DEFAULT_SRC_IP); return 1; }
   return 0;
}

int assert_ip(u_char *bytes, uint32_t srcip, uint32_t dstip, unsigned short data_len, unsigned short proto) {
   struct iphdr iph;
   if (assert_eth_header(bytes, 42, ETH_P_IP)) return 1;
   memcpy(&iph, bytes + ETH_HLEN + ETH_O_VLAN, sizeof iph);
   // checksum test
   ip_checksum(&iph, sizeof(iph), &iph.check);
   if (iph.check != 0) { test_log("ip checksum did not validate"); return 1; }
   // check corrct protocol and such
   if (iph.version != 4) { test_log("wrong ip version"); return 1; }
   if (iph.ihl != 5) { test_log("ip header len != 5"); return 1; }
   if (iph.tot_len != htons(data_len + sizeof(iph))) { test_log("total length wasn't as expected: wanted %u, got %u", data_len + sizeof(iph), ntohs(iph.tot_len)); return 1; }
   if (iph.id != 0x4545) { test_log("IPID wrong"); return 1; }
   if (iph.protocol != proto) { test_log("unexpected protocol: wanted %u, got %u", proto, iph.protocol); return 1; }
   if (iph.saddr != srcip) { test_log("invalid source ip"); return 1; }
   if (iph.daddr != dstip) { test_log("invalid destination ip"); return 1; }
   return 0;
}

void do_pak_handler(const u_char *bytes, ssize_t len) {
   struct pcap_pkthdr h;
   int fd = 4;
   memset(&h, 0, sizeof(h));
   h.len = h.caplen = len;
   gettimeofday(&h.ts,NULL);
   //bin2hex(bytes, h.caplen);
   pak_handler((u_char*)&fd, &h, bytes);
}

int test_valid_arp(void) {
   u_char bytes[ETH_FRAME_LEN];

   build_arp_pak(DEFAULT_DST_MAC, DEFAULT_SRC_MAC, ARPOP_REQUEST, *DEFAULT_DST_IP, *DEFAULT_SRC_IP, bytes);
  
   do_pak_handler(bytes, 64);

   if (test_result_len != 64) {
     test_log("result is not 64 bytes long as expected, was %lu", test_result_len);
     return 1; 
   }

   return assert_arp(test_result_buffer);
}

int test_arp_not_for_me(void) {
   u_char bytes[ETH_FRAME_LEN];

   build_arp_pak(DEFAULT_SRC_MAC, DEFAULT_SRC_MAC, ARPOP_REQUEST, *DEFAULT_SRC_IP, *DEFAULT_SRC_IP, bytes);

   do_pak_handler(bytes, 64);

   if (test_result_len > 0 ) {
     test_log("did not expect reply");
     return 1;
   }
   return 0;
}

int test_invalid_arp(void) {
   u_char bytes[ETH_FRAME_LEN];
   build_arp_pak(DEFAULT_DST_MAC, DEFAULT_SRC_MAC, ARPOP_REQUEST, *DEFAULT_DST_IP, *DEFAULT_SRC_IP, bytes);

   // do some slight corruption
   bytes[ETH_HLEN + ETH_O_VLAN] = 0x5;

   do_pak_handler(bytes, 64);

   if (test_result_len > 0 ) {
     test_log("did not expect reply");
     return 1;
   }
   return 0;
}

int test_broadcast_arp(void) {
   u_char bytes[ETH_FRAME_LEN];

   build_arp_pak((u_char*)"\xff\xff\xff\xff\xff\xff", DEFAULT_SRC_MAC, ARPOP_REQUEST, *DEFAULT_DST_IP, *DEFAULT_SRC_IP, bytes);

   do_pak_handler(bytes, 64);

   if (test_result_len != 64) {
     test_log("result is not 64 bytes long as expected, was %lu", test_result_len);
     return 1;
   }

   return assert_arp(test_result_buffer);
}

int test_checksum_ip(void) {
  struct iphdr hdr;
  hdr.version = 4;
  hdr.ihl = 5;
  hdr.tos = 0;
  hdr.tot_len = htons(84);
  hdr.id = 0x4545;
  hdr.frag_off = 0;
  hdr.ttl = 64;
  hdr.protocol = 1;
  hdr.check = 0xca60;
  hdr.saddr = *DEFAULT_DST_IP;
  hdr.daddr = *DEFAULT_SRC_IP;

  ip_checksum(&hdr, sizeof(hdr), &hdr.check);

  if (hdr.check != 0) {
      test_log("Checksum wrong: (0 != %04x)", hdr.check);
  }

  return hdr.check; 
}

int test_checksum_tcp(void) {
   struct udphdr uh;
   u_char data[64];

   uh.source = htons(7);
   uh.dest = htons(7);
   uh.len = 64;
   uh.check = 0xb423;
   memcpy(data, &uh, sizeof uh);
   memcpy(data+sizeof(uh), "\xbd\x3b\x78\xf8\xbc\x28\x41\x0f\xf7\xcd\x55\x91\xce\xa8\xe7\xac\xb3\xfe\x56\xd0\x6c\xa2\x1d\x41\xc9\x15\x8e\x74\xa0\x09\x4d\x2a\xe8\xd9\x76\xd9\x0c\x10\xb9\x65\x42\x11\xc9\x58\xbe\xce\x90\x89\x67\xaa\x56\xfa\xb7\x5e\xc0\xd0", 56); // just some random data to make things interesting
   tcp_checksum((u_char*)DEFAULT_SRC_IP, (u_char*)DEFAULT_DST_IP, data, 64, &uh.check);

  if (uh.check != 0) {
      test_log("Checksum wrong: (0 != %04x)", uh.check);
  }

   return uh.check;
}

int test_icmp_ping(void) {
   u_char bytes[ETH_FRAME_LEN];
   struct icmphdr ih;   

   build_eth_header(DEFAULT_DST_MAC, DEFAULT_SRC_MAC, 42, ETH_P_IP, bytes);
   // add IP header.
   build_ip_header(*DEFAULT_SRC_IP, *DEFAULT_DST_IP, 64, 1, bytes);
   ih.type = 8;
   ih.code = 0;
   ih.checksum = 0;
   ih.un.echo.id = 0x4343;
   ih.un.echo.sequence = 0x4242;
   // copy into place
   memcpy(bytes + ETH_HLEN + ETH_O_VLAN + sizeof(struct ip), &ih, sizeof(ih));
   // add some payload
   memcpy(bytes + ETH_HLEN + ETH_O_VLAN + sizeof(struct ip) + sizeof(ih), "\xbd\x3b\x78\xf8\xbc\x28\x41\x0f\xf7\xcd\x55\x91\xce\xa8\xe7\xac\xb3\xfe\x56\xd0\x6c\xa2\x1d\x41\xc9\x15\x8e\x74\xa0\x09\x4d\x2a\xe8\xd9\x76\xd9\x0c\x10\xb9\x65\x42\x11\xc9\x58\xbe\xce\x90\x89\x67\xaa\x56\xfa\xb7\x5e\xc0\xd0", 56);
   // checksum
   ip_checksum(bytes + ETH_HLEN + ETH_O_VLAN + sizeof(struct ip), sizeof(ih) + 56, (uint16_t*)(bytes + ETH_HLEN + ETH_O_VLAN + sizeof(struct ip) + 2));

   // let's see what we get
   do_pak_handler(bytes, 102);

   if (test_result_len != 102) {
     test_log("result is not 102 bytes long as expected, was %lu", test_result_len);
     return 1;
   }
   
   if (assert_ip(test_result_buffer, *DEFAULT_DST_IP, *DEFAULT_SRC_IP, 64, 1)) return 1;
   memset(&ih, 0, sizeof ih);

   // check that all is good.
   ip_checksum(test_result_buffer + ETH_HLEN + ETH_O_VLAN + sizeof(struct ip), sizeof(ih)+56, (uint16_t*)(test_result_buffer + ETH_HLEN + ETH_O_VLAN + sizeof(struct ip) + 2));
   memcpy(&ih, test_result_buffer + ETH_HLEN + ETH_O_VLAN + sizeof(struct ip), sizeof ih);

   if (ih.checksum != 0) { test_log("ICMP packet checksum wrong"); return 1; }
   if (ih.type != 0) { test_log("unexpected ICMP type in response"); return 1; }
   if (ih.code != 0) { test_log("unexpected ICMP code in response"); return 1; }
   if (ih.un.echo.id != 0x4343) { test_log("wrong ICMP echo id"); return 1; }
   if (ih.un.echo.sequence != 0x4242) { test_log("wrong ICMP echo sequence"); return 1; }
   
   // check payload
   return memcmp(test_result_buffer + ETH_HLEN + ETH_O_VLAN + sizeof(struct ip) + sizeof(ih), "\xbd\x3b\x78\xf8\xbc\x28\x41\x0f\xf7\xcd\x55\x91\xce\xa8\xe7\xac\xb3\xfe\x56\xd0\x6c\xa2\x1d\x41\xc9\x15\x8e\x74\xa0\x09\x4d\x2a\xe8\xd9\x76\xd9\x0c\x10\xb9\x65\x42\x11\xc9\x58\xbe\xce\x90\x89\x67\xaa\x56\xfa\xb7\x5e\xc0\xd0", 56);
}

void run_test(int (*test_item)(void), const char *test_name) {
   test_log("test #%04d: %s", ++current_test, test_name);
 
   test_sanitize();
   if (test_item() != 0) {
     test_log("test result: FAILED");
     bin2hex(test_result_buffer, test_result_len);
     tests_not_ok++;
   } else {
     test_log("test result: PASS");
     tests_ok++;
   }
}

int main(void) {
   test_log("IP-SLA Responder test suite");

   // bootstrap global vars
   // default IP address
   inet_pton(AF_INET, DEFAULT_IP_ADDR, &dest_ip);
   // sanitize and default
   memcpy(dest_mac, DEFAULT_DST_MAC, sizeof dest_mac);
   debuglevel = 0;
   tests_ok = tests_not_ok = 0;
   dest_udp_ip_sla = htons(DEFAULT_IPSLA_PORT); 

   run_test(test_valid_arp, "replies to valid arp");
   run_test(test_arp_not_for_me, "ignores arp not intended for us");
   run_test(test_invalid_arp, "ignores invalid arp");
   run_test(test_broadcast_arp, "replies to broadcast for our IP");
   run_test(test_checksum_ip, "produces valid ip checksum");
   run_test(test_checksum_tcp, "produces valid tcp checksum");
   run_test(test_icmp_ping, "responds to ICMP echo request");

   printf("OK: %d NOT OK: %d SUCCESS %0.02f%%\n", tests_ok, tests_not_ok, (double)tests_ok/(double)(tests_ok+tests_not_ok)*100.0);

   return EXIT_SUCCESS;
}