// -*- mode: c++; c-basic-offset: 4 -*-
/*
 * toipsummarydump.{cc,hh} -- element writes packet summary in ASCII
 * Eddie Kohler
 *
 * Copyright (c) 2001 International Computer Science Institute
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include "toipsumdump.hh"
#include "fromipsumdump.hh"
#include <click/standard/scheduleinfo.hh>
#include <click/confparse.hh>
#include <click/error.hh>
#include <click/packet_anno.hh>
#include <clicknet/ip.h>
#include <clicknet/udp.h>
#include <clicknet/tcp.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
CLICK_DECLS

ToIPSummaryDump::ToIPSummaryDump()
    : Element(1, 0), _f(0), _task(this)
{
    MOD_INC_USE_COUNT;
}

ToIPSummaryDump::~ToIPSummaryDump()
{
    MOD_DEC_USE_COUNT;
}

int
ToIPSummaryDump::configure(Vector<String> &conf, ErrorHandler *errh)
{
    int before = errh->nerrors();
    String save = "timestamp 'ip src'";
    bool verbose = false;
    bool bad_packets = false;
    bool careful_trunc = true;
    _multipacket = false;

    if (cp_va_parse(conf, this, errh,
		    cpFilename, "dump filename", &_filename,
		    cpKeywords,
		    "CONTENTS", cpArgument, "log contents", &save,
		    "VERBOSE", cpBool, "be verbose?", &verbose,
		    "BANNER", cpString, "banner", &_banner,
		    "MULTIPACKET", cpBool, "output multiple packets based on packet count anno?", &_multipacket,
		    "BAD_PACKETS", cpBool, "output `!bad' messages for non-IP or bad IP packets?", &bad_packets,
		    "CAREFUL_TRUNC", cpBool, "output `!bad' messages for truncated IP packets?", &careful_trunc,
		    0) < 0)
	return -1;

    Vector<String> v;
    cp_spacevec(save, v);
    for (int i = 0; i < v.size(); i++) {
	String word = cp_unquote(v[i]);
	int what = FromIPSummaryDump::parse_content(word);
	if (what > W_NONE && what < W_LAST)
	    _contents.push_back(what);
	else
	    errh->error("unknown content type `%s'", word.cc());
    }
    if (_contents.size() == 0)
	errh->error("no contents specified");

    // remove _multipacket if packet count specified
    for (int i = 0; i < _contents.size(); i++)
	if (_contents[i] == W_COUNT)
	    _multipacket = false;
    
    _verbose = verbose;
    _bad_packets = bad_packets;
    _careful_trunc = careful_trunc;

    return (before == errh->nerrors() ? 0 : -1);
}

int
ToIPSummaryDump::initialize(ErrorHandler *errh)
{
    assert(!_f);
    if (_filename != "-") {
	_f = fopen(_filename, "wb");
	if (!_f)
	    return errh->error("%s: %s", _filename.cc(), strerror(errno));
    } else {
	_f = stdout;
	_filename = "<stdout>";
    }

    if (input_is_pull(0)) {
	ScheduleInfo::join_scheduler(this, &_task, errh);
	_signal = Notifier::upstream_pull_signal(this, 0, &_task);
    }
    _active = true;

    // magic number
    fprintf(_f, "!IPSummaryDump %d.%d\n", FromIPSummaryDump::MAJOR_VERSION, FromIPSummaryDump::MINOR_VERSION);

    if (_banner)
	fprintf(_f, "!creator %s\n", cp_quote(_banner).cc());
    
    // host and start time
    if (_verbose) {
	char buf[BUFSIZ];
	buf[BUFSIZ - 1] = '\0';	// ensure NUL-termination
	if (gethostname(buf, BUFSIZ - 1) >= 0)
	    fprintf(_f, "!host %s\n", buf);

	time_t when = time(0);
	const char *cwhen = ctime(&when);
	struct timeval tv;
	if (gettimeofday(&tv, 0) >= 0)
	    fprintf(_f, "!starttime %ld.%ld (%.*s)\n", (long)tv.tv_sec,
		    (long)tv.tv_usec, (int)(strlen(cwhen) - 1), cwhen);
    }

    // data description
    fprintf(_f, "!data ");
    for (int i = 0; i < _contents.size(); i++)
	fprintf(_f, (i ? " '%s'" : "'%s'"), FromIPSummaryDump::unparse_content(_contents[i]));
    fprintf(_f, "\n");

    _output_count = 0;
    return 0;
}

void
ToIPSummaryDump::cleanup(CleanupStage)
{
    if (_f && _f != stdout)
	fclose(_f);
    _f = 0;
}

bool
ToIPSummaryDump::bad_packet(StringAccum &sa, const String &s, int what) const
{
    assert(_bad_packets);
    int pos = s.find_left("%d");
    if (pos >= 0)
	sa << "!bad " << s.substring(0, pos) << what
	   << s.substring(pos + 2) << "\n";
    else
	sa << "!bad " << s << "\n";
    return true;
}

#define BAD_IP(msg, val)	do { if (_bad_packets) return bad_packet(sa, msg, val); iph = 0; } while (0)
#define BAD_TCP(msg, val)	do { if (_bad_packets) return bad_packet(sa, msg, val); tcph = 0; } while (0)
#define BAD_UDP(msg, val)	do { if (_bad_packets) return bad_packet(sa, msg, val); udph = 0; } while (0)

bool
ToIPSummaryDump::ascii_summary(Packet *p, StringAccum &sa) const
{
    // Not all of these will be valid, but we calculate them just once.
    const click_ip *iph = p->ip_header();
    const click_tcp *tcph = p->tcp_header();
    const click_udp *udph = p->udp_header();

    // Check that the IP header fields are valid.
    if (!iph)
	BAD_IP("no IP header", 0);
    else if (p->network_length() < (int)(sizeof(click_ip)))
	BAD_IP("truncated IP header", 0);
    else if (iph->ip_v != 4)
	BAD_IP("IP version %d", iph->ip_v);
    else if (iph->ip_hl < (sizeof(click_ip) >> 2))
	BAD_IP("IP header length %d", iph->ip_hl);
    else if (p->network_length() < (int)(iph->ip_hl << 2))
	BAD_IP("truncated IP header", 0);
    else if (ntohs(iph->ip_len) < (iph->ip_hl << 2))
	BAD_IP("IP length %d", ntohs(iph->ip_len));
    else if (p->network_length() + EXTRA_LENGTH_ANNO(p) < (uint32_t)(ntohs(iph->ip_len))
	     && _careful_trunc)
	BAD_IP("truncated IP missing %d", ntohs(iph->ip_len) - p->network_length() - EXTRA_LENGTH_ANNO(p));

    if (!iph || !tcph || iph->ip_p != IP_PROTO_TCP || !IP_FIRSTFRAG(iph))
	tcph = 0;
    else if (p->transport_length() < (int)(sizeof(click_tcp)))
	BAD_TCP((IP_ISFRAG(iph) ? "fragmented TCP header" : "truncated TCP header"), 0);
    else if (tcph->th_off < (sizeof(click_tcp) >> 2))
	BAD_TCP("TCP header length %d", tcph->th_off);
    else if (p->transport_length() < (int)(tcph->th_off << 2)
	     || ntohs(iph->ip_len) < (iph->ip_hl << 2) + (tcph->th_off << 2))
	BAD_TCP((IP_ISFRAG(iph) ? "fragmented TCP header" : "truncated TCP header"), 0);

    if (!iph || !udph || iph->ip_p != IP_PROTO_UDP || !IP_FIRSTFRAG(iph))
	udph = 0;
    else if (p->transport_length() < (int)(sizeof(click_udp))
	     || ntohs(iph->ip_len) < (iph->ip_hl << 2) + sizeof(click_udp))
	BAD_UDP((IP_ISFRAG(iph) ? "fragmented UDP header" : "truncated UDP header"), 0);

    // Adjust extra length, since we calculate lengths here based on ip_len.
    if (iph && EXTRA_LENGTH_ANNO(p) > 0) {
	int32_t full_len = p->length() + EXTRA_LENGTH_ANNO(p);
	if (ntohs(iph->ip_len) + 8 >= full_len - p->network_header_offset())
	    SET_EXTRA_LENGTH_ANNO(p, 0);
	else {
	    full_len = full_len - ntohs(iph->ip_len);
	    SET_EXTRA_LENGTH_ANNO(p, full_len);
	}
    }
    
    // Print actual contents.
    for (int i = 0; i < _contents.size(); i++) {
	if (i)
	    sa << ' ';
	
	switch (_contents[i]) {

	  case W_TIMESTAMP:
	    sa << p->timestamp_anno();
	    break;
	  case W_TIMESTAMP_SEC:
	    sa << p->timestamp_anno().tv_sec;
	    break;
	  case W_TIMESTAMP_USEC:
	    sa << p->timestamp_anno().tv_usec;
	    break;
	  case W_SRC:
	    if (!iph) goto no_data;
	    sa << IPAddress(iph->ip_src);
	    break;
	  case W_DST:
	    if (!iph) goto no_data;
	    sa << IPAddress(iph->ip_dst);
	    break;
	  case W_FRAG:
	    if (!iph) goto no_data;
	    sa << (IP_ISFRAG(iph) ? (IP_FIRSTFRAG(iph) ? 'F' : 'f') : '.');
	    break;
	  case W_FRAGOFF:
	    if (!iph) goto no_data;
	    sa << ((htons(iph->ip_off) & IP_OFFMASK) << 3);
	    if (iph->ip_off & htons(IP_MF))
		sa << '+';
	    break;
	  case W_SPORT:
	    if (tcph)
		sa << ntohs(tcph->th_sport);
	    else if (udph)
		sa << ntohs(udph->uh_sport);
	    else
		goto no_data;
	    break;
	  case W_DPORT:
	    if (tcph)
		sa << ntohs(tcph->th_dport);
	    else if (udph)
		sa << ntohs(udph->uh_dport);
	    else
		goto no_data;
	    break;
	  case W_IPID:
	    if (!iph) goto no_data;
	    sa << ntohs(iph->ip_id);
	    break;
	  case W_PROTO:
	    if (!iph) goto no_data;
	    switch (iph->ip_p) {
	      case IP_PROTO_TCP:	sa << 'T'; break;
	      case IP_PROTO_UDP:	sa << 'U'; break;
	      case IP_PROTO_ICMP:	sa << 'I'; break;
	      default:			sa << (int)(iph->ip_p); break;
	    }
	    break;
	  case W_TCP_SEQ:
	    if (!tcph)
		goto no_data;
	    sa << ntohl(tcph->th_seq);
	    break;
	  case W_TCP_ACK:
	    if (!tcph)
		goto no_data;
	    sa << ntohl(tcph->th_ack);
	    break;
	  case W_TCP_FLAGS: {
	      if (!tcph)
		  goto no_data;
	      int flags = tcph->th_flags;
	      for (int i = 0; i < 7; i++)
		  if (flags & (1 << i))
		      sa << FromIPSummaryDump::tcp_flags_word[i];
	      if (!flags)
		  sa << '.';
	      break;
	  }
	  case W_LENGTH: {
	      uint32_t len;
	      if (iph)
		  len = ntohs(iph->ip_len);
	      else
		  len = p->length();
	      sa << (len + EXTRA_LENGTH_ANNO(p));
	      break;
	  }
	  case W_PAYLOAD_LENGTH: {
	      uint32_t len;
	      if (iph) {
		  int32_t off = p->transport_header_offset();
		  if (tcph)
		      off += (tcph->th_off << 2);
		  else if (udph)
		      off += sizeof(click_udp);
		  else if (IP_FIRSTFRAG(iph) && (iph->ip_p == IP_PROTO_TCP || iph->ip_p == IP_PROTO_UDP))
		      off = ntohs(iph->ip_len) + p->network_header_offset();
		  len = ntohs(iph->ip_len) + p->network_header_offset() - off;
	      } else
		  len = p->length();
	      sa << (len + EXTRA_LENGTH_ANNO(p));
	      break;
	  }
	  case W_PAYLOAD: {
	      int32_t off;
	      uint32_t len;
	      if (iph) {
		  off = p->transport_header_offset();
		  if (tcph)
		      off += (tcph->th_off << 2);
		  else if (udph)
		      off += sizeof(click_udp);
		  len = ntohs(iph->ip_len) - off + p->network_header_offset();
		  if (len + off > p->length()) // EXTRA_LENGTH?
		      len = p->length() - off;
	      } else {
		  off = 0;
		  len = p->length();
	      }
	      String s = String::stable_string((const char *)(p->data() + off), len);
	      sa << cp_quote(s);
	      break;
	  }
	  case W_COUNT: {
	      uint32_t count = 1 + EXTRA_PACKETS_ANNO(p);
	      sa << count;
	      break;
	  }
	  case W_LINK: {
	      int link = PAINT_ANNO(p);
	      if (link == 0)
		  sa << 'L';
	      else if (link == 1)
		  sa << 'R';
	      else
		  sa << link;
	      break;
	  }
	  no_data:
	  default:
	    sa << '-';
	    break;
	}
    }
    sa << '\n';
    return true;
}

#undef BAD_IP
#undef BAD_TCP
#undef BAD_UDP

void
ToIPSummaryDump::write_packet(Packet *p, bool multipacket)
{
    if (multipacket && EXTRA_PACKETS_ANNO(p) > 0) {
	uint32_t count = 1 + EXTRA_PACKETS_ANNO(p);
	uint32_t total_len = p->length() + EXTRA_LENGTH_ANNO(p);
	uint32_t len = p->length();
	if (total_len < count * len)
	    total_len = count * len;
	SET_EXTRA_PACKETS_ANNO(p, 0);
	for (uint32_t i = count; i > 0; i--) {
	    uint32_t l = total_len / i;
	    SET_EXTRA_LENGTH_ANNO(p, l - len);
	    total_len -= l;
	    write_packet(p, false);
	}
    } else {
	_sa.clear();
	if (ascii_summary(p, _sa)) {
	    fwrite(_sa.data(), 1, _sa.length(), _f);
	    _output_count++;
	}
    }
}

void
ToIPSummaryDump::push(int, Packet *p)
{
    if (_active)
	write_packet(p, _multipacket);
    p->kill();
}

void
ToIPSummaryDump::run_scheduled()
{
    if (!_active)
	return;
    if (Packet *p = input(0).pull()) {
	write_packet(p, _multipacket);
	p->kill();
    } else if (!_signal)
	return;
    _task.fast_reschedule();
}

void
ToIPSummaryDump::write_string(const String &s)
{
    fwrite(s.data(), 1, s.length(), _f);
}

void
ToIPSummaryDump::flush_buffer()
{
    fflush(_f);
}

void
ToIPSummaryDump::add_handlers()
{
    if (input_is_pull(0))
	add_task_handlers(&_task);
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(userlevel FromIPSummaryDump)
EXPORT_ELEMENT(ToIPSummaryDump)
