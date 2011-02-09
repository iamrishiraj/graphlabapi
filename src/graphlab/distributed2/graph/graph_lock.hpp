#ifndef GRAPH_LOCK_HPP
#define GRAPH_LOCK_HPP
#include <list>
#include <boost/function.hpp>
#include <graphlab/scope/iscope.hpp>
#include <graphlab/parallel/deferred_rwlock.hpp>
#include <graphlab/rpc/dc_dist_object.hpp>
#include <graphlab/util/lazy_deque.hpp>
#include <graphlab/distributed2/graph/distributed_graph.hpp>

namespace graphlab {
// #define COMPILER_WRITE_BARRIER asm volatile("":::"memory")
#define COMPILER_WRITE_BARRIER
/**

  The locking implementation is basically two families of continuations.
  The first family is called the scopelock_continuation
  This family completes the lock of a scope.
  It iterates over the owners of the replicas of the vertex, and issue remote
  calls to acquire locks on them.
  
  The second family is called partiallock_continuation
  It completes the lock on local vertices.
  It iterates over the owned vertices within the scope of the vertex, acquiring
  locks.
  */
template <typename VertexData, typename EdgeData>
class graph_lock {
 public:
  graph_lock(distributed_control &dc,
            distributed_graph<VertexData, EdgeData> &dgraph):dgraph(dgraph), rmi(dc, this) {
    locks.resize(dgraph.owned_vertices().size());
  }

  /**
  Requests a lock on the scope surrounding globalvid.
  This globalvid must be owned by the current machine.
  When lock is complete the handler is called.
  */
  void scope_request(vertex_id_t globalvid,
                    boost::function<void(vertex_id_t)> handler,
                    scope_range::scope_range_enum scopetype) {
    // construct the scope lock parameters
    scopelock_cont_params sparams;
    sparams.globalvid = globalvid;
    
    boost::unordered_map<vertex_id_t, vertex_id_t>::const_iterator 
                              iter = dgraph.global2localvid.find(globalvid);
    assert(iter != dgraph.global2localvid.end());

    sparams.localvid = iter->second;
    sparams.nextowneridx = 0;
    sparams.handler = handler;
    sparams.scopetype = scopetype;
    scopelock_lock.lock();
    typename lazy_deque<scopelock_cont_params>::value_type* 
                    ptr = scopelock_continuation.push_anywhere(sparams);
    scopelock_lock.unlock();
    
    continue_scope_lock(ptr);
  }




        
  /**
  The parameters passed on to the scope lock continuation
  */
  struct scopelock_cont_params {
    vertex_id_t globalvid;
    vertex_id_t localvid;
    procid_t nextowneridx;
    scope_range::scope_range_enum scopetype;
    boost::function<void(vertex_id_t)> handler;
  };

  /**
  The parameters passed on to the partial lock continuation
  */  
  struct partiallock_cont_params {
    size_t inidx;       // the next in idx to consider in the in_edges/out_edges parallel iteration
    size_t outidx;      // the next out idx to consider in the in_edges/out_edges parallel iteration
    vertex_id_t localvid;
    procid_t srcproc;
    size_t src_tag;   // holds a pointer to the caller's scope lock continuation
    scope_range::scope_range_enum scopetype;
    bool curlocked;
    deferred_rwlock::request req;
  } __attribute__ ((aligned (8))) ;
  
 private:
  /// The distributed graph we are locking over
  const distributed_graph<VertexData, EdgeData> &dgraph;
  /// The RMI object
  dc_dist_object<graph_lock<VertexData, EdgeData> > rmi;
  
  /** the set of deferred locks local to this machine.
   * lock i corresponds to local vertex i. (by construction, 
   * the owned vertices always come first in the local store)
   */
  std::vector<deferred_rwlock> locks;

  mutex scopelock_lock;
  lazy_deque<scopelock_cont_params> scopelock_continuation;
  mutex partiallock_lock;
  lazy_deque<partiallock_cont_params> partiallock_continuation;


  /**
  partial lock request on the sending processor
  Requests a lock on the scope surrounding the vertex globalvid 
  on some destination processor. This call completes a lock which is
  purely local to the destination processor.
  This globalvid must be in the fragment of the destination processor, 
  (either owned or a ghost). When locks have been acquired the handler is
  called.
  */
  void partial_lock_request(procid_t destproc,
                           vertex_id_t globalvid,
                           scope_range::scope_range_enum scopetype,
                           size_t scope_continuation_ptr) {
    // here I issue to call to the remote machine, 
    // but I must pass on enough information so that I can call the handler
    // on reply
    // fast track it if the destination processor is local
    if (destproc == rmi.procid()) {
      // fast track! If it is local, just call it directly
      partial_lock_request_impl(destproc, 
                                globalvid, 
                                (size_t)scopetype, 
                                scope_continuation_ptr);
    }
    else {
      rmi.remote_call(destproc,
                      &graph_lock<VertexData,EdgeData>::partial_lock_request_impl,
                      rmi.procid(),
                      globalvid,
                      (size_t)scopetype,
                      scope_continuation_ptr);
    }
      
  }


  void partial_lock_completion(size_t scope_continuation_ptr) {
    typename lazy_deque<scopelock_cont_params>::value_type* 
          ptr = (typename lazy_deque<scopelock_cont_params>::value_type*)(scope_continuation_ptr);
    continue_scope_lock(ptr);
  }

  
  void continue_scope_lock(typename lazy_deque<scopelock_cont_params>::value_type* ptr) {
    // for convenience, lets take a reference to the params
    scopelock_cont_params& params = ptr->first;
    // check if I need to actually lock my replicas
    // do not need to if the adjacent lock type is no lock
    if (adjacent_vertex_lock_type(params.scopetype) != scope_range::NO_LOCK) {
      // the complicated case. I need to lock on my neighbors
      const std::vector<procid_t>& procs = dgraph.localvid_to_replicas(params.localvid);
  
      procid_t curidx = params.nextowneridx;  
      if (curidx < procs.size()) {
        ++params.nextowneridx;
        // process procs[curidx]
        // send a lock request, setting myself as the continuation
        partial_lock_request(procs[curidx],
                              params.globalvid,
                              params.scopetype,
                              (size_t)(ptr));
      }
      else {
        // I am done!
        params.handler(params.globalvid);
        // finish the continuation by erasing the lazy_deque entry
        scopelock_lock.lock();
        scopelock_continuation.erase(ptr);
        scopelock_lock.unlock();
      }
    }
    else {
      // this is the easy case. I only need to lock on myself.
      // first check if I am actually in fact. done.
      if (params.nextowneridx == 0) {
        // no I am not done
        ++params.nextowneridx;
        // issue a partial lock request to to the current machine
        partial_lock_request(rmi.procid(),
                             params.globalvid,
                             params.scopetype,
                             (size_t)(ptr));
      }
      else {
        // I am done!
        params.handler(params.globalvid);
        // finish the continuation by erasing the lazy_deque entry
        scopelock_lock.lock();
        scopelock_continuation.erase(ptr);
        scopelock_lock.unlock();
      }
    }
  }



  /**
  lock request implementation on the receiving processor
  */
  void partial_lock_request_impl(procid_t srcproc,
                         vertex_id_t globalvid,
                         size_t scopetype,
                         size_t src_tag) {
    // construct a partiallock_continuation
    logstream(LOG_DEBUG) << rmi.procid() << ": p-lock request from "<< srcproc << " : " << globalvid << std::endl;
    partiallock_cont_params plockparams;
    plockparams.srcproc = srcproc;
    plockparams.inidx = 0;
    plockparams.outidx = 0;
    plockparams.src_tag = src_tag;
    plockparams.curlocked = false;
    plockparams.scopetype = (scope_range::scope_range_enum)(scopetype);
    // if no lock needed on adjacent vertices
    // set inidx and outidx to infty
    if (adjacent_vertex_lock_type(plockparams.scopetype) == scope_range::NO_LOCK) {
      plockparams.inidx = (vertex_id_t)(-1);
      plockparams.outidx = (vertex_id_t)(-1);
    }
    
    boost::unordered_map<vertex_id_t, vertex_id_t>::const_iterator 
                              iter = dgraph.global2localvid.find(globalvid);
    assert(iter != dgraph.global2localvid.end());
    plockparams.localvid = iter->second;
    // put it inside the partiallock continuation
    partiallock_lock.lock();
    typename lazy_deque<partiallock_cont_params>::value_type* 
                        ptr = partiallock_continuation.push_anywhere(plockparams);
    partiallock_lock.unlock();
    //sqeeze the pointer into WORD_SIZE - 2 bits.
    // essentially assuming a minimum of 4 byte alignment
    assert(((size_t)(ptr) & 3) == 0);
    ptr->first.req.id = (size_t)(ptr) >> 2;
    continue_partial_lock(ptr);
  }


  void continue_partial_lock(typename lazy_deque<partiallock_cont_params>::value_type* ptr) {
    partiallock_cont_params& params = ptr->first;
    // perform a parallel iteration across in and out edges

    vertex_id_t curv = params.localvid;

    edge_list inedges = dgraph.localstore.in_edge_ids(curv);
    edge_list outedges = dgraph.localstore.out_edge_ids(curv);
    
    vertex_id_t inv  = (inedges.size() > params.inidx) ? dgraph.localstore.source(inedges[params.inidx]) : (vertex_id_t)(-1);
    vertex_id_t outv  = (outedges.size() > params.outidx) ? dgraph.localstore.target(outedges[params.outidx]) : (vertex_id_t)(-1);
    // iterate both in order and lock
    // include the current vertex in the iteration
    while (params.inidx < inedges.size() || params.outidx < outedges.size()) {
      if (!params.curlocked && curv < inv  && curv < outv) {
        // if curv is smaller than inv and outv
        params.curlocked = true;
        COMPILER_WRITE_BARRIER;
        // acquire current vertex
        if (dgraph.localvid_is_ghost(curv) == false &&
            issue_deferred_lock(curv, 
                                params.req, 
                                central_vertex_lock_type(params.scopetype)) == false) {
          return;
        }
      } else if (inv < outv) {
        ++params.inidx;
        COMPILER_WRITE_BARRIER;
        if (dgraph.localvid_is_ghost(inv) == false &&
            issue_deferred_lock(inv, 
                                params.req, 
                                adjacent_vertex_lock_type(params.scopetype)) == false) {
          return;
        }
        inv  = (inedges.size() > params.inidx) ? dgraph.localstore.source(inedges[params.inidx]) : (vertex_id_t)(-1);
      } else if (outv < inv) {
        ++params.outidx;
        COMPILER_WRITE_BARRIER;
        if (dgraph.localvid_is_ghost(outv) == false &&
            issue_deferred_lock(outv, 
                                params.req, 
                                adjacent_vertex_lock_type(params.scopetype)) == false) {
          return;
        }
        outv  = (outedges.size() > params.outidx) ? dgraph.localstore.target(outedges[params.outidx]) : (vertex_id_t)(-1);
      } else if (inv == outv){
        ++params.inidx; ++params.outidx;
        COMPILER_WRITE_BARRIER;
        if (dgraph.localvid_is_ghost(outv) == false &&
             issue_deferred_lock(outv, 
                                params.req, 
                                adjacent_vertex_lock_type(params.scopetype)) == false) {
          return;
        }
        inv  = (inedges.size() > params.inidx) ? dgraph.localstore.source(inedges[params.inidx]) : (vertex_id_t)(-1);
        outv  = (outedges.size() > params.outidx) ? dgraph.localstore.target(outedges[params.outidx]) : (vertex_id_t)(-1);
      }
    }
    // just in case we never got around to locking it
    if (!params.curlocked) {
      params.curlocked = true;
      COMPILER_WRITE_BARRIER;
      // acquire current vertex
      if (dgraph.localvid_is_ghost(curv) == false &&
          issue_deferred_lock(curv, 
                              params.req, 
                              central_vertex_lock_type(params.scopetype)) == false) {
        return;
      }
    }
    
    // if we get here, the lock is complete
    if (params.srcproc == rmi.procid()) {
      partial_lock_completion(params.src_tag);
    }
    else {
      rmi.remote_call(params.srcproc,
                      &graph_lock<VertexData, EdgeData>::partial_lock_completion,
                      params.src_tag);
    }
    partiallock_lock.lock();
    partiallock_continuation.erase(ptr);
    partiallock_lock.unlock();
  }
  
  /**
  Issue a deferred lock of type locktype 
  on lock[id] using req as the request handler.
  returns true if lock completes immediately.
  returns false otherwise.
  Calling this function requires great care as the  continuation params
  must be complete and valid at this point. 
  If this function return false, the caller must assume that the 
  continuation params may be invalid or even no longer exist.
  */
  bool issue_deferred_lock(size_t id, deferred_rwlock::request &req,
                           scope_range::lock_type_enum locktype) {
    deferred_rwlock::request* released = NULL;
    size_t numreleased = 0;
    switch(locktype) {
      case scope_range::READ_LOCK:
        logstream(LOG_DEBUG) << "read lock on " << dgraph.local2globalvid[id] << std::endl;
        numreleased = locks[id].readlock(&req, released);
        return complete_release(released, numreleased, &req);
      case scope_range::WRITE_LOCK:
        logstream(LOG_DEBUG) << "write lock on " << dgraph.local2globalvid[id] << std::endl;
        return locks[id].writelock(&req);
      default:
        return false;
    }
  }

  /**
    Starts the continuation on a collection of released requests
    beginning at the link list pointed to by "released" and for 
    "numreleased" entries.
    If watch is one of the released requests, this function
    will not call the continuation on the "watch" request, and will
    return true.
    Returns false otherwise.
  */
  bool complete_release(deferred_rwlock::request *released,
                        size_t numreleased,
                        deferred_rwlock::request* watch) {
    bool ret = false;
    for (size_t i = 0;i < numreleased; ++i) {
      if (released == watch) {
        ret = true;
      }
      else {
        // decompress the pointer
        size_t ptr = released->id;
        ptr = ptr << 2;
        typename lazy_deque<partiallock_cont_params>::value_type*
                      realptr = (typename lazy_deque<partiallock_cont_params>::value_type*)(ptr);
        continue_partial_lock(realptr);
      }
      released = (deferred_rwlock::request*)(released->next);
    }
    return ret;
  }
};

#undef COMPILER_WRITE_BARRIER
} // namespace graphlab
#endif
