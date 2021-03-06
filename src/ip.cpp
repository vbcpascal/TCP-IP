#include "ip.h"

namespace {
bool sameSubnet(ip_addr src, ip_addr dst, ip_addr mask) {
  return (src.s_addr & mask.s_addr) == (dst.s_addr & mask.s_addr);
}
}  // namespace

namespace Ip {

IPPacketReceiveCallback callback = nullptr;

void ipCopy(ip_addr &dst, const ip_addr &src) {
  memcpy(&dst, &src, sizeof(ip_addr));
}

std::string ipToStr(const ip_addr &ip) {
  char ipstr[20];
  ipToStr(ip, ipstr);
  return std::string(ipstr);
}

void ipToStr(const ip_addr &ip, char *ipstr) {
  strcpy(ipstr, inet_ntoa(ip));
  return;
}

// this is necessary for a common callback
int ipCallBack(const void *buf, int len, DeviceId id) {
  IpPacket ipp((u_char *)buf, len);
  if (!ipp.chkChksum()) LOG_WARN("Checksum error.");
  ipp.ntohType();
  ip_addr dstIp = ipp.hdr.ip_dst;

  // is me?
  if (Device::deviceMgr.haveDeviceWithIp(dstIp)) {
    if (callback) {
      return callback(&ipp, len);
    } else {
      return 0;
    }
  }

  // route it! -->
  char tmpipstr[20];
  MAC::MacAddr dstMac;
  Device::DevicePtr dev;
  ipToStr(dstIp, tmpipstr);

  // lookup routing table -->
  Route::RouteItem ri;
  ri = Route::router.lookup(dstIp);
  if (ri.ipPrefix.s_addr == 0) {
    LOG_WARN("No route for %s", tmpipstr);
    return -1;
  }

  // > in my subnet
  if (ri.isDev) {
    dev = ri.dev;
    dstMac = Arp::arpMgr.getMacAddr(dev, dstIp);
    if (MAC::isBroadcast(dstMac)) {
      LOG_ERR("MAC address not found in subnet");
      return -1;
    }
  }

  // > route
  else {
    dev = ri.dev;
    dstMac = ri.nextHopMac;
    LOG_INFO("Route to \033[;1m%s\033[0m via \033[33m%s\033[0m", tmpipstr,
             dev->getName().c_str());
  }

  int packLen = ipp.hdr.ip_len;
  ipp.htonType();
  return Device::deviceMgr.sendFrame(&ipp, packLen, ETHERTYPE_IP, dstMac.addr,
                                     dev);
}

IpPacket::IpPacket(const u_char *buf, int len) { memcpy(&hdr, buf, len); }

void IpPacket::setDefaultHdr() {
  hdr.ip_v = 4;         // version
  hdr.ip_hl = 5;        // Internet Header Length
  hdr.ip_tos = IGNORE;  // type of service
  hdr.ip_id = IGNORE;   // identification
  hdr.ip_off = IP_DF;   // flags and fragment offset
  hdr.ip_ttl = 16;      // time to live
}

int IpPacket::setData(const u_char *buf, int len) {
  if (len > IP_MAXPACKET - hdr.ip_hl * 4) {
    LOG_ERR("packet is to large.");
    return -1;
  }
  hdr.ip_len = len + hdr.ip_hl * 4;
  memcpy(data, buf, len);
  return 0;
}

void IpPacket::setChksum() {
  hdr.ip_sum = 0;
  hdr.ip_sum = getChecksum(&hdr, hdr.ip_hl * 4);
}

int IpPacket::chkChksum() {
  auto res = getChecksum(&hdr, hdr.ip_hl * 4);
  return res == 0;
}

void IpPacket::htonType() {
  hdr.ip_len = htons(hdr.ip_len);
  hdr.ip_id = htons(hdr.ip_id);
  hdr.ip_off = htons(hdr.ip_off);
  hdr.ip_sum = htons(hdr.ip_sum);
}

void IpPacket::ntohType() {
  hdr.ip_len = ntohs(hdr.ip_len);
  hdr.ip_id = ntohs(hdr.ip_id);
  hdr.ip_off = ntohs(hdr.ip_off);
  hdr.ip_sum = ntohs(hdr.ip_sum);
}

int sendIPPacket(const ip_addr src, const ip_addr dest, int proto,
                 const void *buf, int len) {
  char tmpipstr[20];
  // get src device
  auto dev = Device::deviceMgr.getDevicePtr(src);
  if (!dev) {
    ipToStr(src, tmpipstr);
    LOG_ERR("No device with ip %s", tmpipstr);
    return -1;
  }

  MAC::MacAddr dstMac;

  // get dest mac addr if in the same subnet
  if (sameSubnet(src, dest, dev->getSubnetMask())) {
    dstMac = Arp::arpMgr.getMacAddr(dev, dest);
    if (MAC::isBroadcast(dstMac)) {
      LOG_ERR("MAC address not found in subnet");
      return -1;
    }
  }

  // to nexthop (HOW TO DO IT!!!)
  // wtf... is me?
  else {
    auto ri = Route::router.lookup(dest);
    if (ri.ipPrefix.s_addr == 0) {
      ipToStr(dest, tmpipstr);
      LOG_ERR("No route for %s", tmpipstr);
      return -1;
    } else {
      dstMac = ri.nextHopMac;
    }
  }

  // packet
  Ip::IpPacket ipPack;
  ipPack.setDefaultHdr();
  ipPack.hdr.ip_src = src;
  ipPack.hdr.ip_dst = dest;
  ipPack.hdr.ip_p = proto;
  ipPack.setData((u_char *)buf, len);

  int packLen = ipPack.hdr.ip_len;
  // Printer::printIpPacket(ipPack, true);

  ipPack.htonType();
  ipPack.setChksum();
  return Device::deviceMgr.sendFrame(&ipPack, packLen, ETHERTYPE_IP,
                                     dstMac.addr, dev);
}

uint16_t getChecksum(const void *vdata, size_t length) {
  // Cast the data pointer to one that can be indexed.
  char *data = (char *)vdata;

  // Initialise the accumulator.
  uint64_t acc = 0xffff;

  // Handle any partial block at the start of the data.
  unsigned int offset = ((uintptr_t)data) & 3;
  if (offset) {
    size_t count = 4 - offset;
    if (count > length) count = length;
    uint32_t word = 0;
    memcpy(offset + (char *)&word, data, count);
    acc += ntohl(word);
    data += count;
    length -= count;
  }

  // Handle any complete 32-bit blocks.
  char *data_end = data + (length & ~3);
  while (data != data_end) {
    uint32_t word;
    memcpy(&word, data, 4);
    acc += ntohl(word);
    data += 4;
  }
  length &= 3;

  // Handle any partial block at the end of the data.
  if (length) {
    uint32_t word = 0;
    memcpy(&word, data, length);
    acc += ntohl(word);
  }

  // Handle deferred carries.
  acc = (acc & 0xffffffff) + (acc >> 32);
  while (acc >> 16) {
    acc = (acc & 0xffff) + (acc >> 16);
  }

  // If the data began at an odd byte address
  // then reverse the byte order to compensate.
  if (offset & 1) {
    acc = ((acc & 0xff00) >> 8) | ((acc & 0x00ff) << 8);
  }

  // Return the checksum in network byte order.
  return htons(~acc);
}

}  // namespace Ip

namespace Printer {

void printIp(const ip_addr &ip, bool newline) {
  std::string s = Ip::ipToStr(ip);
  printf("%s", s.c_str());
  if (newline) printf("\n");
}

std::map<u_char, std::string> ipProtoNameMap{
    {IPPROTO_TCP, "\033[35mTCP\033[0m"}, {IPPROTO_UDP, "\033[36mUDP\033[0m"}};

void printIpPacket(const Ip::IpPacket &ipp, bool sender) {
  char srcIpStr[20], dstIpStr[20];
  Ip::ipToStr(ipp.hdr.ip_src, srcIpStr);
  Ip::ipToStr(ipp.hdr.ip_dst, dstIpStr);
  printf("\033[;1m%s IP\033[0m %s -> %s, len = %d, proto: %s\n",
         (sender ? "--" : ">>"), srcIpStr, dstIpStr, ipp.hdr.ip_len,
         ipProtoNameMap[ipp.hdr.ip_p].c_str());
}
}  // namespace Printer