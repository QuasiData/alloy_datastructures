#include <gtest/gtest.h>

#include "alloy/datastructures/vector.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <utility>

using alloy::ds::vector;

TEST(Vector, DefaultConstruction) {
    vector<int> vec;

    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(vec.size(), 0ULL);
    EXPECT_EQ(vec.capacity(), 0ULL);
    EXPECT_EQ(vec.data(), nullptr);
    EXPECT_EQ(vec.begin(), vec.end());
}

TEST(Vector, Construction) {
    vector<int> zeroed(3ULL);
    EXPECT_EQ(zeroed, (vector<int>{0, 0, 0}));

    vector<int> filled(3ULL, 42);
    EXPECT_EQ(filled, (vector<int>{42, 42, 42}));

    vector<int> initialized{1, 2, 3};
    EXPECT_EQ(initialized.size(), 3ULL);
    EXPECT_EQ(initialized, (vector<int>{1, 2, 3}));

    const std::array values{4, 5, 6};
    vector<int> from_range(values.begin(), values.end());

    EXPECT_EQ(from_range, (vector<int>{4, 5, 6}));
}

TEST(Vector, ElementAccess) {
    vector<int> vec{10, 20, 30};

    EXPECT_EQ(vec[0ULL], 10);
    EXPECT_EQ(vec.at(1ULL), 20);
    EXPECT_EQ(vec.front(), 10);
    EXPECT_EQ(vec.back(), 30);
    EXPECT_EQ(vec.data(), vec.begin());

    vec[1ULL] = 42;
    EXPECT_EQ(vec.at(1ULL), 42);

    EXPECT_THROW((void)vec.at(3ULL), std::out_of_range);

    const auto& ref = vec;

    EXPECT_EQ(ref.front(), 10);
    EXPECT_EQ(ref.back(), 30);
    EXPECT_EQ(ref[1ULL], 42);
}

TEST(Vector, PushBackAndPopBack) {
    vector<int> vec;

    vec.push_back(10);
    vec.push_back(20);
    vec.push_back(30);

    EXPECT_EQ(vec.size(), 3ULL);
    EXPECT_EQ(vec, (vector<int>{10, 20, 30}));

    vec.pop_back();

    EXPECT_EQ(vec.size(), 2ULL);
    EXPECT_EQ(vec, (vector<int>{10, 20}));

    vec.pop_back();
    vec.pop_back();

    EXPECT_TRUE(vec.empty());
}

TEST(Vector, EmplaceBack) {
    vector<std::string> vec;

    auto& value = vec.emplace_back(5ULL, 'a');

    EXPECT_EQ(value, "aaaaa");
    EXPECT_EQ(&value, &vec.back());

    vec.emplace_back("hello");

    EXPECT_EQ(vec, (vector<std::string>{"aaaaa", "hello"}));
}

TEST(Vector, ReserveAndShrinkToFit) {
    vector<int> vec{1, 2, 3};

    vec.reserve(100ULL);

    EXPECT_GE(vec.capacity(), 100ULL);
    EXPECT_EQ(vec.size(), 3ULL);
    EXPECT_EQ(vec, (vector<int>{1, 2, 3}));

    const auto capacity = vec.capacity();
    const auto* data = vec.data();

    vec.reserve(10ULL);

    EXPECT_EQ(vec.capacity(), capacity);
    EXPECT_EQ(vec.data(), data);

    vec.shrink_to_fit();

    EXPECT_EQ(vec.capacity(), vec.size());
    EXPECT_EQ(vec, (vector<int>{1, 2, 3}));
}

TEST(Vector, Resize) {
    vector<int> vec{1, 2, 3};

    vec.resize(5ULL);

    EXPECT_EQ(vec, (vector<int>{1, 2, 3, 0, 0}));

    vec.resize(7ULL, 42);

    EXPECT_EQ(vec, (vector<int>{1, 2, 3, 0, 0, 42, 42}));

    vec.resize(2ULL);

    EXPECT_EQ(vec, (vector<int>{1, 2}));

    vec.resize(0ULL);

    EXPECT_TRUE(vec.empty());
}

TEST(Vector, ClearAndReuse) {
    vector<std::string> vec{"one", "two", "three"};

    const auto capacity = vec.capacity();

    vec.clear();

    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(vec.size(), 0ULL);
    EXPECT_EQ(vec.capacity(), capacity);
    EXPECT_EQ(vec.begin(), vec.end());

    vec.emplace_back("new");

    EXPECT_EQ(vec, (vector<std::string>{"new"}));
}

TEST(Vector, Insert) {
    vector<int> vec{1, 3, 5};

    auto it = vec.insert(vec.begin() + 1, 2);

    ASSERT_EQ(it, vec.begin() + 1);
    EXPECT_EQ(*it, 2);
    EXPECT_EQ(vec, (vector<int>{1, 2, 3, 5}));

    vec.insert(vec.begin() + 3, 4);
    vec.insert(vec.begin(), 0);
    vec.insert(vec.end(), 6);

    EXPECT_EQ(vec, (vector<int>{0, 1, 2, 3, 4, 5, 6}));
}

TEST(Vector, InsertMultiple) {
    vector<int> vec{1, 4};

    vec.reserve(16ULL);

    auto it = vec.insert(vec.begin() + 1, {2, 3});

    EXPECT_EQ(it, vec.begin() + 1);
    EXPECT_EQ(vec, (vector<int>{1, 2, 3, 4}));

    vec.insert(vec.end(), 3ULL, 5);

    EXPECT_EQ(vec, (vector<int>{1, 2, 3, 4, 5, 5, 5}));
}

TEST(Vector, Erase) {
    vector<int> vec{1, 2, 3, 4, 5};

    auto it = vec.erase(vec.begin() + 1);

    ASSERT_EQ(it, vec.begin() + 1);
    EXPECT_EQ(*it, 3);
    EXPECT_EQ(vec, (vector<int>{1, 3, 4, 5}));

    it = vec.erase(vec.begin() + 1, vec.begin() + 3);

    ASSERT_EQ(it, vec.begin() + 1);
    EXPECT_EQ(*it, 5);
    EXPECT_EQ(vec, (vector<int>{1, 5}));

    it = vec.erase(vec.begin(), vec.end());

    EXPECT_EQ(it, vec.end());
    EXPECT_TRUE(vec.empty());
}

TEST(Vector, Iteration) {
    vector<int> vec{1, 2, 3, 4};

    for (auto& value : vec) {
        value *= 2;
    }

    EXPECT_EQ(vec, (vector<int>{2, 4, 6, 8}));

    int expected = 8;

    for (auto it = vec.rbegin(); it != vec.rend(); ++it) {
        EXPECT_EQ(*it, expected);
        expected -= 2;
    }

    EXPECT_EQ(expected, 0);
}

TEST(Vector, Assign) {
    vector<int> vec{1, 2, 3};

    vec.assign(4ULL, 42);

    EXPECT_EQ(vec, (vector<int>{42, 42, 42, 42}));

    vec.assign({1, 2});

    EXPECT_EQ(vec, (vector<int>{1, 2}));

    const std::array values{3, 4, 5};

    vec.assign_range(values);

    EXPECT_EQ(vec, (vector<int>{3, 4, 5}));
}

TEST(Vector, RangeOperations) {
    vector<int> vec{1, 4};

    const std::array middle{2, 3};
    const std::array tail{5, 6};

    vec.insert_range(vec.begin() + 1, middle);
    vec.append_range(tail);

    EXPECT_EQ(vec, (vector<int>{1, 2, 3, 4, 5, 6}));
}

TEST(Vector, CopyAndMove) {
    vector<std::string> original{"one", "two"};

    auto copy = original;

    EXPECT_EQ(copy, original);
    EXPECT_NE(copy.data(), original.data());

    copy[0ULL] = "changed";

    EXPECT_EQ(original[0ULL], "one");
    EXPECT_EQ(copy[0ULL], "changed");

    const auto* data = copy.data();

    auto moved = std::move(copy);

    EXPECT_EQ(moved.data(), data);
    EXPECT_EQ(moved, (vector<std::string>{"changed", "two"}));
    EXPECT_TRUE(copy.empty());

    copy.emplace_back("new");

    EXPECT_EQ(copy, (vector<std::string>{"new"}));
}

TEST(Vector, Assignment) {
    vector<std::string> lhs{"one", "two"};
    vector<std::string> rhs{"three"};

    rhs = lhs;

    EXPECT_EQ(rhs, lhs);
    EXPECT_NE(rhs.data(), lhs.data());

    rhs = std::move(lhs);

    EXPECT_EQ(rhs, (vector<std::string>{"one", "two"}));
    EXPECT_TRUE(lhs.empty());

    rhs = {"four", "five"};

    EXPECT_EQ(rhs, (vector<std::string>{"four", "five"}));
}

TEST(Vector, Swap) {
    vector<int> lhs{1, 2, 3};
    vector<int> rhs{4, 5};

    const auto* lhs_data = lhs.data();
    const auto* rhs_data = rhs.data();

    lhs.swap(rhs);

    EXPECT_EQ(lhs, (vector<int>{4, 5}));
    EXPECT_EQ(rhs, (vector<int>{1, 2, 3}));

    EXPECT_EQ(lhs.data(), rhs_data);
    EXPECT_EQ(rhs.data(), lhs_data);
}

TEST(Vector, Comparison) {
    const vector<int> lhs{1, 2, 3};
    const vector<int> rhs{1, 2, 4};
    const vector<int> prefix{1, 2};

    EXPECT_EQ(lhs, (vector<int>{1, 2, 3}));
    EXPECT_NE(lhs, rhs);
    EXPECT_LT(lhs, rhs);
    EXPECT_GT(rhs, lhs);
    EXPECT_LT(prefix, lhs);
}

TEST(Vector, Growth) {
    vector<std::string> vec;

    constexpr int n = 1000;

    for (int i = 0; i < n; ++i) {
        vec.emplace_back(std::to_string(i));
    }

    ASSERT_EQ(vec.size(), 1000ULL);
    EXPECT_GE(vec.capacity(), vec.size());

    for (int i = 0; i < n; ++i) {
        EXPECT_EQ(vec[static_cast<std::size_t>(i)],
                  std::to_string(i));
    }
}

TEST(Vector, NonTrivialInsertionAndErase) {
    vector<std::string> vec{"one", "three"};

    vec.reserve(16ULL);

    const auto* data = vec.data();

    vec.emplace(vec.begin() + 1, "two");

    EXPECT_EQ(vec.data(), data);
    EXPECT_EQ(vec, (vector<std::string>{"one", "two", "three"}));

    vec.erase(vec.begin() + 1);

    EXPECT_EQ(vec.data(), data);
    EXPECT_EQ(vec, (vector<std::string>{"one", "three"}));
}

TEST(Vector, MoveOnlyElements) {
    vector<std::unique_ptr<int>> vec;

    vec.push_back(std::make_unique<int>(10));
    vec.push_back(std::make_unique<int>(30));

    vec.reserve(16ULL);

    vec.insert(vec.begin() + 1, std::make_unique<int>(20));

    ASSERT_EQ(vec.size(), 3ULL);
    ASSERT_NE(vec[0ULL], nullptr);
    ASSERT_NE(vec[1ULL], nullptr);
    ASSERT_NE(vec[2ULL], nullptr);

    EXPECT_EQ(*vec[0ULL], 10);
    EXPECT_EQ(*vec[1ULL], 20);
    EXPECT_EQ(*vec[2ULL], 30);

    vec.erase(vec.begin() + 1);

    ASSERT_EQ(vec.size(), 2ULL);
    EXPECT_EQ(*vec.front(), 10);
    EXPECT_EQ(*vec.back(), 30);
}

TEST(Vector, AliasedPushBack) {
    vector<std::string> vec{"one", "two"};

    vec.shrink_to_fit();

    vec.push_back(vec.front());

    EXPECT_EQ(vec, (vector<std::string>{"one", "two", "one"}));
}

TEST(Vector, AliasedInsert) {
    vector<std::string> vec{"one", "two", "three"};

    vec.reserve(16ULL);

    vec.insert(vec.begin(), 2ULL, vec.back());

    EXPECT_EQ(vec, (vector<std::string>{
        "three", "three", "one", "two", "three"
    }));
}

TEST(Vector, SelfRangeInsert) {
    vector<int> vec{1, 2, 3, 4};

    vec.reserve(16ULL);

    vec.insert(vec.begin() + 1, vec.begin(), vec.end());

    EXPECT_EQ(vec, (vector<int>{1, 1, 2, 3, 4, 2, 3, 4}));
}

TEST(Vector, CapacityOverflow) {
    vector<int> vec;

    EXPECT_THROW(
        vec.reserve(vec.max_size() + 1ULL),
        std::length_error
    );

    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(vec.capacity(), 0ULL);
}