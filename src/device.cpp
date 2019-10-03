#include "device.h"

namespace Device {

DeviceController deviceCtrl;
frameReceiveCallback callback;

struct pcapArgs {
  int id;
  std::string name;
  u_char mac[ETHER_ADDR_LEN];

  pcapArgs(int id, std::string name, u_char* m) : id(id), name(name) {
    std::memcpy(mac, m, ETHER_ADDR_LEN);
  }
};

int getMACAddr(u_char* mac, const char* if_name = "en0") {
#ifdef __APPLE__
  ifaddrs* iflist;
  int found = -1;
  if (getifaddrs(&iflist) == 0) {
    for (ifaddrs* cur = iflist; cur; cur = cur->ifa_next) {
      if ((cur->ifa_addr->sa_family == AF_LINK) &&
          (strcmp(cur->ifa_name, if_name) == 0) && cur->ifa_addr) {
        sockaddr_dl* sdl = (sockaddr_dl*)cur->ifa_addr;
        memcpy(mac, LLADDR(sdl), sdl->sdl_alen);
        found = 1;
        break;
      }
    }
    freeifaddrs(iflist);
  }
  return found;
#else
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  ifreq ifr;
  strcpy(ifr.ifr_name, "eth0");
  int res = ioctl(sock, SIOCGIFHWADDR, &ifr);
  for (int i = 0; i < 6; ++i) {
    sprintf(mac[i], "%02x", (unsigned char)ifr.ifr_hwaddr.sa_data[i]);
  }
  return 1;
#endif
}

void getPacket(u_char* args, const struct pcap_pkthdr* header,
               const u_char* packet) {
  // args may be useless?(10.2)
  // NO!!(10.3)
  pcapArgs* pa = (pcapArgs*)args;

  if (header->len != header->caplen) {
    LOG(ERR, "Data Lost.");
    return;
  }
  if (callback != NULL) {
    int res = callback(packet, header->len, pa->id);
    if (res < 0) {
      LOG(ERR, "Callback error!");
    }
  }
}

//////////////////// Device ////////////////////

Device::Device(std::string name) : name(name) {
  id = (max_id++);

  // get MAC
  if (getMACAddr(mac, name.c_str()) < 0) {
    ASSERT(false, "[ ERR ] get MAC address failed.");
  }
  LOG(INFO, "get MAC address succeed.");

  // obtain a PCAP descriptor
  char pcap_errbuf[PCAP_ERRBUF_SIZE];
  memset(pcap_errbuf, 0, PCAP_ERRBUF_SIZE);
  pcap = pcap_open_live(name.c_str(), 65536, 0, 0, pcap_errbuf);
  if (pcap_errbuf[0] != '\0') {
    LOG(ERR, "pcap_open_live error! %s", pcap_errbuf);
  }
  if (!pcap) {
    LOG(ERR, "Cannot get pcap.");
  }

  // start sniffing
  pcapArgs pa(id, name, mac);
  pcap_loop(pcap, -1, getPacket, (u_char*)&pa);
}

int Device::getId() { return id; }

std::string Device::getName() { return name; }

void Device::getMAC(u_char* dst_mac) {
  std::memcpy(dst_mac, mac, ETHER_ADDR_LEN);
}

int Device::sendFrame(EtherFrame& frame) {
  LOG(INFO, "Sending Frame in device %s with id %d.", name.c_str(), id);

  // send the ethernet frame
  if (pcap_inject(pcap, frame.frame, frame.len) == -1) {
    pcap_perror(pcap, 0);
    pcap_close(pcap);
    return -3;
  }
  pcap_close(pcap);
  return 0;
}

//////////////////// DeviceController ////////////////////

int DeviceController::addDevice(std::string name) {
  auto d = Device(name);
  devices.push_back(d);
  return d.getId();
}

int DeviceController::findDevice(std::string name) {
  int id = -1;
  for (auto& dev : devices) {
    if (dev.getName() == name) id = dev.getId();
  }
  return id;
}

int DeviceController::getMACAddr(u_char* mac, int id) {
  int found = -1;
  for (auto& dev : devices) {
    if (dev.getId() == id) {
      found = 1;
      dev.getMAC(mac);
    }
  }
  return found;
}

int DeviceController::sendFrame(int id, EtherFrame& frame) {
  int found = -1;
  for (auto& dev : devices) {
    if (dev.getId() == id) {
      found = 1;
      dev.getMAC(frame.header.ether_shost);
      frame.updateHeader();
      int res = dev.sendFrame(frame);
      if (res < 0) {
        LOG(ERR, "Sending frame failed. error code: %d", res);
        return -1;
      }
      LOG(INFO, "Sending frame succeed.");
      break;
    }
  }
  return 0;
}

int addDevice(const char* device) {
  LOG(INFO, "Add a new device.");
  int id = deviceCtrl.addDevice(device);
  if (id < 0) {
    return -1;
  }
  LOG(INFO, "Add device succeed. id: %d", id);
  return id;
}

int findDevice(const char* device) {
  int id = deviceCtrl.findDevice(device);
  return id;
}

}  // namespace Device