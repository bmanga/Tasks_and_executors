#pragma once
#include <vector>
#include <queue>
#include <mutex>
template <class T, class Container = std::vector<T>>
class atomic_priority_queue
{
public:
	using container_type = Container;
	using reference_type = T&;
	using value_type = T;
	using size_type = size_t;

	reference_type top()
	{
				
	}
private:
	container_type m_data;
};


// unoptimized version. needs to be fixed
template <class T>
class concurrent_priority_queue
{
public:
	using value_type = T;
	using reference = T&;
	using const_reference = const T&;
	using pointer = T*;
	using const_pointer = const T*;
	using size_type = size_t;
	//using iterator;

	value_type dequeue()
	{
		lock lk(m_mutex);

		value_type res = std::move(m_queue.front());

		m_queue.pop_front();

		return res;
	}

	void enqueue(const T& value)
	{
		lock lk(m_mutex);
		m_queue.push(value);
	}

	void enqueue(T&& value)
	{
		lock lk(m_mutex);
		m_queue.push_back(std::move(value));
		std::stable_sort(m_queue.begin(), m_queue.end(), std::greater<>());
	}

	size_type size()
	{
		lock lk(m_mutex);
		return m_queue.size();
	}

	bool empty()
	{
		return size() == 0;
	}
private:
	using lock = std::lock_guard<std::mutex>;
	std::deque<T> m_queue;
	std::mutex m_mutex;
};