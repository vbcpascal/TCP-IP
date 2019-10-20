/**
 * @file router.h
 * @author guanzhichao
 * @brief Library supporting Router
 * @version 0.1
 * @date 2019-10-18
 *
 */

#ifndef ROUTER_H_
#define ROUTER_H_

#include <optional>
#include <set>

#include "device.h"
#include "sdp.h"

namespace SDP {
int sdpCallBack(const void* buf, int len, DeviceId id);
}

namespace Route {

using RoutingTable = std::set<RouteItem>;

class Router {
 public:
  RoutingTable table;

  RouteItem lookup(const ip_addr& ip);
  int setTable(const in_addr& dst, const in_addr& mask,
               const MAC::MacAddr& nextHopMac, const Device::DevicePtr& dev);
  int setItem(const RouteItem& ri);

  /**
   * @brief Init a router: add local device and tell neibours
   *
   */
  void init();

  /**
   * @brief Send routing table to others.
   *
   * Can only be tranferred in two cases:
   * 1. When a new device added into network, and you send routing table back to
   * the specific device. In this case, you should tell me the withDev and toMac
   * 2. Send routing table to neibours. In this case, this two param is useless
   *
   * @param flag flag
   * @param withDev (optional) send with device
   * @param toMac (optional) send to mac address
   */
  void sendRoutingTable(int flag, std::optional<Device::DevicePtr> withDev,
                        std::optional<MAC::MacAddr> toMac);

  /**
   * @brief Update Routing table with a set of SDP items
   *
   * @param sis SDP items
   * @param mac nexthop's MAC address
   * @param dev nexthop via (in my host)
   */
  void update(const SDP::SDPItemVector& sis, const MAC::MacAddr mac,
              const Device::DevicePtr dev);
};

extern Router router;

}  // namespace Route

namespace Printer {
void printRouteItem(const Route::RouteItem& r);
void printRouteTable();
}  // namespace Printer

#endif
