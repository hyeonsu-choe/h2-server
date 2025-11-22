#pragma once

#include <vector>
#include <cassert>

template<typename T>
class CircularQueue {
private:
    std::vector<T> buffer;
    uint32_t front_index;
    uint32_t count;

public:
	CircularQueue(const size_t capacity = 1024) : buffer(capacity), front_index(0), count(0)
	{
	}

	CircularQueue(const CircularQueue& other): buffer(other.buffer), front_index(other.front_index), count(other.count)
	{
	}

	CircularQueue(CircularQueue&& other): buffer(std::move(other.buffer)), front_index(other.front_index), count(other.count)
	{
		other.front_index = 0;
		other.count = 0;
	}

	bool is_full()
	{
		return count == buffer.size();
	}

	bool is_empty()
	{
		return count == 0;
	}

	T& front()
	{
		assert(!is_empty());
		return buffer[front_index];
	}

	bool push(T data)
	{
		if (count < buffer.size()) {
			uint32_t index = (front_index + count) % buffer.size();
			buffer[index] = data;
			count++;
			return true;
		}
		return false;
	}

	bool pop()
	{
		if (!is_empty()) {
			front_index = (front_index + 1) % buffer.size();
			count--;
			return true;
		}
		return false;
	}

	void swap(CircularQueue& other)
	{
		std::swap(buffer, other.buffer);
		std::swap(front_index, other.front_index);
		std::swap(count, other.count);
	}

	friend std::ostream& operator<<(std::ostream& os, const CircularQueue<T>& queue)
	{
		if (queue.buffer.empty()) {
			return os;
		}

		uint32_t count_max = queue.count;
		for (uint32_t i = 0; i < count_max; i++) {
			os << queue.buffer[(queue.front_index + i) % queue.buffer.size()] << ", ";
		}
		os << std::endl;

		return os;
	}
};

