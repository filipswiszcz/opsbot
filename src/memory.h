#ifndef HOR_MEMORY_H
#define HOR_MEMORY_H

#include "common.h"

enum class allocator_status : u8 {
    SUCCESS = 0,
    OUT_OF_MEMORY,
    INVALID_ARGUMENT
};

template<typename T>
struct allocator_result {
    allocator_status status;
    T *ptr;
};

template<typename T, u32 CAPACITY>
class lf_pool_allocator {
public:
    lf_pool_allocator() noexcept {
        for (u32 i = 0; i < CAPACITY; i++) {
            this->used[i].store(0);
        }
    }

    [[nodiscard]] allocator_result<T> allocate(void) noexcept {
        for (u32 i = 0; i < CAPACITY; i++) {
            u8 expected = 0;
            if (this->used[i].compare_exchange_strong(expected, 1)) {
                return {allocator_status::SUCCESS, &this->pool[i]};
            }
        }
        return {allocator_status::OUT_OF_MEMORY, nullptr};
    }

    [[nodiscard]] allocator_status free(const T *ptr) noexcept {
        if (ptr == nullptr) {
            return allocator_status::INVALID_ARGUMENT;
        }
        u32 index = static_cast<u32>(ptr - this->pool);
        if (index < CAPACITY) {
            this->used[index].store(0);
            return allocator_status::SUCCESS;
        }
        return allocator_status::INVALID_ARGUMENT;
    }
private:
    T pool[CAPACITY];
    std::atomic<u8> used[CAPACITY];
};

enum class queue_status : u8 {
    SUCCESS = 0,
    FULL,
    EMPTY
};

template<typename T, u32 CAPACITY>
class lf_spsc_queue final {
    static_assert(CAPACITY >= 2U, "Queue capacity must be at least 2.");
    static_assert(CAPACITY <= std::numeric_limits<u32>::max(), "Queue capacity exceeds index range.");
    static_assert((CAPACITY & (CAPACITY - 1U)) == 0U, "Queue capacity must be a power of two.");
    static_assert(std::is_default_constructible_v<T>, "T must be default constructible.");
    static_assert(std::is_nothrow_copy_assignable_v<T>, "T must be nothrow copy assignable.");
    static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructibe.");
public:
    lf_spsc_queue() noexcept = default;
    lf_spsc_queue(const lf_spsc_queue&) = delete;
    lf_spsc_queue &operator=(const lf_spsc_queue&) = delete;
    lf_spsc_queue(lf_queue&&) = delete;
    lf_spsc_queue &operator=(lf_spsc_queue&&) = delete;

    [[nodiscard]] queue_status push(const T &item) noexcept {
        const u32 curr_head = this->head.load(std::memory_order_relaxed);
        const u32 next_head = (curr_head + 1U) & (CAPACITY - 1U);
        if (next_head == this->tail.load(std::memory_order_acquire)) {
            return queue_status::FULL;
        }

        this->items[curr_head] = item;
        this->head.store(next_head, std::memory_order_release);
        
        return queue_status::SUCCESS;
    }

    [[nodiscard]] queue_status pop(T &item) noexcept {
        const u32 curr_tail = this->tail.load(std::memory_order_relaxed);
        if (curr_tail == this->head.load(std::memory_order_acquire)) {
            return queue_status::EMPTY;
        }

        item = this->items[curr_tail];
        this->tail.store((curr_tail + 1U) & (CAPACITY - 1U), std::memory_order_release);

        return queue_status::SUCCESS;
    }
private:
    T items[CAPACITY];
    std::atomic<u32> head{0U};
    std::atomic<u32> tail{0U};
}

#endif // !HOR_MEMORY_H