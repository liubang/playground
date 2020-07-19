#include <iostream>
#include <pcap.h>

int main(int argc, char* argv[]) {
  char *dev, errbuf[PCAP_ERRBUF_SIZE];
  dev = pcap_lookupdev(errbuf);
  if (NULL == dev) {
    std::cout << "Couldn't find default device: " << errbuf << std::endl;
    return 2;
  }
  std::cout << "Device:" << dev << std::endl;
  return 0;
}
