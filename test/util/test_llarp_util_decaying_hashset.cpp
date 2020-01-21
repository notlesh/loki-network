#include <util/decaying_hashset.hpp>
#include <router_id.hpp>
#include <gtest/gtest.h>

struct DecayingHashSetTest : public ::testing::Test
{
};

TEST_F(DecayingHashSetTest, TestDecayDeterministc)
{
  static constexpr llarp_time_t timeout = 5;
  static constexpr llarp_time_t now     = 1;
  llarp::util::DecayingHashSet< llarp::RouterID > hashset(timeout);
  const llarp::RouterID zero;
  ASSERT_TRUE(zero.IsZero());
  ASSERT_FALSE(hashset.Contains(zero));
  ASSERT_TRUE(hashset.Insert(zero, now));
  ASSERT_TRUE(hashset.Contains(zero));
  hashset.Decay(now + 1);
  ASSERT_TRUE(hashset.Contains(zero));
  hashset.Decay(now + timeout);
  ASSERT_FALSE(hashset.Contains(zero));
  hashset.Decay(now + timeout + 1);
  ASSERT_FALSE(hashset.Contains(zero));
}

TEST_F(DecayingHashSetTest, TestDecay)
{
  static constexpr llarp_time_t timeout = 5;
  const llarp_time_t now                = llarp::time_now_ms();
  llarp::util::DecayingHashSet< llarp::RouterID > hashset(timeout);
  const llarp::RouterID zero;
  ASSERT_TRUE(zero.IsZero());
  ASSERT_FALSE(hashset.Contains(zero));
  ASSERT_TRUE(hashset.Insert(zero));
  ASSERT_TRUE(hashset.Contains(zero));
  hashset.Decay(now + 1);
  ASSERT_TRUE(hashset.Contains(zero));
  hashset.Decay(now + timeout);
  ASSERT_FALSE(hashset.Contains(zero));
  hashset.Decay(now + timeout + 1);
  ASSERT_FALSE(hashset.Contains(zero));
}

TEST_F(DecayingHashSetTest, TestGetInsertionTime_ReturnsZeroOnNoValueFound)
{
  llarp::util::DecayingHashSet<std::string, std::hash<std::string>> hashset;

  EXPECT_FALSE(hashset.Contains("foo"));
  EXPECT_EQ(0, hashset.GetInsertionTime("foo"));
}

TEST_F(DecayingHashSetTest, TestGetInsertionTime_UpdatesEveryInsert)
{
  static constexpr llarp_time_t timeout = 5;
  const llarp_time_t now = llarp::time_now_ms();

  llarp::util::DecayingHashSet<std::string, std::hash<std::string>> hashset(timeout);

  EXPECT_FALSE(hashset.Contains("foo"));
  EXPECT_TRUE(hashset.Insert("foo", now));
  EXPECT_EQ(now, hashset.GetInsertionTime("foo"));

  hashset.Decay(now + timeout + 1);

  EXPECT_FALSE(hashset.Contains("foo"));

  const llarp_time_t newNow = llarp::time_now_ms();
  EXPECT_TRUE(hashset.Insert("foo", newNow));
  EXPECT_EQ(newNow, hashset.GetInsertionTime("foo"));
}

TEST_F(DecayingHashSetTest, TestVisit_VisitsAll)
{
  llarp::util::DecayingHashSet<std::string, std::hash<std::string>> hashset;

  // build a map of objects to insert
  std::unordered_map<std::string, llarp_time_t> original;
  original["foo"] = 1;
  original["bar"] = 2;
  original["baz"] = 4;

  for (auto itr = original.begin(); itr != original.end(); ++itr)
  {
    EXPECT_TRUE(hashset.Insert(itr->first, itr->second));
  }

  // rebuild a new map by visiting each object
  std::unordered_map<std::string, llarp_time_t> visited;
  hashset.Visit([&](const std::string& val, llarp_time_t time)
    {
      visited[val] = time;
    });

  // and compare original map to new one
  EXPECT_EQ(original, visited);

}
