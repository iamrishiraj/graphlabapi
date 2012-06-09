/**  
 * Copyright (c) 2009 Carnegie Mellon University. 
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://www.graphlab.ml.cmu.edu
 *
 */


#ifndef GRAPHLAB_ATOMIC_HPP
#define GRAPHLAB_ATOMIC_HPP

#include <stdint.h>
#include <graphlab/serialization/serialization_includes.hpp>
#include <graphlab/parallel/atomic_ops.hpp>

namespace graphlab {


  /**
   * \brief atomic object toolkit
   * \ingroup util
   * A templated class for creating atomic numbers.
   */
  template<typename T>
  class atomic : public IS_POD_TYPE {
  public:
    //! The current value of the atomic number
    volatile T value;

    //! Creates an atomic number with value "value"
    atomic(const T& value = T()) : value(value) { }
    
    //! Performs an atomic increment by 1, returning the new value
    T inc() { return __sync_add_and_fetch(&value, 1);  }

    //! Performs an atomic decrement by 1, returning the new value
    T dec() { return __sync_sub_and_fetch(&value, 1);  }
    
    //! Lvalue implicit cast
    operator T() const { return value; }

    //! Performs an atomic increment by 1, returning the new value
    T operator++() { return inc(); }

    //! Performs an atomic decrement by 1, returning the new value
    T operator--() { return dec(); }
    
    //! Performs an atomic increment by 'val', returning the new value
    T inc(const T val) { return __sync_add_and_fetch(&value, val);  }
    
    //! Performs an atomic decrement by 'val', returning the new value
    T dec(const T val) { return __sync_sub_and_fetch(&value, val);  }
    
    //! Performs an atomic increment by 'val', returning the new value
    T operator+=(const T val) { return inc(val); }

    //! Performs an atomic decrement by 'val', returning the new value
    T operator-=(const T val) { return dec(val); }

    //! Performs an atomic increment by 1, returning the old value
    T inc_ret_last() { return __sync_fetch_and_add(&value, 1);  }
    
    //! Performs an atomic decrement by 1, returning the old value
    T dec_ret_last() { return __sync_fetch_and_sub(&value, 1);  }

    //! Performs an atomic increment by 1, returning the old value
    T operator++(int) { return inc_ret_last(); }

    //! Performs an atomic decrement by 1, returning the old value
    T operator--(int) { return dec_ret_last(); }

    //! Performs an atomic increment by 'val', returning the old value
    T inc_ret_last(const T val) { return __sync_fetch_and_add(&value, val);  }
    
    //! Performs an atomic decrement by 'val', returning the new value
    T dec_ret_last(const T val) { return __sync_fetch_and_sub(&value, val);  }

    //! Performs an atomic exchange with 'val', returning the previous value
    T exchange(const T val) { return __sync_lock_test_and_set(&value, val);  }
  };

}
#endif

