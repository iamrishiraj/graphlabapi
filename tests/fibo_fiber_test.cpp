#include <iostream>
#include <graphlab/parallel/fiber.hpp>
using namespace graphlab;
fiber_group* fibers;

struct fibonacci_compute_promise {
  mutex* lock;
  size_t argument;
  size_t result;
  size_t parent_tid;
  bool result_set;
};

void fibonacci(void* val) {
  fibonacci_compute_promise* promise = reinterpret_cast<fibonacci_compute_promise*>(val);
  //std::cout << promise->argument << "\n";
  if (promise->argument == 1 ||  promise->argument == 2) {
    promise->result = 1;
  } else {
    // recursive case
    mutex lock;
    fibonacci_compute_promise left, right;
    left.lock = &lock;
    left.argument = promise->argument - 1;
    left.result_set = false;
    left.parent_tid = fiber_group::get_tid();

    right.lock = &lock;
    right.argument = promise->argument - 2;
    right.result_set = false;
    right.parent_tid = fiber_group::get_tid();

    fibers->launch(fibonacci, &left);
    fibers->launch(fibonacci, &right);

    // wait on the left and right promise
    lock.lock();
    while (left.result_set == false || right.result_set == false) {
      fiber_group::deschedule_self(&lock.m_mut);
      lock.lock();
    }
    lock.unlock();

    assert(left.result_set);
    assert(right.result_set);
    promise->result = left.result + right.result;
  }
  promise->lock->lock();
  promise->result_set = true;
  if (promise->parent_tid) fiber_group::schedule_tid(promise->parent_tid);
  promise->lock->unlock();
}


int main(int argc, char** argv) {
  fibers = new fiber_group(4, 8192);

  timer ti; ti.start();

  fibonacci_compute_promise promise;
  mutex lock;
  promise.lock = &lock;
  promise.result_set = false;
  promise.argument = 24;
  promise.parent_tid = 0;
  fibers->launch(fibonacci, &promise);
  fibers->join();
  assert(promise.result_set);
  std::cout << "Fib(" << promise.argument << ") = " << promise.result << "\n";

  std::cout << "Completion in " << ti.current_time() << "s\n";
  std::cout << fibers->total_threads_created() << " threads created\n";

  delete fibers;
}
