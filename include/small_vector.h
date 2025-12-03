#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <vector>

/**
 * @brief Small Vector Optimization for AS paths
 * 
 * Most AS paths are < 16 ASNs. This vector stores up to N elements inline,
 * only heap-allocating for rare longer paths.
 * 
 * Benefits:
 * - No heap allocation for paths <= N elements (99%+ of real paths)
 * - Eliminates malloc/free overhead for common cases
 * - Better cache locality (data in object, not indirected through pointer)
 * 
 * Memory layout:
 * - size_: current number of elements
 * - capacity_: inline capacity (N) or heap capacity
 * - is_heap_: flag indicating if using heap storage
 * - data_: inline storage or pointer to heap storage
 * 
 * @tparam T Element type (uint32_t for ASNs)
 * @tparam N Inline capacity (16 for AS paths)
 */
template<typename T, size_t N>
class SmallVector {
public:
    // Type aliases for STL compatibility
    using value_type = T;
    using size_type = size_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = T*;
    using const_iterator = const T*;

    // Default constructor
    SmallVector() noexcept : size_(0), is_heap_(false) {}

    // Constructor with initial size
    explicit SmallVector(size_t n) : size_(n), is_heap_(false) {
        if (n > N) {
            heap_.ptr = new T[n];
            heap_.capacity = n;
            is_heap_ = true;
        }
    }

    // Constructor with initial size and value
    SmallVector(size_t n, const T& value) : size_(n), is_heap_(false) {
        if (n > N) {
            heap_.ptr = new T[n];
            heap_.capacity = n;
            is_heap_ = true;
            for (size_t i = 0; i < n; ++i) {
                heap_.ptr[i] = value;
            }
        } else {
            for (size_t i = 0; i < n; ++i) {
                inline_[i] = value;
            }
        }
    }

    // Initializer list constructor
    SmallVector(std::initializer_list<T> init) : size_(init.size()), is_heap_(false) {
        if (init.size() > N) {
            heap_.ptr = new T[init.size()];
            heap_.capacity = init.size();
            is_heap_ = true;
            std::memcpy(heap_.ptr, init.begin(), init.size() * sizeof(T));
        } else {
            std::memcpy(inline_, init.begin(), init.size() * sizeof(T));
        }
    }

    // Conversion constructor from std::vector (for compatibility)
    template<typename Alloc>
    SmallVector(const std::vector<T, Alloc>& vec) : size_(vec.size()), is_heap_(false) {
        if (vec.size() > N) {
            heap_.ptr = new T[vec.size()];
            heap_.capacity = vec.size();
            is_heap_ = true;
            std::memcpy(heap_.ptr, vec.data(), vec.size() * sizeof(T));
        } else {
            std::memcpy(inline_, vec.data(), vec.size() * sizeof(T));
        }
    }

    // Copy constructor
    SmallVector(const SmallVector& other) : size_(other.size_), is_heap_(false) {
        if (other.size_ > N) {
            heap_.ptr = new T[other.size_];
            heap_.capacity = other.size_;
            is_heap_ = true;
            std::memcpy(heap_.ptr, other.data(), other.size_ * sizeof(T));
        } else {
            std::memcpy(inline_, other.inline_, other.size_ * sizeof(T));
        }
    }

    // Move constructor
    SmallVector(SmallVector&& other) noexcept : size_(other.size_), is_heap_(other.is_heap_) {
        if (other.is_heap_) {
            heap_.ptr = other.heap_.ptr;
            heap_.capacity = other.heap_.capacity;
            other.heap_.ptr = nullptr;
            other.size_ = 0;
            other.is_heap_ = false;
        } else {
            std::memcpy(inline_, other.inline_, other.size_ * sizeof(T));
        }
    }

    // Destructor
    ~SmallVector() {
        if (is_heap_) {
            delete[] heap_.ptr;
        }
    }

    // Copy assignment
    SmallVector& operator=(const SmallVector& other) {
        if (this != &other) {
            if (is_heap_) {
                delete[] heap_.ptr;
            }
            size_ = other.size_;
            if (other.size_ > N) {
                heap_.ptr = new T[other.size_];
                heap_.capacity = other.size_;
                is_heap_ = true;
                std::memcpy(heap_.ptr, other.data(), other.size_ * sizeof(T));
            } else {
                is_heap_ = false;
                std::memcpy(inline_, other.inline_, other.size_ * sizeof(T));
            }
        }
        return *this;
    }

    // Move assignment
    SmallVector& operator=(SmallVector&& other) noexcept {
        if (this != &other) {
            if (is_heap_) {
                delete[] heap_.ptr;
            }
            size_ = other.size_;
            is_heap_ = other.is_heap_;
            if (other.is_heap_) {
                heap_.ptr = other.heap_.ptr;
                heap_.capacity = other.heap_.capacity;
                other.heap_.ptr = nullptr;
                other.size_ = 0;
                other.is_heap_ = false;
            } else {
                std::memcpy(inline_, other.inline_, other.size_ * sizeof(T));
            }
        }
        return *this;
    }

    // Element access
    T& operator[](size_t i) { return is_heap_ ? heap_.ptr[i] : inline_[i]; }
    const T& operator[](size_t i) const { return is_heap_ ? heap_.ptr[i] : inline_[i]; }

    T& front() { return (*this)[0]; }
    const T& front() const { return (*this)[0]; }
    
    T& back() { return (*this)[size_ - 1]; }
    const T& back() const { return (*this)[size_ - 1]; }

    // Data access
    T* data() { return is_heap_ ? heap_.ptr : inline_; }
    const T* data() const { return is_heap_ ? heap_.ptr : inline_; }

    // Iterators
    iterator begin() { return data(); }
    iterator end() { return data() + size_; }
    const_iterator begin() const { return data(); }
    const_iterator end() const { return data() + size_; }
    const_iterator cbegin() const { return data(); }
    const_iterator cend() const { return data() + size_; }

    // Size
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    size_t capacity() const { return is_heap_ ? heap_.capacity : N; }

    // Modifiers
    void clear() { size_ = 0; }

    void push_back(const T& value) {
        if (size_ < N && !is_heap_) {
            inline_[size_++] = value;
        } else {
            grow_and_push(value);
        }
    }

    void reserve(size_t new_cap) {
        if (new_cap <= capacity()) return;
        grow(new_cap);
    }

    void resize(size_t new_size) {
        if (new_size > capacity()) {
            grow(new_size);
        }
        size_ = new_size;
    }

    // Comparison
    bool operator==(const SmallVector& other) const {
        if (size_ != other.size_) return false;
        return std::memcmp(data(), other.data(), size_ * sizeof(T)) == 0;
    }
    
    bool operator!=(const SmallVector& other) const {
        return !(*this == other);
    }

private:
    void grow(size_t new_cap) {
        T* new_data = new T[new_cap];
        std::memcpy(new_data, data(), size_ * sizeof(T));
        if (is_heap_) {
            delete[] heap_.ptr;
        }
        heap_.ptr = new_data;
        heap_.capacity = new_cap;
        is_heap_ = true;
    }

    void grow_and_push(const T& value) {
        size_t new_cap = capacity() * 2;
        if (new_cap < N) new_cap = N;
        grow(new_cap);
        heap_.ptr[size_++] = value;
    }

    size_t size_;
    bool is_heap_;
    
    // Union to share space between inline storage and heap pointer
    union {
        T inline_[N];
        struct {
            T* ptr;
            size_t capacity;
        } heap_;
    };
};

// Alias for AS paths (12 inline elements = 48 bytes + overhead)
// Covers 95%+ of real AS paths without heap allocation
using ASPath = SmallVector<uint32_t, 12>;
