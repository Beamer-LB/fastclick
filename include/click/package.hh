#ifndef CLICK_PACKAGE_HH
#define CLICK_PACKAGE_HH
#include <click/vector.hh>
#include <click/string.hh>
#ifndef CLICK_TOOL
CLICK_DECLS
class Element;
CLICK_ENDDECLS
#endif

extern "C" {

void click_provide(const char *);
void click_unprovide(const char *);
bool click_has_provision(const char *);
void click_public_packages(CLICK_NAME(Vector)<CLICK_NAME(String)> &);

#ifndef CLICK_TOOL
int click_add_element_type(const char *, CLICK_NAME(Element) *);
void click_remove_element_type(int);
#endif

}

#endif
