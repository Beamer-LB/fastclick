#ifndef CLICK_FORCEICMP_HH
#define CLICK_FORCEICMP_HH
#include <click/element.hh>
#include <click/glue.hh>
CLICK_DECLS

/*
 * =c
 * ForceICMP([TYPE, CODE])
 * =s UDP
 * sets ICMP checksum
 * =d
 * Sets the ICMP checksum of an ICMP-in-IP packet. Optionally
 * sets the TYPE and CODE of the ICMP header.
 */

class ForceICMP : public Element {
public:
  ForceICMP();
  ~ForceICMP();
  
  const char *class_name() const		{ return "ForceICMP"; }
  const char *processing() const		{ return AGNOSTIC; }
  ForceICMP *clone() const;
  int configure(Vector<String> &conf, ErrorHandler *errh);

  Packet *simple_action(Packet *);

private:
  int _count;
  int _type;
  int _code;
};

CLICK_ENDDECLS
#endif
