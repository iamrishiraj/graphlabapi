/*
This file is part of GraphLab.

GraphLab is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as 
published by the Free Software Foundation, either version 3 of 
the License, or (at your option) any later version.

GraphLab is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public 
License along with GraphLab.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
  \author Yucheng Low (ylow)
  An implementation of a distributed integer -> integer map with caching
  capabilities. 

*/

#ifndef CACHING_DHT_HPP
#define CACHING_DHT_HPP
#include <boost/unordered_map.hpp>
#include <boost/intrusive/list.hpp>

#include <graphlab/rpc/dc.hpp>
#include <graphlab/parallel/pthread_tools.hpp>
#include <graphlab/util/synchronized_unordered_map.hpp>
#include <graphlab/util/dense_bitset.hpp>

namespace graphlab {

namespace dc_impl {
/**
 * \ingroup rpc_internal
  A cache entry for the caching_dht. 
  Boost intrusive is used to provide the LRU capabilities here
*/
template<typename KeyType, typename ValueType>
class lru_list{
 public:

  KeyType key; /// the key assiciated with this cache entry
  ValueType value; /// the value assiciated with this cache entry
  typedef boost::intrusive::list_member_hook<
            boost::intrusive::link_mode<boost::intrusive::auto_unlink> >
                                                          lru_member_hook_type;

  lru_member_hook_type member_hook_;
  ~lru_list() { }
  explicit lru_list(const KeyType& k = KeyType(), const ValueType &v = ValueType()) : key(k), value(v) {
  }
};

} // namespace dc_impl

/**
 * \ingroup rpc
This implements a limited distributed key -> value map with caching capabilities
It is up to the user to determine cache invalidation policies. User explicitly
calls the invalidate() function to clear local cache entries
*/
template<typename KeyType, typename ValueType>
class caching_dht{
 public:

  typedef dc_impl::lru_list<KeyType, ValueType> lru_entry_type;
  /// datatype of the data map
  typedef boost::unordered_map<KeyType, ValueType> map_type;
  /// datatype of the local cache map
  typedef boost::unordered_map<KeyType, lru_entry_type* > cache_type;


  typedef boost::intrusive::member_hook<lru_entry_type,
                                        typename lru_entry_type::lru_member_hook_type,
                                        &lru_entry_type::member_hook_> MemberOption;
  /// datatype of the intrusive LRU list embedded in the cache map
  typedef boost::intrusive::list<lru_entry_type, 
                                 MemberOption, 
                                 boost::intrusive::constant_time_size<false> > lru_list_type;

  /// Constructor. Creates the integer map.
  caching_dht(distributed_control &dc, 
                         size_t max_cache_size = 1024):rpc(dc, this),data(11) {
    cache.rehash(max_cache_size);
    maxcache = max_cache_size;
    logger(LOG_INFO, "%d Creating distributed_hash_table. Cache Limit = %d", 
           dc.procid(), maxcache);
    reqs = 0;
    misses = 0;
  }


  ~caching_dht() {
    data.clear();
    typename cache_type::iterator i = cache.begin();
    while (i != cache.end()) {
      delete i->second;
      ++i;
    }
    cache.clear();
  }
  
  
  /// Sets the key to the value
  void set(const KeyType& key, const ValueType &newval)  {
    size_t hashvalue = hasher(key);
    size_t owningmachine = hashvalue % rpc.dc().numprocs();
    if (owningmachine == rpc.dc().procid()) {
      datalock.lock();
      data[key] = newval;
      datalock.unlock();
    }
    else {
      rpc.remote_call(owningmachine, 
                      &caching_dht<KeyType,ValueType>::set, 
                      key,
                      newval);
      update_cache(key, newval);
    }
  }
  

  /** Gets the value associated with the key. returns true on success.. */
  std::pair<bool, ValueType> get(const KeyType &key) const {
    // figure out who owns the key
    size_t hashvalue = hasher(key);
    size_t owningmachine = hashvalue % rpc.dc().numprocs();
    
    std::pair<bool, ValueType> ret;
    // if I own the key, get it from the map table
    if (owningmachine == rpc.dc().procid()) {
      datalock.lock();
      typename map_type::const_iterator iter = data.find(key);    
      if (iter == data.end()) {
        ret.first = false;
      }
      else {
        ret.first = true;
        ret.second = iter->second;
      }
      datalock.unlock();
    }
    else {
      ret = rpc.remote_request(owningmachine, 
                               &caching_dht<KeyType,ValueType>::get, 
                               key);
      if (ret.first) update_cache(key, ret.second);
      else invalidate(key);
    }
    return ret;
  }


  /** Gets the value associated with the key, reading from cache if available
      Note that the cache may be out of date. */
  std::pair<bool, ValueType> get_cached(const KeyType &key) const {
    // if this is to my current machine, just get it and don't go to cache
    size_t hashvalue = hasher(key);
    size_t owningmachine = hashvalue % rpc.dc().numprocs();
    if (owningmachine == rpc.dc().procid()) return get(key);
    
    
    reqs++;
    cachelock.lock();
    // check if it is in the cache
    typename cache_type::iterator i = cache.find(key);
    if (i == cache.end()) {
      // nope. not in cache. Call the regular get
      cachelock.unlock();
      misses++;
      return get(key);
    }
    else {
      // yup. in cache. return the value
      std::pair<bool, ValueType> ret;
      ret.first = true;
      ret.second = i->second->value;
      // shift the cache entry to the head of the LRU list
      lruage.erase(lru_list_type::s_iterator_to(*(i->second)));
      lruage.push_front(*(i->second));
      cachelock.unlock();
      return ret;
    }
  }

  /// Invalidates the cache entry associated with this key
  void invalidate(const KeyType &key) const{
    cachelock.lock();
    // is the key I am invalidating in the cache?
    typename cache_type::iterator i = cache.find(key);
    if (i != cache.end()) {
      // drop it from the lru list
      delete i->second;
      cache.erase(i);
    }
    cachelock.unlock();
  }


  double cache_miss_rate() {
    return double(misses) / double(reqs);
  }

  size_t num_gets() const {
    return reqs;
  }
  size_t num_misses() const {
    return misses;
  }

  size_t cache_size() const {
    return cache.size();
  }

 private:

  mutable dc_dist_object<caching_dht<KeyType, ValueType> > rpc;
  
  mutex datalock;
  map_type data;  /// The actual table data that is distributed

  
  mutex cachelock; /// lock for the cache datastructures
  mutable cache_type cache;   /// The cache table
  mutable lru_list_type lruage; /// THe LRU linked list associated with the cache


  procid_t numprocs;   /// NUmber of processors
  size_t maxcache;     /// Maximum cache size allowed

  mutable size_t reqs;
  mutable size_t misses;
  


  boost::hash<KeyType> hasher;
  

  /// Updates the cache with this new value
  void update_cache(const KeyType &key, const ValueType &val) const{
    cachelock.lock();
    typename cache_type::iterator i = cache.find(key);
    // create a new entry
    if (i == cache.end()) {
      cachelock.unlock();
      // if we are out of room, remove the lru entry
      if (cache.size() >= maxcache) remove_lru();
      cachelock.lock();
      // insert the element, remember the iterator so we can push it
      // straight to the LRU list
      std::pair<typename cache_type::iterator, bool> ret = cache.insert(std::make_pair(key, new lru_entry_type(key, val)));
      if (ret.second)  lruage.push_front(*(ret.first->second));
    }
    else {
        // modify entry in place
        i->second->value = val;
        // swap to front of list
        //boost::swap_nodes(lru_list_type::s_iterator_to(i->second), lruage.begin());
        lruage.erase(lru_list_type::s_iterator_to(*(i->second)));
        lruage.push_front(*(i->second));
    }
    cachelock.unlock();
  }

  /// Removes the least recently used element from the cache
  void remove_lru() const{
    cachelock.lock();
    KeyType keytoerase = lruage.back().key;
    // is the key I am invalidating in the cache?
    typename cache_type::iterator i = cache.find(keytoerase);
    if (i != cache.end()) {
      // drop it from the lru list
      delete i->second;
      cache.erase(i);
    }
    cachelock.unlock();
  }

};

}
#endif
