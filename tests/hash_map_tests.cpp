#include <gtest/gtest.h>

#include "alloy/datastructures/hash_map.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

using alloy::ds::hash_map;

TEST(HashMap, DefaultConstruction) {
    hash_map<int, int> map;

    EXPECT_TRUE(map.empty());
    EXPECT_EQ(map.size(), 0U);
    EXPECT_EQ(map.begin(), map.end());
    EXPECT_EQ(map.find(1), map.end());
    EXPECT_FALSE(map.contains(1));
    EXPECT_FLOAT_EQ(map.load_factor(), 0.0f);
}

TEST(HashMap, InsertAndFind) {
    hash_map<int, std::string> map;

    auto [it, inserted] = map.emplace(1, "one");

    ASSERT_TRUE(inserted);
    ASSERT_NE(it, map.end());

    EXPECT_EQ(it->first, 1);
    EXPECT_EQ(it->second, "one");
    EXPECT_EQ(map.size(), 1U);
    EXPECT_FALSE(map.empty());

    EXPECT_TRUE(map.contains(1));
    EXPECT_EQ(map.count(1), 1U);
    EXPECT_EQ(map.find(1), it);
    EXPECT_EQ(map.at(1), "one");

    EXPECT_FALSE(map.contains(2));
    EXPECT_EQ(map.count(2), 0U);
    EXPECT_EQ(map.find(2), map.end());
    EXPECT_THROW((void)map.at(2), std::out_of_range);
}

TEST(HashMap, DuplicateInsertion) {
    hash_map<int, int> map;

    ASSERT_TRUE(map.emplace(1, 10).second);

    auto [it, inserted] = map.emplace(1, 20);

    EXPECT_FALSE(inserted);
    ASSERT_NE(it, map.end());
    EXPECT_EQ(it->second, 10);
    EXPECT_EQ(map.size(), 1U);
}

TEST(HashMap, InsertOrAssign) {
    hash_map<int, int> map;

    EXPECT_TRUE(map.insert_or_assign(1, 10).second);
    EXPECT_FALSE(map.insert_or_assign(1, 20).second);

    EXPECT_EQ(map.at(1), 20);
    EXPECT_EQ(map.size(), 1U);
}

TEST(HashMap, Subscript) {
    hash_map<int, int> map;

    EXPECT_EQ(map[1], 0);
    EXPECT_EQ(map.size(), 1U);

    map[1] = 42;
    map[2] = 100;

    EXPECT_EQ(map[1], 42);
    EXPECT_EQ(map[2], 100);
    EXPECT_EQ(map.size(), 2U);
}

TEST(HashMap, Erase) {
    hash_map<int, int> map;

    map.emplace(1, 10);
    map.emplace(2, 20);
    map.emplace(3, 30);

    EXPECT_EQ(map.erase(2), 1U);
    EXPECT_EQ(map.erase(2), 0U);

    EXPECT_FALSE(map.contains(2));
    EXPECT_EQ(map.at(1), 10);
    EXPECT_EQ(map.at(3), 30);
    EXPECT_EQ(map.size(), 2U);

    EXPECT_TRUE(map.emplace(2, 200).second);
    EXPECT_EQ(map.at(2), 200);
    EXPECT_EQ(map.size(), 3U);
}

TEST(HashMap, ClearAndReuse) {
    hash_map<int, std::string> map;

    map.emplace(1, "one");
    map.emplace(2, "two");

    map.clear();

    EXPECT_TRUE(map.empty());
    EXPECT_EQ(map.size(), 0U);
    EXPECT_EQ(map.begin(), map.end());
    EXPECT_FALSE(map.contains(1));
    EXPECT_FALSE(map.contains(2));

    EXPECT_TRUE(map.emplace(1, "new").second);
    EXPECT_EQ(map.at(1), "new");
}

TEST(HashMap, Iteration) {
    hash_map<int, int> map;

    constexpr int n = 100;

    for (int i = 0; i < n; ++i) {
        map.emplace(i, i * 2);
    }

    bool seen[n]{};

    for (auto [key, value] : map) {
        ASSERT_GE(key, 0);
        ASSERT_LT(key, n);

        EXPECT_FALSE(seen[key]);
        EXPECT_EQ(value, key * 2);

        seen[key] = true;
        value += 1;
    }

    for (int i = 0; i < n; ++i) {
        EXPECT_TRUE(seen[i]);
        EXPECT_EQ(map.at(i), i * 2 + 1);
    }
}

TEST(HashMap, GrowthAndRehash) {
    hash_map<int, std::string> map;

    constexpr int n = 1000;

    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(map.emplace(i, std::to_string(i)).second);
    }

    EXPECT_EQ(map.size(), static_cast<size_t>(n));

    map.reserve(2LL * n);
    map.rehash(4LL * n);

    EXPECT_EQ(map.size(), static_cast<size_t>(n));

    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(map.contains(i));
        EXPECT_EQ(map.at(i), std::to_string(i));
    }

    EXPECT_LE(map.load_factor(), map.max_load_factor());
}

TEST(HashMap, CopyAndMove) {
    hash_map<int, std::string> original;

    original.emplace(1, "one");
    original.emplace(2, "two");

    auto copy = original;

    EXPECT_EQ(copy, original);

    copy[1] = "changed";

    EXPECT_EQ(original.at(1), "one");
    EXPECT_EQ(copy.at(1), "changed");

    auto moved = std::move(copy);

    EXPECT_EQ(moved.size(), 2U);
    EXPECT_EQ(moved.at(1), "changed");
    EXPECT_EQ(moved.at(2), "two");

    EXPECT_TRUE(copy.empty());
    EXPECT_TRUE(copy.emplace(3, "three").second);
}

namespace {
struct constant_hash {
    auto operator()(int) const noexcept -> size_t {
        return 0;
    }
};
} // namespace

TEST(HashMap, Collisions) {
    hash_map<int, int, constant_hash> map;

    constexpr int n = 64;

    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(map.emplace(i, i * 10).second);
    }

    EXPECT_EQ(map.size(), static_cast<size_t>(n));

    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(map.contains(i));
        EXPECT_EQ(map.at(i), i * 10);
    }

    EXPECT_FALSE(map.contains(n));
}

TEST(HashMap, EraseWithCollisions) {
    hash_map<int, int, constant_hash> map;

    map.reserve(64);

    for (int i = 0; i < 48; ++i) {
        ASSERT_TRUE(map.emplace(i, i * 10).second);
    }

    for (int i = 0; i < 8; ++i) {
        ASSERT_EQ(map.erase(i), 1U);
    }

    for (int i = 0; i < 8; ++i) {
        EXPECT_FALSE(map.contains(i));
    }

    for (int i = 8; i < 48; ++i) {
        ASSERT_TRUE(map.contains(i));
        EXPECT_EQ(map.at(i), i * 10);
    }

    EXPECT_FALSE(map.emplace(47, -1).second);
    EXPECT_EQ(map.at(47), 470);
    EXPECT_EQ(map.size(), 40U);

    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(map.emplace(i, -i).second);
        EXPECT_EQ(map.at(i), -i);
    }

    EXPECT_EQ(map.size(), 48U);
}

namespace {
struct transparent_hash {
    using is_transparent = void;

    auto operator()(std::string_view key) const noexcept -> std::size_t {
        return std::hash<std::string_view>{}(key);
    }
};

struct transparent_equal {
    using is_transparent = void;

    auto operator()(std::string_view lhs, std::string_view rhs) const noexcept -> bool {
        return lhs == rhs;
    }
};

template<class V>
using transparent_map = hash_map<std::string, V, transparent_hash, transparent_equal>;

static_assert(!std::is_convertible_v<std::string_view, std::string>);
} // namespace

TEST(HashMap, HeterogeneousLookup) {
    transparent_map<int> map;

    map.emplace("one", 1);
    map.emplace("two", 2);

    std::string_view key = "one";
    const std::string_view missing = "three";

    EXPECT_TRUE(map.contains(key));
    EXPECT_EQ(map.count(key), 1ULL);

    auto it = map.find(key);

    ASSERT_NE(it, map.end());
    EXPECT_EQ(it->second, 1);

    map.at(key) = 10;
    EXPECT_EQ(it->second, 10);

    auto [first, last] = map.equal_range(key);

    EXPECT_EQ(first, it);
    EXPECT_EQ(last, ++it);

    EXPECT_FALSE(map.contains(missing));
    EXPECT_EQ(map.count(missing), 0ULL);
    EXPECT_EQ(map.find(missing), map.end());
    EXPECT_THROW((void)map.at(missing), std::out_of_range);

    auto [empty_first, empty_last] = map.equal_range(missing);

    EXPECT_EQ(empty_first, map.end());
    EXPECT_EQ(empty_last, map.end());

    const auto& ref = map;

    EXPECT_EQ(ref.at(key), 10);
    EXPECT_NE(ref.find(key), ref.end());

    static_assert(std::is_same_v<decltype(map.at(key)), int&>);
    static_assert(std::is_same_v<decltype(ref.at(key)), const int&>);
}

TEST(HashMap, HeterogeneousErase) {
    transparent_map<int> map;

    map.emplace("one", 1);
    map.emplace("two", 2);
    map.emplace("three", 3);

    std::string_view key = "two";

    EXPECT_EQ(map.erase(key), 1ULL);
    EXPECT_EQ(map.erase(key), 0ULL);
    EXPECT_FALSE(map.contains(key));
    EXPECT_EQ(map.size(), 2ULL);

    EXPECT_EQ(map.at("one"), 1);
    EXPECT_EQ(map.at("three"), 3);

    EXPECT_EQ(map.erase(std::string_view{"three"}), 1ULL);
    EXPECT_EQ(map.size(), 1ULL);

    EXPECT_EQ(map.erase(map.begin()), map.end());
    EXPECT_TRUE(map.empty());
}

TEST(HashMap, HeterogeneousTryEmplace) {
    transparent_map<std::unique_ptr<int>> map;

    std::string_view key = "one";

    auto [it, inserted] = map.try_emplace(key, std::make_unique<int>(1));

    ASSERT_TRUE(inserted);
    ASSERT_NE(it, map.end());
    ASSERT_NE(it->second, nullptr);
    EXPECT_EQ(*it->second, 1);

    auto value = std::make_unique<int>(2);

    auto [duplicate, duplicate_inserted] = map.try_emplace(key, std::move(value));

    EXPECT_FALSE(duplicate_inserted);
    EXPECT_EQ(duplicate, it);
    EXPECT_EQ(map.size(), 1ULL);

    // Failed insertion must not consume the mapped argument.
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 2);
    EXPECT_EQ(*duplicate->second, 1);

    auto hinted = map.try_emplace(map.cend(), std::string_view{"two"}, std::move(value));

    ASSERT_NE(hinted, map.end());
    ASSERT_NE(hinted->second, nullptr);
    EXPECT_EQ(*hinted->second, 2);
    EXPECT_EQ(value, nullptr);
    EXPECT_EQ(map.size(), 2ULL);
}

TEST(HashMap, HeterogeneousInsertOrAssignAndSubscript) {
    transparent_map<int> map;

    std::string_view key = "one";

    auto [it, inserted] = map.insert_or_assign(key, 1);

    ASSERT_TRUE(inserted);
    EXPECT_EQ(it->second, 1);

    auto [assigned, assigned_inserted] = map.insert_or_assign(key, 10);

    EXPECT_FALSE(assigned_inserted);
    EXPECT_EQ(assigned, it);
    EXPECT_EQ(assigned->second, 10);
    EXPECT_EQ(map.size(), 1ULL);

    EXPECT_EQ(map[key], 10);
    EXPECT_EQ(map.size(), 1ULL);

    const std::string_view second = "two";

    EXPECT_EQ(map[second], 0);
    EXPECT_EQ(map.size(), 2ULL);

    map[second] = 20;

    EXPECT_EQ(map.at(second), 20);

    auto hinted = map.insert_or_assign(map.cend(), std::string_view{"three"}, 30);

    EXPECT_EQ(hinted->second, 30);
    EXPECT_EQ(map.size(), 3ULL);
}