#ifndef GRAPHLAB_GL3ENGINE_HPP
#define GRAPHLAB_GL3ENGINE_HPP
#include <vector>

#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/functional/hash.hpp>
#include <boost/type_traits.hpp>
#include <boost/utility/enable_if.hpp>

#include <graphlab/engine/execution_status.hpp>
#include <graphlab/options/graphlab_options.hpp>
#include <graphlab/parallel/qthread_tools.hpp>
#include <graphlab/util/tracepoint.hpp>
#include <graphlab/util/memory_info.hpp>
#include <graphlab/util/hashstream.hpp>
#include <graphlab/rpc/dc_dist_object.hpp>
#include <graphlab/engine/gl3task.hpp>
#include <graphlab/engine/gl3context.hpp>
#include <graphlab/graph/distributed_graph.hpp>
#include <graphlab/scheduler/ischeduler.hpp>
#include <graphlab/scheduler/scheduler_factory.hpp>
#include <graphlab/rpc/async_consensus.hpp>
#include <graphlab/util/inplace_lf_queue2.hpp>
#include <graphlab/parallel/qthread_tools.hpp>
#include <graphlab/util/empty.hpp>



#include <graphlab/macros_def.hpp>
#define REMOVE_CONST_REF(REF) typename boost::remove_const<typename boost::remove_reference<REF>::type>::type
#define FRESULT(F) REMOVE_CONST_REF(typename boost::function<typename boost::remove_pointer<F>::type>::result_type)

#define NORMALIZE_FUNCTION(F) typename boost::function<typename boost::remove_pointer<F>::type>

namespace graphlab {

template <typename GraphType, typename MessageType = empty>
class gl3engine {
 public:
  typedef GraphType graph_type;
  typedef typename GraphType::vertex_type vertex_type;
  typedef typename GraphType::edge_type edge_type;
  typedef typename GraphType::vertex_data_type vertex_data_type;
  typedef typename GraphType::edge_data_type edge_data_type;
  typedef typename GraphType::local_vertex_type    local_vertex_type;
  typedef typename GraphType::local_edge_type      local_edge_type;
  typedef typename GraphType::lvid_type            lvid_type;

  typedef gl3engine<GraphType> engine_type;
  typedef gl3context<engine_type> context_type;
  typedef gl3task_descriptor<GraphType, engine_type> task_descriptor_type;
  typedef MessageType message_type;

  typedef boost::function<void (context_type&,
                                vertex_type&,
                                const message_type&)> update_function_type;



  /// \internal \brief The base type of all schedulers
  typedef ischeduler<message_type> ischeduler_type;
  struct task {
    task* next;
    vertex_id_type vid;
    any param;
    unsigned char task_id;
    procid_t origin;
    size_t handle;
  };
 private:
  dc_dist_object<gl3engine> rmi;
  size_t num_vthreads;
  size_t ncpus;
  graph_type& graph;
  task_descriptor_type* task_types[256];
  bool finished;
  float engine_runtime;

  update_function_type active_function;
  std::vector<inplace_lf_queue2<task>* > local_tasks;
  std::vector<size_t> vdata_hash;
  atomic<size_t> programs_completed;
  atomic<size_t> tasks_completed;

  atomic<size_t> active_vthread_count;

  std::vector<mutex> worker_mutex;
  //! The scheduler
  ischeduler_type* scheduler_ptr;

  async_consensus* consensus;

  /**
   * \brief The vertex locks protect access to vertex specific
   * data-structures including
   * \ref graphlab::synchronous_engine::gather_accum
   * and \ref graphlab::synchronous_engine::messages.
   */
  std::vector<simple_spinlock> vlocks;


  /**
   * \brief The elocks protect individual edges during gather and
   * scatter.  Technically there is a potential race since gather
   * and scatter can modify edge values and can overlap.  The edge
   * lock ensures that only one gather or scatter occurs on an edge
   * at a time.
   */
  std::vector<simple_spinlock> elocks;



 public:
  gl3engine(distributed_control& dc, graph_type& graph,
            const graphlab_options& opts = graphlab_options()):
      rmi(dc, this), graph(graph){
    rmi.barrier();
    num_vthreads = 1000;
    ncpus = opts.get_ncpus();
    worker_mutex.resize(ncpus);
    // read the options
    std::vector<std::string> keys = opts.get_engine_args().get_option_keys();
    foreach(std::string opt, keys) {
      if (opt == "num_vthreads") {
        opts.get_engine_args().get_option("num_vthreads", num_vthreads);
        if (rmi.procid() == 0)
          logstream(LOG_EMPH) << "Engine Option: num_vthreads = "
                              << num_vthreads << std::endl;
      } else {
        logstream(LOG_FATAL) << "Unexpected Engine Option: " << opt << std::endl;
      }
    }

    // create the scheduler
    scheduler_ptr = scheduler_factory<message_type>::
                    new_scheduler(graph.num_local_vertices(),
                                  opts);
    // construct the termination consensus object
    // 1 thread
    consensus = new async_consensus(rmi.dc(), 1);
    // construct the locks
    vlocks.resize(graph.num_local_vertices());
    elocks.resize(graph.num_local_edges());
    vdata_hash.resize(graph.num_local_vertices());

    // construct the queues
    local_tasks.resize(ncpus);
    for (size_t i = 0;i < local_tasks.size(); ++i) {
      local_tasks[i] = new inplace_lf_queue2<task>();
    }

    // make default registrations
    task_types[GL3_BROADCAST_TASK_ID] = new broadcast_task_descriptor<GraphType, engine_type>();
  }


  template <typename T>
  struct create_map_reduce_task_impl{
    typedef boost::function<T (const vertex_type&,
                               edge_type&,
                               const vertex_type&)>  full_map_function_type;

    typedef boost::function<T (const vertex_type&)> basic_map_function_type;


    template <typename MapFn>
    static typename boost::enable_if_c<boost::is_same<MapFn, full_map_function_type>::value,
                    task_descriptor_type*>::type
      create(full_map_function_type mapfn,
             boost::function<void (T&, const T&)> combinefn,
             T zero = T()) {
      return new map_reduce_neighbors_task_descriptor<GraphType, engine_type, T>(mapfn, combinefn, zero);
    }

    static T simple_map_function_dispatch(boost::function<T (const vertex_type&)> mapfn,
                                   const vertex_type&,
                                   edge_type&,
                                   const vertex_type& other) {
      return mapfn(other);
    }

    template <typename MapFn>
    static typename boost::enable_if_c<boost::is_same<MapFn, basic_map_function_type>::value,
                    task_descriptor_type*>::type
      create(basic_map_function_type mapfn,
             boost::function<void (T&, const T&)> combinefn,
             T zero = T()) {
      return new map_reduce_neighbors_task_descriptor<GraphType, engine_type, T>(
          boost::bind(simple_map_function_dispatch, mapfn, _1, _2, _3), combinefn, zero);
    }
  };

  template <typename MapFn, typename CombineFn>
  void register_map_reduce(size_t id,
                           MapFn mapfn,
                           CombineFn combinefn,
                           FRESULT(MapFn) zero = FRESULT(MapFn) () ) {
    rmi.barrier();
    task_types[id] = create_map_reduce_task_impl<FRESULT(MapFn)>
        ::template create<NORMALIZE_FUNCTION(MapFn)>(mapfn, combinefn, zero);
  }


  void signal(vertex_id_type gvid,
              const message_type& message = message_type()) {
    rmi.barrier();
    internal_signal(graph.vertex(gvid), message);
    rmi.barrier();
  }

  void rpc_signal(vertex_id_type gvid,
                  const message_type& message) {
    internal_signal(graph.vertex(gvid), message);
    consensus->cancel();
  }


  void signal_all(const message_type& message = message_type(),
                  const std::string& order = "shuffle") {
    logstream(LOG_DEBUG) << rmi.procid() << ": Schedule All" << std::endl;
    // allocate a vector with all the local owned vertices
    // and schedule all of them.
    std::vector<vertex_id_type> vtxs;
    vtxs.reserve(graph.num_local_own_vertices());
    for(lvid_type lvid = 0;
        lvid < graph.get_local_graph().num_vertices();
        ++lvid) {
      if (graph.l_vertex(lvid).owner() == rmi.procid()) {
        vtxs.push_back(lvid);
      }
    }

    if(order == "shuffle") {
      graphlab::random::shuffle(vtxs.begin(), vtxs.end());
    }
    foreach(lvid_type lvid, vtxs) {
      scheduler_ptr->schedule(lvid, message);
    }
    rmi.barrier();
  } // end of schedule all


  void signal_vset(const vertex_set& vset,
                   const message_type& message = message_type(),
                   const std::string& order = "shuffle") {
    logstream(LOG_DEBUG) << rmi.procid() << ": Schedule All" << std::endl;
    // allocate a vector with all the local owned vertices
    // and schedule all of them.
    std::vector<vertex_id_type> vtxs;
    vtxs.reserve(graph.num_local_own_vertices());
    for(lvid_type lvid = 0;
        lvid < graph.get_local_graph().num_vertices();
        ++lvid) {
      if (graph.l_vertex(lvid).owner() == rmi.procid() &&
          vset.l_contains(lvid)) {
        vtxs.push_back(lvid);
      }
    }

    if(order == "shuffle") {
      graphlab::random::shuffle(vtxs.begin(), vtxs.end());
    }
    foreach(lvid_type lvid, vtxs) {
      scheduler_ptr->schedule(lvid, message);
    }
    rmi.barrier();
  }

  void internal_signal(const vertex_type& vtx,
                       const message_type& message = message_type()) {
    scheduler_ptr->schedule(vtx.local_id(), message);
  } // end of schedule


  struct future_combiner {
    any param;
    any* future_handle;
    procid_t count_down;
    unsigned char task_id;
    simple_spinlock lock;
  };

  any spawn_task(lvid_type lvid,
                 unsigned char task_id,
                 const any& task_param) {
    // create a future
    qthread_future<any> future;
    // create a count down for each mirror
    future_combiner combiner;
    combiner.count_down = graph.l_vertex(lvid).num_mirrors() + 1;
    combiner.future_handle = &(future.get());
    combiner.task_id = task_id;
    combiner.param = task_param;
    local_vertex_type lvertex(graph.l_vertex(lvid));

    /*
    logstream(LOG_EMPH) << "Creating Subtask type "<< (int)task_id
                        << " on vertex " << graph.l_vertex(lvid).global_id() << " Handle " << combiner
                        << "\n";
    */
    size_t newhash = get_vertex_data_hash_lvid(lvid);
    conditional_serialize<vertex_data_type> cs;
    if (newhash != vdata_hash[lvid]) {
      cs.hasval = true;
      cs.val = lvertex.data();
      vdata_hash[lvid] = newhash;
    }
    rmi.remote_call(lvertex.mirrors().begin(), lvertex.mirrors().end(),
                    &engine_type::rpc_receive_task,
                    task_id,
                    lvertex.global_id(),
                    cs,
                    task_param,
                    rmi.procid(),
                    reinterpret_cast<size_t>(&combiner));

    // we execute my own subtasks inplace
    /*
    logstream(LOG_EMPH) << "Execing subtask type " << (int)(task_id)
                        << " on vertex " << lvertex.global_id() << "\n";
    */
    // unlock the task locks so we don't dead-lock with the subtask
    vlocks[lvid].unlock();
    any ret = task_types[task_id]->exec(graph, lvertex.global_id(), task_param,
                                        this, vlocks, elocks);
    task_reply(&combiner, ret);
    future.wait();

    vlocks[lvid].lock();
    return future.get();
  }
  void task_reply_rpc(size_t handle, any& val) {
    future_combiner* combiner = reinterpret_cast<future_combiner*>(handle);
    task_reply(combiner, val);
  }

  void task_reply(future_combiner* combiner, any& val) {
    //logstream(LOG_EMPH) << "some subtask completion on handle " << combiner << "\n";
    combiner->lock.lock();
    ASSERT_GT(combiner->count_down, 0);
    combiner->count_down--;
    if (!combiner->future_handle->empty()) {
      task_types[combiner->task_id]->combine((*combiner->future_handle),
                                             val, combiner->param);
    } else {
      (*combiner->future_handle) = val;
    }
    if (combiner->count_down == 0) {
      combiner->lock.unlock();
      qthread_future<any>::signal(combiner->future_handle);
    } else {
      combiner->lock.unlock();
    }
  }

  void rpc_receive_task(unsigned char task_id,
                        vertex_id_type vid,
                        conditional_serialize<vertex_data_type>& vdata,
                        const any& param,
                        procid_t caller,
                        size_t handle) {
    //logstream(LOG_EMPH) << "Receiving subtask on handle " << (void*)(handle) << "\n";
    lvid_type lvid = graph.local_vid(vid);
    if (vdata.hasval) {
      vlocks[lvid].lock();
      graph.l_vertex(lvid).data() = vdata.val;
      vlocks[lvid].unlock();
    }

    task* t = new task;
    t->origin = caller;
    t->param = param;
    t->task_id = task_id;
    t->handle = handle;
    t->vid = vid;
    local_tasks[lvid % ncpus]->enqueue(t);
    consensus->cancel();
  }

  size_t get_vertex_data_hash_lvid(lvid_type lvid) {
    return boost::hash_value(graph.l_vertex(lvid).data());
  }

  void sync_vdata(vertex_id_type vid,
                  vertex_data_type& vdata) {
    vertex_type vertex(graph.vertex(vid));
    lvid_type lvid = vertex.local_id();
    vlocks[lvid].lock();
    vertex.data() = vdata;
    vlocks[lvid].unlock();
  }

  void vthread_start() {
    context_type context;
    context.engine = this;
    while(!finished) {
      exec_subtasks(qthread_id() % ncpus);
      lvid_type lvid;
      message_type msg;
      sched_status::status_enum stat =
          scheduler_ptr->get_next(qthread_worker(NULL), lvid, msg);
      // get a task from the scheduler
      // if no task... quit
      if (stat == sched_status::EMPTY) break;
      // otherwise run the task
      //
      // lock the vertex
      // if this is not the master, we forward it
      if (!graph.l_is_master(lvid)) {
        const procid_t vowner = graph.l_get_vertex_record(lvid).owner;
        rmi.remote_call(vowner,
                        &gl3engine::rpc_signal,
                        graph.global_vid(lvid),
                        msg);
        continue;
      }
      while (!vlocks[lvid].try_lock()) qthread_yield();
      vertex_type vertex(graph.l_vertex(lvid));
      context.lvid = lvid;

     // logger(LOG_EMPH, "Running vertex %ld", vertex.id());
      active_function(context, vertex, msg);
      programs_completed.inc();
      size_t newhash = get_vertex_data_hash_lvid(lvid);
      // if the hash changed, broadcast
      if (newhash != vdata_hash[lvid]) {
        vdata_hash[lvid] = newhash;
        local_vertex_type lvertex(graph.l_vertex(lvid));
        rmi.remote_call(lvertex.mirrors().begin(), lvertex.mirrors().end(),
                        &engine_type::sync_vdata,
                        lvertex.global_id(),
                        lvertex.data());
      }
      vlocks[lvid].unlock();
    }

    //logger(LOG_EMPH, "Thread Leaving");
    active_vthread_count.dec();
  }

  void ping() {
  }

  atomic<size_t> pingid;

  void exec_subtasks(size_t worker) {
    if (worker_mutex[worker].try_lock()) {
      //rmi.dc().handle_incoming_calls(worker, ncpus);
      //
      task* tasks = local_tasks[worker]->dequeue_all();
      if (tasks != NULL) {
        // execute tasks
        while (!local_tasks[worker]->end_of_dequeue_list(tasks)) {
          task* cur = tasks;
          // execute cur
          /*
             logstream(LOG_EMPH) << "Execing subtask type " << (int)(cur->task_id)
                                 << " on vertex " << cur->vid << "\n";
                                 */
          any ret = task_types[cur->task_id]->exec(graph, cur->vid, cur->param,
                                                   this, vlocks, elocks);
          // return to origin
          rmi.remote_call(cur->origin,
                          &gl3engine::task_reply_rpc,
                          cur->handle,
                          ret);
          tasks_completed.inc();
          // get the next task in the queue
          while(inplace_lf_queue2<task>::get_next(tasks) == NULL) asm volatile ("" : : : "memory");
          tasks = inplace_lf_queue2<task>::get_next(tasks);
          delete cur;
        }
      }
      worker_mutex[worker].unlock();
    }
  }

  void task_exec_start() {
    timer ti;
    ti.start();
    double last_print = 0;
    double next_processing_time = 0.05;
    do {
      exec_subtasks(qthread_id() % ncpus);
      if (ti.current_time() >= next_processing_time) {
        //rmi.dc().start_handler_threads(worker, ncpus);
        size_t p = pingid.inc() % rmi.numprocs();
        if (p == rmi.procid()) {
          p = pingid.inc() % rmi.numprocs();
        }
        request_future<void> reqf = rmi.future_remote_request(p, &gl3engine::ping);
        while(!reqf.is_ready()) {
          qthread_yield();
        }
        reqf.wait();
        next_processing_time = ti.current_time() + 0.05;
        //rmi.dc().stop_handler_threads(worker, ncpus);
      }

      if (ti.current_time() - last_print > 1 && qthread_worker(NULL) == 0) {
        std::cout << programs_completed << " updates completed\n";
        last_print = ti.current_time();
      }

      qthread_yield();
    } while(!finished && active_vthread_count > 0);
  }

  execution_status::status_enum start(update_function_type uf) {
    rmi.full_barrier();
    active_function = uf;
    finished = false;
    // reset counters
    programs_completed = 0;
    tasks_completed = 0;
    active_vthread_count = 0;
    // start the scheduler

    // pre init
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (int i = 0; i < (int)graph.num_local_vertices(); ++i) {
      vdata_hash[i] = get_vertex_data_hash_lvid((lvid_type)i);
    }
    scheduler_ptr->start();
    rmi.full_barrier();
    timer ti;
    ti.start();
    // this will stop all handler threads
    // 32K a stack
    graphlab::qthread_tools::init(ncpus, 128 * 1024);
    graphlab::qthread_group execution_group;
    // launch the task executors
        size_t num_to_spawn = num_vthreads;
    while(1) {
      //rmi.dc().stop_handler_threads(0, 1);
      //logger(LOG_EMPH, "Forking %d subtask executors", ncpus);
      for (size_t i = 0;i < ncpus; ++i) {
        execution_group.launch(boost::bind(&gl3engine::task_exec_start, this));
      }

      //logger(LOG_EMPH, "Forking %d program executors", num_to_spawn);
      for (size_t i = 0;i < num_to_spawn ; ++i) {
        active_vthread_count.inc();
        execution_group.launch(boost::bind(&gl3engine::vthread_start, this));
      }
      execution_group.join();

      // restart handler threads since the subtask executors are dead
      //rmi.dc().start_handler_threads(0, 1);
      consensus->begin_done_critical_section(0);
      bool scheduler_empty = scheduler_ptr->empty();
      bool taskqueues_empty = true;
      // check that all the queues are empty
      for (size_t i = 0;i < local_tasks.size(); ++i) taskqueues_empty &= local_tasks[i]->empty();
      if (!(scheduler_empty && taskqueues_empty)) {
        consensus->cancel_critical_section(0);
      } else {
        if (consensus->end_done_critical_section(0)) break;
      }
      num_to_spawn = std::min(num_vthreads, scheduler_ptr->approx_size());
    }
    finished = true;
    engine_runtime = ti.current_time();
    return execution_status::TASK_DEPLETION;
  }

  size_t num_updates() const {
    return programs_completed;
  }

  float elapsed_seconds() const  {
    return engine_runtime;
  }


};

} // graphlab

#undef FRESULT
#undef REMOVE_CONST_REF

#include <graphlab/macros_undef.hpp>
#endif
