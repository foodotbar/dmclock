// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/*
 * Copyright (C) 2016 Red Hat Inc.
 */


/*
 * The prop_heap does not seem to be necessary. The only thing it
 * would help with is quickly finding the mininum proportion/prioity
 * when an idle client became active
 */
// #define USE_PROP_HEAP

#pragma once


#include <assert.h>

#include <cmath>
#include <memory>
#include <map>
#include <deque>
#include <queue>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <iostream>
#include <limits>

#include <boost/variant.hpp>

#include "indirect_intrusive_heap.h"
#include "run_every.h"
#include "dmclock_util.h"
#include "dmclock_recs.h"

#ifdef PROFILE
#include "profile.h"
#endif

#include "gtest/gtest_prod.h"


namespace crimson {

  namespace dmclock {

    namespace c = crimson;

    constexpr double max_tag = std::numeric_limits<double>::max();
    constexpr double min_tag = std::numeric_limits<double>::lowest();

    struct ClientInfo {
      const double reservation;  // minimum
      const double weight;       // proportional
      const double limit;        // maximum

      // multiplicative inverses of above, which we use in calculations
      // and don't want to recalculate repeatedly
      const double reservation_inv;
      const double weight_inv;
      const double limit_inv;

      // order parameters -- min, "normal", max
      ClientInfo(double _reservation, double _weight, double _limit) :
	reservation(_reservation),
	weight(_weight),
	limit(_limit),
	reservation_inv(0.0 == reservation ? 0.0 : 1.0 / reservation),
	weight_inv(     0.0 == weight      ? 0.0 : 1.0 / weight),
	limit_inv(      0.0 == limit       ? 0.0 : 1.0 / limit)
      {
	// empty
      }


      friend std::ostream& operator<<(std::ostream& out,
				      const ClientInfo& client) {
	out <<
	  "{ r:" << client.reservation <<
	  " w:" << client.weight <<
	  " l:" << client.limit <<
	  " 1/r:" << client.reservation_inv <<
	  " 1/w:" << client.weight_inv <<
	  " 1/l:" << client.limit_inv <<
	  " }";
	return out;
      }
    }; // class ClientInfo


    std::ostream& operator<<(std::ostream& out,
			     const crimson::dmclock::ClientInfo& client);

    struct RequestTag {
      double reservation;
      double proportion;
      double limit;
      bool   ready; // true when within limit

      RequestTag(const RequestTag& prev_tag,
		 const ClientInfo& client,
		 const ReqParams& req_params,
		 const Time& time,
		 const double cost = 0.0) :
	reservation(cost + tag_calc(time,
				    prev_tag.reservation,
				    client.reservation_inv,
				    req_params.rho,
				    true)),
	proportion(tag_calc(time,
			    prev_tag.proportion,
			    client.weight_inv,
			    req_params.delta,
			    true)),
	limit(tag_calc(time,
		       prev_tag.limit,
		       client.limit_inv,
		       req_params.delta,
		       false)),
	ready(false)
      {
	assert(reservation < max_tag || proportion < max_tag);
      }

      RequestTag(double _res, double _prop, double _lim) :
	reservation(_res),
	proportion(_prop),
	limit(_lim),
	ready(false)
      {
	assert(reservation < max_tag || proportion < max_tag);
      }

      RequestTag(const RequestTag& other) :
	reservation(other.reservation),
	proportion(other.proportion),
	limit(other.limit),
	ready(other.ready)
      {
	// empty
      }

    private:

      static double tag_calc(const Time& time,
			     double prev,
			     double increment,
			     uint32_t dist_req_val,
			     bool extreme_is_high) {
	if (0.0 == increment) {
	  return extreme_is_high ? max_tag : min_tag;
	} else {
	  if (0 != dist_req_val) {
	    increment *= dist_req_val;
	  }
	  return std::max(time, prev + increment);
	}
      }

      friend std::ostream& operator<<(std::ostream& out,
				      const RequestTag& tag) {
	out <<
	  "{ r:" << format_time(tag.reservation) <<
	  " p:" << format_time(tag.proportion) <<
	  " l:" << format_time(tag.limit) << " }";
	return out;
      }
    }; // class RequestTag


    std::ostream& operator<<(std::ostream& out,
			     const crimson::dmclock::RequestTag& tag);


    // C is client identifier type, R is request type
    template<typename C, typename R>
    class PriorityQueueBase {
      FRIEND_TEST(dmclock_server, client_idle_erase);
      FRIEND_TEST(dmclock_server, test_iiv);

    public:

      using RequestRef = std::unique_ptr<R>;

    protected:

      using TimePoint = decltype(std::chrono::steady_clock::now());
      using Duration = std::chrono::milliseconds;
      using MarkPoint = std::pair<TimePoint,Counter>;

      enum class ReadyOption {ignore, lowers, raises};

      // forward decl for friend decls
      template<double RequestTag::*, ReadyOption, bool>
      struct ClientCompare;

      class ClientReq {
	friend PriorityQueueBase;

	RequestTag tag;
	C          client_id;
	RequestRef request;

      public:

	ClientReq(const RequestTag& _tag,
		  const C&          _client_id,
		  RequestRef&&      _request) :
	  tag(_tag),
	  client_id(_client_id),
	  request(std::move(_request))
	{
	  // empty
	}

	friend std::ostream& operator<<(std::ostream& out, const ClientReq& c) {
	  out << c.tag;
	  return out;
	}
      }; // class ClientReq


      class ClientRec {
	friend PriorityQueueBase<C,R>;

	C                     client;
	RequestTag            prev_tag;
	std::deque<ClientReq> requests;

	// amount added from the proportion tag as a result of
	// an idle client becoming unidle
	double                prop_delta = 0.0;

	c::IndIntruHeapData   reserv_heap_data;
	c::IndIntruHeapData   lim_heap_data;
	c::IndIntruHeapData   ready_heap_data;

	// index in look_up vector
	c::IndIntruHeapData   lookup_vector_data;
#if USE_PROP_HEAP
	c::IndIntruHeapData   prop_heap_data;
#endif

      public:

	ClientInfo            info;
	bool                  idle;
	Counter               last_tick;

	ClientRec(C _client,
		  const ClientInfo& _info,
		  Counter current_tick) :
	  client(_client),
	  prev_tag(0.0, 0.0, 0.0),
	  info(_info),
	  idle(true),
	  last_tick(current_tick)
	{
	  // empty
	}

	inline const RequestTag& get_req_tag() const {
	  return prev_tag;
	}

	inline void update_req_tag(const RequestTag& _prev,
				   const Counter& _tick) {
	  prev_tag = _prev;
	  last_tick = _tick;
	}

	inline double get_prev_prop_tag() const {
	  return prev_tag.proportion;
	}

	inline void set_prev_prop_tag(double value,
				      bool adjust_by_inc = false) {
	  prev_tag.proportion = value - (adjust_by_inc ? info.weight_inv : 0.0);
	}

	inline void add_request(const RequestTag& tag,
				const C&          client_id,
				RequestRef&&      request) {
	  requests.emplace_back(ClientReq(tag, client_id, std::move(request)));
	}

	inline const ClientReq& next_request() const {
	  return requests.front();
	}

	inline ClientReq& next_request() {
	  return requests.front();
	}

	inline void pop_request() {
	  requests.pop_front();
	}

	inline bool has_request() const {
	  return !requests.empty();
	}

	inline size_t request_count() const {
	  return requests.size();
	}

	// NB: because a deque is the underlying structure, this
	// operation might be expensive
	template<typename Collect>
	bool remove_by_req_filter_forwards(std::function<bool(const R&)> filter,
					   Collect& out) {
	  bool any_removed = false;
	  for (auto i = requests.begin();
	       i != requests.end();
	       /* no inc */) {
	    if (filter(*i->request)) {
	      any_removed = true;
	      out.push_back(*i->request);
	      i = requests.erase(i);
	    } else {
	      ++i;
	    }
	  }
	  return any_removed;
	}

	// NB: because a deque is the underlying structure, this
	// operation might be expensive
	template<typename Collect>
	bool remove_by_req_filter_backwards(std::function<bool(const R&)> filter,
					    Collect& out) {
	  bool any_removed = false;
	  for (auto i = --requests.end();
	       /* no cond */;
	       --i) {
	    if (filter(*i->request)) {
	      any_removed = true;
	      out.push_back(*i->request);
	      i = requests.erase(i);
	    }
	    if (requests.begin() == i) break;
	  }
	  return any_removed;
	}

	template<typename Collect>
	inline bool remove_by_req_filter(std::function<bool(const R&)> filter,
					 Collect& out,
					 bool visit_backwards) {
	  if (visit_backwards) {
	    return remove_by_req_filter_backwards(filter, out);
	  } else {
	    return remove_by_req_filter_forwards(filter, out);
	  }
	}
	
	friend std::ostream&
	operator<<(std::ostream& out,
		   const typename PriorityQueueBase<C,R>::ClientRec& e) {
	  out << "{ client:" << e.client << " top req: " <<
	    (e.has_request() ? e.next_request() : "none") << " }";
	  return out;
	}
      }; // class ClientRec


      using ClientRecRef = std::shared_ptr<ClientRec>;


      // Indirect Intrusive Vector Data Structure
      using IndIntruVectorData = size_t;

      template<typename I, typename T, IndIntruVectorData T::*index_info ,
	/*template < double RequestTag::*, ReadyOption, bool >*/ class Cmp>
      class IndIntruVector {
	friend PriorityQueueBase;

//	static_assert(
//	  std::is_same<T,typename std::pointer_traits<I>::element_type>::value,
//	  "class I must resolve to class T by indirection (pointer dereference)");
//
//	Cmp<&RequestTag::reservation,
//	    ReadyOption::ignore,
//	    false >              cmp_resv;
//
////	static_assert(
////	  std::is_same< bool,
////	  typename std::result_of<Cmp(const T&, const T&)>::type >::value,
////	  "class Cmp must define operator() to take two const T& and return a bool");
//
//	Cmp<&RequestTag::proportion,
//	    ReadyOption::raises,
//	    true >               cmp_ready;
//
//	Cmp<&RequestTag::limit,
//	    ReadyOption::lowers,
//	    false >              cmp_limit;
	Cmp                      cmp;
      protected:
	using index_t = IndIntruVectorData;
	std::vector<I>           data;
	index_t                  count;
	const index_t            top_default;
	bool                     resv_flag, ready_flag, limit_flag;

      public:
	index_t                  resv;
	index_t                  ready;
	index_t                  limit;


	IndIntruVector() :
	  count(0),
	  top_default(0),
	  resv(0),
	  ready(0),
	  limit(0)
	{
	  // empty
	}

	IndIntruVector(const IndIntruVector < I, T, index_info , Cmp > &other) :
	  count(other.count),
	  top_default(0),
	  resv(other.resv),
	  ready(other.ready),
	  limit(other.limit)
	{
	  for (uint i = 0; i < other.count; ++i) {
	    data.push_back(other.data[i]);
	  }
	}

	bool empty() const { return 0 == count; }

	size_t size() const { return count; }

	// slow call, use specialized version
	inline T& top(index_t IndIntruVector::*which_top) {
	  return *data[this->*which_top];
	}

	// slow call, use specialized version
	inline const T& top(index_t IndIntruVector::*which_top) const {
	  return *data[this->*which_top];
	}

	inline T& top_resv() {
	  return *data[resv];
	}

	// slow call, use specialized version
	inline const T& top_resv() const {
	  return *data[resv];
	}

	inline T& top_ready() {
	  return *data[ready];
	}

	// slow call, use specialized version
	inline const T& top_ready() const {
	  return *data[ready];
	}

	inline T& top_limit() {
	  return *data[limit];
	}

	// specialized tops
	inline const T& top_limit() const {
	  return *data[limit];
	}

	void push(I&& item) {
	  index_t i = count++;
	  index_of(item) = i;
	  data.emplace_back(std::move(item));
	  adjust();
	}

	void push(const I& item) {
	  I copy(item);
	  push(std::move(copy));
	}

	void pop(index_t IndIntruVector::*where) {
	  remove(this->*where);
	}

	void pop_resv() {
	  remove(resv);
	}

	void pop_ready() {
	  remove(ready);
	}

	void pop_limit() {
	  remove(limit);
	}

	void pop() {
	  remove(top_default);
	}

//	void adjust_resv(index_t _resv = 0) {
//	  resv = 0;
//	  for (index_t i = 1 ; i < count; i++){
//	    if (cmp_resv(*data[i], *data[resv])){
//	      resv = i;
//	    }
//	  }
//	}

	void adjust_resv() {
	  resv = 0;
	  for (index_t i = 1 ; i < count; i++){
	    cmp(&resv_flag, NULL, NULL, &(*data[i]), &(*data[resv]), NULL, NULL);
	    if (resv_flag){
	      resv = i;
	    }
	  }
	}



//	void adjust_ready_limit() {
//	  ready = limit = 0;
//	  T* elem;
//	  for (index_t i = 1 ; i < count; i++){
//	    elem = &(*data[i]);
//	    if (cmp_ready(*elem, *data[ready])){
//	      ready = i;
//	    }
//	    if (cmp_limit(*elem, *data[limit])){
//	      limit = i;
//	    }
//	  }
//	}

	void adjust_ready_limit() {
	  ready = limit = 0;
	  for (index_t i = 1 ; i < count; i++){
	    cmp(NULL, &ready_flag, &limit_flag, &(*data[i]), NULL, &(*data[ready]), &(*data[limit]));
	    if (ready_flag){
	      ready = i;
	    }
	    if (limit_flag){
		limit = i;
	    }
	  }
//	  for (index_t i = 1 ; i < count; i++){
//	    elem = &(*data[i]);
//	    if (cmp_ready(*elem, *data[ready])){
//	      ready = i;
//	    }
//	    if (cmp_limit(*elem, *data[limit])){
//	      limit = i;
//	    }
//	  }
	}

	// use native loop to update 3 tops in one sweep
//	void adjust() {
//	  resv = ready = limit = 0;
//	  T *elem;
//	  for (index_t i = 1 ; i < count; i++){
//	    elem = &(*data[i]);
//	    if (cmp_resv(*elem, *data[resv])){
//	      resv = i;
//	    }
//
//	    if (cmp_ready(*elem, *data[ready])){
//	      ready = i;
//	    }
//
//	    if (cmp_limit(*elem, *data[limit])){
//	      limit = i;
//	    }
//	  }
//	}

	void adjust() {
	  resv = ready = limit = 0;
	  for (index_t i = 1 ; i < count; i++){
	    cmp(&resv_flag, &ready_flag, &limit_flag, &(*data[i]), &(*data[resv]), &(*data[ready]), &(*data[limit]));
	    if (resv_flag){
	      resv = i;
	    }
	    if (ready_flag){
	      ready = i;
	    }
	    if (limit_flag){
	      limit = i;
	    }
	  }
	}

	void remove(const I& item) {
	  index_t i = (*item).*index_info;
	  if (i < count) {
	    remove(i);
	  }
	}

	friend std::ostream& operator<<(std::ostream& out, const IndIntruVector& h) {
	  bool first = true;
	  for (index_t i = 0 ; i < h.count ; i++){
	    if(!first){
	      out << ", ";
	    }
	    out << h.data[i] << " (" << i << ") ";
	    first = false;
	  }
	  return out;
	}

      protected:

	static index_t& index_of(I& item) {
	  return (*item).*index_info;
	}

	void remove(index_t i) {
	  std::swap(data[i], data[--count]);
	  index_of(data[i]) = i;
	  data.pop_back();
	  adjust();
	}

      }; // class IntruIndirectVector

      // IndIntruVector based ClientRec data-structure
//      using iiv = IndIntruVector<ClientRecRef,
//				 ClientRec,
//				 &ClientRec::lookup_vector_data,
//				 ClientCompare>;
      // forward declaration
      struct ClientCompareAtOnce;
      using iiv = IndIntruVector<ClientRecRef,
				 ClientRec,
				 &ClientRec::lookup_vector_data,
				 ClientCompareAtOnce>;
      iiv                        cl_vec;


    public:
      // when we try to get the next request, we'll be in one of three
      // situations -- we'll have one to return, have one that can
      // fire in the future, or not have any
      enum class NextReqType { returning, future, none };

      // specifies which queue next request will get popped from
      enum class HeapId { reservation, ready
#if USE_PROP_HEAP
	  , proportional
#endif
      };


      // this is returned from next_req to tell the caller the situation
      struct NextReq {
	NextReqType type;
	union {
	  HeapId    heap_id;
	  Time      when_ready;
	};
      };


      // a function that can be called to look up client information
      using ClientInfoFunc = std::function<ClientInfo(const C&)>;


      bool empty() const {
	DataGuard g(data_mtx);
	if (use_heap) {
	  return (resv_heap.empty() || !resv_heap.top().has_request());
	} else {
	  return (cl_vec.empty() || !cl_vec.top_resv().has_request());
	}
      }

      size_t client_count() const {
	DataGuard g(data_mtx);
	if (use_heap) {
	  return resv_heap.size();
	} else {
	  return cl_vec.size();
	}
      }

      size_t request_count() const {
	DataGuard g(data_mtx);
	size_t total = 0;
	if (use_heap) {
	  for (auto i = resv_heap.cbegin(); i != resv_heap.cend(); ++i) {
	    total += i->request_count();
	  }
	} else {
	  for ( auto i = 0 ; i < cl_vec.size() ; i++) {
	    total += cl_vec.data[i]->request_count();
	  }
	}
	return total;
      }


      bool remove_by_req_filter(std::function<bool(const R&)> filter,
				bool visit_backwards = false) {
	struct Sink {
	  void push_back(const R& v) {} // do nothing
	};
	static Sink my_sink;
	return remove_by_req_filter(filter, my_sink, visit_backwards);
      }

      
      template<typename Collect>
      bool remove_by_req_filter(std::function<bool(const R&)> filter,
				Collect& out,
				bool visit_backwards = false) {
	bool any_removed = false;
	DataGuard g(data_mtx);
	for (auto i : client_map) {
	  bool modified =
	    i.second->remove_by_req_filter(filter, out, visit_backwards);
	  if (modified) {
	    if (use_heap) {
	      resv_heap.adjust(*i.second);
	      limit_heap.adjust(*i.second);
	      ready_heap.adjust(*i.second);
#if USE_PROP_HEAP
	      resv_heap.adjust(*i.second);
#endif
	    } else {
	      cl_vec.adjust();
	    }
	    any_removed = true;
	  }
	}
	return any_removed;
      }


      void remove_by_client(const C& client) {
	struct Sink {
	  void push_back(const R& v) {}
	};
	static Sink my_sink;
	remove_by_client(client, my_sink);
      }


      // Collect must support calls to push_back(R), such as
      // std::list<R>.
      template<typename Collect>
      void remove_by_client(const C& client, Collect& out) {
	DataGuard g(data_mtx);

	auto i = client_map.find(client);

	if (i == client_map.end()) return;

	for (auto j = i->second->requests.begin();
	     j != i->second->requests.end();
	     ++j) {
	  out.push_back(*j->request);
	}

	i->second->requests.clear();

	if (use_heap) {
	  resv_heap.adjust(*i->second);
	  limit_heap.adjust(*i->second);
	  ready_heap.adjust(*i->second);
#if USE_PROP_HEAP
	  resv_heap.adjust(*i->second);
#endif
	} else {
	  cl_vec.adjust();
	}
      }


    protected:

      // The ClientCompare functor is essentially doing a precedes?
      // operator, returning true if and only if the first parameter
      // must precede the second parameter. If the second must precede
      // the first, or if they are equivalent, false should be
      // returned. The reason for this behavior is that it will be
      // called to test if two items are out of order and if true is
      // returned it will reverse the items. Therefore false is the
      // default return when it doesn't matter to prevent unnecessary
      // re-ordering.
      //
      // The template is supporting variations in sorting based on the
      // heap in question and allowing these variations to be handled
      // at compile-time.
      //
      // tag_field determines which tag is being used for comparison
      //
      // ready_opt determines how the ready flag influences the sort
      //
      // use_prop_delta determines whether the proportional delta is
      // added in for comparison
      template<double RequestTag::*tag_field,
	       ReadyOption ready_opt,
	       bool use_prop_delta>
      struct ClientCompare {
	bool operator()(const ClientRec& n1, const ClientRec& n2) const {
	  if (n1.has_request()) {
	    if (n2.has_request()) {
	      const auto& t1 = n1.next_request().tag;
	      const auto& t2 = n2.next_request().tag;
	      if (ReadyOption::ignore == ready_opt || t1.ready == t2.ready) {
		// if we don't care about ready or the ready values are the same
		if (use_prop_delta) {
		  return (t1.*tag_field + n1.prop_delta) <
		    (t2.*tag_field + n2.prop_delta);
		} else {
		  return t1.*tag_field < t2.*tag_field;
		}
	      } else if (ReadyOption::raises == ready_opt) {
		// use_ready == true && the ready fields are different
		return t1.ready;
	      } else {
		return t2.ready;
	      }
	    } else {
	      // n1 has request but n2 does not
	      return true;
	    }
	  } else if (n2.has_request()) {
	    // n2 has request but n1 does not
	    return false;
	  } else {
	    // both have none; keep stable w false
	    return false;
	  }
	}
      };

      // compare two clients at once
      struct ClientCompareAtOnce {

	void operator()(bool *resv_flag,
			  bool *ready_flag,
			  bool *limit_flag,
			  const ClientRec *n1,
			  const ClientRec *n2,
			  const ClientRec *n3,
			  const ClientRec *n4
			  ) const {

	  const RequestTag *t1 = NULL;
	  if (n1->has_request()){
	    const RequestTag& _t1 = n1->next_request().tag;
	    t1 = &_t1;
	  }

	  // reservation
	  if (resv_flag) {
	    if (t1) {
	      if (n2->has_request()) {
		//const RequestTag& t1 = n1->next_request().tag;
		const RequestTag& t2 = n2->next_request().tag;
		*resv_flag  = (t1->reservation < t2.reservation);
	      } else {
		// n1 has request but n2 does not
		*resv_flag  = true;
	      }
	    } else if (n2->has_request()) {
	      // n2 has request but n1 does not
	      *resv_flag  = false;
	    } else {
	      // both have none; keep stable w false
	      *resv_flag  = false;
	    }
	  }

	  // proportion
	  if (ready_flag) {
	    if (t1) {
	      if (n3->has_request()) {
		//const RequestTag& t1 = n1->next_request().tag;
		const RequestTag& t3 = n3->next_request().tag;
		*ready_flag = (t1->ready == t3.ready) ?
				 ((t1->proportion + n1->prop_delta) <
				     (t3.proportion + n3->prop_delta)) :
				 t1->ready;
	      } else {
		// n1 has request but n4 does not
		*ready_flag = true;
	      }
	    } else if (n3->has_request()) {
	      // n3 has request but n1 does not
	      *ready_flag = false;
	    } else {
	      // both have none; keep stable w false
	      *ready_flag = false;
	    }
	  }
	  // limit
	  if (limit_flag) {
	    if (t1) {
	      if (n4->has_request()) {
		//const RequestTag& t1 = n1->next_request().tag;
		const RequestTag& t4 = n4->next_request().tag;
		*limit_flag = (t1->ready == t4.ready) ? (t1->limit < t4.limit) : t4.ready;
	      } else {
		// n1 has request but n3 does not
		*limit_flag = true;
	      }
	    } else if (n4->has_request()) {
	      // n4 has request but n1 does not
	      *limit_flag = false;
	    } else {
	      // both have none; keep stable w false
	      *limit_flag = false;
	    }
	  }
	} // end operator(...)
      }; // end ClientCompareAtOnce

      ClientInfoFunc       client_info_f;

      mutable std::mutex data_mtx;
      using DataGuard = std::lock_guard<decltype(data_mtx)>;

      // stable mapping between client ids and client queues
      std::map<C,ClientRecRef> client_map;


      c::IndIntruHeap<ClientRecRef,
		      ClientRec,
		      &ClientRec::reserv_heap_data,
		      ClientCompare<&RequestTag::reservation,
				    ReadyOption::ignore,
				    false>> resv_heap;
#if USE_PROP_HEAP
      c::IndIntruHeap<ClientRecRef,
		      ClientRec,
		      &ClientRec::prop_heap_data,
		      ClientCompare<&RequestTag::proportion,
				    ReadyOption::ignore,
				    true>> prop_heap;
#endif
      c::IndIntruHeap<ClientRecRef,
		      ClientRec,
		      &ClientRec::lim_heap_data,
		      ClientCompare<&RequestTag::limit,
				    ReadyOption::lowers,
				    false>> limit_heap;
      c::IndIntruHeap<ClientRecRef,
		      ClientRec,
		      &ClientRec::ready_heap_data,
		      ClientCompare<&RequestTag::proportion,
				    ReadyOption::raises,
				    true>> ready_heap;



      // if all reservations are met and all other requests are under
      // limit, this will allow the request next in terms of
      // proportion to still get issued
      bool                       allow_limit_break;

      std::atomic_bool           finishing;

      // every request creates a tick
      Counter tick = 0;

      // performance data collection
      size_t                    reserv_sched_count = 0;
      size_t                    prop_sched_count = 0;
      size_t                    limit_break_sched_count = 0;

      Duration                  idle_age;
      Duration                  erase_age;
      Duration                  check_time;
      std::deque<MarkPoint>     clean_mark_points;

      // switching thresholds for IndIntruVector & IndIntruHeap
      size_t                    cutoff_for_iiv;
      size_t                    cutoff_for_iih;
      bool                      use_heap;

      // NB: All threads declared at end, so they're destructed first!
      std::unique_ptr<RunEvery> cleaning_job;


    public:
      // COMMON constructor that others feed into; we can accept three
      // different variations of durations
      template<typename Rep, typename Per>
      PriorityQueueBase(ClientInfoFunc _client_info_f,
			std::chrono::duration<Rep,Per> _idle_age,
			std::chrono::duration<Rep,Per> _erase_age,
			std::chrono::duration<Rep,Per> _check_time,
			bool _allow_limit_break,
			bool _use_heap):
	client_info_f(_client_info_f),
	allow_limit_break(_allow_limit_break),
	finishing(false),
	idle_age(std::chrono::duration_cast<Duration>(_idle_age)),
	erase_age(std::chrono::duration_cast<Duration>(_erase_age)),
	check_time(std::chrono::duration_cast<Duration>(_check_time)),
	cutoff_for_iiv(100),
	cutoff_for_iih(1000),
	use_heap(_use_heap)
      {
	assert(_erase_age >= _idle_age);
	assert(_check_time < _idle_age);
	cleaning_job =
	  std::unique_ptr<RunEvery>(
	    new RunEvery(check_time,
			 std::bind(&PriorityQueueBase::do_clean, this)));
      }


      ~PriorityQueueBase() {
	finishing = true;
      }


      // data_mtx must be held by caller
      void do_add_request(RequestRef&&     request,
			  const C&         client_id,
			  const ReqParams& req_params,
			  const Time       time,
			  const double     cost = 0.0) {
	++tick;

	// this pointer will help us create a reference to a shared
	// pointer, no matter which of two code-paths we take
	ClientRec* temp_client;
	
	auto client_it = client_map.find(client_id);
	if (client_map.end() != client_it) {
	  temp_client = &(*client_it->second); // address of obj of shared_ptr
	} else {
	  ClientInfo info = client_info_f(client_id);
	  ClientRecRef client_rec =
	    std::make_shared<ClientRec>(client_id, info, tick);

	  // make a decision which data-structure to use -- need more thoughts
#if 0
	  if (cutoff_for_iiv > client_map.size()) {
	    use_heap = true;
	  } else {
	    use_heap = false;
	  }
#endif

	  if (use_heap) {
	    resv_heap.push(client_rec);
#if USE_PROP_HEAP
	    prop_heap.push(client_rec);
#endif
	    limit_heap.push(client_rec);
	    ready_heap.push(client_rec);
	  } else {
	    cl_vec.push(client_rec);
	  }

	  client_map[client_id] = client_rec;
	  temp_client = &(*client_rec); // address of obj of shared_ptr
	}

	// for convenience, we'll create a reference to the shared pointer
	ClientRec& client = *temp_client;

	if (client.idle) {
	  // We need to do an adjustment so that idle clients compete
	  // fairly on proportional tags since those tags may have
	  // drifted from real-time. Either use the lowest existing
	  // proportion tag -- O(1) -- or the client with the lowest
	  // previous proportion tag -- O(n) where n = # clients.
	  //
	  // So we don't have to maintain a proportional queue that
	  // keeps the minimum on proportional tag alone (we're
	  // instead using a ready queue), we'll have to check each
	  // client.
	  //
	  // The alternative would be to maintain a proportional queue
	  // (define USE_PROP_TAG) and do an O(1) operation here.
	  double lowest_prop_tag = NaN; // mark unset value as NaN
	  for (auto const &c : client_map) {
	    // don't use ourselves (or anything else that might be
	    // listed as idle) since we're now in the map
	    if (!c.second->idle) {
	      // use either lowest proportion tag or previous proportion tag
	      if (c.second->has_request()) {
		double p = c.second->next_request().tag.proportion +
		  c.second->prop_delta;
		if (std::isnan(lowest_prop_tag) || p < lowest_prop_tag) {
		  lowest_prop_tag = p;
		}
	      }
	    }
	  }
	  if (!std::isnan(lowest_prop_tag)) {
	    client.prop_delta = lowest_prop_tag - time;
	  }
	  client.idle = false;
	} // if this client was idle

	RequestTag tag(client.get_req_tag(), client.info, req_params, time, cost);
	client.add_request(tag, client.client, std::move(request));

	// copy tag to previous tag for client
	client.update_req_tag(tag, tick);

	if (use_heap) {
	  resv_heap.adjust(client);
	  limit_heap.adjust(client);
	  ready_heap.adjust(client);
#if USE_PROP_HEAP
	  prop_heap.adjust(client);
#endif
	} else {
	  cl_vec.adjust();
	}
      } // add_request


      // data_mtx should be held when called; top of heap should have
      // a ready request
      template<typename C1, IndIntruHeapData ClientRec::*C2, typename C3>
      void pop_process_request(IndIntruHeap<C1, ClientRec, C2, C3>& heap,
			       std::function<void(const C& client,
						  RequestRef& request)> process) {
	// gain access to data
	ClientRec& top = heap.top();
	ClientReq& first = top.next_request();
	RequestRef request = std::move(first.request);

	// pop request and adjust heaps
	top.pop_request();
	resv_heap.demote(top);
	limit_heap.demote(top);
#if USE_PROP_HEAP
	prop_heap.demote(top);
#endif
	ready_heap.demote(top);

	// process
	process(top.client, request);
      } // pop_process_request

      // iiv version
      void pop_process_request(PhaseType phase ,
			       std::function<void(const C& client,
						  RequestRef& request)> process) {
	// gain access to data
	ClientRec* top;
	switch(phase){
	  case PhaseType::reservation:
	    top = &cl_vec.top_resv();
	    break;
	  case PhaseType::priority:
	    top = &cl_vec.top_ready();
	    break;
	  default:
	    assert(false);
	}
	//ClientRec& top = cl_vec.data[which_top];
	//ClientRec& top = top_f();
	ClientReq& first = top->next_request();
	RequestRef request = std::move(first.request);

	// pop request and adjust vector
	top->pop_request();
	cl_vec.adjust();

	// process
	process(top->client, request);
      } // pop_process_request

      // for debugging
      void display_queues(bool show_res = true,
			  bool show_lim = true,
			  bool show_ready = true,
			  bool show_prop = true) {
	auto filter = [](const ClientRecRef& e)->bool { return !e->handled; };
	if (use_heap){
	  if (show_res) {
	    resv_heap.display_sorted(std::cout << "RESER:", filter) << std::endl;
	  }
	  if (show_lim) {
	    limit_heap.display_sorted(std::cout << "LIMIT:", filter) << std::endl;
	  }
	  if (show_ready) {
	    ready_heap.display_sorted(std::cout << "READY:", filter) << std::endl;
	  }
#if USE_PROP_HEAP
	  if (show_prop) {
	    prop_heap.display_sorted(std::cout << "PROPO:", filter) << std::endl;
	  }
#endif
	} else {
	  std::cout << cl_vec << std::endl;
	}
      } // display_queues


      // data_mtx should be held when called
      void reduce_reservation_tags(ClientRec& client) {
	for (auto& r : client.requests) {
	  r.tag.reservation -= client.info.reservation_inv;
	}
	// don't forget to update previous tag
	client.prev_tag.reservation -= client.info.reservation_inv;
	if (use_heap) {
	  resv_heap.promote(client);
	} else {
	  //cl_vec.adjust();
	  cl_vec.adjust_resv();
	}
      }


      // data_mtx should be held when called
      void reduce_reservation_tags(const C& client_id) {
	auto client_it = client_map.find(client_id);

	// means the client was cleaned from map; should never happen
	// as long as cleaning times are long enough
	assert(client_map.end() != client_it);
	reduce_reservation_tags(*client_it->second);
      }


      // data_mtx should be held when called
      NextReq do_next_request(Time now) {
	NextReq result;

	// if resv_heap or client_vec is empty, all are empty (i.e., no active clients)
	if((use_heap && resv_heap.empty()) ||
	  (!use_heap && cl_vec.empty())) {
	  result.type = NextReqType::none;
	  return result;
	}

	// try constraint (reservation) based scheduling
	auto& reserv = use_heap ? resv_heap.top() : cl_vec.top_resv();
	if (reserv.has_request() &&
	    reserv.next_request().tag.reservation <= now) {
	  result.type = NextReqType::returning;
	  result.heap_id = HeapId::reservation;
	  return result;
	}

	// no existing reservations before now, so try weight-based
	// scheduling

	// all items that are within limit are eligible based on
	// priority
	auto limits = use_heap ? &limit_heap.top() : &cl_vec.top_limit();
	while (limits->has_request() &&
	       !limits->next_request().tag.ready &&
	       limits->next_request().tag.limit <= now) {
	  limits->next_request().tag.ready = true;
	  if (use_heap) {
	    ready_heap.promote(*limits);
	    limit_heap.demote(*limits);
	  } else {
	    cl_vec.adjust_ready_limit();
	  }
	  limits = use_heap ? &limit_heap.top() : &cl_vec.top_limit();
	}

	auto& readys = use_heap ? ready_heap.top() : cl_vec.top_ready();
	if (readys.has_request() &&
	    readys.next_request().tag.ready &&
	    readys.next_request().tag.proportion < max_tag) {
	  result.type = NextReqType::returning;
	  result.heap_id = HeapId::ready;
	  return result;
	}

	// if nothing is schedulable by reservation or
	// proportion/weight, and if we allow limit break, try to
	// schedule something with the lowest proportion tag or
	// alternatively lowest reservation tag.
	if (allow_limit_break) {
	  if (readys.has_request() &&
	      readys.next_request().tag.proportion < max_tag) {
	    result.type = NextReqType::returning;
	    result.heap_id = HeapId::ready;
	    return result;
	  } else if (reserv.has_request() &&
		     reserv.next_request().tag.reservation < max_tag) {
	    result.type = NextReqType::returning;
	    result.heap_id = HeapId::reservation;
	    return result;
	  }
	}

	// nothing scheduled; make sure we re-run when next
	// reservation item or next limited item comes up

	Time next_call = TimeMax;
	auto& r_top = use_heap ? resv_heap.top() : cl_vec.top_resv();
	if (r_top.has_request()) {
	  next_call =
	    min_not_0_time(next_call,
			   r_top.next_request().tag.reservation);
	}

	auto& l_top = use_heap ? limit_heap.top() : cl_vec.top_limit();
	if (l_top.has_request()) {
	  const auto& next = l_top.next_request();
	  assert(!next.tag.ready);
	  next_call = min_not_0_time(next_call, next.tag.limit);
	}

	if (next_call < TimeMax) {
	  result.type = NextReqType::future;
	  result.when_ready = next_call;
	  return result;
	} else {
	  result.type = NextReqType::none;
	  return result;
	}
      } // schedule_request

      // if possible is not zero and less than current then return it;
      // otherwise return current; the idea is we're trying to find
      // the minimal time but ignoring zero
      static inline const Time& min_not_0_time(const Time& current,
					       const Time& possible) {
	return TimeZero == possible ? current : std::min(current, possible);
      }


      /*
       * This is being called regularly by RunEvery. Every time it's
       * called it notes the time and delta counter (mark point) in a
       * deque. It also looks at the deque to find the most recent
       * mark point that is older than clean_age. It then walks the
       * map and delete all server entries that were last used before
       * that mark point.
       */
      void do_clean() {
	TimePoint now = std::chrono::steady_clock::now();
	DataGuard g(data_mtx);
	clean_mark_points.emplace_back(MarkPoint(now, tick));

	// first erase the super-old client records

	Counter erase_point = 0;
	auto point = clean_mark_points.front();
	while (point.first <= now - erase_age) {
	  erase_point = point.second;
	  clean_mark_points.pop_front();
	  point = clean_mark_points.front();
	}

	Counter idle_point = 0;
	for (auto i : clean_mark_points) {
	  if (i.first <= now - idle_age) {
	    idle_point = i.second;
	  } else {
	    break;
	  }
	}

	if (erase_point > 0 || idle_point > 0) {
	  for (auto i = client_map.begin(); i != client_map.end(); /* empty */) {
	    auto i2 = i++;
	    if (erase_point && i2->second->last_tick <= erase_point) {
	      client_map.erase(i2);
	      delete_from_heaps(i2->second);
	    } else if (idle_point && i2->second->last_tick <= idle_point) {
	      i2->second->idle = true;
	    }
	  } // for
	} // if
      } // do_clean


      // data_mtx must be held by caller
      template<IndIntruHeapData ClientRec::*C1,typename C2>
      void delete_from_heap(ClientRecRef& client,
			    c::IndIntruHeap<ClientRecRef,ClientRec,C1,C2>& heap) {
	auto i = heap.rfind(client);
	heap.remove(i);
      }


      // data_mtx must be held by caller
      void delete_from_heaps(ClientRecRef& client) {
	if (use_heap) {
	  delete_from_heap(client, resv_heap);
#if USE_PROP_HEAP
	  delete_from_heap(client, prop_heap);
#endif
	  delete_from_heap(client, limit_heap);
	  delete_from_heap(client, ready_heap);
	} else {
	  cl_vec.remove(client);
	}
      }
    }; // class PriorityQueueBase


    template<typename C, typename R>
    class PullPriorityQueue : public PriorityQueueBase<C,R> {
      using super = PriorityQueueBase<C,R>;

    public:

      // When a request is pulled, this is the return type.
      struct PullReq {
	struct Retn {
	  C                           client;
	  typename super::RequestRef  request;
	  PhaseType                   phase;
	};

	typename super::NextReqType   type;
	boost::variant<Retn,Time>     data;

	bool is_none() const { return type == super::NextReqType::none; }

	bool is_retn() const { return type == super::NextReqType::returning; }
	Retn& get_retn() {
	  return boost::get<Retn>(data);
	}

	bool is_future() const { return type == super::NextReqType::future; }
	Time getTime() const { return boost::get<Time>(data); }
      };


#ifdef PROFILE
      ProfileTimer<std::chrono::nanoseconds> pull_request_timer;
      ProfileTimer<std::chrono::nanoseconds> add_request_timer;
#endif

      template<typename Rep, typename Per>
      PullPriorityQueue(typename super::ClientInfoFunc _client_info_f,
			std::chrono::duration<Rep,Per> _idle_age,
			std::chrono::duration<Rep,Per> _erase_age,
			std::chrono::duration<Rep,Per> _check_time,
			bool _allow_limit_break = false,
			bool _use_heap = true) :
	super(_client_info_f,
	      _idle_age, _erase_age, _check_time,
	      _allow_limit_break,
	      _use_heap)
      {
	// empty
      }


      // pull convenience constructor
      PullPriorityQueue(typename super::ClientInfoFunc _client_info_f,
			bool _allow_limit_break = false,
			bool _use_heap = true) :
	PullPriorityQueue(_client_info_f,
			  std::chrono::minutes(10),
			  std::chrono::minutes(15),
			  std::chrono::minutes(6),
			  _allow_limit_break,
			  _use_heap)
      {
	// empty
      }


      inline void add_request(const R& request,
			      const C& client_id,
			      const ReqParams& req_params,
			      double addl_cost = 0.0) {
	add_request(typename super::RequestRef(new R(request)),
		    client_id,
		    req_params,
		    get_time(),
		    addl_cost);
      }


      inline void add_request(const R& request,
			      const C& client_id,
			      double addl_cost = 0.0) {
	static const ReqParams null_req_params;
	add_request(typename super::RequestRef(new R(request)),
		    client_id,
		    null_req_params,
		    get_time(),
		    addl_cost);
      }



      inline void add_request_time(const R& request,
				   const C& client_id,
				   const ReqParams& req_params,
				   const Time time,
				   double addl_cost = 0.0) {
	add_request(typename super::RequestRef(new R(request)),
		    client_id,
		    req_params,
		    time,
		    addl_cost);
      }


      inline void add_request(typename super::RequestRef&& request,
			      const C& client_id,
			      const ReqParams& req_params,
			      double addl_cost = 0.0) {
	add_request(request, req_params, client_id, get_time(), addl_cost);
      }


      inline void add_request(typename super::RequestRef&& request,
			      const C& client_id,
			      double addl_cost = 0.0) {
	static const ReqParams null_req_params;
	add_request(request, null_req_params, client_id, get_time(), addl_cost);
      }


      // this does the work; the versions above provide alternate interfaces
      void add_request(typename super::RequestRef&& request,
		       const C&                     client_id,
		       const ReqParams&             req_params,
		       const Time                   time,
		       double                       addl_cost = 0.0) {
	typename super::DataGuard g(this->data_mtx);
#ifdef PROFILE
	add_request_timer.start();
#endif
	super::do_add_request(std::move(request),
			      client_id,
			      req_params,
			      time,
			      addl_cost);
	// no call to schedule_request for pull version
#ifdef PROFILE
	add_request_timer.stop();
#endif
      }


      inline PullReq pull_request() {
	return pull_request(get_time());
      }


      PullReq pull_request(Time now) {
	PullReq result;
	typename super::DataGuard g(this->data_mtx);
#ifdef PROFILE
	pull_request_timer.start();
#endif

	typename super::NextReq next = super::do_next_request(now);
	result.type = next.type;
	switch(next.type) {
	case super::NextReqType::none:
	  return result;
	  break;
	case super::NextReqType::future:
	  result.data = next.when_ready;
	  return result;
	  break;
	case super::NextReqType::returning:
	  // to avoid nesting, break out and let code below handle this case
	  break;
	default:
	  assert(false);
	}

	// we'll only get here if we're returning an entry

	auto process_f =
	  [&] (PullReq& pull_result, PhaseType phase) ->
	  std::function<void(const C&,
			     typename super::RequestRef&)> {
	  return [&pull_result, phase](const C& client,
				       typename super::RequestRef& request) {
	    pull_result.data =
	    typename PullReq::Retn{client, std::move(request), phase};
	  };
	};

	switch(next.heap_id) {
	case super::HeapId::reservation:
	  if (super::use_heap) {
	    super::pop_process_request(this->resv_heap,
				     process_f(result, PhaseType::reservation));
	  } else {
	    super::pop_process_request(PhaseType::reservation,
				     process_f(result, PhaseType::reservation));
	  }
	  ++this->reserv_sched_count;
	  break;
	case super::HeapId::ready:
	{
	  if (super::use_heap) {
	    super::pop_process_request(this->ready_heap,
				     process_f(result, PhaseType::priority));
	  } else {
	    super::pop_process_request(PhaseType::priority,
				     process_f(result, PhaseType::priority));
	  }
	  auto& retn = boost::get<typename PullReq::Retn>(result.data);
	  super::reduce_reservation_tags(retn.client);
	  ++this->prop_sched_count;
	}
	break;
#if USE_PROP_HEAP
	case super::HeapId::proportional:
	{
	  super::pop_process_request(this->prop_heap,
				     process_f(result, PhaseType::priority));
	  auto& retn = boost::get<typename PullReq::Retn>(result.data);
	  super::reduce_reservation_tags(retn.client);
	  ++this->limit_break_sched_count;
	}
	break;
#endif
	default:
	  assert(false);
	}

#ifdef PROFILE
	pull_request_timer.stop();
#endif
	return result;
      } // pull_request


    protected:


      // data_mtx should be held when called; unfortunately this
      // function has to be repeated in both push & pull
      // specializations
      typename super::NextReq next_request() {
	return next_request(get_time());
      }
    }; // class PullPriorityQueue


    // PUSH version
    template<typename C, typename R>
    class PushPriorityQueue : public PriorityQueueBase<C,R> {

    protected:

      using super = PriorityQueueBase<C,R>;

    public:

      // a function to see whether the server can handle another request
      using CanHandleRequestFunc = std::function<bool(void)>;

      // a function to submit a request to the server; the second
      // parameter is a callback when it's completed
      using HandleRequestFunc =
	std::function<void(const C&,typename super::RequestRef,PhaseType)>;

    protected:

      CanHandleRequestFunc can_handle_f;
      HandleRequestFunc    handle_f;
      // for handling timed scheduling
      std::mutex  sched_ahead_mtx;
      std::condition_variable sched_ahead_cv;
      Time sched_ahead_when = TimeZero;

#ifdef PROFILE
    public:
      ProfileTimer<std::chrono::nanoseconds> add_request_timer;
      ProfileTimer<std::chrono::nanoseconds> request_complete_timer;
    protected:
#endif

      // NB: threads declared last, so constructed last and destructed first

      std::thread sched_ahead_thd;

    public:

      // push full constructor
      template<typename Rep, typename Per>
      PushPriorityQueue(typename super::ClientInfoFunc _client_info_f,
			CanHandleRequestFunc _can_handle_f,
			HandleRequestFunc _handle_f,
			std::chrono::duration<Rep,Per> _idle_age,
			std::chrono::duration<Rep,Per> _erase_age,
			std::chrono::duration<Rep,Per> _check_time,
			bool _allow_limit_break = false,
			bool _use_heap = true) :
	super(_client_info_f,
	      _idle_age, _erase_age, _check_time,
	      _allow_limit_break,
	      _use_heap)
      {
	can_handle_f = _can_handle_f;
	handle_f = _handle_f;
	sched_ahead_thd = std::thread(&PushPriorityQueue::run_sched_ahead, this);
      }


      // push convenience constructor
      PushPriorityQueue(typename super::ClientInfoFunc _client_info_f,
			CanHandleRequestFunc _can_handle_f,
			HandleRequestFunc _handle_f,
			bool _allow_limit_break = false,
			bool _use_heap = true) :
	PushPriorityQueue(_client_info_f,
			  _can_handle_f,
			  _handle_f,
			  std::chrono::minutes(10),
			  std::chrono::minutes(15),
			  std::chrono::minutes(6),
			  _allow_limit_break,
			  _use_heap)
      {
	// empty
      }


      ~PushPriorityQueue() {
	this->finishing = true;
	sched_ahead_cv.notify_one();
	sched_ahead_thd.join();
      }

    public:

      inline void add_request(const R& request,
			      const C& client_id,
			      const ReqParams& req_params,
			      double addl_cost = 0.0) {
	add_request(typename super::RequestRef(new R(request)),
		    client_id,
		    req_params,
		    get_time(),
		    addl_cost);
      }


      inline void add_request(typename super::RequestRef&& request,
			      const C& client_id,
			      const ReqParams& req_params,
			      double addl_cost = 0.0) {
	add_request(request, req_params, client_id, get_time(), addl_cost);
      }


      inline void add_request_time(const R& request,
				   const C& client_id,
				   const ReqParams& req_params,
				   const Time time,
				   double addl_cost = 0.0) {
	add_request(typename super::RequestRef(new R(request)),
		    client_id,
		    req_params,
		    time,
		    addl_cost);
      }


      void add_request(typename super::RequestRef&& request,
		       const C&         client_id,
		       const ReqParams& req_params,
		       const Time       time,
		       double           addl_cost = 0.0) {
	typename super::DataGuard g(this->data_mtx);
#ifdef PROFILE
	add_request_timer.start();
#endif
	super::do_add_request(std::move(request),
			      client_id,
			      req_params,
			      time,
			      addl_cost);
	schedule_request();
#ifdef PROFILE
	add_request_timer.stop();
#endif
      }


      void request_completed() {
	typename super::DataGuard g(this->data_mtx);
#ifdef PROFILE
	request_complete_timer.start();
#endif
	schedule_request();
#ifdef PROFILE
	request_complete_timer.stop();
#endif
      }

    protected:

      // data_mtx should be held when called; furthermore, the heap
      // should not be empty and the top element of the heap should
      // not be already handled
      template<typename C1, IndIntruHeapData super::ClientRec::*C2, typename C3>
      C submit_top_request(IndIntruHeap<C1,typename super::ClientRec,C2,C3>& heap,
			   PhaseType phase) {
	C client_result;
	super::pop_process_request(heap,
				   [this, phase, &client_result]
				   (const C& client,
				    typename super::RequestRef& request) {
				     client_result = client;
				     handle_f(client, std::move(request), phase);
				   });
	return client_result;
      }

      // iiv version
      C submit_top_request(PhaseType phase) {
	C client_result;

	super::pop_process_request( phase,
				   [this, phase, &client_result]
				   (const C& client,
				    typename super::RequestRef& request) {
				     client_result = client;
				     handle_f(client, std::move(request), phase);
				   });
	return client_result;
      }


      // data_mtx should be held when called
      void submit_request(typename super::HeapId heap_id) {
	C client;
	switch(heap_id) {
	case super::HeapId::reservation:
	  // don't need to note client
	  if (super::use_heap) {
	    (void) submit_top_request(this->resv_heap, PhaseType::reservation);
	  } else {
	    (void) submit_top_request(PhaseType::reservation);
	  }
	  // unlike the other two cases, we do not reduce reservation
	  // tags here
	  ++this->reserv_sched_count;
	  break;
	case super::HeapId::ready:
	  if (super::use_heap) {
	    client = submit_top_request(this->ready_heap, PhaseType::priority);
	  } else {
	    client = submit_top_request(PhaseType::priority);
	  }
	  super::reduce_reservation_tags(client);
	  ++this->prop_sched_count;
	  break;
#if USE_PROP_HEAP
	case super::HeapId::proportional:
	  client = submit_top_request(this->prop_heap, PhaseType::priority);
	  super::reduce_reservation_tags(client);
	  ++this->limit_break_sched_count;
	  break;
#endif
	default:
	  assert(false);
	}
      } // submit_request


      // data_mtx should be held when called; unfortunately this
      // function has to be repeated in both push & pull
      // specializations
      typename super::NextReq next_request() {
	return next_request(get_time());
      }


      // data_mtx should be held when called; overrides member
      // function in base class to add check for whether a request can
      // be pushed to the server
      typename super::NextReq next_request(Time now) {
	if (!can_handle_f()) {
	  typename super::NextReq result;
	  result.type = super::NextReqType::none;
	  return result;
	} else {
	  return super::do_next_request(now);
	}
      } // next_request


      // data_mtx should be held when called
      void schedule_request() {
	typename super::NextReq next_req = next_request();
	switch (next_req.type) {
	case super::NextReqType::none:
	  return;
	case super::NextReqType::future:
	  sched_at(next_req.when_ready);
	  break;
	case super::NextReqType::returning:
	  submit_request(next_req.heap_id);
	  break;
	default:
	  assert(false);
	}
      }


      // this is the thread that handles running schedule_request at
      // future times when nothing can be scheduled immediately
      void run_sched_ahead() {
	std::unique_lock<std::mutex> l(sched_ahead_mtx);

	while (!this->finishing) {
	  if (TimeZero == sched_ahead_when) {
	    sched_ahead_cv.wait(l);
	  } else {
	    Time now;
	    while (!this->finishing && (now = get_time()) < sched_ahead_when) {
	      long microseconds_l = long(1 + 1000000 * (sched_ahead_when - now));
	      auto microseconds = std::chrono::microseconds(microseconds_l);
	      sched_ahead_cv.wait_for(l, microseconds);
	    }
	    sched_ahead_when = TimeZero;
	    if (this->finishing) return;

	    l.unlock();
	    if (!this->finishing) {
	      typename super::DataGuard g(this->data_mtx);
	      schedule_request();
	    }
	    l.lock();
	  }
	}
      }


      void sched_at(Time when) {
	std::lock_guard<std::mutex> l(sched_ahead_mtx);
	if (TimeZero == sched_ahead_when || when < sched_ahead_when) {
	  sched_ahead_when = when;
	  sched_ahead_cv.notify_one();
	}
      }
    }; // class PushPriorityQueue

  } // namespace dmclock
} // namespace crimson
