#ifndef CLICK_LOOKUPIP6ROUTE_HH
#define CLICK_LOOKUPIP6ROUTE_HH
#include <click/element.hh>
#include <click/ip6table.hh>
CLICK_DECLS

/*
 * =c
 * LookupIP6Route(DST1 MASK1 GW1 OUT1, DST2 MAS2 GW2 OUT2, ...)
 * =s IPv6
 * 
 * =d
 * Input: IP6 packets (no ether header).
 * Expects a destination IP6 address annotation with each packet.
 * Looks up the address, sets the destination annotation to
 * the corresponding GW (if non-zero), and emits the packet
 * on the indicated OUTput.
 *
 * Each comma-separated argument is a route, specifying
 * a destination and mask, a gateway (zero means none),
 * and an output index.
 *
 * =e
 *
 *   ... -> GetIP6Address(24) -> rt;
 *   rt :: LookupIP6Route(
 *          3ffe:1ce1:2::/128 ::0 0,
 *          3ffe:1ce1:2:0:200::/128  ::0 0,
 *          3ffe:1ce1:2:/80 ::0 1,
 *          3ffe:1ce1:2:0:200::/80: ::0 2,
 *          0::ffff:0:0/96 ::0 3,
 *          ::0/0 3ffe:1ce1:2::2 1);
	
 *   rt[0] -> ToLinux;
 *   rt[1] -> ... -> ToDevice(eth0);
 *   rt[2] -> ... -> ToDevice(eth1);
 *   ...
 *
 */

class LookupIP6Route : public Element {
public:
  LookupIP6Route();
  ~LookupIP6Route();
  
  const char *class_name() const		{ return "LookupIP6Route"; }
  const char *processing() const		{ return AGNOSTIC; }
  LookupIP6Route *clone() const;
  
  int configure(Vector<String> &, ErrorHandler *);
  int initialize(ErrorHandler *);

  void push(int port, Packet *p);

private:

  IP6Table _t;

  IP6Address _last_addr;
  IP6Address _last_gw;
  int _last_output;

#ifdef IP_RT_CACHE2
  IPAddress _last_addr2;
  IPAddress _last_gw2;
  int _last_output2;
#endif
  
};

CLICK_ENDDECLS
#endif
