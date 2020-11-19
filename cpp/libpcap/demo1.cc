#include <iostream>
#include <cstdio>
#include <pcap.h>
#include <gflags/gflags.h>

DEFINE_string(net_inter, "enp0s25", "");

int main(int argc, char* argv[]) {
  // clang-format off
  pcap_t* handle;                /* Session handle */
  char errbuf[PCAP_ERRBUF_SIZE]; /* Error string */
  struct bpf_program fp;         /* The compiled filter */
  char filter_exp[] = "port 22"; /* The filter expression */
  bpf_u_int32 mask;              /* Our netmask */
  bpf_u_int32 net;               /* Our IP */
  struct pcap_pkthdr header;     /* The header that pcap gives us */
  const u_char* packet;          /* The actual packet */
  // clang-format on

  // /* Define the device */
  // dev = pcap_lookupdev(errbuf);
  // if (nullptr == dev) {
  //   ::fprintf(stderr, "Couldn't find default device: %s\n", errbuf);
  //   return 2;
  // }
  //

  /* Find the properties for the device */
  if (pcap_lookupnet(FLAGS_net_inter.data(), &net, &mask, errbuf) == -1) {
    ::fprintf(
        stderr,
        "Couldn't get netmask for device %s: %s\n",
        FLAGS_net_inter.data(),
        errbuf);
    net = 0;
    mask = 0;
  }

  /* Open the session in promiscuous mode */
  handle = pcap_open_live(FLAGS_net_inter.data(), BUFSIZ, 1, 1000, errbuf);
  if (nullptr == handle) {
    ::fprintf(
        stderr,
        "Couldn't open device %s: %s\n",
        FLAGS_net_inter.data(),
        errbuf);
    return 2;
  }

  /* Compile and apply the filter */
  if (pcap_compile(handle, &fp, filter_exp, 0, net) == -1) {
    ::fprintf(
        stderr,
        "Couldn't parse filter %s: %s\n",
        filter_exp,
        pcap_geterr(handle));
    return 2;
  }

  if (pcap_setfilter(handle, &fp) == -1) {
    ::fprintf(
        stderr,
        "Couldn't install filter %s: %s\n",
        filter_exp,
        pcap_geterr(handle));
    return 2;
  }

  /* Grab a packet */
  packet = pcap_next(handle, &header);
  /* Print its length */
  ::printf("Jacked a packet with length of [%d]\n", header.len);
  // ::printf("packet is: %s\n", packet);
  /* And close the session */
  pcap_close(handle);

  return 0;
}
