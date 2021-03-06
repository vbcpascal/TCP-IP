/**
 * @file testIP.cpp
 * @author guanzhichao (vbcpascal@gmail.com)
 * @version 0.1
 * @date 2019-11-22
 *
 * @brief Test: IP packet sent and received.
 *
 */

#include "api.h"
#include "ip.h"

int myIpCallback(const void* buf, int len) {
  Ip::IpPacket ipp((u_char*)buf, len);
  Printer::printIpPacket(ipp);
  return 0;
}

int main(int argc, char* argv[]) {
  api::init();
  api::setIPPacketReceiveCallback(myIpCallback);
  api::addAllDevice(true);

  std::string srcStr, dstStr, msg, tmp;

  while (true) {
    std::cout << "Input an ip address to send packet: ";
    std::getline(std::cin, tmp);
    if (tmp != "") srcStr = tmp;
    if (tmp == "end") break;
    std::cout << "Input destination ip address: ";
    std::getline(std::cin, tmp);
    if (tmp != "") dstStr = tmp;
    std::cout << "Input your message: ";
    std::getline(std::cin, tmp);
    if (tmp != "") msg = tmp;
    std::cout << "length of messsage: " << msg.length() << std::endl;
    std::cout << srcStr << " -> " << dstStr << " : " << msg << std::endl;

    ip_addr src, dst;
    inet_aton(srcStr.c_str(), &src);
    inet_aton(dstStr.c_str(), &dst);
    api::sendIPPacket(src, dst, IPPROTO_UDP, msg.c_str(), msg.length() + 1);
  }

  Device::deviceMgr.keepReceiving();
  return 0;
}
