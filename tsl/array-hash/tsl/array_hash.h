/**
 * MIT License
 * 
 * Copyright (c) 2017 Tessil
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef TSL_ARRAY_HASH_H
#define TSL_ARRAY_HASH_H

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>
#include "array_growth_policy.h"


#ifdef __has_include
    /*
     * __has_include is a bit useless (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=79433),
     * check also __cplusplus version.
     */
    #if __has_include(<string_view>) && __cplusplus >= 201703L
    #define TSL_HAS_STRING_VIEW
    #endif
#endif


#ifdef TSL_HAS_STRING_VIEW
#include <string_view>
#endif


#ifndef tsl_assert
    #ifdef TSL_DEBUG
    #define tsl_assert(expr) assert(expr)
    #else
    #define tsl_assert(expr) (static_cast<void>(0))
    #endif
#endif



/**
 * Implementation of the array hash structure described in the 
 * "Cache-conscious collision resolution in string hash tables." (Askitis Nikolas and Justin Zobel, 2005) paper.
 */
namespace tsl {
    
namespace ah {
/**
 * FNV-1a hash
 */    
template<class CharT>
struct str_hash {
    std::size_t operator()(const CharT* key, std::size_t key_size) const {
        static const std::size_t init = std::size_t((sizeof(std::size_t) == 8)?0xcbf29ce484222325:0x811c9dc5);
        static const std::size_t multiplier = std::size_t((sizeof(std::size_t) == 8)?0x100000001b3:0x1000193);
        
        std::size_t hash = init;
        for (std::size_t i = 0; i < key_size; ++i) {
            hash ^= key[i];
            hash *= multiplier;
        }
        
        return hash;
    }
};  

template<class CharT>
struct str_equal {
    bool operator()(const CharT* key_lhs, std::size_t key_size_lhs,
                    const CharT* key_rhs, std::size_t key_size_rhs) const
    {
        if(key_size_lhs != key_size_rhs) {
            return false;
        }
        else {
            return std::memcmp(key_lhs, key_rhs, key_size_lhs * sizeof(CharT)) == 0;
        }
    }
}; 
}


namespace detail_array_hash {
    
static constexpr bool is_power_of_two(std::size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

/**
 * For each string in the bucket, store the size of the string, the chars of the string 
 * and T, if it's not void. T should be either void or an unsigned type.
 * 
 * End the buffer with END_OF_BUCKET flag. END_OF_BUCKET has the same type as the string size variable.
 * 
 * m_buffer (CharT*):
 * | size of str1 (KeySizeT) | str1 (const CharT*) | value (T if T != void) | ... | 
 * | size of strN (KeySizeT) | strN (const CharT*) | value (T if T != void) | END_OF_BUCKET (KeySizeT) |
 * 
 * m_buffer is null if there is no string in the bucket.
 * 
 * KeySizeT and T are extended to be a multiple of CharT when stored in the buffer.
 * 
 * Use std::malloc and std::free instead of new and delete so we can have access to std::realloc.
 */
template<class CharT,
         class T,
         class KeyEqual,
         class KeySizeT,
         bool StoreNullTerminator>
class array_bucket {
    template<typename U>
    using has_mapped_type = typename std::integral_constant<bool, !std::is_same<U, void>::value>;
    
    static_assert(!has_mapped_type<T>::value || std::is_unsigned<T>::value, 
                  "T should be either void or an unsigned type.");
    
    static_assert(std::is_unsigned<KeySizeT>::value, "KeySizeT should be an unsigned type.");
    
public:
    template<bool IsConst>
    class array_bucket_iterator;
    
    using char_type = CharT;
    using key_size_type = KeySizeT;
    using mapped_type = T;
    using size_type = std::size_t;
    using key_equal = KeyEqual;
    using iterator = array_bucket_iterator<false>;
    using const_iterator = array_bucket_iterator<true>;
    
    static_assert(sizeof(KeySizeT) <= sizeof(size_type), "sizeof(KeySizeT) should be <= sizeof(std::size_t;)");
    static_assert(std::is_unsigned<size_type>::value, "");
    
private:
    /**
     * Return how much space in bytes the type U will take when stored in the buffer.
     * As the buffer is of type CharT, U may take more space than sizeof(U).
     * 
     * Example: sizeof(CharT) = 4, sizeof(U) = 2 => U will take 4 bytes in the buffer instead of 2.
     */
    template<typename U>
    static constexpr size_type sizeof_in_buff() noexcept {
        static_assert(is_power_of_two(sizeof(U)), "sizeof(U) should be a power of two.");
        static_assert(is_power_of_two(sizeof(CharT)), "sizeof(CharT) should be a power of two.");
        
        return std::max(sizeof(U), sizeof(CharT));
    }
    
    /**
     * Same as sizeof_in_buff<U>, but instead of returning the size in bytes return it in term of sizeof(CharT).
     */
    template<typename U>
    static constexpr size_type size_as_char_t() noexcept {
        return sizeof_in_buff<U>() / sizeof(CharT);
    }

    static key_size_type read_key_size(const CharT* buffer) noexcept {
        key_size_type key_size;
        std::memcpy(&key_size, buffer, sizeof(key_size));
        
        return key_size;
    }
    
    static mapped_type read_value(const CharT* buffer) noexcept {
        mapped_type value;
        std::memcpy(&value, buffer, sizeof(value));
        
        return value;
    }
    
    static bool is_end_of_bucket(const CharT* buffer) noexcept {
        return read_key_size(buffer) == END_OF_BUCKET;
    }
    
public:
    /**
     * Return the size required for an entry with a key of size 'key_size'.
     */
    template<class U = T, typename std::enable_if<!has_mapped_type<U>::value>::type* = nullptr>
    static size_type entry_required_bytes(size_type key_size) noexcept {
        return sizeof_in_buff<key_size_type>() + (key_size + KEY_EXTRA_SIZE) * sizeof(CharT);
    }
    
    template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
    static size_type entry_required_bytes(size_type key_size) noexcept {
        return sizeof_in_buff<key_size_type>() + (key_size + KEY_EXTRA_SIZE) * sizeof(CharT) + 
               sizeof_in_buff<mapped_type>();
    }
    
private:
    /**
     * Return the size of the current entry in buffer.
     */
    static size_type entry_size_bytes(const CharT* buffer) noexcept {
        return entry_required_bytes(read_key_size(buffer));
    }
    
public:    
    template<bool IsConst>
    class array_bucket_iterator {
        friend class array_bucket;
        
        using buffer_type = typename std::conditional<IsConst, const CharT, CharT>::type;
        
        explicit array_bucket_iterator(buffer_type* position) noexcept: m_position(position) {
        }
        
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = void;
        using difference_type = std::ptrdiff_t;
        using reference = void;
        using pointer = void;
        
    public:        
        array_bucket_iterator() noexcept: m_position(nullptr) {
        }
        
        const CharT* key() const {
            return m_position + size_as_char_t<key_size_type>();
        }
        
        size_type key_size() const {
            return read_key_size(m_position);
        }
        
        template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
        U value() const {
            return read_value(m_position + size_as_char_t<key_size_type>() + key_size() + KEY_EXTRA_SIZE);
        }
        
        
        template<class U = T, typename std::enable_if<has_mapped_type<U>::value && !IsConst>::type* = nullptr>
        void set_value(U value) noexcept {
            std::memcpy(m_position + size_as_char_t<key_size_type>() + key_size() + KEY_EXTRA_SIZE, 
                        &value, sizeof(value));
        }
        
        array_bucket_iterator& operator++() {
            m_position += entry_size_bytes(m_position)/sizeof(CharT);
            if(is_end_of_bucket(m_position)) {
                m_position = nullptr;
            }
            
            return *this;
        }
    
        array_bucket_iterator operator++(int) {
            array_bucket_iterator tmp(*this);
            ++*this;
            
            return tmp;
        }
        
        friend bool operator==(const array_bucket_iterator& lhs, const array_bucket_iterator& rhs) { 
            return lhs.m_position == rhs.m_position; 
        }
        
        friend bool operator!=(const array_bucket_iterator& lhs, const array_bucket_iterator& rhs) { 
            return !(lhs == rhs); 
        }
        
    private:
        buffer_type* m_position;
    };
    
    
    
    static iterator end_it() noexcept {
        return iterator(nullptr);
    }
    
    static const_iterator cend_it() noexcept {
        return const_iterator(nullptr);
    }
    
public:    
    array_bucket() : m_buffer(nullptr) {
    }
    
    ~array_bucket() {
        clear();
    }
    
    array_bucket(const array_bucket& other) {
        if(other.m_buffer == nullptr) {
            m_buffer = nullptr;
            return;
        }
        
        const size_type other_buffer_size = other.size_bytes();
        m_buffer = static_cast<CharT*>(std::malloc(other_buffer_size));
        if(m_buffer == nullptr) {
            throw std::bad_alloc();
        }
        
        std::memcpy(m_buffer, other.m_buffer, other_buffer_size);
    }
    
    array_bucket(array_bucket&& other) noexcept: m_buffer(other.m_buffer) {
        other.m_buffer = nullptr;
    }
    
    array_bucket& operator=(array_bucket other) noexcept {
        other.swap(*this);
        
        return *this;
    }
    
    void swap(array_bucket& other) noexcept {
        std::swap(m_buffer, other.m_buffer);
    }
    
    iterator begin() noexcept { return iterator(m_buffer); }
    iterator end() noexcept { return iterator(nullptr); }
    const_iterator begin() const noexcept { return cbegin(); }
    const_iterator end() const noexcept { return cend(); }
    const_iterator cbegin() const noexcept { return const_iterator(m_buffer); }
    const_iterator cend() const noexcept { return const_iterator(nullptr); }
    
    /**
     * Return an iterator pointing to the key entry if presents or, if not there, to the position 
     * past the last element of the bucket. Return end() if the bucket has not be initialized yet.
     * 
     * The boolean of the pair is set to true if the key is there, false otherwise.
     */
    std::pair<const_iterator, bool> find_or_end_of_bucket(const CharT* key, size_type key_size) const noexcept {
        if(m_buffer == nullptr) {
            return std::make_pair(cend(), false);
        }
        
        const CharT* buffer_ptr_in_out = m_buffer;
        const bool found = find_or_end_of_bucket_impl(key, key_size, buffer_ptr_in_out);
        
        return std::make_pair(const_iterator(buffer_ptr_in_out), found);
    }
    
    /**
     * Append the element 'key' with its potential value at the end of the bucket. 
     * 'end_of_bucket' should point past the end of the last element in the bucket, end() if the bucket
     * was not intiailized yet. You usually get this value from find_or_end_of_bucket.
     * 
     * Return the position where the element was actually inserted.
     */
    template<class... ValueArgs>
    const_iterator append(const_iterator end_of_bucket, const CharT* key, size_type key_size, 
                          ValueArgs&&... value) 
    {
        const key_size_type key_sz = as_key_size_type(key_size);
        
        if(end_of_bucket == cend()) {
            tsl_assert(m_buffer == nullptr);
            
            const size_type buffer_size = entry_required_bytes(key_sz) + sizeof_in_buff<decltype(END_OF_BUCKET)>();
                                            
            m_buffer = static_cast<CharT*>(std::malloc(buffer_size));
            if(m_buffer == nullptr) {
                throw std::bad_alloc();
            }
            
            append_impl(key, key_sz, m_buffer, std::forward<ValueArgs>(value)...);
            
            return const_iterator(m_buffer);
        }
        else {
            tsl_assert(is_end_of_bucket(end_of_bucket.m_position));
            
            const size_type current_size = ((end_of_bucket.m_position + size_as_char_t<decltype(END_OF_BUCKET)>()) - 
                                              m_buffer) * sizeof(CharT);
            const size_type new_size = current_size + entry_required_bytes(key_sz);
            
            
            CharT* new_buffer = static_cast<CharT*>(std::realloc(m_buffer, new_size));
            if(new_buffer == nullptr) {
                throw std::bad_alloc();
            }
            m_buffer = new_buffer;
            
            
            CharT* buffer_append_pos = m_buffer + current_size / sizeof(CharT) - 
                                       size_as_char_t<decltype(END_OF_BUCKET)>();
            append_impl(key, key_sz, buffer_append_pos, std::forward<ValueArgs>(value)...);  
            
            return const_iterator(buffer_append_pos);
        }
        
    }
    
    const_iterator erase(const_iterator position) noexcept {
        tsl_assert(position.m_position != nullptr && !is_end_of_bucket(position.m_position));
        
        // get mutable pointers
        CharT* start_entry = m_buffer + (position.m_position - m_buffer); 
        CharT* start_next_entry = start_entry + entry_size_bytes(start_entry) / sizeof(CharT);
        
        
        CharT* end_buffer_ptr = start_next_entry;
        while(!is_end_of_bucket(end_buffer_ptr)) {
            end_buffer_ptr += entry_size_bytes(end_buffer_ptr) / sizeof(CharT);
        }
        end_buffer_ptr += size_as_char_t<decltype(END_OF_BUCKET)>();
        
        
        const size_type size_to_move = (end_buffer_ptr - start_next_entry) * sizeof(CharT);
        std::memmove(start_entry, start_next_entry, size_to_move);
        
        
        if(is_end_of_bucket(m_buffer)) {
            clear();
            return cend();
        }
        else if(is_end_of_bucket(start_entry)) {
            return cend();
        }
        else {
            return const_iterator(start_entry);
        }
    }
    
    /**
     * Return true if the element has been erased
     */
    bool erase(const CharT* key, size_type key_size) noexcept {
        if(m_buffer == nullptr) {
            return false;
        }
        
        const CharT* entry_buffer_ptr_in_out = m_buffer;
        bool found = find_or_end_of_bucket_impl(key, key_size, entry_buffer_ptr_in_out);
        if(found) {
            erase(const_iterator(entry_buffer_ptr_in_out));
            
            return true;
        }
        else {
            return false;
        }
    }
    
    void reserve(size_type size) {
        if(m_buffer != nullptr || size == 0) {
            throw std::invalid_argument("Should reserve a size > 0 on empty bucket.");
        }
        
        m_buffer = static_cast<CharT*>(std::malloc(size + sizeof_in_buff<decltype(END_OF_BUCKET)>()));
        if(m_buffer == nullptr) {
            throw std::bad_alloc();
        }
        
        const auto end_of_bucket = END_OF_BUCKET;
        std::memcpy(m_buffer, &end_of_bucket, sizeof(end_of_bucket));
    }
    
    /**
     * Bucket should be big enough and there is no check to see if the key already exists.
     * No check on key_size.
     */
    template<class... ValueArgs>
    void append_in_reserved_bucket_no_check(const CharT* key, size_type key_size, ValueArgs&&... value) noexcept {
        CharT* buffer_ptr = m_buffer;
        while(!is_end_of_bucket(buffer_ptr)) {
            buffer_ptr += entry_size_bytes(buffer_ptr)/sizeof(CharT);
        }
        
        append_impl(key, key_size_type(key_size), buffer_ptr, std::forward<ValueArgs>(value)...);
    }
    
    bool empty() const noexcept {
        return m_buffer == nullptr || is_end_of_bucket(m_buffer);
    }
    
    void clear() noexcept {
        std::free(m_buffer);
        m_buffer = nullptr;
    }
    
    iterator mutable_iterator(const_iterator pos) noexcept {
        return iterator(m_buffer + (pos.m_position - m_buffer)); 
    }
    
private:
    key_size_type as_key_size_type(size_type key_size) const {
        if(key_size > MAX_KEY_SIZE) {
            throw std::length_error("Key is too long.");
        }
        
        return key_size_type(key_size);
    }
    
    /*
     * Return true if found, false otherwise. 
     * If true, buffer_ptr_in_out points to the start of the entry matching 'key'.
     * If false, buffer_ptr_in_out points to where the 'key' should be inserted.
     * 
     * Start search from buffer_ptr_in_out.
     */
    bool find_or_end_of_bucket_impl(const CharT* key, size_type key_size, 
                                    const CharT* & buffer_ptr_in_out) const noexcept
    {
        while(!is_end_of_bucket(buffer_ptr_in_out)) {
            const key_size_type buffer_key_size = read_key_size(buffer_ptr_in_out); 
            const CharT* buffer_str = buffer_ptr_in_out + size_as_char_t<key_size_type>();
            if(KeyEqual()(buffer_str, buffer_key_size, key, key_size)) {
                return true;
            }
            
            buffer_ptr_in_out += entry_size_bytes(buffer_ptr_in_out)/sizeof(CharT);
        }
        
        return false;
    }

    template<typename U = T, typename std::enable_if<!has_mapped_type<U>::value>::type* = nullptr>
    void append_impl(const CharT* key, key_size_type key_size, CharT* buffer_append_pos) noexcept {
        std::memcpy(buffer_append_pos, &key_size, sizeof(key_size));
        buffer_append_pos += size_as_char_t<key_size_type>();
        
        std::memcpy(buffer_append_pos, key, key_size * sizeof(CharT));
        buffer_append_pos += key_size;
        
        const CharT zero = 0;
        std::memcpy(buffer_append_pos, &zero, KEY_EXTRA_SIZE * sizeof(CharT));
        buffer_append_pos += KEY_EXTRA_SIZE;
        
        const auto end_of_bucket = END_OF_BUCKET;
        std::memcpy(buffer_append_pos, &end_of_bucket, sizeof(end_of_bucket));
    }
    
    template<typename U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
    void append_impl(const CharT* key, key_size_type key_size, CharT* buffer_append_pos, 
                typename array_bucket<CharT, U, KeyEqual, KeySizeT, StoreNullTerminator>::mapped_type value) noexcept 
    {        
        std::memcpy(buffer_append_pos, &key_size, sizeof(key_size));
        buffer_append_pos += size_as_char_t<key_size_type>();
        
        std::memcpy(buffer_append_pos, key, key_size * sizeof(CharT));
        buffer_append_pos += key_size;
        
        const CharT zero = 0;
        std::memcpy(buffer_append_pos, &zero, KEY_EXTRA_SIZE * sizeof(CharT));
        buffer_append_pos += KEY_EXTRA_SIZE;
        
        std::memcpy(buffer_append_pos, &value, sizeof(value));
        buffer_append_pos += size_as_char_t<mapped_type>();
        
        const auto end_of_bucket = END_OF_BUCKET;
        std::memcpy(buffer_append_pos, &end_of_bucket, sizeof(end_of_bucket));
    }
    
    size_type size_bytes() const noexcept {
        if(m_buffer == nullptr) {
            return 0;
        }
        
        CharT* buffer_ptr = m_buffer;
        while(!is_end_of_bucket(buffer_ptr)) {
            buffer_ptr += entry_size_bytes(buffer_ptr)/sizeof(CharT);
        }
        buffer_ptr += size_as_char_t<decltype(END_OF_BUCKET)>();
        
        return (buffer_ptr - m_buffer)*sizeof(CharT);
    }
    
private:
    static const key_size_type END_OF_BUCKET = std::numeric_limits<key_size_type>::max();
    static const key_size_type KEY_EXTRA_SIZE = StoreNullTerminator?1:0;
    
    CharT* m_buffer;
    
public:
    static const key_size_type MAX_KEY_SIZE = 
                    // -1 for END_OF_BUCKET
                    key_size_type(std::numeric_limits<key_size_type>::max() - KEY_EXTRA_SIZE - 1);
};


template<class T>
class value_container {
public:
    void clear() noexcept {
        m_values.clear();
    }

    friend void swap(value_container& lhs, value_container& rhs) {
        lhs.m_values.swap(rhs.m_values);
    }
    
protected:
    static constexpr float VECTOR_GROWTH_RATE = 1.5f;
    
    // TODO use a sparse array? or a std::dequeu
    std::vector<T> m_values;
};

template<>
class value_container<void> {
public:
    void clear() noexcept {
    }
};



/**
 * If there is no value in the array_hash (in the case of a set for example), T should be void.
 * 
 * The size of a key string is limited to std::numeric_limits<KeySizeT>::max() - 1.
 * 
 * The number of elements in the map is limited to std::numeric_limits<IndexSizeT>::max().
 */
template<class CharT,
         class T, 
         class Hash,
         class KeyEqual,
         bool StoreNullTerminator,
         class KeySizeT,
         class IndexSizeT,
         class GrowthPolicy>
class array_hash: private value_container<T>, private Hash, private GrowthPolicy {
private:
    template<typename U>
    using has_mapped_type = typename std::integral_constant<bool, !std::is_same<U, void>::value>;
    
    /**
     * If there is a mapped type in array_hash, we store the values in m_values of value_container class 
     * and we store an index to m_values in the bucket. The index is of type IndexSizeT.
     */
    using array_bucket = tsl::detail_array_hash::array_bucket<CharT,
                                                              typename std::conditional<has_mapped_type<T>::value,
                                                                                        IndexSizeT,
                                                                                        void>::type,
                                                              KeyEqual, KeySizeT, StoreNullTerminator>;
    
public:
    template<bool IsConst>
    class array_hash_iterator;
    
    using char_type = CharT;
    using key_size_type = KeySizeT;
    using index_size_type = IndexSizeT;
    using size_type = std::size_t;
    using hasher = Hash;
    using key_equal = KeyEqual;
    using iterator = array_hash_iterator<false>;
    using const_iterator = array_hash_iterator<true>;
   
    
/*
 * Iterator classes
 */ 
public:
    template<bool IsConst>
    class array_hash_iterator {
        friend class array_hash;
        
    private:
        using iterator_array_bucket = typename array_bucket::const_iterator;
                                                            
        using iterator_buckets = typename std::conditional<IsConst, 
                                                           typename std::vector<array_bucket>::const_iterator, 
                                                           typename std::vector<array_bucket>::iterator>::type;
                                                            
        using array_hash_ptr = typename std::conditional<IsConst, 
                                                         const array_hash*, 
                                                         array_hash*>::type;

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = typename std::conditional<has_mapped_type<T>::value, T, void>::type;
        using difference_type = std::ptrdiff_t;
        using reference = typename std::conditional<has_mapped_type<T>::value, 
                                                    typename std::conditional<
                                                                IsConst, 
                                                                typename std::add_lvalue_reference<const T>::type,
                                                                typename std::add_lvalue_reference<T>::type>::type, 
                                                    void>::type;
        using pointer = typename std::conditional<has_mapped_type<T>::value, 
                                                  typename std::conditional<IsConst, const T*, T*>::type, 
                                                  void>::type;
        
                                                        
    private:                                                            
        array_hash_iterator(iterator_buckets buckets_iterator, iterator_array_bucket array_bucket_iterator, 
                            array_hash_ptr array_hash_p) noexcept: 
            m_buckets_iterator(buckets_iterator),
            m_array_bucket_iterator(array_bucket_iterator),
            m_array_hash(array_hash_p)
        {
            tsl_assert(m_array_hash != nullptr);
        }
        
    public:
        array_hash_iterator() noexcept: m_array_hash(nullptr) {
        }
        
        array_hash_iterator(const array_hash_iterator<false>& other) noexcept : 
                                                m_buckets_iterator(other.m_buckets_iterator), 
                                                m_array_bucket_iterator(other.m_array_bucket_iterator),
                                                m_array_hash(other.m_array_hash)
        {
        }
        
        const CharT* key() const {
            return m_array_bucket_iterator.key();
        }
        
        size_type key_size() const {
            return m_array_bucket_iterator.key_size();
        }
        
#ifdef TSL_HAS_STRING_VIEW
        std::basic_string_view<CharT> key_sv() const {
            return std::basic_string_view<CharT>(key(), key_size());
        }
#endif
        
        template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
        reference value() const {
            return this->m_array_hash->m_values[value_position()];
        }
        
        template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
        reference operator*() const {
            return value();
        }
        
        template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
        pointer operator->() const {
            return std::addressof(value());
        }
        
        array_hash_iterator& operator++() {
            tsl_assert(m_buckets_iterator != m_array_hash->m_buckets.end());
            tsl_assert(m_array_bucket_iterator != m_buckets_iterator->cend());
            
            ++m_array_bucket_iterator;
            if(m_array_bucket_iterator == m_buckets_iterator->cend()) {
                do {
                    ++m_buckets_iterator;
                } while(m_buckets_iterator != m_array_hash->m_buckets.end() && 
                        m_buckets_iterator->empty());
                
                if(m_buckets_iterator != m_array_hash->m_buckets.end()) {
                    m_array_bucket_iterator = m_buckets_iterator->cbegin();
                }
            }
            
            return *this; 
        }
        
        array_hash_iterator operator++(int) {
            array_hash_iterator tmp(*this);
            ++*this;
            
            return tmp;
        }
        
        friend bool operator==(const array_hash_iterator& lhs, const array_hash_iterator& rhs) { 
            return lhs.m_buckets_iterator == rhs.m_buckets_iterator && 
                   lhs.m_array_bucket_iterator == rhs.m_array_bucket_iterator &&
                   lhs.m_array_hash == rhs.m_array_hash; 
        }
        
        friend bool operator!=(const array_hash_iterator& lhs, const array_hash_iterator& rhs) { 
            return !(lhs == rhs); 
        }
        
    private:
        template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
        IndexSizeT value_position() const {
            return this->m_array_bucket_iterator.value();
        }
        
    private:
        iterator_buckets m_buckets_iterator;
        iterator_array_bucket m_array_bucket_iterator;
        
        array_hash_ptr m_array_hash;
    };
    
    
    
public:    
    array_hash(size_type bucket_count, 
               const Hash& hash,
               float max_load_factor): Hash(hash), GrowthPolicy((bucket_count == 0)?++bucket_count:bucket_count), 
                                       m_buckets(bucket_count > max_bucket_count()? 
                                                    throw std::length_error("The map exceeds its maxmimum size."):
                                                    bucket_count), 
                                       m_nb_elements(0) 
    {
        this->max_load_factor(max_load_factor);
    }
    
    array_hash(const array_hash& other) = default;
    
    array_hash(array_hash&& other) noexcept(std::is_nothrow_move_constructible<value_container<T>>::value &&
                                            std::is_nothrow_move_constructible<Hash>::value &&
                                            std::is_nothrow_move_constructible<GrowthPolicy>::value &&
                                            std::is_nothrow_move_constructible<std::vector<array_bucket>>::value)
                                  : value_container<T>(std::move(other)),
                                    Hash(std::move(other)),
                                    GrowthPolicy(std::move(other)),
                                    m_buckets(std::move(other.m_buckets)),
                                    m_nb_elements(other.m_nb_elements),
                                    m_max_load_factor(other.m_max_load_factor),
                                    m_load_threshold(other.m_load_threshold)
    {
        other.clear();
    }
    
    array_hash& operator=(const array_hash& other) = default;
    
    array_hash& operator=(array_hash&& other) {
        other.swap(*this);
        other.clear();
        
        return *this;
    }
    
    
    /*
     * Iterators 
     */
    iterator begin() noexcept {
        auto begin = m_buckets.begin();
        while(begin != m_buckets.end() && begin->empty()) {
            ++begin;
        }
        
        return (begin != m_buckets.end())?iterator(begin, begin->cbegin(), this):end();
    }
    
    const_iterator begin() const noexcept {
        return cbegin();
    }
    
    const_iterator cbegin() const noexcept {
        auto begin = m_buckets.cbegin();
        while(begin != m_buckets.cend() && begin->empty()) {
            ++begin;
        }
        
        return (begin != m_buckets.cend())?const_iterator(begin, begin->cbegin(), this):cend();
    }
    
    iterator end() noexcept {
        return iterator(m_buckets.end(), array_bucket::cend_it(), this);
    }
    
    const_iterator end() const noexcept {
        return cend();
    }
    
    const_iterator cend() const noexcept {
        return const_iterator(m_buckets.end(), array_bucket::cend_it(), this);
    }
    
    
    /*
     * Capacity
     */
    bool empty() const noexcept {
        return m_nb_elements == 0;
    }
    
    size_type size() const noexcept {
        return m_nb_elements;
    }
    
    size_type max_size() const noexcept {
        return std::numeric_limits<IndexSizeT>::max();
    }
    
    size_type max_key_size() const noexcept {
        return MAX_KEY_SIZE;
    }
    
    template<class U = T, typename std::enable_if<!has_mapped_type<U>::value>::type* = nullptr>
    void shrink_to_fit() {
        rehash_impl(size_type(std::ceil(float(size())/max_load_factor())));
    }
    
    template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
    void shrink_to_fit() {
        clear_old_erased_values();
        this->m_values.shrink_to_fit();
        
        rehash_impl(size_type(std::ceil(float(size())/max_load_factor())));
    }
    
    /*
     * Modifiers
     */
    void clear() noexcept {
        value_container<T>::clear();
        
        for(auto& bucket : m_buckets) {
            bucket.clear();
        }
        
        m_nb_elements = 0;
    }
    
    
    
    template<class... ValueArgs>
    std::pair<iterator, bool> emplace(const CharT* key, size_type key_size, ValueArgs&&... value_args) {
        const std::size_t hash = hash_key(key, key_size);
        std::size_t ibucket = bucket_for_hash(hash);
        
        auto it_find = m_buckets[ibucket].find_or_end_of_bucket(key, key_size);
        if(it_find.second) {
            return std::make_pair(iterator(m_buckets.begin() + ibucket, it_find.first, this), false);
        }
        
        if(grow_on_high_load()) {
            ibucket = bucket_for_hash(hash);
            it_find = m_buckets[ibucket].find_or_end_of_bucket(key, key_size);
        }
        
        return emplace_impl(ibucket, it_find.first, key, key_size, std::forward<ValueArgs>(value_args)...);
    }
    
    template<class M>
    std::pair<iterator, bool> insert_or_assign(const CharT* key, size_type key_size, M&& obj) { 
        auto it = emplace(key, key_size, std::forward<M>(obj));
        if(!it.second) {
            it.first.value() = std::forward<M>(obj);
        }
        
        return it;
    }
    
    
    
    iterator erase(const_iterator pos) {
        if(shoud_clear_old_erased_values()) {
            clear_old_erased_values();
        }
        
        return erase_from_bucket(mutable_iterator(pos));
    }
    
    iterator erase(const_iterator first, const_iterator last) {
        if(first == last) {
            return mutable_iterator(first);
        }
        
        auto to_delete = erase_from_bucket(mutable_iterator(first));
        while(to_delete != last) {
            to_delete = erase_from_bucket(to_delete);
        }
        
        if(shoud_clear_old_erased_values()) {
            clear_old_erased_values();
        }
        
        return to_delete;
    }
    
    
    
    size_type erase(const CharT* key, size_type key_size) {
        return erase(key, key_size, hash_key(key, key_size));
    }
    
    size_type erase(const CharT* key, size_type key_size, std::size_t hash) {
        if(shoud_clear_old_erased_values()) {
            clear_old_erased_values();
        }
        
        const std::size_t ibucket = bucket_for_hash(hash);
        if(m_buckets[ibucket].erase(key, key_size)) {
            m_nb_elements--;
            return 1;
        }
        else {
            return 0;
        }
    }
    
    
    
    void swap(array_hash& other) {
        using std::swap;
        
        swap(static_cast<value_container<T>&>(*this), static_cast<value_container<T>&>(other));
        swap(static_cast<Hash&>(*this), static_cast<Hash&>(other));
        swap(static_cast<GrowthPolicy&>(*this), static_cast<GrowthPolicy&>(other));
        swap(m_buckets, other.m_buckets);
        swap(m_nb_elements, other.m_nb_elements);
        swap(m_max_load_factor, other.m_max_load_factor);
        swap(m_load_threshold, other.m_load_threshold);
    }
    
    /*
     * Lookup
     */
    template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
    U& at(const CharT* key, size_type key_size) {
        return at(key, key_size, hash_key(key, key_size));      
    }
    
    template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
    const U& at(const CharT* key, size_type key_size) const {
        return at(key, key_size, hash_key(key, key_size));      
    }
    
    template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
    U& at(const CharT* key, size_type key_size, std::size_t hash) {
        return const_cast<U&>(static_cast<const array_hash*>(this)->at(key, key_size, hash));
    }
    
    template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
    const U& at(const CharT* key, size_type key_size, std::size_t hash) const {
        const std::size_t ibucket = bucket_for_hash(hash);
        auto it_find = m_buckets[ibucket].find_or_end_of_bucket(key, key_size);
        if(it_find.second) {
            return this->m_values[it_find.first.value()];
        }
        else {
            throw std::out_of_range("Couldn't find key.");
        }        
    }
    
    
    
    template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
    U& access_operator(const CharT* key, size_type key_size) {
        const std::size_t hash = hash_key(key, key_size);
        std::size_t ibucket = bucket_for_hash(hash);
        
        auto it_find = m_buckets[ibucket].find_or_end_of_bucket(key, key_size);
        if(it_find.second) {
            return this->m_values[it_find.first.value()];
        }
        else {
            if(grow_on_high_load()) {
                ibucket = bucket_for_hash(hash);
                it_find = m_buckets[ibucket].find_or_end_of_bucket(key, key_size);
            }
            
            return emplace_impl(ibucket, it_find.first, key, key_size, U{}).first.value();
        }
    }
    
    
    
    size_type count(const CharT* key, size_type key_size) const {
        return count(key, key_size, hash_key(key, key_size));
    }
    
    size_type count(const CharT* key, size_type key_size, std::size_t hash) const {
        const std::size_t ibucket = bucket_for_hash(hash);
        auto it_find = m_buckets[ibucket].find_or_end_of_bucket(key, key_size);
        if(it_find.second) {
            return 1;
        }
        else {
            return 0;
        }
    }
    
    
    
    iterator find(const CharT* key, size_type key_size) {
        return find(key, key_size, hash_key(key, key_size));
    }
    
    const_iterator find(const CharT* key, size_type key_size) const {
        return find(key, key_size, hash_key(key, key_size));
    }
    
    iterator find(const CharT* key, size_type key_size, std::size_t hash) {
        const std::size_t ibucket = bucket_for_hash(hash);
        auto it_find = m_buckets[ibucket].find_or_end_of_bucket(key, key_size);
        if(it_find.second) {
            return iterator(m_buckets.begin() + ibucket, it_find.first, this);
        }
        else {
            return end();
        }
    }
    
    const_iterator find(const CharT* key, size_type key_size, std::size_t hash) const {
        const std::size_t ibucket = bucket_for_hash(hash);
        auto it_find = m_buckets[ibucket].find_or_end_of_bucket(key, key_size);
        if(it_find.second) {
            return const_iterator(m_buckets.cbegin() + ibucket, it_find.first, this);
        }
        else {
            return cend();
        }
    }
    
    
    
    std::pair<iterator, iterator> equal_range(const CharT* key, size_type key_size) {
        return equal_range(key, key_size, hash_key(key, key_size));
    }
    
    std::pair<const_iterator, const_iterator> equal_range(const CharT* key, size_type key_size) const {
        return equal_range(key, key_size, hash_key(key, key_size));
    }
    
    std::pair<iterator, iterator> equal_range(const CharT* key, size_type key_size, std::size_t hash) {
        iterator it = find(key, key_size, hash);
        return std::make_pair(it, (it == end())?it:std::next(it));
    }
    
    std::pair<const_iterator, const_iterator> equal_range(const CharT* key, size_type key_size, 
                                                          std::size_t hash) const 
    {
        const_iterator it = find(key, key_size, hash);
        return std::make_pair(it, (it == cend())?it:std::next(it));
    }
    
    /*
     * Bucket interface 
     */
    size_type bucket_count() const {
        return m_buckets.size();
    }
    
    size_type max_bucket_count() const { 
        return std::min(GrowthPolicy::max_bucket_count(), m_buckets.max_size());
    }
    
    
    /*
     *  Hash policy 
     */
    float load_factor() const {
        return float(m_nb_elements) / float(bucket_count());
    }
    
    float max_load_factor() const {
        return m_max_load_factor;
    }
    
    void max_load_factor(float ml) {
        m_max_load_factor = ml;
        m_load_threshold = size_type(float(bucket_count())*m_max_load_factor);
    }
    
    void rehash(size_type count) {
        count = std::max(count, size_type(std::ceil(float(size())/max_load_factor())));
        rehash_impl(count);
    }
    
    void reserve(size_type count) {
        rehash(size_type(std::ceil(float(count)/max_load_factor())));
    }
    
    /*
     * Observers
     */
    hasher hash_function() const { 
        return static_cast<hasher>(*this); 
    }
    
    // TODO add support for statefull KeyEqual
    key_equal key_eq() const { 
        return KeyEqual(); 
    }
    
    /*
     * Other
     */
    iterator mutable_iterator(const_iterator it) noexcept {
        auto it_bucket = m_buckets.begin() + std::distance(m_buckets.cbegin(), it.m_buckets_iterator);
        return iterator(it_bucket, it.m_array_bucket_iterator, this);
    }
    
private:
    std::size_t hash_key(const CharT* key, size_type key_size) const {
        return Hash::operator()(key, key_size);
    }
    
    std::size_t bucket_for_hash(std::size_t hash) const {
        return GrowthPolicy::bucket_for_hash(hash);
    }
    
    /**
     * If there is a mapped_type, the mapped value in m_values is not erased now.
     * It will be erased when the ratio between the size of the map and 
     * the size of the map + the number of deleted values still stored is low enough (see clear_old_erased_values).
     */
    iterator erase_from_bucket(iterator pos) noexcept {
        auto array_bucket_next_it = pos.m_buckets_iterator->erase(pos.m_array_bucket_iterator);
        m_nb_elements--;
        
        if(array_bucket_next_it != pos.m_buckets_iterator->cend()) {
            return iterator(pos.m_buckets_iterator, array_bucket_next_it, this);
        }
        else {
            do {
                ++pos.m_buckets_iterator;
            } while(pos.m_buckets_iterator != m_buckets.end() && pos.m_buckets_iterator->empty());
            
            if(pos.m_buckets_iterator != m_buckets.end()) {
                return iterator(pos.m_buckets_iterator, pos.m_buckets_iterator->cbegin(), this);
            }
            else {
                return end();
            }
        }
    }
    
    
    template<class U = T, typename std::enable_if<!has_mapped_type<U>::value>::type* = nullptr>
    bool shoud_clear_old_erased_values(float /*threshold*/ = DEFAULT_CLEAR_OLD_ERASED_VALUE_THRESHOLD) const {
        return false;
    }
    
    template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
    bool shoud_clear_old_erased_values(float threshold = DEFAULT_CLEAR_OLD_ERASED_VALUE_THRESHOLD) const {
        if(this->m_values.size() == 0) {
            return false;
        }
        
        return float(m_nb_elements)/float(this->m_values.size()) < threshold;
    }
    
    template<class U = T, typename std::enable_if<!has_mapped_type<U>::value>::type* = nullptr>
    void clear_old_erased_values() {
    }
    
    template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
    void clear_old_erased_values() {
        static_assert(std::is_nothrow_move_constructible<U>::value || 
                      std::is_copy_constructible<U>::value, 
                      "mapped_value must be either copy constructible or nothrow move constructible.");

        if(m_nb_elements == this->m_values.size()) {
            return;
        }
        
        std::vector<T> new_values;
        new_values.reserve(size());
        
        for(auto it = begin(); it != end(); ++it) {
            new_values.push_back(std::move_if_noexcept(it.value()));
        }
        
        
        IndexSizeT ivalue = 0;
        for(auto it = begin(); it != end(); ++it) {
            auto it_array_bucket = it.m_buckets_iterator->mutable_iterator(it.m_array_bucket_iterator);
            it_array_bucket.set_value(ivalue);
            ivalue++;
        }
        
        new_values.swap(this->m_values);
        tsl_assert(m_nb_elements == this->m_values.size());
    }
    
    /**
     * Return true if a rehash occured.
     */
    bool grow_on_high_load() {
        if(size() >= m_load_threshold) {
            rehash_impl(GrowthPolicy::next_bucket_count());
            return true;
        }
        
        return false;
    }
    
    template<class... ValueArgs, class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
    std::pair<iterator, bool> emplace_impl(std::size_t ibucket, typename array_bucket::const_iterator end_of_bucket, 
                                          const CharT* key, size_type key_size, ValueArgs&&... value_args) 
    {
        if(this->m_values.size() >= max_size()) {
            // Try to clear old erased values lingering in m_values. Throw if it doesn't change anything.
            clear_old_erased_values();
            if(this->m_values.size() >= max_size()) {
                throw std::length_error("Can't insert value, too much values in the map.");
            }
        }
        
        if(this->m_values.size() == this->m_values.capacity()) {
            this->m_values.reserve(std::size_t(float(this->m_values.size()) * value_container<T>::VECTOR_GROWTH_RATE));
        }
        
        
        this->m_values.emplace_back(std::forward<ValueArgs>(value_args)...);
        
        try {
            auto it = m_buckets[ibucket].append(end_of_bucket, key, key_size, IndexSizeT(this->m_values.size() - 1));
            m_nb_elements++;
            
            return std::make_pair(iterator(m_buckets.begin() + ibucket, it, this), true);
        } catch(...) {
            // Rollback
            this->m_values.pop_back();
            throw;
        }
    }
    
    template<class U = T, typename std::enable_if<!has_mapped_type<U>::value>::type* = nullptr>
    std::pair<iterator, bool> emplace_impl(std::size_t ibucket, typename array_bucket::const_iterator end_of_bucket, 
                                          const CharT* key, size_type key_size) 
    {
        if(m_nb_elements >= max_size()) {
            throw std::length_error("Can't insert value, too much values in the map.");
        }
        
        auto it = m_buckets[ibucket].append(end_of_bucket, key, key_size);
        m_nb_elements++;
        
        return std::make_pair(iterator(m_buckets.begin() + ibucket, it, this), true);
    }
    
    void rehash_impl(size_type bucket_count) {
        GrowthPolicy new_growth_policy(bucket_count);
        if(bucket_count == this->bucket_count()) {
            return;
        }
        
        
        if(shoud_clear_old_erased_values(0.9f)) {
            clear_old_erased_values();
        }
        
        
        std::vector<std::size_t> required_size_for_bucket(bucket_count, 0);
        std::vector<std::size_t> bucket_for_ivalue(size(), 0);
        
        std::size_t ivalue = 0;
        for(auto it = begin(); it != end(); ++it) {
            const std::size_t hash = hash_key(it.key(), it.key_size());
            const std::size_t ibucket = new_growth_policy.bucket_for_hash(hash);
            
            bucket_for_ivalue[ivalue] = ibucket;
            required_size_for_bucket[ibucket] += array_bucket::entry_required_bytes(it.key_size());
            ivalue++;
        }
        
        
        
        
        std::vector<array_bucket> new_buckets(bucket_count);
        for(std::size_t ibucket = 0; ibucket < bucket_count; ibucket++) {
            if(required_size_for_bucket[ibucket] > 0) {
                new_buckets[ibucket].reserve(required_size_for_bucket[ibucket]);
            }
        }
        
        
        ivalue = 0;
        for(auto it = begin(); it != end(); ++it) {
            const std::size_t ibucket = bucket_for_ivalue[ivalue];
            append(new_buckets[ibucket], it);
            
            ivalue++;
        }
        
        
        using std::swap;
        swap(static_cast<GrowthPolicy&>(*this), new_growth_policy);
        
        m_buckets.swap(new_buckets);
        
        // Call max_load_factor to change m_load_threshold
        max_load_factor(m_max_load_factor);
    }
    
    template<class U = T, typename std::enable_if<!has_mapped_type<U>::value>::type* = nullptr>
    void append(array_bucket& bucket, iterator it) {
        bucket.append_in_reserved_bucket_no_check(it.key(), it.key_size());
    }
    
    template<class U = T, typename std::enable_if<has_mapped_type<U>::value>::type* = nullptr>
    void append(array_bucket& bucket, iterator it) {
        bucket.append_in_reserved_bucket_no_check(it.key(), it.key_size(), it.value_position());
    }
    
public:    
    static const size_type DEFAULT_INIT_BUCKET_COUNT = 16;
    static constexpr float DEFAULT_MAX_LOAD_FACTOR = 2.0f;
    static const size_type MAX_KEY_SIZE = array_bucket::MAX_KEY_SIZE;
    
private:
    static constexpr float DEFAULT_CLEAR_OLD_ERASED_VALUE_THRESHOLD = 0.6f;
    
    std::vector<array_bucket> m_buckets;
    
    IndexSizeT m_nb_elements;
    float m_max_load_factor;
    size_type m_load_threshold;
};

} // end namespace detail_array_hash
} //end namespace tsl

#endif
