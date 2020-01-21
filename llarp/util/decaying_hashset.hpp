#ifndef LLARP_UTIL_DECAYING_HASHSET_HPP
#define LLARP_UTIL_DECAYING_HASHSET_HPP

#include <util/time.hpp>
#include <unordered_map>
#include <functional>

namespace llarp
{
  namespace util
  {
    template < typename Val_t, typename Hash_t = typename Val_t::Hash >
    struct DecayingHashSet
    {
      DecayingHashSet(std::chrono::milliseconds cacheInterval)
          : DecayingHashSet(cacheInterval.count())
      {
      }
      DecayingHashSet(llarp_time_t cacheInterval = 5000)
          : m_CacheInterval(cacheInterval)
      {
      }

      /// determine if we have v contained in our decaying hashset
      bool
      Contains(const Val_t& v) const
      {
        return m_Values.find(v) != m_Values.end();
      }

      /// return true if inserted
      /// return false if not inserted
      bool
      Insert(const Val_t& v, llarp_time_t now = 0)
      {
        if(now == 0)
          now = llarp::time_now_ms();
        return m_Values.emplace(v, now).second;
      }

      /// decay hashset entries
      void
      Decay(llarp_time_t now = 0)
      {
        if(now == 0)
          now = llarp::time_now_ms();
        auto itr = m_Values.begin();
        while(itr != m_Values.end())
        {
          if((m_CacheInterval + itr->second) <= now)
            itr = m_Values.erase(itr);
          else
            ++itr;
        }
      }

      llarp_time_t
      DecayInterval() const
      {
        return m_CacheInterval;
      }

      void
      DecayInterval(llarp_time_t interval)
      {
        m_CacheInterval = interval;
      }

      /// return the insertion time of the inserted value,
      /// or 0 if it isn't found
      llarp_time_t
      GetInsertionTime(const Val_t& val) const
      {
        auto itr = m_Values.find(val);
        if (itr == m_Values.end())
          return 0;
        else
          return itr->second;
      }

      /// Visit each value in the set along with its insertion time
      using VisitFunc = std::function<void(const Val_t&, llarp_time_t)>;
      void Visit(VisitFunc visit) const
      {
        for (auto itr = m_Values.begin(); itr != m_Values.end(); ++itr)
        {
          visit(itr->first, itr->second);
        }
      }

     private:
      llarp_time_t m_CacheInterval;
      std::unordered_map< Val_t, llarp_time_t, Hash_t > m_Values;
    };
  }  // namespace util
}  // namespace llarp

#endif
