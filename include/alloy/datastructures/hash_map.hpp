#pragma once
#include "alloy/core/core.hpp"
#include "alloy/core/compare.hpp"
#include "alloy/core/lifetime.hpp"
#include "alloy/core/hash.hpp"

#include "alloy/alloc/allocator.hpp"
#include "alloy/alloc/allocation_guard.hpp"

#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

#ifndef ALLOY_HASH_MAP_USE_SSE2
#    if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#        define ALLOY_HASH_MAP_USE_SSE2 1
#    else
#        define ALLOY_HASH_MAP_USE_SSE2 0
#    endif
#endif

#if ALLOY_HASH_MAP_USE_SSE2
#    include <emmintrin.h>
#else
#    include <array>
#endif

namespace alloy::ds {
namespace details::hash_map {
    [[nodiscard]] constexpr auto reduced_hash(size_t hash) noexcept -> uint8_t {
        const auto h = static_cast<uint8_t>(hash);
        return static_cast<uint8_t>(h < 2 ? h + 8 : h);
    }

    [[nodiscard]] constexpr auto max_size_value(size_t a, size_t b) noexcept -> size_t {
        return a < b ? b : a;
    }

#if ALLOY_HASH_MAP_USE_SSE2
    struct group {
        static constexpr size_t group_size = 15;
        static constexpr uint8_t available = 0;
        static constexpr uint8_t sentinel = 1;

        void init() noexcept {
            _mm_store_si128(reinterpret_cast<__m128i*>(meta_), _mm_setzero_si128());
        }

        void set(size_t pos, size_t hash) noexcept {
            assert(pos < group_size);
            meta_[pos] = reduced_hash(hash);
        }

        void set_sentinel() noexcept {
            meta_[group_size - 1] = sentinel;
        }

        void reset(size_t pos) noexcept {
            assert(pos < group_size);
            meta_[pos] = available;
        }

        [[nodiscard]] auto is_sentinel(size_t pos) const noexcept -> bool {
            assert(pos < group_size);
            return meta_[pos] == sentinel;
        }

        [[nodiscard]] auto match(size_t hash) const noexcept -> int32_t {
            const auto fingerprint = _mm_set1_epi8(std::bit_cast<char>(reduced_hash(hash)));
            return _mm_movemask_epi8(_mm_cmpeq_epi8(load_metadata(), fingerprint)) & 0x7FFF;
        }

        [[nodiscard]] auto match_available() const noexcept -> int32_t {
            return _mm_movemask_epi8(_mm_cmpeq_epi8(load_metadata(), _mm_setzero_si128())) & 0x7FFF;
        }

        [[nodiscard]] auto match_occupied() const noexcept -> int32_t {
            return ~match_available() & 0x7FFF;
        }

        [[nodiscard]] auto is_occupied(size_t pos) const noexcept -> bool {
            assert(pos < group_size);
            return meta_[pos] != available;
        }

        [[nodiscard]] auto is_not_overflow(size_t hash) const noexcept -> bool {
            return (overflow() & (1U << (hash & 7))) == 0;
        }

        void mark_overflow(size_t hash) noexcept {
            overflow() |= static_cast<uint8_t>(1U << (hash & 7));
        }

        [[nodiscard]] auto maybe_caused_overflow_pos(size_t pos) const noexcept -> bool {
            assert(pos < group_size);

            const auto rh = meta_[pos];

            if (rh == available || rh == sentinel) {
                return false;
            }

            return !is_not_overflow(rh);
        }

    private:
        alignas(16) uint8_t meta_[16];

        [[nodiscard]] auto load_metadata() const noexcept -> __m128i {
            return _mm_load_si128(reinterpret_cast<const __m128i*>(meta_));
        }

        [[nodiscard]] auto overflow() noexcept -> uint8_t& {
            return meta_[group_size];
        }

        [[nodiscard]] auto overflow() const noexcept -> uint8_t {
            return meta_[group_size];
        }
    };

#else
    struct group {
        static constexpr size_t group_size = 15;
        static constexpr uint8_t available = 0;
        static constexpr uint8_t sentinel = 1;

        void init() noexcept {
            m[0] = 0;
            m[1] = 0;
        }

        void set(size_t pos, size_t hash) noexcept {
            assert(pos < group_size);
            set_impl(pos, reduced_hash(hash));
        }

        void set_sentinel() noexcept {
            set_impl(group_size - 1, sentinel);
        }

        void reset(size_t pos) noexcept {
            assert(pos < group_size);
            set_impl(pos, available);
        }

        [[nodiscard]] auto is_sentinel(size_t pos) const noexcept -> bool {
            assert(pos < group_size);

            const auto bit = uint64_t{1} << pos;
            const auto plane = plane_bits << pos;

            return (m[0] & plane) == bit && (m[1] & plane) == 0;
        }

        [[nodiscard]] auto match(size_t hash) const noexcept -> int32_t {
            return match_impl(reduced_hash(hash));
        }

        [[nodiscard]] auto match_available() const noexcept -> int32_t {
            const auto x = ~(m[0] | m[1]);
            const auto y = x & (x >> 32);

            return static_cast<int32_t>((y & (y >> 16)) & 0x7FFF);
        }

        [[nodiscard]] auto match_occupied() const noexcept -> int32_t {
            const auto x = m[0] | m[1];
            const auto y = x | (x >> 32);

            return static_cast<int32_t>((y | (y >> 16)) & 0x7FFF);
        }

        [[nodiscard]] auto is_occupied(size_t pos) const noexcept -> bool {
            assert(pos < group_size);

            return ((m[0] | m[1]) & (plane_bits << pos)) != 0;
        }

        [[nodiscard]] auto is_not_overflow(size_t hash) const noexcept -> bool {
            const size_t bit = 15 + 16 * (hash & 3);

            return (m[(hash >> 2) & 1] & (uint64_t{1} << bit)) == 0;
        }

        void mark_overflow(size_t hash) noexcept {
            const size_t bit = 15 + 16 * (hash & 3);

            m[(hash >> 2) & 1] |= uint64_t{1} << bit;
        }

        [[nodiscard]] auto maybe_caused_overflow_pos(size_t pos) const noexcept -> bool {
            assert(pos < group_size);

            if (!is_occupied(pos) || is_sentinel(pos)) {
                return false;
            }

            const auto x = (m[0] >> pos) & uint64_t{0x000100010001};

            const auto y = static_cast<uint32_t>(x | (x >> 15) | (x >> 30));

            return !is_not_overflow(y);
        }

    private:
        using word_type = uint64_t;

        static constexpr word_type plane_bits = 0x0001000100010001ULL;

        alignas(16) word_type m[2];

        [[nodiscard]] static constexpr auto set_pattern(size_t nibble) noexcept -> word_type {
            word_type result = 0;

            for (size_t bit = 0; bit < 4; ++bit) {
                result |= ((static_cast<word_type>(nibble) >> bit) & 1U) << (16 * bit);
            }

            return result;
        }

        static constexpr void set_impl(word_type& word, size_t pos, size_t nibble) noexcept {
            static constexpr auto patterns = [] {
                std::array<word_type, 16> result{};

                for (size_t i = 0; i < result.size(); ++i) {
                    result[i] = set_pattern(i);
                }

                return result;
            }();

            assert(pos < group_size);
            assert(nibble < patterns.size());

            word = (word & ~(plane_bits << pos)) | (patterns[nibble] << pos);
        }

        void set_impl(size_t pos, size_t fingerprint) noexcept {
            assert(pos < group_size);
            assert(fingerprint < 256);

            set_impl(m[0], pos, fingerprint & 0xF);
            set_impl(m[1], pos, fingerprint >> 4);
        }

        [[nodiscard]] constexpr auto match_impl(size_t fingerprint) const noexcept -> int32_t {
            static constexpr auto patterns = [] {
                std::array<word_type, 16> result{};

                for (size_t i = 0; i < result.size(); ++i) {
                    result[i] = set_pattern(i) * 0xFFFFULL;
                }

                return result;
            }();

            assert(fingerprint < 256);

            const auto x = ~((m[0] ^ patterns[fingerprint & 0xF]) | (m[1] ^ patterns[fingerprint >> 4]));

            const auto y = x & (x >> 32);

            return static_cast<int32_t>((y & (y >> 16)) & 0x7FFF);
        }
    };

#endif

    static_assert(sizeof(group) == 16);
    static_assert(alignof(group) == 16);
    static_assert(std::is_trivial_v<group>);

    struct prober {
        size_t pos;
        size_t step = 0;

        explicit constexpr prober(size_t initial) noexcept
            : pos(initial) {}

        [[nodiscard]] constexpr auto get() const noexcept -> size_t {
            return pos;
        }

        constexpr auto next(size_t mask) noexcept -> bool {
            ++step;
            pos = (pos + step) & mask;

            return step <= mask;
        }
    };

    template<class K, class V>
    inline constexpr bool nothrow_transfer =
        (std::is_nothrow_move_constructible_v<K> || std::is_nothrow_copy_constructible_v<K>) &&
        (std::is_nothrow_move_constructible_v<V> || std::is_nothrow_copy_constructible_v<V>);

    template<class E, class L, class R>
    concept compares_keys =
        std::predicate<const E&, const L&, const R&> && requires(const E& eq, const L& lhs, const R& rhs) {
            { eq(lhs, rhs) } -> std::convertible_to<bool>;
        };

    template<class K, class V, class H, class E>
    concept map_types = std::move_constructible<K> && std::move_constructible<V> &&
                        (std::is_copy_constructible_v<K> || nothrow_transfer<K, V> ||
                         alloy::is_trivially_relocatable<std::pair<K, V>>) &&
                        std::copy_constructible<H> && std::copy_constructible<E> &&
                        alloy::hashing::hashable_with<H, K> && compares_keys<E, K, K>;

    template<class T>
    inline constexpr bool is_pair = false;

    template<class A, class B>
    inline constexpr bool is_pair<std::pair<A, B>> = true;

    template<class P, class K, class V>
    concept pair_source = is_pair<std::remove_cvref_t<P>> && requires(P&& p) {
        requires std::constructible_from<K, decltype((std::forward<P>(p).first))>;
        requires std::constructible_from<V, decltype((std::forward<P>(p).second))>;
    };

    template<class R>
    struct arrow_proxy {
        R ref;

        [[nodiscard]] auto operator->() const noexcept -> const R* {
            return alloy::addressof(ref);
        }
    };
} // namespace details::hash_map

template<typename Key, typename Val, typename Hash = alloy::hash<Key>, typename KeyEqual = alloy::equal_to<Key>>
    requires details::hash_map::map_types<Key, Val, Hash, KeyEqual>
struct hash_map {
private:
    using storage_type = std::pair<Key, Val>;

    template<class K, class V, class H, class E>
        requires details::hash_map::map_types<K, V, H, E>
    friend struct hash_map;

    static constexpr bool nothrow_policy_swap =
        std::is_nothrow_swappable_v<Hash> && std::is_nothrow_swappable_v<KeyEqual>;
    static constexpr bool can_replace_policies =
        nothrow_policy_swap || (std::is_copy_assignable_v<Hash> && std::is_copy_assignable_v<KeyEqual>);

public:
    using key_type = Key;
    using mapped_type = Val;
    using value_type = std::pair<const Key, Val>;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using hasher = Hash;
    using key_equal = KeyEqual;
    using allocator_type = alloy::alloc::allocator;
    using is_trivially_relocatable =
        std::conditional_t<alloy::is_trivially_relocatable<allocator_type> && alloy::is_trivially_relocatable<Hash> &&
                               alloy::is_trivially_relocatable<KeyEqual>,
                           void,
                           bool>;

    using reference = std::pair<const Key&, Val&>;
    using const_reference = std::pair<const Key&, const Val&>;
    using pointer = details::hash_map::arrow_proxy<reference>;
    using const_pointer = details::hash_map::arrow_proxy<const_reference>;

    template<bool IsConst>
    struct basic_iterator {
        using map_type = std::conditional_t<IsConst, const hash_map, hash_map>;

        using value_type = hash_map::value_type;
        using difference_type = hash_map::difference_type;

        using reference = std::conditional_t<IsConst, hash_map::const_reference, hash_map::reference>;
        using pointer = std::conditional_t<IsConst, hash_map::const_pointer, hash_map::pointer>;
        using rvalue_reference = std::pair<const Key&, std::conditional_t<IsConst, const Val&&, Val&&>>;

        using iterator_category = std::input_iterator_tag;
        using iterator_concept = std::forward_iterator_tag;

        basic_iterator() = default;
        ~basic_iterator() = default;
        basic_iterator(const basic_iterator&) = default;
        basic_iterator(basic_iterator&&) noexcept = default;

        auto operator=(const basic_iterator&) -> basic_iterator& = default;

        auto operator=(basic_iterator&&) noexcept -> basic_iterator& = default;

        basic_iterator(const basic_iterator<false>& other)
            requires IsConst
            : map_(other.map_)
            , group_(other.group_)
            , slot_(other.slot_) {}

        [[nodiscard]] auto operator*() const noexcept -> reference {
            auto& value = map_->data_[group_ * group_size + slot_];
            return {value.first, value.second};
        }

        [[nodiscard]] auto operator->() const noexcept -> pointer {
            return {**this};
        }

        friend auto iter_move(const basic_iterator& it) noexcept -> rvalue_reference {
            auto ref = *it;
            return {ref.first, std::move(ref.second)};
        }

        auto operator++() noexcept -> basic_iterator& {
            advance();
            return *this;
        }

        auto operator++(int) noexcept -> basic_iterator {
            auto tmp = *this;
            advance();
            return tmp;
        }

        friend auto operator==(const basic_iterator& lhs, const basic_iterator& rhs) noexcept -> bool {
            return lhs.map_ == rhs.map_ && lhs.group_ == rhs.group_ && lhs.slot_ == rhs.slot_;
        }

    private:
        map_type* map_ = nullptr;
        size_type group_ = 0;
        uint32_t slot_ = 0;

        friend struct hash_map;
        friend struct basic_iterator<!IsConst>;

        basic_iterator(map_type* map, size_type group, uint32_t slot) noexcept
            : map_(map)
            , group_(group)
            , slot_(slot) {}

        void to_end() noexcept {
            if (map_) {
                group_ = map_->group_mask_ + 1;
                slot_ = 0;
            }
        }

        void seek_first() noexcept {
            if (map_ == nullptr || map_->len_ == 0) {
                to_end();
                return;
            }

            for (size_type g = 0; g <= map_->group_mask_; ++g) {
                const auto mask = map_->match_really_occupied(g);

                if (mask != 0) {
                    group_ = g;
                    slot_ = std::countr_zero(static_cast<uint32_t>(mask));
                    return;
                }
            }

            to_end();
        }

        void advance() noexcept {
            if (map_ == nullptr || group_ > map_->group_mask_) {
                to_end();
                return;
            }

            const auto mask = static_cast<uint32_t>(map_->match_really_occupied(group_));

            const auto remaining = mask & ~((uint32_t{1} << (slot_ + 1)) - 1);

            if (remaining != 0) {
                slot_ = std::countr_zero(remaining);
                return;
            }

            for (size_type g = group_ + 1; g <= map_->group_mask_; ++g) {
                const auto next = map_->match_really_occupied(g);

                if (next != 0) {
                    group_ = g;
                    slot_ = std::countr_zero(static_cast<uint32_t>(next));
                    return;
                }
            }

            to_end();
        }
    };

    using iterator = basic_iterator<false>;
    using const_iterator = basic_iterator<true>;

    explicit hash_map(allocator_type alloc = {}, const hasher& hash = hasher{}, const key_equal& equal = key_equal{})
        : alloc_(alloc)
        , hash_(hash)
        , eq_(equal) {}

    explicit hash_map(size_type bucket_count,
                      allocator_type alloc = {},
                      const hasher& hash = hasher{},
                      const key_equal& equal = key_equal{})
        : alloc_(alloc)
        , hash_(hash)
        , eq_(equal) {
        allocate(bucket_count);
    }

    template<std::input_iterator InputIt>
        requires std::constructible_from<storage_type, std::iter_reference_t<InputIt>>
    hash_map(InputIt first,
             InputIt last,
             size_type bucket_count = 0,
             allocator_type alloc = {},
             const hasher& hash = hasher{},
             const key_equal& equal = key_equal{})
        : hash_map(bucket_count, alloc, hash, equal) {
        insert(first, last);
    }

    hash_map(const hash_map& other)
        requires std::is_copy_constructible_v<Key> && std::is_copy_constructible_v<Val>
        : hash_map(other, alloy::alloc::default_allocator()) {}

    hash_map(const hash_map& other, allocator_type alloc)
        requires std::is_copy_constructible_v<Key> && std::is_copy_constructible_v<Val>
        : hash_map(other.capacity_raw(), alloc, other.hash_, other.eq_) {
        copy_layout_from(other);
    }

    hash_map(hash_map&& other) noexcept(std::is_nothrow_copy_constructible_v<Hash> &&
                                        std::is_nothrow_copy_constructible_v<KeyEqual>)
        : alloc_(other.alloc_)
        , hash_(other.hash_)
        , eq_(other.eq_) {
        take_storage(other);
    }

    hash_map(hash_map&& other, allocator_type alloc)
        : hash_map(alloc, other.hash_, other.eq_) {
        if (allocators_compatible(other)) {
            take_storage(other);
        } else {
            allocate(other.capacity_raw());
            move_layout_from(other);
            other.clear();
        }
    }

    auto operator=(const hash_map& other) -> hash_map&
        requires std::is_copy_constructible_v<Key> && std::is_copy_constructible_v<Val> && can_replace_policies
    {
        if (this != &other) {
            hash_map tmp(other, alloc_);
            replace_with(tmp);
        }
        return *this;
    }

    // NOLINTNEXTLINE(bugprone-unsafe-to-allow-exceptions)
    auto operator=(hash_map&& other) noexcept(false) -> hash_map&
        requires can_replace_policies
    {
        if (this == &other) {
            return *this;
        }

        if (allocators_compatible(other)) {
            replace_with(other);
        } else if constexpr (nothrow_policy_swap) {
            hash_map tmp(other.capacity_raw(), alloc_, other.hash_, other.eq_);
            tmp.move_layout_from(other);
            other.clear();
            replace_with(tmp);
        } else {
            hash_map tmp(other.capacity_raw(), alloc_, other.hash_, other.eq_);
            replace_with(tmp);
            try {
                move_layout_from(other);
            } catch (...) {
                clear();
                throw;
            }
            other.clear();
        }
        return *this;
    }

    ~hash_map() {
        destroy_and_dealloc();
    }

    [[nodiscard]] auto begin() noexcept -> iterator {
        iterator it(this, 0, 0);
        it.seek_first();
        return it;
    }

    [[nodiscard]] auto begin() const noexcept -> const_iterator {
        const_iterator it(this, 0, 0);
        it.seek_first();
        return it;
    }

    [[nodiscard]] auto cbegin() const noexcept -> const_iterator {
        return begin();
    }

    [[nodiscard]] auto end() noexcept -> iterator {
        return iterator(this, group_mask_ + 1, 0);
    }

    [[nodiscard]] auto end() const noexcept -> const_iterator {
        return const_iterator(this, group_mask_ + 1, 0);
    }

    [[nodiscard]] auto cend() const noexcept -> const_iterator {
        return end();
    }

    [[nodiscard]] auto get_allocator() const noexcept -> allocator_type {
        return alloc_;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return len_ == 0;
    }

    [[nodiscard]] auto size() const noexcept -> size_type {
        return len_;
    }

    [[nodiscard]] auto max_size() const noexcept -> size_type {
        constexpr auto ng = max_groups();
        return ng ? load_limit(ng * group_size - 1) : 0;
    }

    [[nodiscard]] auto load_factor() const noexcept -> float {
        const auto capacity = capacity_raw();

        return capacity != 0 ? static_cast<float>(len_) / static_cast<float>(capacity) : 0.0F;
    }

    [[nodiscard]] auto max_load_factor() const noexcept -> float {
        return max_load;
    }

    void clear() noexcept {
        destroy();
        len_ = 0;
        reset_metadata();
    }

    void swap(hash_map& other) noexcept
        requires nothrow_policy_swap
    {
        if (this == &other) {
            return;
        }
        if (!allocators_compatible(other)) [[unlikely]] {
            std::terminate();
        }

        using std::swap;
        swap(hash_, other.hash_);
        swap(eq_, other.eq_);
        swap(groups_, other.groups_);
        swap(data_, other.data_);
        swap(group_mask_, other.group_mask_);
        swap(groups_size_index_, other.groups_size_index_);
        swap(len_, other.len_);
        swap(max_len_, other.max_len_);
    }

    auto insert(const value_type& value) -> std::pair<iterator, bool>
        requires std::is_copy_constructible_v<Key> && std::is_copy_constructible_v<Val>
    {
        return insert_value(value);
    }

    auto insert(value_type&& value) -> std::pair<iterator, bool>
        requires std::is_copy_constructible_v<Key>
    {
        return insert_value(std::move(value));
    }

    template<class P>
        requires details::hash_map::pair_source<P, Key, Val>
    auto insert(P&& value) -> std::pair<iterator, bool> {
        return insert_value(std::forward<P>(value));
    }

    auto insert(const_iterator, const value_type& value) -> iterator
        requires std::is_copy_constructible_v<Key> && std::is_copy_constructible_v<Val>
    {
        return insert(value).first;
    }

    auto insert(const_iterator, value_type&& value) -> iterator
        requires std::is_copy_constructible_v<Key>
    {
        return insert(std::move(value)).first;
    }

    template<class P>
        requires details::hash_map::pair_source<P, Key, Val>
    auto insert(const_iterator, P&& value) -> iterator {
        return insert(std::forward<P>(value)).first;
    }

    template<class InputIt>
        requires std::input_iterator<InputIt> && std::constructible_from<storage_type, std::iter_reference_t<InputIt>>
    void insert(InputIt first, InputIt last) {
        for (; first != last; ++first) {
            emplace(*first);
        }
    }

    template<class K, class M>
        requires std::constructible_from<Key, K&&> && std::constructible_from<Val, M&&>
    auto emplace(K&& key, M&& value) -> std::pair<iterator, bool> {
        return emplace_key(std::forward<K>(key), std::forward<M>(value));
    }

    template<class... Ks, class... Vs>
        requires std::constructible_from<Key, Ks...> && std::constructible_from<Val, Vs...>
    auto emplace(std::piecewise_construct_t, std::tuple<Ks...> keys, std::tuple<Vs...> values)
        -> std::pair<iterator, bool> {
        return std::apply(
            [&](auto&&... key_args) {
                return std::apply(
                    [&](auto&&... value_args) {
                        if constexpr (sizeof...(Ks) == 1) {
                            return emplace_key(std::forward<decltype(key_args)>(key_args)...,
                                               std::forward<decltype(value_args)>(value_args)...);
                        } else {
                            Key key(std::forward<decltype(key_args)>(key_args)...);
                            return try_emplace_impl(std::move_if_noexcept(key),
                                                    std::forward<decltype(value_args)>(value_args)...);
                        }
                    },
                    std::move(values));
            },
            std::move(keys));
    }

    template<class P>
        requires details::hash_map::pair_source<P, Key, Val>
    auto emplace(P&& value) -> std::pair<iterator, bool> {
        return insert_value(std::forward<P>(value));
    }

    template<class... Args>
        requires std::constructible_from<storage_type, Args...>
    auto emplace(Args&&... args) -> std::pair<iterator, bool> {
        storage_type value(std::forward<Args>(args)...);
        return try_emplace_impl(std::move_if_noexcept(value.first), std::move_if_noexcept(value.second));
    }

    template<class... Args>
        requires requires(hash_map& map, Args&&... args) { map.emplace(std::forward<Args>(args)...); }
    auto emplace_hint(const_iterator, Args&&... args) -> iterator {
        return emplace(std::forward<Args>(args)...).first;
    }

    template<class... Args>
        requires std::is_copy_constructible_v<Key> && std::constructible_from<mapped_type, Args...>
    auto try_emplace(const key_type& key, Args&&... args) -> std::pair<iterator, bool> {
        return try_emplace_impl(key, std::forward<Args>(args)...);
    }

    template<class... Args>
        requires std::constructible_from<mapped_type, Args...>
    auto try_emplace(key_type&& key, Args&&... args) -> std::pair<iterator, bool> {
        return try_emplace_impl(std::move(key), std::forward<Args>(args)...);
    }

    template<class... Args>
        requires std::is_copy_constructible_v<Key> && std::constructible_from<mapped_type, Args...>
    auto try_emplace(const_iterator, const key_type& key, Args&&... args) -> iterator {
        return try_emplace(key, std::forward<Args>(args)...).first;
    }

    template<class... Args>
        requires std::constructible_from<mapped_type, Args...>
    auto try_emplace(const_iterator, key_type&& key, Args&&... args) -> iterator {
        return try_emplace(std::move(key), std::forward<Args>(args)...).first;
    }

    template<class M>
        requires std::is_copy_constructible_v<Key> && std::is_assignable_v<mapped_type&, M&&> &&
                 std::constructible_from<mapped_type, M&&>
    auto insert_or_assign(const key_type& key, M&& value) -> std::pair<iterator, bool> {
        auto [it, inserted] = try_emplace(key, std::forward<M>(value));

        if (!inserted) {
            it->second = std::forward<M>(value);
        }

        return {it, inserted};
    }

    template<class M>
        requires std::is_assignable_v<mapped_type&, M&&> && std::constructible_from<mapped_type, M&&>
    auto insert_or_assign(key_type&& key, M&& value) -> std::pair<iterator, bool> {
        auto [it, inserted] = try_emplace(std::move(key), std::forward<M>(value));

        if (!inserted) {
            it->second = std::forward<M>(value);
        }

        return {it, inserted};
    }

    template<class M>
        requires std::is_copy_constructible_v<Key> && std::is_assignable_v<mapped_type&, M&&> &&
                 std::constructible_from<mapped_type, M&&>
    auto insert_or_assign(const_iterator, const key_type& key, M&& value) -> iterator {
        return insert_or_assign(key, std::forward<M>(value)).first;
    }

    template<class M>
        requires std::is_assignable_v<mapped_type&, M&&> && std::constructible_from<mapped_type, M&&>
    auto insert_or_assign(const_iterator, key_type&& key, M&& value) -> iterator {
        return insert_or_assign(std::move(key), std::forward<M>(value)).first;
    }

    auto erase(iterator pos) -> iterator {
        return erase(const_iterator(pos));
    }

    auto erase(const_iterator pos) -> iterator {
        assert(pos.map_ == this);
        assert(pos != cend());

        auto next = pos;
        ++next;

        erase_impl({.group = pos.group_, .slot = pos.slot_, .ptr = data_ + pos.group_ * group_size + pos.slot_});

        return iterator(this, next.group_, next.slot_);
    }

    auto erase(const key_type& key) -> size_type {
        const auto loc = find_impl(key);

        if (!loc) {
            return 0;
        }

        erase_impl(loc);
        return 1;
    }

    auto erase(const_iterator first, const_iterator last) -> iterator {
        while (first != last) {
            first = erase(first);
        }

        return iterator(this, last.group_, last.slot_);
    }

    [[nodiscard]] auto find(this auto& self, const key_type& key) {
        using self_t = std::remove_reference_t<decltype(self)>;

        using it = std::conditional_t<std::is_const_v<self_t>, const_iterator, iterator>;

        const auto loc = self.find_impl(key);

        if (!loc) {
            return self.end();
        }

        return it(&self, loc.group, loc.slot);
    }

    template<class K>
        requires alloy::hashing::transparent<Hash> && alloy::hashing::transparent<KeyEqual> &&
                 alloy::hashing::hashable_with<Hash, K> && details::hash_map::compares_keys<KeyEqual, Key, K>
    [[nodiscard]] auto find(this auto& self, const K& key) {
        using self_t = std::remove_reference_t<decltype(self)>;

        using it = std::conditional_t<std::is_const_v<self_t>, const_iterator, iterator>;

        const auto loc = self.find_impl(key);

        if (!loc) {
            return self.end();
        }

        return it(&self, loc.group, loc.slot);
    }

    [[nodiscard]] auto contains(const key_type& key) const -> bool {
        return static_cast<bool>(find_impl(key));
    }

    template<class K>
        requires alloy::hashing::transparent<Hash> && alloy::hashing::transparent<KeyEqual> &&
                 alloy::hashing::hashable_with<Hash, K> && details::hash_map::compares_keys<KeyEqual, Key, K>
    [[nodiscard]] auto contains(const K& key) const -> bool {
        return static_cast<bool>(find_impl(key));
    }

    [[nodiscard]] auto count(const key_type& key) const -> size_type {
        return contains(key);
    }

    template<class K>
        requires alloy::hashing::transparent<Hash> && alloy::hashing::transparent<KeyEqual> &&
                 alloy::hashing::hashable_with<Hash, K> && details::hash_map::compares_keys<KeyEqual, Key, K>
    [[nodiscard]] auto count(const K& key) const -> size_type {
        return contains(key);
    }

    [[nodiscard]] auto equal_range(this auto& self, const key_type& key) {
        auto it = self.find(key);
        auto next = it;

        if (it != self.end()) {
            ++next;
        }

        return std::pair{it, next};
    }

    auto operator[](const key_type& key) -> mapped_type&
        requires std::is_copy_constructible_v<Key> && std::default_initializable<mapped_type>
    {
        return try_emplace(key).first->second;
    }

    auto operator[](key_type&& key) -> mapped_type&
        requires std::default_initializable<mapped_type>
    {
        return try_emplace(std::move(key)).first->second;
    }

    [[nodiscard]] auto at(this auto& self, const key_type& key) -> decltype(auto) {
        auto it = self.find(key);

        if (it == self.end()) [[unlikely]] {
            throw std::out_of_range("hash_map::at");
        }

        return (it->second);
    }

    [[nodiscard]] auto hash_function() const -> hasher {
        return hash_;
    }

    [[nodiscard]] auto key_eq() const -> key_equal {
        return eq_;
    }

    void reserve(size_type element_count) {
        if (element_count > max_len_) {
            rehash(details::hash_map::max_size_value(capacity_raw(), slots_for_elements(element_count)));
        }
    }

    void rehash(size_type bucket_count) {
        bucket_count = details::hash_map::max_size_value(bucket_count, slots_for_elements(len_));
        if (bucket_count == 0) {
            destroy_and_dealloc();
            return;
        }
        if (groups_ && groups_for(bucket_count) == num_groups() && max_len_ == load_limit(capacity_raw())) {
            return;
        }

        hash_map tmp(bucket_count, alloc_, hash_, eq_);
        relocate_into(tmp);
        destroy_and_dealloc();
        take_storage(tmp);
    }

    template<class H2, class E2>
    void merge(hash_map<Key, Val, H2, E2>& src) {
        if constexpr (std::same_as<Hash, H2> && std::same_as<KeyEqual, E2>) {
            if (this == &src)
                return;
        }
        for (auto it = src.begin(); it != src.end();) {
            auto& value = src.storage_at(it);
            const auto h = hash_key(value.first);
            if (len_ >= max_len_) [[unlikely]] {
                if (find_hashed(value.first, h)) {
                    ++it;
                    continue;
                }
                rehash(recommended_growth_capacity());
            }
            const auto loc = insert_impl(value.first, h);
            if (loc.existing) {
                ++it;
                continue;
            }
            if constexpr (alloy::is_trivially_relocatable<storage_type>) {
                auto next = it;
                ++next;
                auto* slot = data_ + loc.insert_group * group_size + loc.insert_slot;
                alloy::relocate_n(slot, alloy::addressof(value), 1);
                commit_insert(loc);
                src.recover_relocated(it);
                it = next;
            } else {
                with_transfer_args(value, [&](auto&&... args) {
                    finish_insert(loc, std::forward<decltype(args)>(args)...);
                });
                it = src.erase(it);
            }
        }
    }

    template<class H2, class E2>
    void merge(hash_map<Key, Val, H2, E2>&& src) {
        merge(src);
    }

private:
    using group = details::hash_map::group;
    using prober = details::hash_map::prober;

    static constexpr size_type group_size = group::group_size;
    static constexpr float max_load = 0.875F;

    const allocator_type alloc_;

    group* groups_ = nullptr;
    storage_type* data_ = nullptr;

    size_type group_mask_ = 0;
    size_type groups_size_index_ = 0;

    size_type len_ = 0;
    size_type max_len_ = 0;

    ALLOY_NO_UNIQUE_ADDRESS Hash hash_;
    ALLOY_NO_UNIQUE_ADDRESS KeyEqual eq_;

    struct locator {
        size_type group = 0;
        uint32_t slot = 0;
        storage_type* ptr = nullptr;

        explicit operator bool() const noexcept {
            return ptr != nullptr;
        }
    };

    struct insert_location {
        locator existing;

        size_type hash = 0;
        size_type insert_group = 0;
        uint32_t insert_slot = 0;

        bool can_insert = false;
        bool needs_overflow = false;
        prober first_unmarked{0};
    };

    template<class K>
    [[nodiscard]] auto hash_key(const K& key) const noexcept(noexcept(alloy::hashing::hash_key(hash_, key)))
        -> size_type {
        return alloy::hashing::hash_key(hash_, key);
    }

    [[nodiscard]] auto num_groups() const noexcept -> size_type {
        return group_mask_ + 1;
    }

    [[nodiscard]] auto capacity_raw() const noexcept -> size_type {
        return groups_ ? num_groups() * group_size - 1 : 0;
    }

    [[nodiscard]] auto position_for(size_type hash) const noexcept -> size_type {
        return (hash >> groups_size_index_) & group_mask_;
    }

    [[nodiscard]] auto match_really_occupied(size_type group) const noexcept -> int32_t {
        auto mask = groups_[group].match_occupied();

        if (group == group_mask_) {
            mask &= ~(int32_t{1} << (group_size - 1));
        }

        return mask;
    }

    [[nodiscard]] static constexpr auto load_limit(size_type slots) noexcept -> size_type {
        return slots - slots / 8 - (slots % 8 != 0);
    }

    [[nodiscard]] static auto slots_for_elements(size_type n) -> size_type {
        const auto extra = n / 7 + (n % 7 != 0);

        if (n > std::numeric_limits<size_type>::max() - extra) {
            throw std::length_error("hash_map capacity overflow");
        }

        return n + extra;
    }

    static constexpr size_type storage_alignment =
        details::hash_map::max_size_value(alignof(group), alignof(storage_type));

    [[nodiscard]] static constexpr auto max_groups() noexcept -> size_type {
        constexpr auto max = static_cast<size_type>(std::numeric_limits<difference_type>::max());
        constexpr auto padding = storage_alignment - 1;
        if constexpr (sizeof(storage_type) > (max - sizeof(group)) / group_size) {
            return 0;
        } else {
            constexpr auto stride = sizeof(group) + group_size * sizeof(storage_type);
            return std::bit_floor((max - padding) / stride);
        }
    }

    struct allocation_layout {
        size_type data_offset;
        size_type bytes;
    };

    [[nodiscard]] static constexpr auto layout_for(size_type ng) noexcept -> allocation_layout {
        assert(ng != 0 && ng <= max_groups());
        constexpr auto padding = alignof(storage_type) - 1;
        const auto offset = (sizeof(group) * ng + padding) & ~padding;
        return {offset, offset + (ng * group_size - 1) * sizeof(storage_type)};
    }

    [[nodiscard]] static auto groups_for(size_type capacity) -> size_type {
        const auto needed = capacity / group_size + 1;
        if (needed > max_groups()) {
            throw std::length_error("hash_map capacity overflow");
        }

        return std::bit_ceil(needed);
    }

    [[nodiscard]] auto allocators_compatible(const hash_map& other) const noexcept -> bool {
        return alloc_ == other.alloc_;
    }

    void take_storage(hash_map& other) noexcept {
        assert(!groups_ && !data_);
        groups_ = other.groups_;
        data_ = other.data_;
        group_mask_ = other.group_mask_;
        groups_size_index_ = other.groups_size_index_;
        len_ = other.len_;
        max_len_ = other.max_len_;
        other.reset_state();
    }

    void replace_with(hash_map& other) noexcept(nothrow_policy_swap || (std::is_nothrow_copy_assignable_v<Hash> &&
                                                                        std::is_nothrow_copy_assignable_v<KeyEqual>)) {
        destroy_and_dealloc();
        if constexpr (nothrow_policy_swap) {
            using std::swap;
            swap(hash_, other.hash_);
            swap(eq_, other.eq_);
        } else {
            hash_ = other.hash_;
            eq_ = other.eq_;
        }
        take_storage(other);
    }

    void reset_state() noexcept {
        groups_ = nullptr;
        data_ = nullptr;

        group_mask_ = 0;
        groups_size_index_ = 0;

        len_ = 0;
        max_len_ = 0;
    }

    void reset_metadata() noexcept {
        if (groups_ == nullptr) {
            return;
        }

        const auto ng = num_groups();

        for (size_type i = 0; i < ng; ++i) {
            groups_[i].init();
        }

        groups_[ng - 1].set_sentinel();

        max_len_ = load_limit(ng * group_size - 1);
    }

    void allocate(size_type capacity) {
        assert(!groups_ && !data_);

        if (capacity == 0) {
            return;
        }

        const auto ng = groups_for(capacity);
        const auto layout = layout_for(ng);

        auto* raw = alloc_.allocate(layout.bytes, storage_alignment);

        if (!raw) [[unlikely]] {
            throw std::bad_alloc{};
        }

        auto* bytes = ::new (raw) std::byte[layout.bytes];

        groups_ = ::new (static_cast<void*>(bytes)) group[ng]{};

        data_ = reinterpret_cast<storage_type*>(bytes + layout.data_offset);

        group_mask_ = ng - 1;

        constexpr auto bits = std::numeric_limits<size_type>::digits;

        groups_size_index_ = ng == 1 ? 0 : bits - std::bit_width(ng - 1);

        groups_[ng - 1].set_sentinel();

        max_len_ = load_limit(ng * group_size - 1);
    }

    void dealloc() noexcept {
        if (groups_) {
            const auto layout = layout_for(num_groups());
            alloc_.deallocate(groups_, layout.bytes, storage_alignment);
        }
        reset_state();
    }

    template<bool Nothrow, class Other, class Construct>
    void transfer_layout_from(Other& other, Construct&& construct) noexcept(Nothrow) {
        if (!other.groups_)
            return;
        assert(groups_ && num_groups() == other.num_groups() && len_ == 0);
        for (size_type g = 0; g <= group_mask_; ++g) {
            const auto occupied = static_cast<uint32_t>(other.match_really_occupied(g));
            auto remaining = occupied;
            auto* dst = data_ + g * group_size;
            using source_type = std::conditional_t<std::is_const_v<Other>, const storage_type, storage_type>;
            source_type* src = other.data_ + g * group_size;
            auto construct_group = [&] {
                while (remaining) {
                    const auto idx = std::countr_zero(remaining);
                    construct(dst + idx, src[idx]);
                    remaining &= remaining - 1;
                }
            };
            if constexpr (Nothrow) {
                construct_group();
            } else {
                try {
                    construct_group();
                } catch (...) {
                    auto constructed = occupied & ~remaining;
                    while (constructed) {
                        alloy::destroy_at(dst + std::countr_zero(constructed));
                        constructed &= constructed - 1;
                    }
                    throw;
                }
            }
            groups_[g] = other.groups_[g];
            len_ += std::popcount(occupied);
        }
        max_len_ = other.max_len_;
    }

    void copy_layout_from(const hash_map& other) {
        transfer_layout_from<std::is_nothrow_copy_constructible_v<storage_type>>(
            other, [](storage_type* dst, const storage_type& src) {
                alloy::construct_at(dst, src);
            });
    }

    void move_layout_from(hash_map& other) {
        if constexpr (alloy::is_trivially_relocatable<storage_type>) {
            transfer_layout_from<true>(other, [](storage_type* dst, storage_type& src) noexcept {
                alloy::relocate_n(dst, alloy::addressof(src), 1);
            });
            other.len_ = 0;
        } else {
            transfer_layout_from<details::hash_map::nothrow_transfer<Key, Val>>(
                other, [](storage_type* dst, storage_type& src) {
                    with_transfer_args(src, [dst](auto&&... args) {
                        alloy::construct_at(dst, std::forward<decltype(args)>(args)...);
                    });
                });
        }
    }

    void destroy() noexcept {
        if constexpr (std::is_trivially_destructible_v<storage_type>) {
            return;
        }

        if (len_ == 0) {
            return;
        }

        for (size_type g = 0; g <= group_mask_; ++g) {
            auto mask = match_really_occupied(g);

            while (mask != 0) {
                const auto idx = std::countr_zero(static_cast<uint32_t>(mask));

                alloy::destroy_at(data_ + g * group_size + idx);

                mask &= mask - 1;
            }
        }
    }

    void destroy_and_dealloc() noexcept {
        destroy();
        dealloc();
    }

    void recover_slot(size_type group, size_type slot) noexcept {
        assert(group <= group_mask_);
        assert(slot < group_size);
        assert(len_ != 0);

        auto& g = groups_[group];

        if (g.maybe_caused_overflow_pos(slot)) {
            if (max_len_ > 0) {
                --max_len_;
            }
        }

        g.reset(slot);
        --len_;
    }

    void recover_relocated(const_iterator it) noexcept {
        assert(it.map_ == this && it != cend());
        recover_slot(it.group_, it.slot_);
    }

    void erase_impl(const locator& loc) noexcept {
        assert(loc.ptr != nullptr);

        alloy::destroy_at(loc.ptr);
        recover_slot(loc.group, loc.slot);
    }

    [[nodiscard]] auto recommended_growth_capacity() const -> size_type {
        const auto current = capacity_raw();

        if (current == 0) {
            return group_size - 1;
        }

        const auto nominal = load_limit(current);
        if (len_ < nominal - nominal / 8) {
            return current;
        }
        return details::hash_map::max_size_value(current + 1, slots_for_elements(len_ + 1));
    }

    template<class... Args>
    auto finish_insert(const insert_location& loc, Args&&... args) -> std::pair<iterator, bool> {
        assert(loc.can_insert);
        assert(!loc.existing);

        auto* slot = data_ + loc.insert_group * group_size + loc.insert_slot;

        alloy::construct_at(slot, std::forward<Args>(args)...);
        return commit_insert(loc);
    }

    auto commit_insert(const insert_location& loc) noexcept -> std::pair<iterator, bool> {
        if (loc.needs_overflow) {
            for (auto p = loc.first_unmarked; p.get() != loc.insert_group;) {
                groups_[p.get()].mark_overflow(loc.hash);
                p.next(group_mask_);
            }
        }
        groups_[loc.insert_group].set(loc.insert_slot, loc.hash);

        ++len_;

        return {iterator(this, loc.insert_group, loc.insert_slot), true};
    }

    template<class K, class... Args>
    auto insert_with_key(const K& key, Args&&... args) -> std::pair<iterator, bool> {
        const auto h = hash_key(key);
        if (len_ >= max_len_) [[unlikely]] {
            if (const auto loc = find_hashed(key, h); loc) {
                return {iterator(this, loc.group, loc.slot), false};
            }

            hash_map tmp(recommended_growth_capacity(), alloc_, hash_, eq_);
            const auto loc = tmp.insert_unique(h, std::forward<Args>(args)...);
            relocate_into(tmp);
            destroy_and_dealloc();
            take_storage(tmp);
            return {iterator(this, loc.group, loc.slot), true};
        }

        const auto loc = insert_impl(key, h);
        if (loc.existing) {
            return {iterator(this, loc.existing.group, loc.existing.slot), false};
        }
        return finish_insert(loc, std::forward<Args>(args)...);
    }

    template<class K, class... Args>
    auto emplace_key(K&& source, Args&&... args) -> std::pair<iterator, bool> {
        if constexpr (std::same_as<std::remove_cvref_t<K>, Key>) {
            return try_emplace_impl(std::forward<K>(source), std::forward<Args>(args)...);
        } else {
            Key key(std::forward<K>(source));
            return try_emplace_impl(std::move_if_noexcept(key), std::forward<Args>(args)...);
        }
    }

    template<class V>
    auto insert_value(V&& value) -> std::pair<iterator, bool> {
        return emplace_key(std::forward<V>(value).first, std::forward<V>(value).second);
    }

    template<class K, class... Args>
    auto try_emplace_impl(K&& key, Args&&... args) -> std::pair<iterator, bool> {
        return insert_with_key(key,
                               std::piecewise_construct,
                               std::forward_as_tuple(std::forward<K>(key)),
                               std::forward_as_tuple(std::forward<Args>(args)...));
    }

    template<class K>
    [[nodiscard]] auto find_impl(const K& key) const -> locator {
        if (groups_ == nullptr) [[unlikely]] {
            return {};
        }

        return find_hashed(key, hash_key(key));
    }

    template<class K>
    [[nodiscard]] auto find_hashed(const K& key, size_type h) const -> locator {
        if (!groups_)
            return {};
        const auto pos0 = position_for(h);

        for (prober p(pos0);;) {
            const auto pos = p.get();
            const auto& g = groups_[pos];

            auto mask = g.match(h);

            if (mask != 0) {
                auto* base = data_ + pos * group_size;

                do {
                    const auto idx = std::countr_zero(static_cast<uint32_t>(mask));

                    if (std::as_const(eq_)(std::as_const(base[idx].first), key)) {
                        return {pos, static_cast<uint32_t>(idx), base + idx};
                    }

                    mask &= mask - 1;
                } while (mask != 0);
            }

            if (g.is_not_overflow(h) || !p.next(group_mask_)) [[likely]] {
                return {};
            }
        }
    }

    template<class K>
    [[nodiscard]] auto insert_impl(const K& key, size_type h) -> insert_location {
        const auto pos0 = position_for(h);

        insert_location result;
        result.hash = h;
        bool check_existing = true;

        for (prober p(pos0);;) {
            const auto pos = p.get();
            const auto& g = groups_[pos];

            auto mask = check_existing ? g.match(h) : 0;

            if (mask != 0) {
                auto* base = data_ + pos * group_size;

                do {
                    const auto idx = std::countr_zero(static_cast<uint32_t>(mask));

                    if (std::as_const(eq_)(std::as_const(base[idx].first), key)) {
                        result.existing = {pos, static_cast<uint32_t>(idx), base + idx};

                        return result;
                    }

                    mask &= mask - 1;
                } while (mask != 0);
            }

            const bool stops_lookup = check_existing && g.is_not_overflow(h);
            if (!result.can_insert) {
                if (const auto available = g.match_available(); available) {
                    result.can_insert = true;
                    result.insert_group = pos;
                    result.insert_slot = std::countr_zero(static_cast<uint32_t>(available));
                } else if (stops_lookup) {
                    result.first_unmarked = p;
                    result.needs_overflow = true;
                }
            }

            if (stops_lookup) {
                check_existing = false;
            }
            if (result.can_insert && !check_existing) {
                return result;
            }
            if (!p.next(group_mask_)) {
                assert(result.can_insert);
                return result;
            }
        }
    }

    template<class Construct>
    auto insert_unique_with(size_type h, Construct&& construct) -> locator {
        for (prober p(position_for(h));;) {
            const auto pos = p.get();
            auto& g = groups_[pos];
            if (const auto available = g.match_available(); available) {
                const auto idx = static_cast<uint32_t>(std::countr_zero(static_cast<uint32_t>(available)));
                auto* slot = data_ + pos * group_size + idx;
                construct(slot);
                g.set(idx, h);
                ++len_;
                return {pos, idx, slot};
            }
            g.mark_overflow(h);
            if (!p.next(group_mask_))
                std::abort();
        }
    }

    template<class... Args>
    auto insert_unique(size_type h, Args&&... args) -> locator {
        return insert_unique_with(h, [&](storage_type* slot) {
            alloy::construct_at(slot, std::forward<Args>(args)...);
        });
    }

    template<class F>
    void for_each_occupied(F&& f) {
        for (size_type g = 0; groups_ && g <= group_mask_; ++g) {
            auto mask = match_really_occupied(g);
            while (mask != 0) {
                const auto idx = std::countr_zero(static_cast<uint32_t>(mask));
                f(data_[g * group_size + idx]);
                mask &= mask - 1;
            }
        }
    }

    [[nodiscard]] auto storage_at(const_iterator it) noexcept -> storage_type& {
        assert(it.map_ == this && it != cend());
        return data_[it.group_ * group_size + it.slot_];
    }

    template<class F>
    static decltype(auto) with_transfer_args(storage_type& value, F&& f) {
        if constexpr (details::hash_map::nothrow_transfer<Key, Val>) {
            return std::forward<F>(f)(std::piecewise_construct,
                                      std::forward_as_tuple(std::move_if_noexcept(value.first)),
                                      std::forward_as_tuple(std::move_if_noexcept(value.second)));
        } else if constexpr (std::is_copy_constructible_v<Val>) {
            return std::forward<F>(f)(std::as_const(value));
        } else {
            return std::forward<F>(f)(std::piecewise_construct,
                                      std::forward_as_tuple(std::as_const(value.first)),
                                      std::forward_as_tuple(std::move(value.second)));
        }
    }

    void relocate_into(hash_map& target) {
        if (len_ == 0) {
            return;
        }

        constexpr bool raw_relocation = alloy::is_trivially_relocatable<storage_type>;
        constexpr bool safe_transfer = raw_relocation || details::hash_map::nothrow_transfer<Key, Val>;

        constexpr bool safe_hash = noexcept(hash_key(std::declval<const Key&>()));

        constexpr bool may_move = safe_transfer || !std::is_copy_constructible_v<Val>;

        auto relocate_one = [&](storage_type& value, size_type h) {
            if constexpr (raw_relocation) {
                target.insert_unique_with(h, [&](storage_type* slot) noexcept {
                    alloy::relocate_n(slot, alloy::addressof(value), 1);
                });
            } else {
                with_transfer_args(value, [&](auto&&... args) {
                    target.insert_unique(h, std::forward<decltype(args)>(args)...);
                });
            }
        };

        if constexpr (may_move && !safe_hash) {
            constexpr auto max_count =
                static_cast<size_type>(std::numeric_limits<difference_type>::max()) / sizeof(size_type);

            if (len_ > max_count) {
                throw std::length_error("HashMap relocation overflow");
            }

            alloy::alloc::allocation_guard scratch(alloc_, len_ * sizeof(size_type), alignof(size_type));

            auto* hashes = ::new (scratch.get()) size_type[len_];

            size_type i = 0;

            for_each_occupied([&](const storage_type& value) {
                hashes[i++] = hash_key(value.first);
            });

            i = 0;

            for_each_occupied([&](storage_type& value) {
                relocate_one(value, hashes[i++]);
            });
        } else {
            for_each_occupied([&](storage_type& value) {
                relocate_one(value, hash_key(value.first));
            });
        }
        if constexpr (raw_relocation) {
            len_ = 0;
        }
    }
};

template<class Key, class Val, class H, class E>
void swap(hash_map<Key, Val, H, E>& lhs, hash_map<Key, Val, H, E>& rhs) noexcept(noexcept(lhs.swap(rhs)))
    requires requires { lhs.swap(rhs); }
{
    lhs.swap(rhs);
}

template<class Key, class Val, class H, class E>
[[nodiscard]] auto operator==(const hash_map<Key, Val, H, E>& lhs, const hash_map<Key, Val, H, E>& rhs) -> bool
    requires std::equality_comparable<Key> && std::equality_comparable<Val>
{
    if (lhs.size() != rhs.size()) {
        return false;
    }

    for (const auto& [key, value] : lhs) {
        const auto it = rhs.find(key);

        if (it == rhs.end() || !(it->first == key) || !(it->second == value)) {
            return false;
        }
    }

    return true;
}
} // namespace alloy::ds
