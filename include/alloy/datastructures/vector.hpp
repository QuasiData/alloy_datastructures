#pragma once
#include "alloy/core/core.hpp"
#include "alloy/core/lifetime.hpp"

#include "alloy/alloc/allocator.hpp"

#include <algorithm>
#include <cassert>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace alloy::ds {
namespace details::vector {
    template<class T>
    concept vector_type =
        std::is_object_v<T> && !std::is_array_v<T> && std::same_as<T, std::remove_cv_t<T>> &&
        std::is_nothrow_destructible_v<T> && (std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>);

    template<class T>
    inline constexpr bool transferable_assign = std::is_move_assignable_v<T> || std::is_copy_assignable_v<T>;

    template<class T>
    inline constexpr bool shiftable = alloy::is_trivially_relocatable<T> || transferable_assign<T>;

    template<class T>
    inline constexpr bool trivial_transfer =
        std::is_trivially_copyable_v<T> &&
        (std::is_trivially_move_constructible_v<T> || std::is_trivially_copy_constructible_v<T>);

    template<class T>
    inline constexpr bool trivial_shift = std::is_trivially_copyable_v<T> && (std::is_trivially_move_assignable_v<T> ||
                                                                              std::is_trivially_copy_assignable_v<T>);

    template<class T>
    concept less_comparable = requires(const T& lhs, const T& rhs) {
        { lhs < rhs } -> std::convertible_to<bool>;
        { rhs < lhs } -> std::convertible_to<bool>;
    };

    template<class T>
        requires std::three_way_comparable<T> || less_comparable<T>
    [[nodiscard]] auto compare(const T& lhs, const T& rhs) {
        if constexpr (std::three_way_comparable<T>) {
            return lhs <=> rhs;
        } else {
            if (lhs < rhs) {
                return std::weak_ordering::less;
            }
            if (rhs < lhs) {
                return std::weak_ordering::greater;
            }
            return std::weak_ordering::equivalent;
        }
    }
} // namespace details::vector

template<class T>
    requires details::vector::vector_type<T>
struct vector {
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using allocator_type = alloy::alloc::allocator;
    using is_trivially_relocatable = std::conditional_t<alloy::is_trivially_relocatable<allocator_type>, void, bool>;

    using reference = value_type&;
    using const_reference = const value_type&;
    using pointer = value_type*;
    using const_pointer = const value_type*;

    using iterator = pointer;
    using const_iterator = const_pointer;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    explicit vector(allocator_type alloc = {}) noexcept
        : alloc_(alloc) {}

    explicit vector(size_type count, allocator_type alloc = {})
        requires std::default_initializable<value_type>
        : vector(alloc) {
        resize(count);
    }

    vector(size_type count, const_reference value, allocator_type alloc = {})
        requires std::is_copy_constructible_v<value_type>
        : vector(alloc) {
        resize(count, value);
    }

    template<std::input_iterator InputIt, std::sentinel_for<InputIt> Sentinel>
        requires std::constructible_from<value_type, std::iter_reference_t<InputIt>>
    vector(InputIt first, Sentinel last, allocator_type alloc = {})
        : vector(alloc) {
        if constexpr (std::forward_iterator<InputIt> || std::sized_sentinel_for<Sentinel, InputIt>) {
            const auto count = range_count(first, last);
            allocate(count);

            construction_guard guard(data_);
            auto source = iterator_source<InputIt>{std::move(first)};
            construct_from(guard, source, count);
            len_ = count;
            guard.release();
        } else {
            for (; first != last; ++first) {
                emplace_back(*first);
            }
        }
    }

    vector(std::initializer_list<value_type> values, allocator_type alloc = {})
        requires std::is_copy_constructible_v<value_type>
        : vector(values.begin(), values.end(), alloc) {}

    vector(const vector& other)
        requires std::is_copy_constructible_v<value_type>
        : vector(other, alloy::alloc::default_allocator()) {}

    vector(const vector& other, allocator_type alloc)
        requires std::is_copy_constructible_v<value_type>
        : vector(alloc) {
        allocate(other.len_);
        append_copy(other.data_, other.len_);
    }

    vector(vector&& other) noexcept
        : alloc_(other.alloc_) {
        take_storage(other);
    }

    vector(vector&& other, allocator_type alloc)
        : vector(alloc) {
        if (allocators_compatible(other)) {
            take_storage(other);
        } else {
            allocate(other.len_);
            append_relocate(other.data_, other.len_);
            if constexpr (alloy::is_trivially_relocatable<value_type>) {
                other.len_ = 0;
            }
            other.clear();
        }
    }

    auto operator=(const vector& other) -> vector&
        requires std::is_copy_constructible_v<value_type>
    {
        if (this != &other) {
            vector tmp(other, alloc_);
            replace_with(tmp);
        }
        return *this;
    }

    // NOLINTNEXTLINE(bugprone-unsafe-to-allow-exceptions)
    auto operator=(vector&& other) noexcept(false) -> vector& {
        if (this == &other) {
            return *this;
        }

        if (allocators_compatible(other)) {
            replace_with(other);
        } else {
            vector tmp(std::move(other), alloc_);
            replace_with(tmp);
        }
        return *this;
    }

    auto operator=(std::initializer_list<value_type> values) -> vector&
        requires std::is_copy_constructible_v<value_type>
    {
        assign(values.begin(), values.end());
        return *this;
    }

    ~vector() {
        destroy_and_dealloc();
    }

    [[nodiscard]] auto begin() noexcept -> iterator {
        return data_;
    }

    [[nodiscard]] auto begin() const noexcept -> const_iterator {
        return data_;
    }

    [[nodiscard]] auto cbegin() const noexcept -> const_iterator {
        return begin();
    }

    [[nodiscard]] auto end() noexcept -> iterator {
        return ptr_at(len_);
    }

    [[nodiscard]] auto end() const noexcept -> const_iterator {
        return ptr_at(len_);
    }

    [[nodiscard]] auto cend() const noexcept -> const_iterator {
        return end();
    }

    [[nodiscard]] auto rbegin() noexcept -> reverse_iterator {
        return reverse_iterator(end());
    }

    [[nodiscard]] auto rbegin() const noexcept -> const_reverse_iterator {
        return const_reverse_iterator(end());
    }

    [[nodiscard]] auto crbegin() const noexcept -> const_reverse_iterator {
        return rbegin();
    }

    [[nodiscard]] auto rend() noexcept -> reverse_iterator {
        return reverse_iterator(begin());
    }

    [[nodiscard]] auto rend() const noexcept -> const_reverse_iterator {
        return const_reverse_iterator(begin());
    }

    [[nodiscard]] auto crend() const noexcept -> const_reverse_iterator {
        return rend();
    }

    [[nodiscard]] auto data() noexcept -> pointer {
        return data_;
    }

    [[nodiscard]] auto data() const noexcept -> const_pointer {
        return data_;
    }

    [[nodiscard]] auto operator[](this auto& self, size_type pos) noexcept -> decltype(auto) {
        assert(pos < self.len_);
        return self.begin()[pos];
    }

    [[nodiscard]] auto at(this auto& self, size_type pos) -> decltype(auto) {
        if (pos >= self.len_) [[unlikely]] {
            throw std::out_of_range("vector::at");
        }
        return self.begin()[pos];
    }

    [[nodiscard]] auto front(this auto& self) noexcept -> decltype(auto) {
        assert(self.len_ != 0);
        return (*self.begin());
    }

    [[nodiscard]] auto back(this auto& self) noexcept -> decltype(auto) {
        assert(self.len_ != 0);
        return self.begin()[self.len_ - 1];
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

    [[nodiscard]] auto capacity() const noexcept -> size_type {
        return cap_;
    }

    [[nodiscard]] auto max_size() const noexcept -> size_type {
        return max_capacity();
    }

    void reserve(size_type count) {
        if (count > cap_) {
            reallocate(count);
        }
    }

    void shrink_to_fit() {
        if (len_ != cap_) {
            reallocate(len_);
        }
    }

    void clear() noexcept {
        destroy_n(data_, len_);
        len_ = 0;
    }

    void swap(vector& other) noexcept {
        if (this == &other) {
            return;
        }

        if (!allocators_compatible(other)) [[unlikely]] {
            std::terminate();
        }

        using std::swap;
        swap(data_, other.data_);
        swap(len_, other.len_);
        swap(cap_, other.cap_);
    }

    void assign(size_type count, const_reference value)
        requires std::is_copy_constructible_v<value_type>
    {
        vector tmp(count, value, alloc_);
        replace_with(tmp);
    }

    template<std::input_iterator InputIt, std::sentinel_for<InputIt> Sentinel>
        requires std::constructible_from<value_type, std::iter_reference_t<InputIt>>
    void assign(InputIt first, Sentinel last) {
        vector tmp(std::move(first), std::move(last), alloc_);
        replace_with(tmp);
    }

    void assign(std::initializer_list<value_type> values)
        requires std::is_copy_constructible_v<value_type>
    {
        assign(values.begin(), values.end());
    }

    template<std::ranges::input_range R>
        requires std::constructible_from<value_type, std::ranges::range_reference_t<R>>
    void assign_range(R&& range) {
        assign(std::ranges::begin(range), std::ranges::end(range));
    }

    void resize(size_type count)
        requires std::default_initializable<value_type>
    {
        resize_impl(count, [](pointer dst) -> auto {
            alloy::construct_at(dst);
        });
    }

    void resize(size_type count, const_reference value)
        requires std::is_copy_constructible_v<value_type>
    {
        resize_impl(count, [&](pointer dst) -> auto {
            alloy::construct_at(dst, value);
        });
    }

    template<class... Args>
        requires std::constructible_from<value_type, Args...>
    auto emplace_back(Args&&... args) -> reference {
        if (len_ == cap_) [[unlikely]] {
            return *grow_emplace(len_, std::forward<Args>(args)...);
        }

        auto* slot = data_ + len_;
        alloy::construct_at(slot, std::forward<Args>(args)...);
        ++len_;
        return *slot;
    }

    void push_back(const_reference value)
        requires std::is_copy_constructible_v<value_type>
    {
        emplace_back(value);
    }

    void push_back(value_type&& value)
        requires std::is_move_constructible_v<value_type>
    {
        emplace_back(std::move(value));
    }

    void pop_back() noexcept {
        assert(len_ != 0);
        --len_;
        alloy::destroy_at(data_ + len_);
    }

    template<class... Args>
        requires std::constructible_from<value_type, Args...> && details::vector::shiftable<value_type>
    auto emplace(const_iterator pos, Args&&... args) -> iterator {
        const auto idx = index_of(pos);

        if (idx == len_) {
            return alloy::addressof(emplace_back(std::forward<Args>(args)...));
        }

        if (len_ == cap_) [[unlikely]] {
            return grow_emplace(idx, std::forward<Args>(args)...);
        }

        value_type value(std::forward<Args>(args)...);
        return insert_known(idx, 1, transfer_source{alloy::addressof(value)});
    }

    auto insert(const_iterator pos, const_reference value) -> iterator
        requires std::is_copy_constructible_v<value_type> && details::vector::shiftable<value_type>
    {
        return emplace(pos, value);
    }

    auto insert(const_iterator pos, value_type&& value) -> iterator
        requires std::is_move_constructible_v<value_type> && details::vector::shiftable<value_type>
    {
        return emplace(pos, std::move(value));
    }

    auto insert(const_iterator pos, size_type count, const_reference value) -> iterator
        requires std::is_copy_constructible_v<value_type> &&
                 (alloy::is_trivially_relocatable<value_type> || std::is_copy_assignable_v<value_type>)
    {
        const auto idx = index_of(pos);
        const auto required = checked_size(count);

        if (count == 0) {
            return ptr_at(idx);
        }

        if (required <= cap_ && idx != len_ && contains_address(alloy::addressof(value))) {
            const value_type stable(value);
            return insert_known(idx, count, fill_source{alloy::addressof(stable)});
        }
        return insert_known(idx, count, fill_source{alloy::addressof(value)});
    }

    template<std::input_iterator InputIt, std::sentinel_for<InputIt> Sentinel>
        requires std::constructible_from<value_type, std::iter_reference_t<InputIt>> &&
                 (alloy::is_trivially_relocatable<value_type> ||
                  std::is_assignable_v<value_type&, std::iter_reference_t<InputIt>>) &&
                 details::vector::shiftable<value_type>
    auto insert(const_iterator pos, InputIt first, Sentinel last) -> iterator {
        const auto idx = index_of(pos);

        if (first == last) {
            return ptr_at(idx);
        }

        if constexpr (std::forward_iterator<InputIt>) {
            const auto count = range_count(first, last);
            const auto required = checked_size(count);

            if constexpr (std::same_as<InputIt, iterator> || std::same_as<InputIt, const_iterator>) {
                if (required <= cap_ && idx != len_ && contains_address(first)) {
                    vector tmp(first, last, alloc_);
                    return insert_known(idx, tmp.len_, transfer_source{tmp.data_});
                }
            }
            return insert_known(idx, count, iterator_source<InputIt>{std::move(first)});
        } else {
            vector tmp(alloc_);
            if constexpr (std::sized_sentinel_for<Sentinel, InputIt>) {
                const auto count = range_count(first, last);
                (void)checked_size(count);
                tmp.reserve(count);
            }

            for (; first != last; ++first) {
                if (tmp.len_ == max_capacity() - len_) [[unlikely]] {
                    throw std::length_error("vector capacity overflow");
                }
                tmp.emplace_back(*first);
            }
            return insert_known(idx, tmp.len_, transfer_source{tmp.data_});
        }
    }

    auto insert(const_iterator pos, std::initializer_list<value_type> values) -> iterator
        requires std::is_copy_constructible_v<value_type> &&
                 (alloy::is_trivially_relocatable<value_type> || std::is_copy_assignable_v<value_type>)
    {
        return insert(pos, values.begin(), values.end());
    }

    template<std::ranges::input_range R>
        requires std::constructible_from<value_type, std::ranges::range_reference_t<R>> &&
                 (alloy::is_trivially_relocatable<value_type> ||
                  std::is_assignable_v<value_type&, std::ranges::range_reference_t<R>>) &&
                 details::vector::shiftable<value_type>
    auto insert_range(const_iterator pos, R&& range) -> iterator {
        return insert(pos, std::ranges::begin(range), std::ranges::end(range));
    }

    template<std::ranges::input_range R>
        requires std::constructible_from<value_type, std::ranges::range_reference_t<R>>
    void append_range(R&& range) {
        auto first = std::ranges::begin(range);
        auto last = std::ranges::end(range);
        if constexpr (std::forward_iterator<decltype(first)>) {
            const auto count = range_count(first, last);
            append_known(count, iterator_source<decltype(first)>{std::move(first)});
        } else {
            vector tmp(std::move(first), std::move(last), alloc_);
            append_known(tmp.len_, transfer_source{tmp.data_});
        }
    }

    auto erase(const_iterator pos) -> iterator
        requires details::vector::shiftable<value_type>
    {
        assert(pos != cend());
        return erase(pos, pos + 1);
    }

    auto erase(const_iterator first, const_iterator last) -> iterator
        requires details::vector::shiftable<value_type>
    {
        const auto idx = index_of(first);
        const auto finish = index_of(last);
        assert(idx <= finish);

        const auto count = finish - idx;
        if (count == 0) {
            return ptr_at(idx);
        }

        const auto tail = len_ - finish;
        if constexpr (alloy::is_trivially_relocatable<value_type>) {
            destroy_n(data_ + idx, count);
            alloy::relocate_overlapping_n(data_ + idx, data_ + finish, tail);
        } else if constexpr (details::vector::trivial_shift<value_type>) {
            if (tail != 0) {
                std::memmove(static_cast<void*>(data_ + idx), data_ + finish, tail * sizeof(value_type));
            }
        } else {
            for (size_type i = 0; i < tail; ++i) {
                transfer_assign(data_[idx + i], data_[finish + i]);
            }
        }

        if constexpr (!alloy::is_trivially_relocatable<value_type>) {
            destroy_n(data_ + len_ - count, count);
        }
        len_ -= count;
        return data_ + idx;
    }

private:
    const allocator_type alloc_;

    pointer data_ = nullptr;
    size_type len_ = 0;
    size_type cap_ = 0;

    struct construction_guard {
        pointer data;
        size_type len = 0;

        explicit construction_guard(pointer ptr) noexcept
            : data(ptr) {}

        construction_guard(const construction_guard&) = delete;

        auto operator=(const construction_guard&) -> construction_guard& = delete;

        construction_guard(construction_guard&&) = delete;

        auto operator=(construction_guard&&) -> construction_guard& = delete;

        ~construction_guard() {
            destroy_n(data, len);
        }

        void release() noexcept {
            len = 0;
        }
    };

    template<class I>
    struct iterator_source {
        I it;

        void construct(pointer dst) {
            alloy::construct_at(dst, *it);
        }

        void assign(reference dst) {
            dst = *it;
        }

        void advance() {
            ++it;
        }
    };

    struct fill_source {
        const_pointer value;

        void construct(pointer dst) const {
            alloy::construct_at(dst, *value);
        }

        void assign(reference dst) const {
            dst = *value;
        }

        void advance() const noexcept {}
    };

    struct transfer_source {
        pointer value;

        void construct(pointer dst) const {
            alloy::construct_at(dst, std::move_if_noexcept(*value));
        }

        void assign(reference dst) const {
            transfer_assign(dst, *value);
        }

        void advance() noexcept {
            ++value;
        }
    };

    [[nodiscard]] static constexpr auto max_capacity() noexcept -> size_type {
        return static_cast<size_type>(std::numeric_limits<difference_type>::max()) / sizeof(value_type);
    }

    [[nodiscard]] auto checked_size(size_type extra) const -> size_type {
        if (extra > max_capacity() - len_) [[unlikely]] {
            throw std::length_error("vector capacity overflow");
        }
        return len_ + extra;
    }

    [[nodiscard]] auto recommended_growth_capacity(size_type required) const -> size_type {
        if (required > max_capacity()) [[unlikely]] {
            throw std::length_error("vector capacity overflow");
        }

        const auto extra = cap_ / 2 + (cap_ % 2 != 0);
        const auto grown = extra > max_capacity() - cap_ ? max_capacity() : cap_ + extra;
        return grown < required ? required : grown;
    }

    template<class I, class S>
    [[nodiscard]] static auto range_count(const I& first, const S& last) -> size_type {
        const auto count = [&] -> auto {
            if constexpr (std::sized_sentinel_for<S, I>) {
                return last - first;
            } else {
                return std::ranges::distance(first, last);
            }
        }();
        const auto result = static_cast<size_type>(count);
        if (count < 0 || static_cast<decltype(count)>(result) != count || result > max_capacity()) [[unlikely]] {
            throw std::length_error("vector capacity overflow");
        }
        return result;
    }

    [[nodiscard]] auto ptr_at(size_type idx) const noexcept -> pointer {
        return idx == 0 ? data_ : data_ + idx;
    }

    [[nodiscard]] auto contains_address(const_pointer ptr) const noexcept -> bool {
        const auto compare = std::compare_three_way{};

        return len_ != 0 && compare(ptr, static_cast<const_pointer>(data_)) >= 0 &&
               compare(ptr, static_cast<const_pointer>(data_ + len_)) < 0;
    }

    [[nodiscard]] auto index_of(const_iterator pos) const noexcept -> size_type {
        if (pos == data_) {
            return 0;
        }
        assert(contains_address(pos) || pos == end());
        return static_cast<size_type>(pos - data_);
    }

    [[nodiscard]] auto allocators_compatible(const vector& other) const noexcept -> bool {
        return alloc_ == other.alloc_;
    }

    void allocate(size_type capacity) {
        assert(data_ == nullptr && len_ == 0 && cap_ == 0);

        if (capacity == 0) {
            return;
        }
        if (capacity > max_capacity()) [[unlikely]] {
            throw std::length_error("vector capacity overflow");
        }

        const auto bytes = capacity * sizeof(value_type);
        auto* raw = alloc_.allocate(bytes, alignof(value_type));
        if (!raw) [[unlikely]] {
            throw std::bad_alloc{};
        }

        auto* storage = ::new (raw) std::byte[bytes];
        data_ = reinterpret_cast<pointer>(storage);
        cap_ = capacity;
    }

    void reset_state() noexcept {
        data_ = nullptr;
        len_ = 0;
        cap_ = 0;
    }

    void take_storage(vector& other) noexcept {
        assert(data_ == nullptr && len_ == 0 && cap_ == 0);
        data_ = other.data_;
        len_ = other.len_;
        cap_ = other.cap_;
        other.reset_state();
    }

    void replace_with(vector& other) noexcept {
        assert(allocators_compatible(other));
        destroy_and_dealloc();
        take_storage(other);
    }

    static void destroy_n(pointer data, size_type count) noexcept {
        if constexpr (!std::is_trivially_destructible_v<value_type>) {
            while (count != 0) {
                alloy::destroy_at(data + --count);
            }
        }
    }

    void destroy_and_dealloc() noexcept {
        if (data_) {
            destroy_n(data_, len_);
            alloc_.deallocate(data_, cap_ * sizeof(value_type), alignof(value_type));
        }
        reset_state();
    }

    static void transfer_assign(reference dst, reference src) noexcept(std::is_nothrow_move_assignable_v<value_type> ||
                                                                       std::is_nothrow_copy_assignable_v<value_type>) {
        if constexpr (std::is_nothrow_move_assignable_v<value_type> || !std::is_copy_assignable_v<value_type>) {
            dst = std::move(src);
        } else {
            dst = std::as_const(src);
        }
    }

    template<class Source>
    static void construct_from(construction_guard& guard, Source& source, size_type count) {
        if (count == 0) {
            return;
        }

        if constexpr (std::same_as<Source, transfer_source> && details::vector::trivial_transfer<value_type>) {
            std::memcpy(static_cast<void*>(guard.data + guard.len), source.value, count * sizeof(value_type));
            guard.len += count;
            source.value += count;
        } else if constexpr ((std::same_as<Source, iterator_source<pointer>> ||
                              std::same_as<Source, iterator_source<const_pointer>>) &&
                             std::is_trivially_copyable_v<value_type> &&
                             std::is_trivially_copy_constructible_v<value_type>) {
            std::memcpy(static_cast<void*>(guard.data + guard.len), source.it, count * sizeof(value_type));
            guard.len += count;
            source.it += count;
        } else {
            for (size_type i = 0; i < count; ++i) {
                source.construct(guard.data + guard.len);
                ++guard.len;
                source.advance();
            }
        }
    }

    template<class Source>
    static void assign_from(pointer dst, Source& source, size_type count) {
        if (count == 0) {
            return;
        }

        if constexpr (std::same_as<Source, transfer_source> && details::vector::trivial_shift<value_type>) {
            std::memcpy(static_cast<void*>(dst), source.value, count * sizeof(value_type));
            source.value += count;
        } else if constexpr ((std::same_as<Source, iterator_source<pointer>> ||
                              std::same_as<Source, iterator_source<const_pointer>>) &&
                             std::is_trivially_copyable_v<value_type> &&
                             std::is_trivially_copy_assignable_v<value_type>) {
            std::memcpy(static_cast<void*>(dst), source.it, count * sizeof(value_type));
            source.it += count;
        } else {
            for (size_type i = 0; i < count; ++i) {
                source.assign(dst[i]);
                source.advance();
            }
        }
    }

    static void transfer_n(construction_guard& guard, pointer src, size_type count) {
        auto source = transfer_source{src};
        construct_from(guard, source, count);
    }

    void append_transfer(pointer src, size_type count) {
        assert(count <= cap_ - len_);
        construction_guard guard(ptr_at(len_));
        transfer_n(guard, src, count);
        len_ += count;
        guard.release();
    }

    void append_relocate(pointer src, size_type count) {
        if constexpr (alloy::is_trivially_relocatable<value_type>) {
            assert(count <= cap_ - len_);
            alloy::relocate_n(ptr_at(len_), src, count);
            len_ += count;
        } else {
            append_transfer(src, count);
        }
    }

    void append_copy(const_pointer src, size_type count) {
        assert(count <= cap_ - len_);
        construction_guard guard(ptr_at(len_));
        auto source = iterator_source<const_pointer>{src};
        construct_from(guard, source, count);
        len_ += count;
        guard.release();
    }

    void reallocate(size_type capacity) {
        assert(capacity >= len_);
        vector tmp(alloc_);
        tmp.allocate(capacity);
        tmp.append_relocate(data_, len_);
        if constexpr (alloy::is_trivially_relocatable<value_type>) {
            len_ = 0;
        }
        replace_with(tmp);
    }

    template<class Construct>
    auto grow_insert(size_type idx, size_type count, size_type capacity, Construct&& construct) -> iterator {
        vector tmp(alloc_);
        tmp.allocate(capacity);

        construction_guard inserted(tmp.ptr_at(idx));
        construct(inserted);
        assert(inserted.len == count);

        tmp.append_relocate(data_, idx);
        tmp.len_ += count;
        inserted.release();
        tmp.append_relocate(ptr_at(idx), len_ - idx);

        if constexpr (alloy::is_trivially_relocatable<value_type>) {
            len_ = 0;
        }
        replace_with(tmp);
        return ptr_at(idx);
    }

    template<class... Args>
    auto grow_emplace(size_type idx, Args&&... args) -> iterator {
        const auto capacity = recommended_growth_capacity(checked_size(1));
        return grow_insert(idx, 1, capacity, [&](construction_guard& guard) -> auto {
            alloy::construct_at(guard.data, std::forward<Args>(args)...);
            ++guard.len;
        });
    }

    template<class Construct>
    void resize_impl(size_type count, Construct&& construct) {
        if (count <= len_) {
            destroy_n(ptr_at(count), len_ - count);
            len_ = count;
            return;
        }

        const auto extra = count - len_;
        auto construct_tail = [&](construction_guard& guard) -> auto {
            for (size_type i = 0; i < extra; ++i) {
                construct(guard.data + guard.len);
                ++guard.len;
            }
        };

        if (count > cap_) {
            grow_insert(len_, extra, recommended_growth_capacity(count), construct_tail);
        } else {
            construction_guard guard(ptr_at(len_));
            construct_tail(guard);
            len_ = count;
            guard.release();
        }
    }

    template<class Source>
    void append_known(size_type count, Source source) {
        const auto required = checked_size(count);
        if (count == 0) {
            return;
        }

        if (required > cap_) {
            grow_insert(len_, count, recommended_growth_capacity(required), [&](construction_guard& guard) -> auto {
                construct_from(guard, source, count);
            });
        } else {
            construction_guard guard(ptr_at(len_));
            construct_from(guard, source, count);
            len_ = required;
            guard.release();
        }
    }

    template<class Source>
    auto insert_known(size_type idx, size_type count, Source source) -> iterator {
        const auto required = checked_size(count);
        if (count == 0) {
            return ptr_at(idx);
        }
        if (required > cap_) {
            return grow_insert(idx, count, recommended_growth_capacity(required), [&](construction_guard& guard) -> auto {
                construct_from(guard, source, count);
            });
        }
        if (idx == len_) {
            append_known(count, std::move(source));
            return ptr_at(idx);
        }

        const auto tail = len_ - idx;
        if constexpr (alloy::is_trivially_relocatable<value_type>) {
            auto* slot = data_ + idx;
            alloy::relocate_overlapping_n(slot + count, slot, tail);

            try {
                construction_guard guard(slot);
                construct_from(guard, source, count);
                guard.release();
            } catch (...) {
                alloy::relocate_overlapping_n(slot, slot + count, tail);
                throw;
            }
            len_ = required;
        } else {
            construction_guard guard(data_ + len_);

            if (count <= tail) {
                transfer_n(guard, data_ + len_ - count, count);

                if constexpr (details::vector::trivial_shift<value_type>) {
                    const auto remaining = tail - count;
                    if (remaining != 0) {
                        std::memmove(
                            static_cast<void*>(data_ + idx + count), data_ + idx, remaining * sizeof(value_type));
                    }
                } else {
                    for (auto i = len_ - count; i > idx; --i) {
                        transfer_assign(data_[i + count - 1], data_[i - 1]);
                    }
                }
                assign_from(data_ + idx, source, count);
            } else {
                auto extra = source;
                for (size_type i = 0; i < tail; ++i) {
                    extra.advance();
                }

                construct_from(guard, extra, count - tail);
                transfer_n(guard, data_ + idx, tail);
                assign_from(data_ + idx, source, tail);
            }

            len_ = required;
            guard.release();
        }
        return data_ + idx;
    }
};

template<class T>
void swap(vector<T>& lhs, vector<T>& rhs) noexcept {
    lhs.swap(rhs);
}

template<class T>
[[nodiscard]] auto operator==(const vector<T>& lhs, const vector<T>& rhs) -> bool
    requires std::equality_comparable<T>
{
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

template<class T>
[[nodiscard]] auto operator<=>(const vector<T>& lhs, const vector<T>& rhs)
    requires std::three_way_comparable<T> || details::vector::less_comparable<T>
{
    return std::lexicographical_compare_three_way(
        lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), details::vector::compare<T>);
}
} // namespace alloy::ds
