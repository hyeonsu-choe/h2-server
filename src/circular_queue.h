#pragma once

#include <vector>
#include <atomic>
#include <cassert>

template<typename T>
class CircularQueue {
private:
    std::vector<T> buffer;
	size_t capacity;
	std::atomic<uint32_t> head;
	std::atomic<uint32_t> tail; // 데이터 삽입 할 위치

	size_t next(const uint32_t index)
	{
		return (index + 1) & (capacity - 1);
	}

	// 가까운 2의 거급 제곱 값으로 round up or down
	size_t round_to_pow2(const size_t size)
	{
		uint32_t upper = 0;
		uint32_t lower = 0;
		uint32_t value = size - 1;

		// 1, 2, 4, 8, 16 로 진행하며 하위 비트 모두 1로 채움
		for (uint32_t shift = 1; shift < 32; shift <<= 1) {
			value |= value >> shift;
		}

		upper = value + 1;
		lower = upper >> 1;
		if (size - lower <= upper - size) {
			return lower;
		} else {
			return upper;
		}
	}

public:
	CircularQueue(const size_t capacity = 4096) : buffer(), capacity(round_to_pow2(capacity)), head(0), tail(0)
	{
		buffer.resize(this->capacity);
	}

	CircularQueue(const CircularQueue& other): buffer(other.buffer), capacity(other.capacity), head(0), tail(0)
	{
		head = other.head.load(std::memory_order_relaxed);
		tail = other.tail.load(std::memory_order_relaxed);
	}

	CircularQueue(CircularQueue&& other): buffer(std::move(other.buffer)), capacity(other.capacity), head(0), tail(0)
	{
		head = other.head.exchange(0, std::memory_order_relaxed);
		tail = other.tail.exchange(0, std::memory_order_relaxed);
	}

	bool is_full()
	{
		uint32_t t = tail.load(std::memory_order_relaxed);
		uint32_t h = head.load(std::memory_order_acquire);

		return next(t) == h;
	}

	bool is_empty()
	{
		uint32_t h = head.load(std::memory_order_acquire);
		uint32_t t = tail.load(std::memory_order_acquire);

		return h == t; // 한 칸을 비워두고 empty와 full 상태 구분, 그렇지 않으면 head == tail 조건 하나만으론  empty인지 full인지 구분을 할 수 없어 count를 도입해야함
	}

	T& front()
	{
		assert(!is_empty());
		uint32_t h = head.load(std::memory_order_relaxed);
		return buffer[h];
	}

	bool push(T data)
	{
		uint32_t t = tail.load(std::memory_order_relaxed);
		uint32_t next_t = next(t);
		if (next_t != head.load(std::memory_order_acquire)) {
			buffer[t] = data;
			tail.store(next_t, std::memory_order_release);
			return true;
		}
		return false;
	}

	bool pop()
	{
		uint32_t h = head.load(std::memory_order_relaxed);
		if (h == tail.load(std::memory_order_acquire)) {
			return false;
		}

		uint32_t next_h = next(h);
		head.store(next_h, std::memory_order_release);
		return true;
	}

	void swap(CircularQueue& other)
	{
		std::swap(buffer, other.buffer);
		std::swap(capacity, other.capacity);
		head = other.head.exchange(head, std::memory_order_relaxed);
		tail = other.tail.exchange(tail, std::memory_order_relaxed);
	}

	friend std::ostream& operator<<(std::ostream& os, const CircularQueue<T>& queue)
	{
		uint32_t h = queue.head.load(std::memory_order_acquire);
		uint32_t t = queue.tail.load(std::memory_order_acquire);

		os << "size: " << queue.capacity << ", head: " << h << ", tail: " << t << std::endl;

		for (uint32_t index = h; index != t; index = (index + 1) & (queue.size - 1)) {
			os << queue.buffer[index] << ", ";
		}

		return os;
	}
};

