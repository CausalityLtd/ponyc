#include <platform.h>

#include <mem/pool.h>

#include <gtest/gtest.h>

#ifdef POOL_USE_DEFAULT

typedef char block_t[32];

TEST(Pool, Fifo)
{
  void* p1 = ponyint_pool_alloc(0);
  ponyint_pool_free(0, p1);

  void* p2 = POOL_ALLOC(block_t);
  ASSERT_EQ(p1, p2);
  POOL_FREE(block_t, p2);
}

TEST(Pool, Size)
{
  void* p1 = ponyint_pool_alloc(0);
  ponyint_pool_free(0, p1);

  void* p2 = ponyint_pool_alloc_size(17);
  ASSERT_EQ(p1, p2);
  ponyint_pool_free_size(17, p2);

  void* p3 = ponyint_pool_alloc(0);
  ASSERT_EQ(p2, p3);
  ponyint_pool_free(0, p3);
}

TEST(Pool, ExceedBlock)
{
  void* p[2048];

  for(int i = 0; i < 2048; i++)
  {
    p[i] = POOL_ALLOC(block_t);

    for(int j = 0; j < i; j++)
    {
      ASSERT_NE(p[i], p[j]);
    }
  }

  for(int i = 2047; i >= 0; i--)
    POOL_FREE(block_t, p[i]);

  void* q[2048];

  for(int i = 0; i < 2048; i++)
  {
    q[i] = POOL_ALLOC(block_t);
    ASSERT_EQ(p[i], q[i]);

    for(int j = 0; j < i; j++)
    {
      ASSERT_NE(q[i], q[j]);
    }
  }

  for(int i = 2047; i >= 0; i--)
    POOL_FREE(block_t, q[i]);
}

TEST(Pool, LargeAlloc)
{
  void* p = ponyint_pool_alloc_size(1 << 20);
  ASSERT_TRUE(p != NULL);
  ponyint_pool_free_size(1 << 20, p);
}

#endif

TEST(Pool, Index)
{
  size_t expected_index = 0;
  size_t index = ponyint_pool_index(1);
  ASSERT_TRUE(index == 0);

  index = ponyint_pool_index(7);
  ASSERT_TRUE(index == expected_index);

  index = ponyint_pool_index(8);
  ASSERT_TRUE(index == expected_index);

  index = ponyint_pool_index(9);
  ASSERT_TRUE(index == expected_index);

  index = ponyint_pool_index(15);
  ASSERT_TRUE(index == expected_index);

  index = ponyint_pool_index(16);
  ASSERT_TRUE(index == expected_index);

#ifndef POOL_USE_DEFAULT
  expected_index++;
#endif

  index = ponyint_pool_index(17);
  ASSERT_TRUE(index == expected_index);

  index = ponyint_pool_index(31);
  ASSERT_TRUE(index == expected_index);

  index = ponyint_pool_index(32);
  ASSERT_TRUE(index == expected_index);

  expected_index++;

  index = ponyint_pool_index(33);
  ASSERT_TRUE(index == expected_index);

  index = ponyint_pool_index(63);
  ASSERT_TRUE(index == expected_index);

  index = ponyint_pool_index(64);
  ASSERT_TRUE(index == expected_index);

  expected_index++;

  index = ponyint_pool_index(65);
  ASSERT_TRUE(index == expected_index);

  index = ponyint_pool_index(POOL_MAX - 1);
  ASSERT_TRUE(index == POOL_COUNT - 1);

  index = ponyint_pool_index(POOL_MAX);
  ASSERT_TRUE(index == POOL_COUNT - 1);

  index = ponyint_pool_index(POOL_MAX + 1);
  ASSERT_TRUE(index == POOL_COUNT);
}
