// -*- c-basic-offset: 2; related-file-name: "../include/click/router.hh" -*-
/*
 * router.{cc,hh} -- a Click router configuration
 * Eddie Kohler
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology
 * Copyright (c) 2000 Mazu Networks, Inc.
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
#include <click/router.hh>
#include <click/routerthread.hh>
#include <click/bitvector.hh>
#include <click/error.hh>
#include <click/straccum.hh>
#include <click/elemfilter.hh>
#include <click/confparse.hh>
#include <click/subvector.hh>
#include <click/timer.hh>
#include <click/bighashmap_arena.hh>
#include <click/standard/errorelement.hh>
#include <click/standard/drivermanager.hh>
#include <cstdarg>
#ifdef CLICK_USERLEVEL
# include <unistd.h>
#endif
#ifdef CLICK_NS
#include "../elements/ns/fromsimdevice.hh"
#endif

CLICK_DECLS

static Router::Handler *globalh;
static int nglobalh;
static int globalh_cap;

Router::Router()
  : _preinitialized(false), _initialized(false), _initialize_attempted(false),
    _cleaned(false), _have_connections(false), _have_hookpidx(false),
    _handlers(0), _nhandlers(-1), _handlers_cap(0), _root_element(0),
    _arena_factory(new BigHashMap_ArenaFactory)
{
  _refcount = 0;
  _driver_runcount = 0;
  // RouterThreads will add themselves to _threads
  (void) new RouterThread(this);	// quiescent thread
  (void) new RouterThread(this);	// first normal thread

#ifdef CLICK_NS
  _clickinst = 0;
  _siminst = 0;
#endif
}

Router::~Router()
{
  if (_refcount > 0)
    click_chatter("deleting router while ref count > 0");
  
  // delete the ArenaFactory, which detaches the Arenas
  delete _arena_factory;

  // Clean up elements in reverse configuration order
  if (_initialized) {
    for (int ord = _elements.size() - 1; ord >= 0; ord--)
      _elements[ _configure_order[ord] ]->cleanup(Element::CLEANUP_ROUTER_INITIALIZED);
  } else if (!_cleaned) {
    assert(_configure_order.size() == 0);
    for (int i = _elements.size() - 1; i >= 0; i--)
      _elements[i]->cleanup(Element::CLEANUP_NO_ROUTER);
  }
  
  // Unschedule tasks and timers, delete threads
  for (int i = 0; i < _threads.size(); i++) {
    _threads[i]->unschedule_all_tasks();
    delete _threads[i];
  }
  _timer_list.unschedule_all();
  
  // Delete elements in reverse configuration order
  if (_configure_order.size())
    for (int ord = _elements.size() - 1; ord >= 0; ord--)
      delete _elements[ _configure_order[ord] ];
  else
    for (int i = 0; i < _elements.size(); i++)
      delete _elements[i];
  
  delete _root_element;
  delete[] _handlers;
}

void
Router::unuse()
{
  if (_refcount.dec_and_test())
    delete this;
}


// ACCESS

Element *
Router::find(const String &name, String prefix, ErrorHandler *errh) const
{
  while (1) {
    int got = -1;
    String n = prefix + name;
    for (int i = 0; i < _elements.size(); i++)
      if (_element_names[i] == n) {
	if (got >= 0) {
	  if (errh) errh->error("more than one element named `%s'", n.cc());
	  return 0;
	} else
	  got = i;
      }
    if (got >= 0)
      return _elements[got];
    if (!prefix)
      break;
    
    int slash = prefix.find_right('/', prefix.length() - 2);
    prefix = (slash >= 0 ? prefix.substring(0, slash + 1) : String());
  }
  
  if (errh) errh->error("no element named `%s'", String(name).cc());
  return 0;
}

Element *
Router::find(const String &name, Element *context, ErrorHandler *errh) const
{
  String prefix = ename(context->eindex());
  int slash = prefix.find_right('/');
  return find(name, (slash >= 0 ? prefix.substring(0, slash + 1) : String()), errh);
}

Element *
Router::element(int ei) const
{
  if (ei < 0 || ei >= nelements())
    return 0;
  else
    return _elements[ei];
}

const String &
Router::ename(int ei) const
{
  if (ei < 0 || ei >= nelements())
    return String::null_string();
  else
    return _element_names[ei];
}

const String &
Router::default_configuration_string(int ei) const
{
  if (ei < 0 || ei >= nelements())
    return String::null_string();
  else
    return _configurations[ei];
}

void
Router::set_default_configuration_string(int ei, const String &s)
{
  if (ei >= 0 && ei < nelements())
    _configurations[ei] = s;
}

const String &
Router::elandmark(int ei) const
{
  if (ei < 0 || ei >= nelements())
    return String::null_string();
  else
    return _element_landmarks[ei];
}

Element *
Router::root_element()
{
  if (!_root_element) {
    _root_element = new ErrorElement;
    _root_element->attach_router(this, -1);
  }
  return _root_element;
}


// CREATION 

int
Router::add_element(Element *e, const String &ename, const String &conf,
		    const String &landmark)
{
  // router now owns the element
  if (_preinitialized || !e) return -1;
  _elements.push_back(e);
  _element_names.push_back(ename);
  _element_landmarks.push_back(landmark);
  _configurations.push_back(conf);
  int i = _elements.size() - 1;
  e->attach_router(this, i);
  return i;
}

int
Router::add_connection(int from_idx, int from_port, int to_idx, int to_port)
{
  assert(from_idx >= 0 && from_port >= 0 && to_idx >= 0 && to_port >= 0);
  if (_preinitialized) return -1;
  Hookup hfrom(from_idx, from_port);
  Hookup hto(to_idx, to_port);
  // only add new connections
  for (int i = 0; i < _hookup_from.size(); i++)
    if (_hookup_from[i] == hfrom && _hookup_to[i] == hto)
      return 0;
  _hookup_from.push_back(hfrom);
  _hookup_to.push_back(hto);
  return 0;
}

void
Router::add_requirement(const String &r)
{
  assert(cp_is_word(r));
  _requirements.push_back(r);
}

void
Router::add_thread(RouterThread *rt)
{
  rt->set_thread_id(_threads.size() - 1);
  _threads.push_back(rt);
}

void
Router::remove_thread(RouterThread *rt)
{
  for (int i = 0; i < _threads.size(); i++)
    if (_threads[i] == rt) {
      _threads[i] = 0;
      return;
    }
}


// CHECKING HOOKUP

void
Router::remove_hookup(int c)
{
  _hookup_from[c] = _hookup_from.back();
  _hookup_from.pop_back();
  _hookup_to[c] = _hookup_to.back();
  _hookup_to.pop_back();
}

void
Router::hookup_error(const Hookup &h, bool is_from, const char *message,
		     ErrorHandler *errh)
{
  bool is_output = is_from;
  const char *kind = (is_output ? "output" : "input");
  element_lerror(errh, _elements[h.idx], message,
		 _elements[h.idx], kind, h.port);
}

int
Router::check_hookup_elements(ErrorHandler *errh)
{
  if (!errh) errh = ErrorHandler::default_handler();
  int before = errh->nerrors();
  
  // Check each hookup to ensure it connects valid elements
  for (int c = 0; c < _hookup_from.size(); c++) {
    Hookup &hfrom = _hookup_from[c];
    Hookup &hto = _hookup_to[c];
    int before = errh->nerrors();
    
    if (hfrom.idx < 0 || hfrom.idx >= nelements() || !_elements[hfrom.idx])
      errh->error("bad element number `%d'", hfrom.idx);
    if (hto.idx < 0 || hto.idx >= nelements() || !_elements[hto.idx])
      errh->error("bad element number `%d'", hto.idx);
    if (hfrom.port < 0)
      errh->error("bad port number `%d'", hfrom.port);
    if (hto.port < 0)
      errh->error("bad port number `%d'", hto.port);
    
    // remove the connection if there were errors
    if (errh->nerrors() != before) {
      remove_hookup(c);
      c--;
    }
  }
  
  return (errh->nerrors() == before ? 0 : -1);
}

void
Router::notify_hookup_range()
{
  // Count inputs and outputs, and notify elements how many they have
  Vector<int> nin(nelements(), -1);
  Vector<int> nout(nelements(), -1);
  for (int c = 0; c < _hookup_from.size(); c++) {
    if (_hookup_from[c].port > nout[_hookup_from[c].idx])
      nout[_hookup_from[c].idx] = _hookup_from[c].port;
    if (_hookup_to[c].port > nin[_hookup_to[c].idx])
      nin[_hookup_to[c].idx] = _hookup_to[c].port;
  }
  for (int f = 0; f < nelements(); f++) {
    _elements[f]->notify_ninputs(nin[f] + 1);
    _elements[f]->notify_noutputs(nout[f] + 1);
  }
}

void
Router::check_hookup_range(ErrorHandler *errh)
{
  // Check each hookup to ensure its port numbers are within range
  for (int c = 0; c < _hookup_from.size(); c++) {
    Hookup &hfrom = _hookup_from[c];
    Hookup &hto = _hookup_to[c];
    int before = errh->nerrors();
    
    if (hfrom.port >= _elements[hfrom.idx]->noutputs())
      hookup_error(hfrom, true, "`%{element}' has no %s %d", errh);
    if (hto.port >= _elements[hto.idx]->ninputs())
      hookup_error(hto, false, "`%{element}' has no %s %d", errh);
    
    // remove the connection if there were errors
    if (errh->nerrors() != before) {
      remove_hookup(c);
      c--;
    }
  }
}

void
Router::check_hookup_completeness(ErrorHandler *errh)
{
  Bitvector used_outputs(noutput_pidx(), false);
  Bitvector used_inputs(ninput_pidx(), false);
  
  // Check each hookup to ensure it doesn't reuse a port.
  // Completely duplicate connections never got into the Router
  for (int c = 0; c < _hookup_from.size(); c++) {
    Hookup &hfrom = _hookup_from[c];
    Hookup &hto = _hookup_to[c];
    int before = errh->nerrors();
    
    int from_pidx = _output_pidx[hfrom.idx] + hfrom.port;
    int to_pidx = _input_pidx[hto.idx] + hto.port;
    if (used_outputs[from_pidx]
	&& _elements[hfrom.idx]->output_is_push(hfrom.port))
      hookup_error(hfrom, true, "can't reuse `%{element}' push %s %d", errh);
    else if (used_inputs[to_pidx]
	     && _elements[hto.idx]->input_is_pull(hto.port))
      hookup_error(hto, false, "can't reuse `%{element}' pull %s %d", errh);
    
    // remove the connection if there were errors
    if (errh->nerrors() != before) {
      remove_hookup(c);
      c--;
    } else {
      used_outputs[from_pidx] = true;
      used_inputs[to_pidx] = true;
    }
  }

  // Check for unused inputs and outputs.
  for (int i = 0; i < ninput_pidx(); i++)
    if (!used_inputs[i]) {
      Hookup h(input_pidx_element(i), input_pidx_port(i));
      hookup_error(h, false, "`%{element}' %s %d unused", errh);
    }
  for (int i = 0; i < noutput_pidx(); i++)
    if (!used_outputs[i]) {
      Hookup h(output_pidx_element(i), output_pidx_port(i));
      hookup_error(h, true, "`%{element}' %s %d unused", errh);
    }
}


// PORT INDEXES

void
Router::make_pidxes()
{
  _input_pidx.clear();
  _input_pidx.push_back(0);
  _output_pidx.clear();
  _output_pidx.push_back(0);
  for (int i = 0; i < _elements.size(); i++) {
    Element *e = _elements[i];
    _input_pidx.push_back(_input_pidx.back() + e->ninputs());
    _output_pidx.push_back(_output_pidx.back() + e->noutputs());
    for (int j = 0; j < e->ninputs(); j++)
      _input_eidx.push_back(i);
    for (int j = 0; j < e->noutputs(); j++)
      _output_eidx.push_back(i);
  }
}

inline int
Router::input_pidx(const Hookup &h) const
{
  return _input_pidx[h.idx] + h.port;
}

inline int
Router::input_pidx_element(int pidx) const
{
  return _input_eidx[pidx];
}

inline int
Router::input_pidx_port(int pidx) const
{
  return pidx - _input_pidx[_input_eidx[pidx]];
}

inline int
Router::output_pidx(const Hookup &h) const
{
  return _output_pidx[h.idx] + h.port;
}

inline int
Router::output_pidx_element(int pidx) const
{
  return _output_eidx[pidx];
}

inline int
Router::output_pidx_port(int pidx) const
{
  return pidx - _output_pidx[_output_eidx[pidx]];
}

void
Router::make_hookpidxes()
{
  if (_have_hookpidx) return;
  for (int c = 0; c < _hookup_from.size(); c++) {
    int p1 = _output_pidx[_hookup_from[c].idx] + _hookup_from[c].port;
    _hookpidx_from.push_back(p1);
    int p2 = _input_pidx[_hookup_to[c].idx] + _hookup_to[c].port;
    _hookpidx_to.push_back(p2);
  }
}

// PROCESSING

int
Router::processing_error(const Hookup &hfrom, const Hookup &hto, bool aggie,
			 int processing_from, ErrorHandler *errh)
{
  const char *type1 = (processing_from == Element::VPUSH ? "push" : "pull");
  const char *type2 = (processing_from == Element::VPUSH ? "pull" : "push");
  if (!aggie)
    errh->error("`%{element}' %s output %d connected to `%{element}' %s input %d",
		_elements[hfrom.idx], type1, hfrom.port,
		_elements[hto.idx], type2, hto.port);
  else
    errh->error("agnostic `%{element}' in mixed context: %s input %d, %s output %d",
		_elements[hfrom.idx], type2, hto.port, type1, hfrom.port);
  return -1;
}

int
Router::check_push_and_pull(ErrorHandler *errh)
{
  if (!errh) errh = ErrorHandler::default_handler();
  
  // set up processing vectors
  Vector<int> input_pers(ninput_pidx(), 0);
  Vector<int> output_pers(noutput_pidx(), 0);
  for (int f = 0; f < nelements(); f++) {
    Subvector<int> i(input_pers, _input_pidx[f], _elements[f]->ninputs());
    Subvector<int> o(output_pers, _output_pidx[f], _elements[f]->noutputs());
    _elements[f]->processing_vector(i, o, errh);
  }
  
  // add fake connections for agnostics
  Vector<Hookup> hookup_from = _hookup_from;
  Vector<Hookup> hookup_to = _hookup_to;
  Bitvector bv;
  for (int i = 0; i < ninput_pidx(); i++)
    if (input_pers[i] == Element::VAGNOSTIC) {
      int ei = _input_eidx[i];
      int port = i - _input_pidx[ei];
      _elements[ei]->forward_flow(port, &bv);
      int opidx = _output_pidx[ei];
      for (int j = 0; j < bv.size(); j++)
	if (bv[j] && output_pers[opidx+j] == Element::VAGNOSTIC) {
	  hookup_from.push_back(Hookup(ei, j));
	  hookup_to.push_back(Hookup(ei, port));
	}
    }
  
  int before = errh->nerrors();
  int first_agnostic = _hookup_from.size();
  
  // spread personalities
  while (true) {
    
    bool changed = false;
    for (int c = 0; c < hookup_from.size(); c++) {
      if (hookup_from[c].idx < 0)
	continue;
      
      int offf = output_pidx(hookup_from[c]);
      int offt = input_pidx(hookup_to[c]);
      int pf = output_pers[offf];
      int pt = input_pers[offt];
      
      switch (pt) {
	
       case Element::VAGNOSTIC:
	if (pf != Element::VAGNOSTIC) {
	  input_pers[offt] = pf;
	  changed = true;
	}
	break;
	
       case Element::VPUSH:
       case Element::VPULL:
	if (pf == Element::VAGNOSTIC) {
	  output_pers[offf] = pt;
	  changed = true;
	} else if (pf != pt) {
	  processing_error(hookup_from[c], hookup_to[c], c >= first_agnostic,
			   pf, errh);
	  hookup_from[c].idx = -1;
	}
	break;
	
      }
    }
    
    if (!changed) break;
  }
  
  if (errh->nerrors() != before)
    return -1;

  for (int e = 0; e < nelements(); e++) {
    Subvector<int> i(input_pers, _input_pidx[e], _elements[e]->ninputs());
    Subvector<int> o(output_pers, _output_pidx[e], _elements[e]->noutputs());
    _elements[e]->initialize_ports(i, o);
  }
  return 0;
}


// SET CONNECTIONS

int
Router::element_lerror(ErrorHandler *errh, Element *e,
		       const char *format, ...) const
{
  va_list val;
  va_start(val, format);
  errh->verror(ErrorHandler::ERR_ERROR, e->landmark(), format, val);
  va_end(val);
  return -1;
}

void
Router::set_connections()
{
  // actually assign ports
  for (int c = 0; c < _hookup_from.size(); c++) {
    Hookup &hfrom = _hookup_from[c];
    Element *frome = _elements[hfrom.idx];
    Hookup &hto = _hookup_to[c];
    Element *toe = _elements[hto.idx];
    frome->connect_output(hfrom.port, toe, hto.port);
    toe->connect_input(hto.port, frome, hfrom.port);
  }
}


// DRIVER STOPPAGE

void
Router::set_driver_reservations(int x)
{
  _runcount_lock.acquire();
  _driver_runcount = x;
  _runcount_lock.release();
}

void
Router::adjust_driver_reservations(int x)
{
  _runcount_lock.acquire();
  _driver_runcount += x;
  _runcount_lock.release();
}

bool
Router::check_driver()
{
  _runcount_lock.acquire();

  if (_driver_runcount <= 0) {
    DriverManager *dm = (DriverManager *)(attachment("DriverManager"));
    if (dm && initialized())
      while (1) {
	int was_runcount = _driver_runcount;
	dm->handle_stopped_driver();
	if (_driver_runcount <= was_runcount || _driver_runcount > 0)
	  break;
      }
  }
  bool more = (_driver_runcount > 0);

  _runcount_lock.release();
  return more;
}


// FLOWS

int
Router::downstream_inputs(Element *first_element, int first_output,
			  ElementFilter *stop_filter, Bitvector &results)
{
  if (!_have_connections) return -1;
  make_hookpidxes();
  int nipidx = ninput_pidx();
  int nopidx = noutput_pidx();
  int nhook = _hookpidx_from.size();
  
  Bitvector old_results(nipidx, false);
  results.assign(nipidx, false);
  Bitvector diff, scratch;
  
  Bitvector outputs(nopidx, false);
  int first_eid = first_element->eindex(this);
  if (first_eid < 0) return -1;
  for (int i = 0; i < _elements[first_eid]->noutputs(); i++)
    if (first_output < 0 || first_output == i)
      outputs[_output_pidx[first_eid]+i] = true;
  
  while (true) {
    old_results = results;
    for (int i = 0; i < nhook; i++)
      if (outputs[_hookpidx_from[i]])
	results[_hookpidx_to[i]] = true;
    
    diff = results - old_results;
    if (diff.zero()) break;
    
    outputs.assign(nopidx, false);
    for (int i = 0; i < nipidx; i++)
      if (diff[i]) {
	int ei = _input_eidx[i];
	int port = input_pidx_port(i);
	if (!stop_filter || !stop_filter->match(_elements[ei], port)) {
	  _elements[ei]->forward_flow(port, &scratch);
	  outputs.or_at(scratch, _output_pidx[ei]);
	}
      }
  }
  
  return 0;
}

int
Router::downstream_elements(Element *first_element, int first_output,
			    ElementFilter *stop_filter,
			    Vector<Element *> &results)
{
  Bitvector bv;
  if (downstream_inputs(first_element, first_output, stop_filter, bv) < 0)
    return -1;
  int last_input_eidx = -1;
  for (int i = 0; i < ninput_pidx(); i++)
    if (bv[i] && _input_eidx[i] != last_input_eidx) {
      last_input_eidx = _input_eidx[i];
      results.push_back(_elements[last_input_eidx]);
    }
  return 0;
}

int
Router::downstream_elements(Element *first_element, int first_output,
			    Vector<Element *> &results)
{
  return downstream_elements(first_element, first_output, 0, results);
}

int
Router::downstream_elements(Element *first_element, Vector<Element *> &results)
{
  return downstream_elements(first_element, -1, 0, results);
}

int
Router::upstream_outputs(Element *first_element, int first_input,
			 ElementFilter *stop_filter, Bitvector &results)
{
  if (!_have_connections) return -1;
  make_hookpidxes();
  int nipidx = ninput_pidx();
  int nopidx = noutput_pidx();
  int nhook = _hookpidx_from.size();
  
  Bitvector old_results(nopidx, false);
  results.assign(nopidx, false);
  Bitvector diff, scratch;
  
  Bitvector inputs(nipidx, false);
  int first_eid = first_element->eindex(this);
  if (first_eid < 0) return -1;
  for (int i = 0; i < _elements[first_eid]->ninputs(); i++)
    if (first_input < 0 || first_input == i)
      inputs[_input_pidx[first_eid]+i] = true;
  
  while (true) {
    old_results = results;
    for (int i = 0; i < nhook; i++)
      if (inputs[_hookpidx_to[i]])
	results[_hookpidx_from[i]] = true;
    
    diff = results - old_results;
    if (diff.zero()) break;
    
    inputs.assign(nipidx, false);
    for (int i = 0; i < nopidx; i++)
      if (diff[i]) {
	int ei = _output_eidx[i];
	int port = output_pidx_port(i);
	if (!stop_filter || !stop_filter->match(_elements[ei], port)) {
	  _elements[ei]->backward_flow(port, &scratch);
	  inputs.or_at(scratch, _input_pidx[ei]);
	}
      }
  }
  
  return 0;
}

int
Router::upstream_elements(Element *first_element, int first_input,
			  ElementFilter *stop_filter,
			  Vector<Element *> &results)
{
  Bitvector bv;
  if (upstream_outputs(first_element, first_input, stop_filter, bv) < 0)
    return -1;
  int last_output_eidx = -1;
  for (int i = 0; i < noutput_pidx(); i++)
    if (bv[i] && _output_eidx[i] != last_output_eidx) {
      last_output_eidx = _output_eidx[i];
      results.push_back(_elements[last_output_eidx]);
    }
  return 0;
}

int
Router::upstream_elements(Element *first_element, int first_input,
			  Vector<Element *> &results)
{
  return upstream_elements(first_element, first_input, 0, results);
}

int
Router::upstream_elements(Element *first_element, Vector<Element *> &results)
{
  return upstream_elements(first_element, -1, 0, results);
}


// INITIALIZATION

String
Router::context_message(int element_no, const char *message) const
{
  Element *e = _elements[element_no];
  StringAccum sa;
  if (e->landmark())
    sa << e->landmark() << ": ";
  sa << message << " `" << e->declaration() << "':";
  return sa.take_string();
}

static const Vector<int> *configure_order_phase;

extern "C" {
static int
configure_order_compar(const void *athunk, const void *bthunk)
{
  const int *a = (const int *)athunk, *b = (const int *)bthunk;
  return (*configure_order_phase)[*a] - (*configure_order_phase)[*b];
}
}

void
Router::preinitialize()
{
#if CLICK_USERLEVEL
  FD_ZERO(&_read_select_fd_set);
  FD_ZERO(&_write_select_fd_set);
  _max_select_fd = -1;
  assert(!_selectors.size());
#endif

  // initialize handlers to empty
  initialize_handlers(false, false);
  
  // clear attachments
  _attachment_names.clear();
  _attachments.clear();

  _preinitialized = true;
}

void
Router::initialize_handlers(bool defaults, bool specifics)
{
  _ehandler_first_by_element.assign(nelements(), -1);
  _ehandler_to_handler.clear();
  _ehandler_next.clear();

  _handler_first_by_name.clear();

  _nhandlers = (defaults || specifics ? 0 : -1);

  if (defaults)
    for (int i = 0; i < _elements.size(); i++)
      _elements[i]->add_default_handlers(specifics);

  if (specifics)
    for (int i = 0; i < _elements.size(); i++)
      _elements[i]->add_handlers();
}

int
Router::initialize(ErrorHandler *errh, bool verbose_errors)
{
  assert(!_initialized);
  if (!_preinitialized)
    preinitialize();
  if (_initialize_attempted)
    return errh->error("second attempt to initialize router");
  _driver_runcount = 1;
  _initialize_attempted = true;

#if CLICK_DMALLOC
  char dmalloc_buf[12];
#endif
  
  if (check_hookup_elements(errh) < 0)
    return -1;
  
  Bitvector element_ok(nelements(), true);
  bool all_ok = true;

  notify_hookup_range();

  // set up configuration order
  _configure_order.assign(nelements(), 0);
  if (_configure_order.size()) {
    Vector<int> configure_phase(nelements(), 0);
    configure_order_phase = &configure_phase;
    for (int i = 0; i < _elements.size(); i++) {
      configure_phase[i] = _elements[i]->configure_phase();
      _configure_order[i] = i;
    }
    click_qsort(&_configure_order[0], _configure_order.size(), sizeof(int), configure_order_compar);
  }

  // Configure all elements in configure order. Remember the ones that failed
  Element::CleanupStage failure_stage = Element::CLEANUP_CONFIGURE_FAILED;
  Element::CleanupStage success_stage = Element::CLEANUP_CONFIGURED;
  Vector<String> conf;
  for (int ord = 0; ord < _elements.size(); ord++) {
    int i = _configure_order[ord];
#if CLICK_DMALLOC
    sprintf(dmalloc_buf, "c%d  ", i);
    CLICK_DMALLOC_REG(dmalloc_buf);
#endif
    ContextErrorHandler cerrh
      (errh, context_message(i, "While configuring"));
    ErrorHandler *errh1 = (verbose_errors ? &cerrh : errh);
    int before = errh1->nerrors();
    conf.clear();
    cp_argvec(_configurations[i], conf);
    if (_elements[i]->configure(conf, errh1) < 0) {
      element_ok[i] = all_ok = false;
      if (errh1->nerrors() == before)
	errh1->error("unspecified error");
    }
  }

#if CLICK_DMALLOC
  CLICK_DMALLOC_REG("iHoo");
#endif
  
  int before = errh->nerrors();
  check_hookup_range(errh);
  make_pidxes();
  check_push_and_pull(errh);
  check_hookup_completeness(errh);
  set_connections();
  _have_connections = true;
  if (before != errh->nerrors())
    all_ok = false;

  // Initialize elements if OK so far.
  if (all_ok) {
    failure_stage = Element::CLEANUP_INITIALIZE_FAILED;
    success_stage = Element::CLEANUP_INITIALIZED;
    initialize_handlers(true, true);
    for (int ord = 0; ord < _elements.size(); ord++) {
      int i = _configure_order[ord];
      if (element_ok[i]) {
#if CLICK_DMALLOC
	sprintf(dmalloc_buf, "i%d  ", i);
	CLICK_DMALLOC_REG(dmalloc_buf);
#endif
	ContextErrorHandler cerrh
	  (errh, context_message(i, "While initializing"));
	ErrorHandler *errh1 = (verbose_errors ? &cerrh : errh);
	int before = errh1->nerrors();
	if (_elements[i]->initialize(errh1) < 0) {
	  element_ok[i] = all_ok = false;
	  // don't report `unspecified error' for ErrorElements: keep error
	  // messages clean
	  if (errh1->nerrors() == before && !_elements[i]->cast("Error"))
	    errh1->error("unspecified error");
	}
      }
    }
  }

#if CLICK_DMALLOC
  CLICK_DMALLOC_REG("iXXX");
#endif
  
  // If there were errors, uninitialize any elements that we initialized
  // successfully and return -1 (error). Otherwise, we're all set!
  if (all_ok) {
    _initialized = true;
    return 0;
  } else {
    if (verbose_errors)
      errh->error("Router could not be initialized!");
    
    // Clean up elements
    for (int ord = _elements.size() - 1; ord >= 0; ord--) {
      int i = _configure_order[ord];
      _elements[i]->cleanup(element_ok[i] ? success_stage : failure_stage);
    }
    _cleaned = true;
    
    // Unschedule tasks and timers
    for (int i = 0; i < _threads.size(); i++)
      _threads[i]->unschedule_all_tasks();
    _timer_list.unschedule_all();

    // Remove element-specific handlers
    initialize_handlers(true, false);
    
    _driver_runcount = 0;
    return -1;
  }
}


// steal state

void
Router::take_state(Router *r, ErrorHandler *errh)
{
  assert(_initialized);
  for (int i = 0; i < _elements.size(); i++) {
    Element *e = _elements[i];
    Element *other = r->find(_element_names[e->eindex()]);
    if (other) {
      ContextErrorHandler cerrh
	(errh, context_message(i, "While hot-swapping state into"));
      e->take_state(other, &cerrh);
    }
  }
}


// HANDLERS

String
Router::Handler::unparse_name(Element *e, const String &hname)
{
  if (e)
    return e->id() + "." + hname;
  else
    return hname;
}

String
Router::Handler::unparse_name(Element *e) const
{
  return unparse_name(e, _name);
}

// 11.Jul.2000 - We had problems with handlers for medium-sized configurations
// (~400 elements): the Linux kernel would crash with a "kmalloc too large".
// The solution: Observe that most handlers are shared. For example, all
// `name' handlers can share a Handler structure, since they share the same
// read function, read thunk, write function (0), write thunk, and name. This
// introduced a bunch of structure to go from elements to handler indices and
// from handler names to handler indices, but it was worth it: it reduced the
// amount of space required by a normal set of Router::Handlers by about a
// factor of 100 -- there used to be 2998 Router::Handlers, now there are 30.
// (Some of this space is still not available for system use -- it gets used
// up by the indexing structures, particularly _ehandlers. Every element has
// its own list of "element handlers", even though most elements with element
// class C could share one such list. The space cost is about (48 bytes * # of
// elements) more or less. Detecting this sharing would be harder to
// implement.)

int
Router::find_ehandler(int eindex, const String &name, bool call_star) const
{
  int eh = _ehandler_first_by_element[eindex];
  int star_h = -1;
  while (eh >= 0) {
    int h = _ehandler_to_handler[eh];
    const String &hname = _handlers[h].name();
    if (hname == name)
      return eh;
    else if (hname.length() == 1 && hname[0] == '*')
      star_h = h;
    eh = _ehandler_next[eh];
  }
  if (call_star && star_h >= 0 && _handlers[star_h].writable()) {
    if (_handlers[star_h].call_write(name, element(eindex), ErrorHandler::default_handler()) >= 0)
      return find_ehandler(eindex, name, false);
  }
  return -1;
}

Router::Handler
Router::fetch_handler(const Router *r, int eindex, const String &name)
{
  if (eindex < 0) {		// global handler
    for (int i = 0; i < nglobalh; i++)
      if (globalh[i]._name == name)
	return globalh[i];
  } else if (r && eindex < r->nelements()) {
    int eh = r->find_ehandler(eindex, name);
    if (eh >= 0)
      return r->_handlers[r->_ehandler_to_handler[eh]];
  }
  return Handler(name);
}

void
Router::store_handler(int eindex, const Handler &to_store)
{
  // allow global handlers
  if (eindex < 0) {
    store_global_handler(to_store);
    return;
  }
  
  int old_eh = find_ehandler(eindex, to_store.name());
  if (old_eh >= 0)
    _handlers[_ehandler_to_handler[old_eh]]._use_count--;
  
  // find the offset in _name_handlers
  int name_index;
  for (name_index = 0;
       name_index < _handler_first_by_name.size();
       name_index++) {
    int h = _handler_first_by_name[name_index];
    if (_handlers[h]._name == to_store._name)
      break;
  }
  if (name_index == _handler_first_by_name.size())
    _handler_first_by_name.push_back(-1);

  // find a similar handler, if any exists
  int h = _handler_first_by_name[name_index];
  int blank_h = -1;
  int stored_h = -1;
  while (h >= 0 && stored_h < 0) {
    const Handler &han = _handlers[h];
    if (han.compatible(to_store))
      stored_h = h;
    else if (han._use_count == 0)
      blank_h = h;
    h = han._next_by_name;
  }

  // if none exists, assign this one to a blank spot
  if (stored_h < 0 && blank_h >= 0) {
    stored_h = blank_h;
    _handlers[stored_h] = to_store;
    _handlers[stored_h]._use_count = 0;
  }

  // if no blank spot, add a handler
  if (stored_h < 0) {
    if (_nhandlers >= _handlers_cap) {
      int new_cap = (_handlers_cap ? 2*_handlers_cap : 16);
      Handler *new_handlers = new Handler[new_cap];
      if (!new_handlers) {	// out of memory
	if (old_eh >= 0)	// restore use count
	  _handlers[_ehandler_to_handler[old_eh]]._use_count++;
	return;
      }
      for (int i = 0; i < _nhandlers; i++)
	new_handlers[i] = _handlers[i];
      delete[] _handlers;
      _handlers = new_handlers;
      _handlers_cap = new_cap;
    }
    stored_h = _nhandlers;
    _nhandlers++;
    _handlers[stored_h] = to_store;
    _handlers[stored_h]._use_count = 0;
    _handlers[stored_h]._next_by_name = _handler_first_by_name[name_index];
    _handler_first_by_name[name_index] = stored_h;
  }

  // point ehandler list at new handler
  if (old_eh >= 0)
    _ehandler_to_handler[old_eh] = stored_h;
  else {
    int new_eh = _ehandler_to_handler.size();
    _ehandler_to_handler.push_back(stored_h);
    _ehandler_next.push_back(_ehandler_first_by_element[eindex]);
    _ehandler_first_by_element[eindex] = new_eh;
  }

  // increment use count
  _handlers[stored_h]._use_count++;
}

void
Router::add_read_handler(int eindex, const String &name,
			 ReadHandler read, void *thunk)
{
  Handler to_add = fetch_handler(this, eindex, name);
  to_add._read = read;
  to_add._read_thunk = thunk;
  store_handler(eindex, to_add);
}

void
Router::add_write_handler(int eindex, const String &name,
			  WriteHandler write, void *thunk)
{
  Handler to_add = fetch_handler(this, eindex, name);
  to_add._write = write;
  to_add._write_thunk = thunk;
  store_handler(eindex, to_add);
}

int
Router::change_handler_flags(Element *e, const String &name,
			     uint32_t clear_flags, uint32_t set_flags)
{
  Router *r = (e ? e->router() : 0);
  int eindex = (e ? e->eindex() : -1);
  Handler to_add = fetch_handler(r, eindex, name);
  if (to_add._use_count > 0) {	// only modify existing handlers
    to_add._flags = (to_add._flags & ~clear_flags) | set_flags;
    if (r)
      r->store_handler(eindex, to_add);
    else
      store_global_handler(to_add);
    return 0;
  } else
    return -1;
}

int
Router::find_handler(Router *router, int eindex, const String &name)
{
  if (eindex < 0)
    return find_global_handler(name);
  else if (router && eindex < router->_ehandler_first_by_element.size()) {
    int eh = router->find_ehandler(eindex, name, true);
    return (eh >= 0 ? router->_ehandler_to_handler[eh] : -1);
  } else
    return -1;
}

void
Router::element_handlers(const Router *r, int eindex, Vector<int> &handlers)
{
  if (eindex < 0) {		// global handlers
    for (int i = 0; i < nglobal_handlers(); i++)
      handlers.push_back(FIRST_GLOBAL_HANDLER + i);
  } else if (r && eindex < r->_elements.size()) { // element handlers
    for (int eh = r->_ehandler_first_by_element[eindex];
	 eh >= 0;
	 eh = r->_ehandler_next[eh]) {
      int h = r->_ehandler_to_handler[eh];
      if (h >= 0)
	handlers.push_back(h);
    }
  }
}


// global handlers

void
Router::store_global_handler(const Handler &h)
{
  for (int i = 0; i < nglobalh; i++)
    if (globalh[i]._name == h._name) {
      globalh[i] = h;
      globalh[i]._use_count = 1;
      return;
    }
  
  if (nglobalh >= globalh_cap) {
    int n = (globalh_cap ? 2 * globalh_cap : 4);
    Router::Handler *hs = new Router::Handler[n];
    if (!hs)			// out of memory
      return;
    for (int i = 0; i < nglobalh; i++)
      hs[i] = globalh[i];
    delete[] globalh;
    globalh = hs;
    globalh_cap = n;
  }

  globalh[nglobalh] = h;
  globalh[nglobalh]._use_count = 1;
  nglobalh++;
}

void
Router::add_global_read_handler(const String &name,
				ReadHandler read, void *thunk)
{
  Handler to_add = fetch_handler(0, -1, name);
  to_add._read = read;
  to_add._read_thunk = thunk;
  store_global_handler(to_add);
}

void
Router::add_global_write_handler(const String &name,
				 WriteHandler write, void *thunk)
{
  Handler to_add = fetch_handler(0, -1, name);
  to_add._write = write;
  to_add._write_thunk = thunk;
  store_global_handler(to_add);
}

int
Router::find_global_handler(const String &name)
{
  for (int i = 0; i < nglobalh; i++)
    if (globalh[i]._name == name)
      return i + FIRST_GLOBAL_HANDLER;
  return -1;
}

int
Router::nglobal_handlers()
{
  return nglobalh;
}

void
Router::cleanup_global_handlers()
{
  delete[] globalh;
  globalh = 0;
  nglobalh = globalh_cap = 0;
}

const Router::Handler *
Router::handlerp(const Router *r, int hi)
{
  if (r && hi >= 0 && hi < r->_nhandlers)
    return &r->_handlers[hi];
  else if (hi >= FIRST_GLOBAL_HANDLER && hi < FIRST_GLOBAL_HANDLER + nglobalh)
    return &globalh[hi - FIRST_GLOBAL_HANDLER];
  else
    return 0;
}


// ATTACHMENTS

void *
Router::attachment(const String &name) const
{
  for (int i = 0; i < _attachments.size(); i++)
    if (_attachment_names[i] == name)
      return _attachments[i];
  return 0;
}

void *&
Router::force_attachment(const String &name)
{
  for (int i = 0; i < _attachments.size(); i++)
    if (_attachment_names[i] == name)
      return _attachments[i];
  _attachment_names.push_back(name);
  _attachments.push_back(0);
  return _attachments.back();
}

void *
Router::set_attachment(const String &name, void *value)
{
  for (int i = 0; i < _attachments.size(); i++)
    if (_attachment_names[i] == name) {
      void *v = _attachments[i];
      _attachments[i] = value;
      return v;
    }
  _attachment_names.push_back(name);
  _attachments.push_back(value);
  return 0;
}

ErrorHandler *
Router::chatter_channel(const String &name) const
{
  if (!name || name == "default")
    return ErrorHandler::default_handler();
  else if (void *v = attachment("ChatterChannel." + name))
    return (ErrorHandler *)v;
  else
    return ErrorHandler::silent_handler();
}


// SELECT

#if CLICK_USERLEVEL

int
Router::add_select(int fd, int element, int mask)
{
  if (fd < 0)
    return -1;
  assert(element >= 0 && element < nelements() && (mask & ~SELECT_RELEVANT) == 0);

  int our_si = -1;
  for (int si = 0; si < _selectors.size(); si++)
    if (_selectors[si].fd == fd) {
      // There is exactly one match per element-fd pair.
      assert(our_si == -1 || _selectors[si].element != element);
      if (_selectors[si].element == element)
	our_si = si;
      else if (_selectors[si].mask & mask)
	return -1;
    }

  // Add a new selector
  if (our_si >= 0)
    _selectors[our_si].mask |= mask;
  else
    _selectors.push_back(Selector(fd, element, mask));

  // Add 'mask' to the fd_sets
  if (mask & SELECT_READ)
    FD_SET(fd, &_read_select_fd_set);
  if (mask & SELECT_WRITE)
    FD_SET(fd, &_write_select_fd_set);
  if ((mask & (SELECT_READ | SELECT_WRITE)) && fd > _max_select_fd)
    _max_select_fd = fd;

  return 0;
}

int
Router::remove_select(int fd, int element, int mask)
{
  if (fd < 0)
    return -1;
  assert(element >= 0 && element < nelements() && (mask & ~SELECT_RELEVANT) == 0);

  // Exit early if no selector defined
  if ((!(mask & SELECT_READ) || !FD_ISSET(fd, &_read_select_fd_set))
      && (!(mask & SELECT_WRITE) || !FD_ISSET(fd, &_write_select_fd_set)))
    return 0;

  // Otherwise, search for selector
  for (int i = 0; i < _selectors.size(); i++) {
    Selector &s = _selectors[i];
    if (s.fd == fd && s.element == element) {
      mask &= s.mask;
      if (!mask)
	return -1;
      if (mask & SELECT_READ)
	FD_CLR(fd, &_read_select_fd_set);
      if (mask & SELECT_WRITE)
	FD_CLR(fd, &_write_select_fd_set);
      s.mask &= ~mask;
      if (!(s.mask & SELECT_RELEVANT)) {
	s = _selectors.back();
	_selectors.pop_back();
	if (_selectors.size() == 0)
	  _max_select_fd = -1;
      }
      return 0;
    }
  }
  
  return -1;
}


void
Router::run_selects(bool more_tasks)
{
  // Wait in select() for input or timer, and call relevant elements'
  // selected() methods.

  // Return early if there are no selectors and there are tasks to run.
  if (_selectors.size() == 0 && more_tasks)
    return;

  fd_set read_mask = _read_select_fd_set;
  fd_set write_mask = _write_select_fd_set;

  // never wait if anything is scheduled; otherwise, if no timers, block
  // indefinitely.
  struct timeval wait, *wait_ptr = &wait;
  bool timers = _timer_list.get_next_delay(&wait);
  if (more_tasks)
    wait.tv_sec = wait.tv_usec = 0;
  else if (!timers)
    wait_ptr = 0;
#ifdef CLICK_NS
  // Never block if we're running in the simulator.
  wait.tv_sec = wait.tv_usec = 0;
#endif

  int n = select(_max_select_fd + 1, &read_mask, &write_mask, (fd_set *)0, wait_ptr);
  
  if (n < 0 && errno != EINTR)
    perror("select");
  else if (n > 0) {
    for (int i = 0; i < _selectors.size(); i++) {
      const Selector &s = _selectors[i];
      if (((s.mask & SELECT_READ) && FD_ISSET(s.fd, &read_mask))
	  || ((s.mask & SELECT_WRITE) && FD_ISSET(s.fd, &write_mask)))
	_elements[s.element]->selected(s.fd);
    }
  }
}

#endif


// PRINTING

void
Router::unparse_requirements(StringAccum &sa, const String &indent) const
{
  // requirements
  if (_requirements.size())
    sa << indent << "require(" << cp_unargvec(_requirements) << ");\n\n";
}

void
Router::unparse_classes(StringAccum &, const String &) const
{
  // there are never any compound element classes here
}

void
Router::unparse_declarations(StringAccum &sa, const String &indent) const
{  
  // element classes
  Vector<String> conf;
  for (int i = 0; i < nelements(); i++) {
    sa << indent << _element_names[i] << " :: " << _elements[i]->class_name();
    String conf = (initialized() ? _elements[i]->configuration() : _configurations[i]);
    if (conf.length())
      sa << "(" << conf << ")";
    sa << ";\n";
  }
  
  if (nelements() > 0)
    sa << "\n";
}

void
Router::unparse_connections(StringAccum &sa, const String &indent) const
{  
  int nhookup = _hookup_from.size();
  Vector<int> next(nhookup, -1);
  Bitvector startchain(nhookup, true);
  for (int c = 0; c < nhookup; c++) {
    const Hookup &ht = _hookup_to[c];
    if (ht.port != 0) continue;
    int result = -1;
    for (int d = 0; d < nhookup; d++)
      if (d != c && _hookup_from[d] == ht) {
	result = d;
	if (_hookup_to[d].port == 0)
	  break;
      }
    if (result >= 0) {
      next[c] = result;
      startchain[result] = false;
    }
  }
  
  // print hookup
  Bitvector used(nhookup, false);
  bool done = false;
  while (!done) {
    // print chains
    for (int c = 0; c < nhookup; c++) {
      if (used[c] || !startchain[c]) continue;
      
      const Hookup &hf = _hookup_from[c];
      sa << indent << _element_names[hf.idx];
      if (hf.port)
	sa << " [" << hf.port << "]";
      
      int d = c;
      while (d >= 0 && !used[d]) {
	if (d == c) sa << " -> ";
	else sa << "\n" << indent << "    -> ";
	const Hookup &ht = _hookup_to[d];
	if (ht.port)
	  sa << "[" << ht.port << "] ";
	sa << _element_names[ht.idx];
	used[d] = true;
	d = next[d];
      }
      
      sa << ";\n";
    }

    // add new chains to include cycles
    done = true;
    for (int c = 0; c < nhookup && done; c++)
      if (!used[c])
	startchain[c] = true, done = false;
  }
}

void
Router::unparse(StringAccum &sa, const String &indent) const
{
  unparse_requirements(sa, indent);
  unparse_classes(sa, indent);
  unparse_declarations(sa, indent);
  unparse_connections(sa, indent);
}

String
Router::flat_configuration_string() const
{
  StringAccum sa;
  unparse(sa);
  return sa.take_string();
}

String
Router::element_list_string() const
{
  StringAccum sa;
  sa << nelements() << "\n";
  for (int i = 0; i < nelements(); i++)
    sa << _element_names[i] << "\n";
  return sa.take_string();
}

String
Router::element_ports_string(int ei) const
{
  if (ei < 0 || ei >= nelements())
    return String();
  
  StringAccum sa;
  Element *e = _elements[ei];
  Vector<int> pers(e->ninputs() + e->noutputs(), 0);
  Subvector<int> in_pers(pers, 0, e->ninputs());
  Subvector<int> out_pers(pers, e->ninputs(), e->noutputs());
  e->processing_vector(in_pers, out_pers, 0);

  sa << e->ninputs() << (e->ninputs() == 1 ? " input\n" : " inputs\n");
  for (int i = 0; i < e->ninputs(); i++) {
    // processing
    const char *persid = (e->input_is_pull(i) ? "pull" : "push");
    if (in_pers[i] == Element::VAGNOSTIC)
      sa << persid << "~\t";
    else
      sa << persid << "\t";
    
    // counts
#if CLICK_STATS >= 1
    if (e->input_is_pull(i) || CLICK_STATS >= 2)
      sa << e->input(i).npackets() << "\t";
    else
#endif
      sa << "-\t";
    
    // connections
    Hookup h(ei, i);
    const char *sep = "";
    for (int c = 0; c < _hookup_from.size(); c++)
      if (_hookup_to[c] == h) {
	sa << sep << _element_names[_hookup_from[c].idx]
	   << " [" << _hookup_from[c].port << "]";
	sep = ", ";
      }
    sa << "\n";
  }

  sa << e->noutputs() << (e->noutputs() == 1 ? " output\n" : " outputs\n");
  for (int i = 0; i < e->noutputs(); i++) {
    // processing
    const char *persid = (e->output_is_push(i) ? "push" : "pull");
    if (out_pers[i] == Element::VAGNOSTIC)
      sa << persid << "~\t";
    else
      sa << persid << "\t";
    
    // counts
#if CLICK_STATS >= 1
    if (e->output_is_push(i) || CLICK_STATS >= 2)
      sa << e->output(i).npackets() << "\t";
    else
#endif
      sa << "-\t";
    
    // hookup
    Hookup h(ei, i);
    const char *sep = "";
    for (int c = 0; c < _hookup_from.size(); c++)
      if (_hookup_from[c] == h) {
	sa << sep << "[" << _hookup_to[c].port << "] "
	   << _element_names[_hookup_to[c].idx];
	sep = ", ";
      }
    sa << "\n";
  }
  
  return sa.take_string();
}


#ifdef CLICK_NS

int
Router::sim_get_ifid(const char* ifname) {
  return simclick_sim_ifid_from_name(_siminst,ifname);
}

Vector<int> *
Router::sim_listenvec(int ifid) {
  for (int i = 0; i < _listenvecs.size(); i++)
    if (_listenvecs[i]->at(0) == ifid)
      return _listenvecs[i];
  Vector<int> *new_vec = new Vector<int>(1, ifid);
  if (new_vec)
    _listenvecs.push_back(new_vec);
  return new_vec;
}

int
Router::sim_listen(int ifid, int element) {
  if (Vector<int> *vec = sim_listenvec(ifid)) {
    for (int i = 1; i < vec->size(); i++)
      if ((*vec)[i] == element)
	return 0;
    vec->push_back(element);
    return 0;
  } else
    return -1;
}

int
Router::sim_write(int ifid,int ptype,const unsigned char* data,int len,
		     simclick_simpacketinfo* pinfo) {
  return simclick_sim_send_to_if(_siminst,_clickinst,ifid,ptype,data,len,pinfo);
}

int
Router::sim_if_ready(int ifid) {
  return simclick_sim_if_ready(_siminst,_clickinst,ifid);
}

int
Router::sim_incoming_packet(int ifid, int ptype, const unsigned char* data,
			    int len, simclick_simpacketinfo* pinfo) {
  if (Vector<int> *vec = sim_listenvec(ifid))
    for (int i = 1; i < vec->size(); i++)
      ((FromSimDevice *)element((*vec)[i]))->incoming_packet(ifid, ptype, data,
							     len, pinfo);
  return 0;
}
#endif // CLICK_NS

#if CLICK_USERLEVEL
// Vector template instance
# include <click/vector.cc>
#endif
CLICK_ENDDECLS
