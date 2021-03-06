#pragma once

#include <algorithm>
#include <bitset>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include <iostream>

namespace linear
{
namespace detail
{
// KERNEL

template <
    class Value,
    class Key,
    class Hash,
    class KeyExtract,
    class MappedExtract,
    class KeyEqual,
    class Allocator>
class kernel
{
protected:
    // Defined below.
    template <typename T> class iterator_template;

public:
    // MEMBER TYPES

    using key_type        = Key;
    using value_type      = Value;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using hasher          = Hash;
    using key_equal       = KeyEqual;
    using key_extract     = KeyExtract;
    using mapped_extract  = MappedExtract;
    using allocator_type  = Allocator;
    using reference       = value_type&;
    using const_reference = const value_type&;
    using pointer         =
        typename std::allocator_traits<Allocator>::pointer;
    using const_pointer   =
        typename std::allocator_traits<Allocator>::const_pointer;
    using iterator        = iterator_template<value_type>;
    using const_iterator  = iterator_template<const value_type>;

    // CONSTRUCTORS, ET CETERA

    // Default constructor.
    explicit kernel(
        size_t bucket_count = 16,
        const hasher& hash = hasher(),
        const key_equal& equal = key_equal(),
        const allocator_type& alloc = allocator_type())
      // Instances of functors.
    : hash_{hash},
      equal_{equal},
      extract_{},
      alloc_{alloc},
      // Bucket vector.
      buckets_{alloc_},
      min_load_{0.3},
      max_load_{0.7},
      // We always have a number of buckets that is a power of two.
      bucket_count_{upper_power_of_two(bucket_count)},
      // ...
      size_{0},
      min_size_{size_t(bucket_count_ * min_load_)},
      max_size_{size_t(bucket_count_ * max_load_)}
    {
        // Allocate space.
        buckets_.resize(bucket_count_);
    }

    // Copy constructor.
    kernel(
        const kernel& other)
    : hash_{other.hash_},
      equal_{other.equal_},
      extract_{},
      alloc_{other.alloc_},
      buckets_{other.buckets_},
      min_load_{other.min_load_},
      max_load_{other.max_load_},
      bucket_count_{other.bucket_count_},
      size_{other.size_},
      min_size_{other.min_size_},
      max_size_{other.max_size_}
    {}

    // Copy constructor with allocator.
    kernel(
        const kernel& other,
        const allocator_type& alloc)
    : hash_{other.hash_},
      equal_{other.equal_},
      extract_{},
      alloc_{alloc},
      buckets_{other.buckets_, alloc},
      min_load_{other.min_load_},
      max_load_{other.max_load_},
      bucket_count_{other.bucket_count_},
      size_{other.size_},
      min_size_{other.min_size_},
      max_size_{other.max_size_}
    {}

    // Move constructor.
    kernel(kernel&& other)
    : kernel{}
    {
        swap(*this, other);
    }

    // Swapping (member).
    void swap(kernel& other)
    {
        swap(*this, other);
    }

    // Swapping (friend).
    friend void swap(kernel& lhs, kernel& rhs)
    {
        using std::swap;
        swap(lhs.hash_, rhs.hash_);
        swap(lhs.equal_, rhs.equal_);
        swap(lhs.alloc_, rhs.alloc_);
        swap(lhs.buckets_, rhs.buckets_);
        swap(lhs.min_load_, rhs.min_load_);
        swap(lhs.max_load_, rhs.max_load_);
        swap(lhs.bucket_count_, rhs.bucket_count_);
        swap(lhs.size_, rhs.size_);
        swap(lhs.min_size_, rhs.min_size_);
        swap(lhs.max_size_, rhs.max_size_);
    }

    // Copy assignment.
    kernel& operator=(kernel other)
    {
        swap(*this, other);
        return *this;
    }

    // ITERATOR GETTERS

    iterator begin() {
        return make_iterator(buckets_begin());
    }

    const_iterator begin() const {
        return make_const_iterator(buckets_begin());
    }

    const_iterator cbegin() const {
        return make_const_iterator(buckets_begin());
    }

    iterator end() {
        return make_iterator(buckets_end());
    }

    const_iterator end() const {
        return make_const_iterator(buckets_end());
    }

    const_iterator cend() const {
        return make_const_iterator(buckets_end());
    }

    // CAPACITY

    bool empty() const {
        return size_ == 0;
    }

    size_t size() const {
        return size_;
    }

    // CLEAR

    void clear()
    {
        for (int index = 0; index < bucket_count_; ++index)
        {
            auto& bucket = buckets_[index];
            bucket.memory()->~value_type();
        }
        buckets_.clear();
        size_ = 0;
    }

    // ERASE

    size_t erase(const key_type& key)
    {
        size_t index = index_from_key(key);
        size_t erased_index;
        size_t erased_count = 0;

        while (buckets_[index].has_value) {
            auto& bucket = buckets_[index];

            if (equal_(key, extract_(*bucket.memory()))) {
                bucket.memory()->~value_type();
                bucket.has_value = false;
                erased_index = index;
                erased_count = 1;
                size_ -= 1;
                index = index_add(index, 1);
                break;
            }

            index = index_add(index, 1);
        }

        while (buckets_[index].has_value) {
            const auto& move_bucket = buckets_[index];
            const auto& hash = index_from_key(extract_(*move_bucket.memory()));

            if ((erased_index < index &&
                    (hash <= erased_index || hash > index)) ||
                (erased_index > index &&
                    (hash <= erased_index && hash > index)))
            {
                auto& free_bucket = buckets_[erased_index];
                new (free_bucket.memory())
                    value_type{std::move(*move_bucket.memory())};
                move_bucket.memory()->~value_type();

                buckets_[index].has_value = false;
                buckets_[erased_index].has_value = true;
                erased_index = index;
            }

            index = index_add(index, 1);
        }

        // Rehash if this brought us below min load.
        if (size_ < min_size_ && size_ > 16) {
            rehash(bucket_count_/2);
        }

        return erased_count;
    }

    // COUNT

    size_t count(const Key& key) const
    {
        size_t result = 0;

        const size_t virtual_index = index_from_key(key);

        while (buckets_[virtual_index]) {
            auto& value = buckets_[virtual_index];
            if (equal_(key, extract_(*value.memory())))
            {
                ++result;
            }
            virtual_index = index_add(virtual_index, 1);
        }

        return count;
    }

    // OPERATOR[]

    auto& operator[](const Key& key) {
        return mapped_extract_(
            *(buckets_[find(key, index_from_key(key))].memory()));
    }

    const auto& operator[](const Key& key) const {
        return mapped_extract_(
            *(buckets_[find(key, index_from_key(key))].memory()));
    }

    // FIND

    iterator find(const Key& key) {
        return make_iterator(find(key, index_from_key(key)));
    }

    const_iterator find(const Key& key) const {
        return make_const_iterator(find(key, index_from_key(key)));
    }

    // BUCKET INTERFACE

    size_t bucket_count() const {
        return bucket_count_;
    }

    // HASH POLICY

    float load_factor() const {
        return (float)size_/(float)bucket_count_;
    }

    float min_load_factor() const {
        return min_load_;
    }

    float max_load_factor() const {
        return max_load_;
    }

    void min_load_factor(float min_load)
    {
        min_load_ = min_load;

        // Check if we need to rehash.
        min_size_ = min_load_ * bucket_count_;
        if (size_ < min_size_) {
            rehash(bucket_count_/2);
        }
    }

    void max_load_factor(float max_load)
    {
        max_load_ = max_load;

        // Check if we need to rehash.
        max_size_ = max_load_ * bucket_count_;
        if (size_ > max_size_) {
            rehash(bucket_count_*2);
        }
    }

    void rehash(size_t count)
    {
        bucket_count_ = upper_power_of_two(count);

        // Create the new bucket vector, saving the old one.
        bucket_vector old_buckets(bucket_count_);
        std::swap(buckets_, old_buckets);

        // Set up the state so we can insert correctly.
        size_ = 0;
        min_size_ = min_load_ * bucket_count_;
        max_size_ = max_load_ * bucket_count_;

        // Rehash.
        for (size_t index = 0; index < old_buckets.size(); ++index)
        {
            if (old_buckets[index].has_value)
            {
                auto& bucket = old_buckets[index];
                insert(std::move(*bucket.memory()),
                    index_from_value(*bucket.memory()));
                // Detect recursive rehash and break.
                if (bucket_count_ != count) { break; }
            }
        }
    }

    void reserve(size_t count)
    {
        rehash(std::ceil((float)count/max_load_factor()));
    }

    // OBSERVERS

    hasher hash_function() const {
        return hash_;
    }

    key_equal key_eq() const {
        return equal_;
    }

    allocator_type get_allocator() const {
        return alloc_;
    }

protected:
    // BUCKET TYPE

    class bucket_type
    {
    public:
        value_type* memory() {
            return reinterpret_cast<value_type*>(&memory_);
        }

        const value_type* memory() const {
            return reinterpret_cast<const value_type*>(&memory_);
        }

        bool has_value;

    private:
        struct { unsigned char _[sizeof(value_type)]; } memory_;
    };

    using bucket_vector = std::vector<bucket_type,
          typename allocator_type::template rebind<bucket_type>::other>;

    // ITERATOR TYPE

    template <typename T>
    class iterator_template : std::iterator<std::forward_iterator_tag, T>
    {
        // NOT CONVERTED

        friend kernel;

    public:
        iterator_template()
        : index_(0),
          buckets_(nullptr)
        {}

        iterator_template(const iterator_template& other)
        : index_(other.index_),
          buckets_(other.buckets_)
        {}

        iterator_template& operator=(const iterator_template& other)
        {
            index_ = other.index_;
            buckets_ = other.buckets_;
            return *this;
        }

        ~iterator_template()
        {}

        reference operator*() {
            return *(buckets_->operator[](index_).memory());
            }

        const_reference operator*() const {
            return *(buckets_->operator[](index_).memory());
        }

        pointer operator->() {
            return buckets_->operator[](index_).memory();
        }

        const_pointer operator->() const {
            return buckets_->operator[](index_).memory();
        }

        iterator_template& operator++()
        {
            do {
                ++index_;
            }
            while (index_ != buckets_->size() &&
                !buckets_->operator[](index_).has_value);
            return *this;
        }

        iterator_template operator++(int)
        {
            iterator_template old(*this);
            ++*this;
            return old;
        }

        bool operator==(const iterator_template& other) const {
            return index_ == other.index_ && buckets_ == other.buckets_;
        }

        bool operator!=(const iterator_template& other) const {
            return index_ != other.index_ || buckets_ != other.buckets_;
        }

    private:
        iterator_template(
            size_t index,
            bucket_vector* buckets)
        : index_(index),
          buckets_(buckets)
        {}

        size_t index_;

        bucket_vector* buckets_;
    };

    // DATA MEMBERS

    hasher hash_;
    key_equal equal_;
    key_extract extract_;
    mapped_extract mapped_extract_;
    allocator_type alloc_;

    bucket_vector buckets_;
    float min_load_;
    float max_load_;

    size_t bucket_count_;

    size_t size_;
    size_t min_size_;
    size_t max_size_;

    // UPPER POWER OF TWO

    size_t upper_power_of_two(size_t x) const
    {
        // This implementation was found here (adjusted for 64-bit):
        // http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
        --x;
        x |= x >> 1;
        x |= x >> 2;
        x |= x >> 4;
        x |= x >> 8;
        x |= x >> 16;
        x |= x >> 32;
        ++x;
        return x;
    }

    // INDEX HELPER

    size_t index_from_value(const value_type& value) const {
        return index_from_key(extract_(value));
    }

    size_t index_from_key(const key_type& key) const {
        // This bitwise and is the same as doing a modulo because the bucket
        // count is guaranteed to be a power of two.
        return hash_(key) & (bucket_count_ - 1);
    }

    size_t index_add(size_t index, size_t x) const {
        // This bitwise and is the same as doing a modulo because the bucket
        // count is guaranteed to be a power of two.
        return (index + x) & (bucket_count_ - 1);
    }

    size_t index_sub(size_t index, size_t x) const {
        // As above, this corresponds to a modulo operation, except that we
        // always get a positive value this way (as is desired).
        return (index - x) & (bucket_count_ - 1);
    }

    // ITERATOR HELPERS

    size_t buckets_begin()
    {
        if (empty()) {
            return buckets_.size();
        } else {
            auto bucket_it = buckets_.begin();
            while (!bucket_it->has_value) { ++bucket_it; }
            return bucket_it - buckets_.begin();
        }
    }

    size_t buckets_end() {
        return buckets_.size();
    }

    iterator make_iterator(size_t index) {
        return {index, &buckets_};
    }

    const_iterator make_const_iterator(size_t index) const {
        return {index, &buckets_};
    }

public:
    // INSERT

    std::pair<iterator, bool>
    insert(value_type value)
    {
        size_t index = index_from_value(value);
        size_t res = find(extract_(value), index);

        if (res == buckets_.size()) {
            return {insert(std::move(value), index), true};
        } else {
            return {make_iterator(res), false};
        }
    }

    iterator
    insert(
        const_iterator hint,
        value_type value)
    {
        (void)hint;  // Silence 'unused parameter'
        return insert(std::move(value)).first;
    }

private:
    // INSERT IMPLEMENTATION

    iterator insert(value_type value, size_t virtual_index)
    {
        // Start by rehashing, if this will bring us above max load.
        if (size_ == max_size_) {
            rehash(bucket_count_*2);
            return insert(value, index_from_value(value));
        }

        // Find the nearest free bucket, wrapping if we move past the end.
        size_t free_dist = 0;
        size_t free_index = virtual_index;
        while (buckets_[free_index].has_value) {
            free_dist += 1;
            free_index = index_add(free_index, 1);
        }

        // We should have a free bucket in the neighborhood now.
        auto& free_bucket = buckets_[free_index];
        new (free_bucket.memory()) value_type{std::move(value)};

        buckets_[free_index].has_value = true;

        ++size_;

        return {free_index, &buckets_};
    }

    // FIND IMPLEMENTATION

    size_t find(const key_type& key, size_t virtual_index) const
    {
        // Search from there until we find what we are looking for or an
        // empty bucket
        while (buckets_[virtual_index].has_value) {
            const auto& value = buckets_[virtual_index];
            if (equal_(key, extract_(*value.memory()))) {
                return virtual_index;
            }
            virtual_index = index_add(virtual_index, 1);
        }

        // We found nothing, return end.
        return bucket_count_;
    }
};

} // namespace detail

// UNORDERED SET

#define BASE detail::kernel<\
    Key,\
    Key,\
    Hash,\
    extract::identity,\
    extract::identity,\
    KeyEqual,\
    Allocator>
template <
    class Key,
    class Hash = std::hash<Key>,
    class KeyEqual = std::equal_to<Key>,
    class Allocator = std::allocator<Key>>
class unordered_set : public BASE
{};
#undef BASE

// UNORDERED MAP

#define BASE detail::kernel<\
    std::pair<const Key, T>,\
    Key,\
    Hash,\
    extract::first,\
    extract::second,\
    KeyEqual,\
    Allocator>
template <
    class Key,
    class T,
    class Hash = std::hash<Key>,
    class KeyEqual = std::equal_to<Key>,
    class Allocator = std::allocator<std::pair<const Key, T>>>
class unordered_map : public BASE
{
public:
    using mapped_type = T;
};
#undef BASE

} // namespace hopscotch

