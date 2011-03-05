#ifndef GRAPHLAB_ISCHEDULER_HPP
#define GRAPHLAB_ISCHEDULER_HPP

#include <vector>
#include <sstream>
#include <ostream>
#include <graphlab/tasks/update_task.hpp>
#include <graphlab/monitoring/imonitor.hpp>
#include <graphlab/schedulers/icallback.hpp>
#include <graphlab/schedulers/scheduler_options.hpp>

namespace graphlab {
  template <typename Graph> class iengine;
  
  /**
   * This is an enumeration for the possible return values for
   * get_next_tasks
   */
  struct sched_status {
    enum status_enum {
      NEWTASK,      /**< The get_next_tasks function returned a new task 
                        to be executed */
      EMPTY,         /**< The schedule is empty. */
      
      /// Deprecated options. to be phased out. do not use.
      WAITING,      /// \deprecated
      COMPLETE      /// \deprecated
    };
  };
  
 /// \deprecated
  struct scheduler_options_enum {
    enum options_enum {
      UPDATE_FUNCTION,    /// used by 1-update function schedulers
      MAX_ITERATIONS,     /// maximum iteration count. Used by round-robin
      START_VERTEX,       /// vertex to start at. used by round-robin
      VERTICES_PER_PARTITION,  /// Used by cluster_priority
      PARTITION_METHOD,        /// Used by cluster_priority
      SWEEP_PERMUTE,           /// used by sweep scheduler
      SPLASH_SIZE,             /// used by splash scheduler
      BARRIER,
      DISTRIBUTED_CONTROL
    };
  };
  

  /**
   * \ingroup group_schedulers
   * This describes the interface/concept for the scheduler. The
   * engine will be passed the scheduler type as a template argument,
   * so the scheduler must inherit and satisfy this interface
   * EXACTLY. Note that all functions (with the exception of the
   * constructor and destructor) must be thread-safe.
   */
  template<typename Graph>
  class ischeduler {
  public:

    typedef Graph graph_type;
    typedef update_task<Graph> update_task_type;
    typedef typename update_task_type::update_function_type 
    update_function_type;

    typedef iengine<Graph> iengine_type;
    typedef icallback<Graph> callback_type;
    typedef imonitor<Graph> monitor_type;

    /** Defines the preferred terminator algorithm */
    typedef char terminator_type;
    terminator_type terminator;
    /** 
     * Constructor: The scheduler must be provided with the graph, and the 
     * number of cpus.  All initialization of the scheduler internal state must
     * be performed here.
     *
     */
    //    ischeduler(iengine_type* engine, Graph& g, size_t ncpus) : monitor(NULL) { }
    ischeduler() : monitor(NULL) {}
    
    /// destructor
    virtual ~ischeduler() {};
        
    /** Called by engine before starting the schedule.
     *  This function will only be called once throughout the lifetime
     * of the scheduler.
     */
    virtual void start() = 0;


    /**
     * Adds an update task with a particular priority. 
     * This function may be called at anytime.
     */
    virtual void add_task(update_task_type task, double priority) = 0;
    
    /** 
     * Creates a collection of tasks on all the vertices in
     * 'vertices', and all with the same update function and priority
     * This function may be called at anytime.
     */
    virtual void add_tasks(const std::vector<vertex_id_t>& vertices, 
                           update_function_type func, double priority) = 0;
    
    /** 
     * Creates a collection of tasks on all the vertices in the graph,
     * with the same update function and priority
     * This function may be called at anytime.
     */
    virtual void add_task_to_all(update_function_type func, 
                                 double priority) = 0;
    
    /**
     * This function returns a reference to the scheduling callback to
     * be used for a particular cpu. This callback will be passed to
     * update functions, and is the main interface which allow the
     * update functions to create new tasks.
     */
    virtual callback_type& get_callback(size_t cpuid) = 0;

    /**
     * This function is called by the engine to ask for new work to
     * do.  The update task to be executed is returned in ret_task.
     *
     *  \retval NEWTASK There is an update task in ret_task to be
     *   executed
     * 
     *  \retval EMPTY There are no tasks available in the scheduler.
     */
    virtual sched_status::status_enum get_next_task(size_t cpuid, 
                                       update_task_type &ret_task) = 0;

    /**
     * This is called after a task has been executed
     */
    virtual void completed_task(size_t cpuid, 
                                const update_task_type &task) = 0;


    /** Installs a listener (done by the engine) */
    virtual void register_monitor(monitor_type* monitor_) { 
      monitor = monitor_;
    }

    virtual void set_options(const scheduler_options &opts) { };

    static void print_options_help(std::ostream &out) { };


    /// UNUSED!!! Only used for temporary backward compatibility with the distributed code
    virtual void set_option(scheduler_options_enum::options_enum, void*) { };
    
    /** Returns a reference to the terminator */
    terminator_type& get_terminator() {
      return terminator;
    };
    
  protected:
    monitor_type* monitor;

  };

}
#endif

