/*
 * autoratefallback.{cc,hh} -- sets wifi txrate annotation on a packet
 * John Bicket
 *
 * Copyright (c) 2003 Massachusetts Institute of Technology
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
#include <click/confparse.hh>
#include <click/error.hh>
#include <click/glue.hh>
#include <click/packet_anno.hh>
#include <click/straccum.hh>
#include <clicknet/ether.h>
#include <elements/wifi/availablerates.hh>
#include "autoratefallback.hh"

CLICK_DECLS

#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))

AutoRateFallback::AutoRateFallback()
  : Element(2, 1),
    _stepup(0),
    _stepdown(0),
    _offset(0),
    _packet_size_threshold(0)
{
  MOD_INC_USE_COUNT;

  /* bleh */
  static unsigned char bcast_addr[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
  _bcast = EtherAddress(bcast_addr);
}

AutoRateFallback::~AutoRateFallback()
{
  MOD_DEC_USE_COUNT;
}

int
AutoRateFallback::configure(Vector<String> &conf, ErrorHandler *errh)
{
  _alt_rate = false;
  _active = true;
  int ret = cp_va_parse(conf, this, errh,
			cpKeywords, 
			"OFFSET", cpUnsigned, "offset", &_offset,
			"STEPUP", cpInteger, "0-100", &_stepup,
			"STEPDOWN", cpInteger, "0-100", &_stepdown,
			"RT", cpElement, "availablerates", &_rtable,
			"THRESHOLD", cpUnsigned, "xxx", &_packet_size_threshold,
			"ALT_RATE", cpBool, "xxx", &_alt_rate,
			"ACTIVE", cpBool, "xxx", &_active,
			cpEnd);
  return ret;
}

void
AutoRateFallback::process_feedback(Packet *p_in)
{
  if (!p_in) {
    return;
  }
  uint8_t *dst_ptr = (uint8_t *) p_in->data() + _offset;
  EtherAddress dst = EtherAddress(dst_ptr);
  int status = WIFI_TX_STATUS_ANNO(p_in);  
  int rate = WIFI_RATE_ANNO(p_in);

  struct timeval now;
  click_gettimeofday(&now);
  
  if (dst == _bcast) {
    /* don't record info for bcast packets */
    return;
  }

  if (0 == rate) {
    /* rate wasn't set */
    return;
  }
  if (!status && p_in->length() < _packet_size_threshold) {
    /* 
     * don't deal with short packets, 
     * since they can skew what rate we should be at 
     */
    return;
  }

  DstInfo *nfo = _neighbors.findp(dst);
  if (!nfo) {
    return;
  }


  if (nfo->pick_rate() != rate) {
    return;
  }

  if ((status & WIFI_FAILURE)) {
    click_chatter("%{element} packet failed %s status %d rate %d alt %d\n",
		  this,
		  dst.s().cc(),
		  status,
		  rate,
		  WIFI_ALT_RATE_ANNO(p_in)
		  );
  }


  if (status == 0) {
    nfo->_successes++;
    if (nfo->_successes > _stepup && 
	nfo->_current_index != nfo->_rates.size() - 1) {
      if (nfo->_rates.size()) {
	click_chatter("%{element} steping up for %s from %d to %d\n",
		      this,
		      nfo->_eth.s().cc(),
		      nfo->_rates[nfo->_current_index],
		      nfo->_rates[min(nfo->_rates.size() - 1, 
				      nfo->_current_index + 1)]);
      }
      nfo->_current_index = min(nfo->_current_index + 1, nfo->_rates.size() - 1);
      nfo->_successes = 0;
    }
  } else {
    if (nfo->_rates.size()) {
      click_chatter("%{element} steping down for %s from %d to %d\n",
		    this,
		    nfo->_eth.s().cc(),
		    nfo->_rates[nfo->_current_index],
		    nfo->_rates[max(0, nfo->_current_index - 1)]);
    }
    nfo->_successes = 0;
    nfo->_current_index = max(nfo->_current_index - 1, 0);
  }
  return;
}


void
AutoRateFallback::assign_rate(Packet *p_in)
{
  if (!p_in) {
    click_chatter("%{element} ah, !p_in\n",
		  this);
    return;
  }

  uint8_t *dst_ptr = (uint8_t *) p_in->data() + _offset;
  EtherAddress dst = EtherAddress(dst_ptr);
  SET_WIFI_FROM_CLICK(p_in);

  if (dst == _bcast) {
    Vector<int> rates = _rtable->lookup(_bcast);
    if (rates.size()) {
      SET_WIFI_RATE_ANNO(p_in, rates[0]);
    } else {
      SET_WIFI_RATE_ANNO(p_in, 2);
    }
    return;
  }
  DstInfo *nfo = _neighbors.findp(dst);
  if (!nfo || !nfo->_rates.size()) {
    _neighbors.insert(dst, DstInfo(dst));
    nfo = _neighbors.findp(dst);
    nfo->_rates = _rtable->lookup(dst);
    nfo->_successes = 0;

    nfo->_current_index = 0;
    click_chatter("%{element} initial rate for %s is %d\n",
		  this,
		  nfo->_eth.s().cc(),
		  nfo->_rates[nfo->_current_index]);
  }

  int rate = nfo->pick_rate();
  int alt_rate = (_alt_rate) ? nfo->pick_alt_rate() : 0;
  int max_retries = (_alt_rate) ? 4 : 8;
  int alt_max_retries = (_alt_rate) ? 4 : 0;
  SET_WIFI_RATE_ANNO(p_in, rate);
  SET_WIFI_MAX_RETRIES_ANNO(p_in, max_retries);


  SET_WIFI_ALT_RATE_ANNO(p_in, alt_rate);
  SET_WIFI_ALT_MAX_RETRIES_ANNO(p_in, alt_max_retries);
  return;
  
}


Packet *
AutoRateFallback::pull(int port)
{
  Packet *p = input(port).pull();
  if (p && _active) {
    assign_rate(p);
  }
  return p;
}

void
AutoRateFallback::push(int port, Packet *p_in)
{
  if (!p_in) {
    return;
  }
  if (port != 0) {
    if (_active) {
      process_feedback(p_in);
    }
    p_in->kill();
  } else {
    if (_active) {
      assign_rate(p_in);
    }
    output(port).push(p_in);
  }
}


String
AutoRateFallback::print_rates() 
{
    StringAccum sa;
  for (NIter iter = _neighbors.begin(); iter; iter++) {
    DstInfo nfo = iter.value();
    sa << nfo._eth << " ";
    sa << nfo._rates[nfo._current_index] << " ";
    sa << nfo._successes << "\n";
  }
  return sa.take_string();
}


enum {H_DEBUG, H_STEPUP, H_STEPDOWN, H_THRESHOLD, H_RATES, H_RESET, 
      H_OFFSET, H_ACTIVE, H_ALT_RATE};


static String
AutoRateFallback_read_param(Element *e, void *thunk)
{
  AutoRateFallback *td = (AutoRateFallback *)e;
  switch ((uintptr_t) thunk) {
  case H_DEBUG:
    return String(td->_debug) + "\n";
  case H_STEPDOWN:
    return String(td->_stepdown) + "\n";
  case H_STEPUP:
    return String(td->_stepup) + "\n";
  case H_THRESHOLD:
    return String(td->_packet_size_threshold) + "\n";
  case H_OFFSET:
    return String(td->_offset) + "\n";
  case H_RATES: {
    return td->print_rates();
  }
  case H_ACTIVE: 
    return String(td->_active) + "\n";
  case H_ALT_RATE: 
    return String(td->_alt_rate) + "\n";
  default:
    return String();
  }
}
static int 
AutoRateFallback_write_param(const String &in_s, Element *e, void *vparam,
		      ErrorHandler *errh)
{
  AutoRateFallback *f = (AutoRateFallback *)e;
  String s = cp_uncomment(in_s);
  switch((int)vparam) {
  case H_DEBUG: {
    bool debug;
    if (!cp_bool(s, &debug)) 
      return errh->error("debug parameter must be boolean");
    f->_debug = debug;
    break;
  }
  case H_STEPUP: {
    unsigned m;
    if (!cp_unsigned(s, &m)) 
      return errh->error("stepup parameter must be unsigned");
    f->_stepup = m;
    break;
  }
  case H_STEPDOWN: {
    unsigned m;
    if (!cp_unsigned(s, &m)) 
      return errh->error("stepdown parameter must be unsigned");
    f->_stepdown = m;
    break;
  }
  case H_THRESHOLD: {
    unsigned m;
    if (!cp_unsigned(s, &m)) 
      return errh->error("threshold parameter must be unsigned");
    f->_packet_size_threshold = m;
    break;
  }
  case H_OFFSET: {
    unsigned m;
    if (!cp_unsigned(s, &m)) 
      return errh->error("offset parameter must be unsigned");
    f->_offset = m;
    break;
  }
  case H_RESET: 
    f->_neighbors.clear();
    break;
 case H_ACTIVE: {
    bool active;
    if (!cp_bool(s, &active)) 
      return errh->error("active must be boolean");
    f->_active = active;
    break;
  }
 case H_ALT_RATE: {
    bool alt_rate;
    if (!cp_bool(s, &alt_rate)) 
      return errh->error("alt_rate must be boolean");
    f->_alt_rate = alt_rate;
    break;
  }
  }
  return 0;
}


void
AutoRateFallback::add_handlers()
{
  add_default_handlers(true);

  add_read_handler("debug", AutoRateFallback_read_param, (void *) H_DEBUG);
  add_read_handler("rates", AutoRateFallback_read_param, (void *) H_RATES);
  add_read_handler("threshold", AutoRateFallback_read_param, (void *) H_THRESHOLD);
  add_read_handler("stepup", AutoRateFallback_read_param, (void *) H_STEPUP);
  add_read_handler("stepdown", AutoRateFallback_read_param, (void *) H_STEPDOWN);
  add_read_handler("offset", AutoRateFallback_read_param, (void *) H_OFFSET);
  add_read_handler("active", AutoRateFallback_read_param, (void *) H_ACTIVE);
  add_read_handler("alt_rate", AutoRateFallback_read_param, (void *) H_ALT_RATE);

  add_write_handler("debug", AutoRateFallback_write_param, (void *) H_DEBUG);
  add_write_handler("threshold", AutoRateFallback_write_param, (void *) H_THRESHOLD);
  add_write_handler("stepup", AutoRateFallback_write_param, (void *) H_STEPUP);
  add_write_handler("stepdown", AutoRateFallback_write_param, (void *) H_STEPDOWN);
  add_write_handler("reset", AutoRateFallback_write_param, (void *) H_RESET);
  add_write_handler("offset", AutoRateFallback_write_param, (void *) H_OFFSET);
  add_write_handler("active", AutoRateFallback_write_param, (void *) H_ACTIVE);
  add_write_handler("alt_rate", AutoRateFallback_write_param, (void *) H_ALT_RATE);
}
// generate Vector template instance
#include <click/bighashmap.cc>
#include <click/dequeue.cc>
#if EXPLICIT_TEMPLATE_INSTANCES
template class HashMap<EtherAddress, AutoRateFallback::DstInfo>;
template class DEQueue<AutoRateFallback::tx_result>;
#endif
CLICK_ENDDECLS
EXPORT_ELEMENT(AutoRateFallback)

