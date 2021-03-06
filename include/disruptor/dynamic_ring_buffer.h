#ifndef DISRUPTOR_DYNAMIC_RING_BUFFER_H_
#define DISRUPTOR_DYNAMIC_RING_BUFFER_H_

#include <disruptor/sequencer.h>

namespace disruptor {

// Ring based store of reusable entries containing the data representing an
// event beign exchanged between publisher and {@link EventProcessor}s.
//
// @param <T> implementation storing the data for sharing during exchange
// or parallel coordination of an event.
template <typename T>
class DynamicRingBuffer
{
public:

    struct Block : private stdext::noncopyable
    {
        ALIGN(CACHE_LINE_SIZE_IN_BYTES);
        Sequence tail_; // sequence_

        ALIGN(CACHE_LINE_SIZE_IN_BYTES);
        Sequence head_; // gating_sequence_

        ALIGN(CACHE_LINE_SIZE_IN_BYTES);
        stdext::atomic<Block*> next_;
        char padding_[CACHE_LINE_SIZE_IN_BYTES - sizeof(stdext::atomic<Block*>)];

        const size_t size_;
        stdext::scoped_array<T> events_;

        Block(size_t size)
            : tail_(INITIAL_CURSOR_VALUE)
            , head_(INITIAL_CURSOR_VALUE)
            , size_(size)
            , events_(new T[size_])
        {
            assert(size < (size_t)std::numeric_limits<int64_t>::max());
        }

        size_t mask() const { return size_ - 1; }

        T& get(const int64_t& sequence)
        {
            return events_[sequence & mask()];
        }

        void set(const int64_t& sequence, const T& event)
        {
            events_[sequence & mask()] = event;
        }

        bool empty() const
        {
            return tail_.get() == head_.get();
        }

        bool hasAvailableCapacity() const
        {
            // size_ can't be bigger than max of int64_t
            return (tail_.get() + 1 - (int64_t)size_) <= head_.get();
        }

        void advanceTail()
        {
            tail_.incrementAndGet(1);
        }

        void advanceTailTo(int64_t i)
        {
            tail_.incrementAndGet(i);
        }

        void advanceHead()
        {
            head_.incrementAndGet(1);
        }

        void advanceHeadTo(int64_t i)
        {
            head_.incrementAndGet(i);
        }
    };

    // Construct a DynamicRingBuffer with the full option set.
    //
    // @param buffer_size of the DynamicRingBuffer, must be a power of 2.
    // @param claim_strategy_option is useless for this ringbuffer
    // @param wait_strategy_option waiting strategy employed by
    // processors_to_track waiting in entries becoming available.
    //
    DynamicRingBuffer(size_t buffer_size,
               ClaimStrategyOption claim_strategy_option,
               WaitStrategyOption wait_strategy_option,
               const TimeConfig& timeConfig=TimeConfig())
        : buffer_size_(ceilToPow2(buffer_size))
        , num_blocks_(1)
    {
        Block* first_block = new Block(buffer_size_);
        first_block->next_ = first_block;
        tail_block_ = first_block;
        front_block_ = first_block;
    }

    ~DynamicRingBuffer()
    {
        Block* block = front_block_;
        do {
            Block* next_block = block->next_;
            delete block;
            block = next_block;
        }
        while (block != front_block_);
    }

    void enqueue(const T& event)
    {
        // Blocks can be created but never deleted!
        Block* tail = tail_block_.load(stdext::memory_order_relaxed);
        int64_t block_tail = tail->tail_.get(stdext::memory_order_relaxed);
        stdext::atomic_thread_fence(stdext::memory_order_acquire);

        if (tail->hasAvailableCapacity()) {
            // get sequence from the current block
            tail->set(block_tail + 1, event);
            tail->advanceTail();
        }
        else {
            // current block full, there's another block available(empty)
            if (tail->next_.load(stdext::memory_order_relaxed)
                    != front_block_.load(stdext::memory_order_relaxed)) {
                stdext::atomic_thread_fence(stdext::memory_order_acquire); // for the above read

                Block* tail_block_next = tail->next_.load(stdext::memory_order_relaxed);
                int64_t block_head = tail_block_next->head_.get(stdext::memory_order_relaxed);
                block_tail = tail_block_next->tail_.get(stdext::memory_order_relaxed);
                stdext::atomic_thread_fence(stdext::memory_order_acquire);

                assert(block_tail == block_head);

                tail_block_next->set(block_tail + 1, event);
                tail_block_next->advanceTail();

                stdext::atomic_thread_fence(stdext::memory_order_release);
                tail_block_ = tail_block_next;
            }
            else {
                // no other block available, create a new one
                Block* new_block = new Block(buffer_size_);
                block_tail = new_block->tail_.get(stdext::memory_order_relaxed);
                new_block->set(block_tail + 1, event);
                new_block->advanceTail();

                new_block->next_ = tail->next_.load(stdext::memory_order_relaxed);
                tail->next_ = new_block;

                stdext::atomic_thread_fence(stdext::memory_order_release);
                tail_block_ = new_block;
                ++num_blocks_;
            }
        }
    }

    bool dequeue(T& event)
    {
        // TODO: how do I read more than one elememnts without comparing
        // with head/tail more than once?
        // don't think I can do it for all the available elements, but I should
        // be able to return the available sequence of the current block
        Block* tail_block_at_start = tail_block_.load(stdext::memory_order_relaxed);
        stdext::atomic_thread_fence(stdext::memory_order_acquire);

        Block* head = front_block_.load(stdext::memory_order_relaxed);
        int64_t block_head = head->head_.get(stdext::memory_order_relaxed);
        stdext::atomic_thread_fence(stdext::memory_order_acquire);

        if (!head->empty()) {
            // dequeue from head block
            event = head->get(block_head + 1);
            head->advanceHead();
        }
        else if (head != tail_block_at_start) {
            // head block is empty, but there's another block ahead
            Block* next_block = head->next_;

            int64_t block_head = next_block->head_.get(stdext::memory_order_relaxed);
            int64_t block_tail = next_block->tail_.get(stdext::memory_order_relaxed);
            stdext::atomic_thread_fence(stdext::memory_order_acquire);
            assert(block_head != block_tail);

            stdext::atomic_thread_fence(stdext::memory_order_release);
            head = front_block_ = next_block;

            event = head->get(block_head + 1);
            head->advanceHead();
        }
        else {
            // nothing to dequeue
            return false;
        }

        return true;
    }

    size_t occupied_approx() const
    {
        size_t result = 0;
        Block* head_block = front_block_.load(stdext::memory_order_relaxed);
        Block* block = head_block;
        do {
            size_t head = block->head_.get(stdext::memory_order_relaxed);
            size_t tail = block->tail_.get(stdext::memory_order_relaxed);
            block = block->next_.load(stdext::memory_order_relaxed);
            result += tail - head;
        } while (block != front_block_);

        return result;
    }

    size_t available_approx() const
    {
        return buffer_size_ * num_blocks_ - this->occupied_approx();
    }

    size_t num_blocks() const
    {
        return num_blocks_;
    }

    bool has_available_capacity() const
    {
        return this->available_approx() > 0;
    }

private:
    ALIGN(CACHE_LINE_SIZE_IN_BYTES);
    stdext::atomic<Block*> front_block_;
    char padding1_[CACHE_LINE_SIZE_IN_BYTES - sizeof(stdext::atomic<Block*>)];

    ALIGN(CACHE_LINE_SIZE_IN_BYTES);
    stdext::atomic<Block*> tail_block_;
    char padding2_[CACHE_LINE_SIZE_IN_BYTES - sizeof(stdext::atomic<Block*>)];

    const int buffer_size_;
    size_t num_blocks_;
};

}

#endif
