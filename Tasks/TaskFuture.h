#pragma once
#include <future>
#include <memory>
#include <mutex>
#include <chrono>


#include "TaskExecutor.h"

#define DISABLE_EXCEPTIONS false

/// Common implementation for shared_state<T> and shared_state<void>
template <class T>
class shared_state_base
{
public:
	bool is_ready() const
	{
		return m_is_ready;
	}
	
	void wait() noexcept
	{
		std::unique_lock<std::mutex> lk(m_mutex);
		while (!is_ready())
			m_cv.wait(lk);
	}

	template <class Rep, class Period>
	bool wait_for (const std::chrono::duration<Rep, Period>& dur_time)
	{
		std::unique_lock<std::mutex> lk(m_mutex);
		return m_cv.wait_for(lk, dur_time, [&] {return m_is_ready; });
	}

	template <class Rep, class Period>
	bool wait_until(const std::chrono::duration<Rep, Period>& end_time)
	{
		std::unique_lock<std::mutex> lk(m_mutex);
		return m_cv.wait_until(lk, end_time, [&] {return m_is_ready; });
	}

protected:
	void set_ready() noexcept
	{
		std::unique_lock<std::mutex> lk(m_mutex);
		m_is_ready = true;

		//Wake up whoever is waiting for this future to become ready
		m_cv.notify_all();

#ifndef DISABLE_CONTINUATIONS
		if (m_continuation != nullptr)
			m_task_executor_base->
			    m_schedule_continuation(std::move(m_continuation));
#endif
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_cv;

	//The following is required for continuations (*.then()*)
#ifndef DISABLE_CONTINUATIONS
public:
	void set_executor_base(Executor_base* e) { m_task_executor_base = e; }
	Executor_base* executor_base() const
	{
		return m_task_executor_base;
	}

	void set_continuation(std::unique_ptr<Executable> exe)
	{
		m_continuation = std::move(exe);
	}

private:
	using Continuation = std::unique_ptr<Executable>;

	Continuation m_continuation{nullptr};
	Executor_base* m_task_executor_base = nullptr;
#endif

protected:
	bool m_is_ready = false;
};

template <class T>
class shared_state : public shared_state_base<T>
{
	//Only TaskPromise can set the state value
	template <class> friend class TaskPromise;
	using _Base = shared_state_base<T>;
public: 
	~shared_state()
	{
		//call destructor explicitly after placement new
		if (_Base::is_ready())
			reinterpret_cast<T*>(&m_value)->~T();
	}
	bool already_retrieved() const { return m_is_retrieved; }

	//////////////////////////////////////////////////////////////////////
	/* user is responsible for checking that the value has not been
	moved from. Use already_retrieved()! */

	T get_move_value_or_void()
	{
		_Base::wait();
		m_is_retrieved = true;
		return std::move(*reinterpret_cast<T*>(&m_value));
	}

	T get_copy_value()
	{
		_Base::wait();
		return (*reinterpret_cast<T*>(&m_value));
	}

	const T& get_const_ref_value_or_void()
	{
		_Base::wait();
		return (*reinterpret_cast<T*>(&m_value));
	}

	////////////////////////////////////////////////////////////////////
private:
	template <class Res>
	void set_value(Res&& res) noexcept
	{
		/* NOTE: set is assumed to be called only by one thread
		(the owner of the promise) so no locking is required */
		if (_Base::m_is_ready) return;

		//Placement new on the aligned storage
		new (&m_value) T (std::forward<Res>(res));
		_Base::set_ready();
	}

private:
	//Use aligned_storage_t to get rid of default contruction.
	//T is thus only required to be copy or move constructible
	std::aligned_storage_t<sizeof(T), alignof(T)> m_value;
	bool m_is_retrieved = false;
};

///Specialization for void
template <>
class shared_state<void> : public shared_state_base<void>
{
public:
	template <class > friend class TaskPromise;
	using _Base = shared_state_base<void>;

	void get_move_value_or_void()
	{
		_Base::wait();
	}

	void get_const_ref_value_or_void()
	{
		_Base::wait();
	}
};


template <class Res>
class TaskFuture_Base
{
public:
	using SharedState_ptr = std::shared_ptr<shared_state<Res>>;

	bool is_ready() const
	{
		return m_state_ptr->is_ready();
	}

	void wait()
	{
		m_state_ptr->wait();
	}

	template <class Rep, class Period>
	bool wait_for(const std::chrono::duration<Rep, Period>& dur_time)
	{
		return m_state_ptr->wait_for(dur_time);
	}

	template <class Rep, class Period>
	bool wait_until(const std::chrono::duration<Rep, Period>& end_time)
	{
		return m_state_ptr->wait_until(end_time);
	}

protected:


	//template <class TSharedState>
	TaskFuture_Base(SharedState_ptr state) :
		m_state_ptr(state) {}

	//TaskFuture_Base(const SharedState_ptr& state) :
	//	m_state_ptr(state) {}
private:
	friend class Executor_base;

	void set_executor_base(Executor_base* e)
	{
		m_state_ptr->set_executor_base(e);
	}

protected:
	SharedState_ptr m_state_ptr;
};

///

template <class Res>
class SharedTaskFuture : public TaskFuture_Base<Res>
{
	template <class>
	friend class TaskFuture;

public:
	using _Base = TaskFuture_Base<Res>;
	using Sharedstate_ptr = typename _Base::SharedState_ptr;

	SharedTaskFuture(const SharedTaskFuture& other_future) :
		TaskFuture_Base(other_future._Base::m_state_ptr) {};

	SharedTaskFuture (SharedTaskFuture&& other_future) :
		TaskFuture_Base(std::move(other_future._Base::m_state_ptr)){}

	SharedTaskFuture& operator=(const SharedTaskFuture& other)
	{
		if (this == &other)
			return *this;
		_Base::m_state_ptr = other._Base::m_state_ptr;
		return *this;
	}

	SharedTaskFuture& operator=(SharedTaskFuture&& other)
	{
		if (this == &other)
			return *this;
		using std::swap;
		swap(_Base::m_state_ptr, other._Base::m_state_ptr);
		return *this;
	}

	decltype(auto) get()
	{ // either Ref& or void, depending on the specialization
		return _Base::m_state_ptr->get_const_ref_value_or_void();
	}

	template <class T = Res>
	std::enable_if_t<!std::is_same<T, void>::value, Res>
	get_copy()
	{ // only available for non-void types
		return _Base::m_state_ptr->get_copy_value();
	}

private:
	explicit SharedTaskFuture(Sharedstate_ptr&& state) :
		TaskFuture_Base(std::move(state)) {};

};

///

template <class Res>
class TaskFuture : public TaskFuture_Base<Res>
{
	template <class>
	friend class TaskPromise;

public:
	using _Base = TaskFuture_Base<Res>;
	using SharedState_ptr = typename _Base::SharedState_ptr;

	TaskFuture(const TaskFuture&) = delete;
	TaskFuture& operator= (const TaskFuture&) = delete;

	TaskFuture(TaskFuture&& other_future) : 
		TaskFuture_Base(std::move(other_future._Base::m_state_ptr)) {}

	TaskFuture& operator= (TaskFuture&& rhs) 
	{
		this->_Base::m_state_ptr =  rhs._Base::m_state_ptr;
		
		return *this;
	}

	decltype(auto) get()
	{ // returns either Res or void, depending on the specialization
		return _Base::m_state_ptr->get_move_value_or_void();
	}

	SharedTaskFuture<Res> share()
	{
		return SharedTaskFuture<Res>(std::move(_Base::m_state_ptr));
	}

	template <class TTask>
	decltype(auto) then(TTask&& task, Priority p = MEDIUM) 
	{
		return _Base::m_state_ptr->executor_base()->schedule_continuation(
			*this, std::forward<TTask>(task), p);
	}
	
	TaskFuture() : _Base(std::make_shared<shared_state<Res>>()) {}
private:
	explicit TaskFuture(const SharedState_ptr& state) : _Base(state) {}
};

///

template <class Res>
class TaskPromise
{
public:
	TaskPromise() : m_state(std::make_shared<shared_state<Res>>()){}
	TaskFuture<Res> get_future()
	{
		return TaskFuture<Res>(m_state);
	}

	template <class T, class R = Res>
	std::enable_if_t<!std::is_same<R, void>::value, void>
	set_value(T&& t)
	{
		m_state->set_value(std::forward<T>(t));
		return;
	}

	template <class R = Res>
	std::enable_if_t<std::is_same<R, void>::value, void>
	set_value()
	{
		m_state->set_ready();
		return;
	}

private:
	using SharedState_ptr = std::shared_ptr<shared_state<Res>>;
	SharedState_ptr m_state;
};

