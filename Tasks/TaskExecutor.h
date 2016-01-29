#pragma once
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <memory>
#include <typeinfo>

#include "TaskFuture.h"
#include "priority_queue_threadsafe.h"
#include "Task.h"

enum Priority : unsigned char
{
	LAST_TO_EXECUTE = 0, //this can be used to enforce last task
	LOW = 40,
	MEDIUM_LOW = 80,
	MEDIUM = 120,
	MEDIUM_HIGH = 160,
	HIGH = 200,
	CRITICAL = 240,
	FIRST_TO_EXECUTE = 255 // this can be used to enforce first task
};

//Forward declarations from header file "TaskFuture.h". Solve circular dependency
template <class> class TaskFuture;
template <class, class...> class Task;
template <class, class...> class PackagedTask;
template <class, class...> struct task_data;
template <class> class shared_state_base;

//Type erasure for TaskExe
class Executable
{
public:
	Executable (Priority p) : m_priority(p) {}
	virtual ~Executable() {}
	virtual void execute() = 0;

protected:
	Priority m_priority;

public:
	//implement operator< for priority queue
	bool operator<(const Executable& rhs) const
	{
		return rhs.m_priority < this->m_priority;
	}
};

//We need to have Executable Sorted in the priority queue.
//But Executable is stored through a unique_ptr. Hence
inline bool operator< (const std::unique_ptr<Executable>& lhs,
	const std::unique_ptr<Executable>& rhs)
{
	return (*rhs.get() < *lhs.get());
}

//Common parts for the various specializations
template <class Res, class... Args>
class TaskExe_base : public Executable
{
	using _Base = Executable;
public:
	using TaskDataHandle = std::unique_ptr<task_data<Res, Args...>>;

	// Not default-constructible and not copyable
	TaskExe_base()                                = delete;
	TaskExe_base(const TaskExe_base&)             = delete;
	TaskExe_base& operator= (const TaskExe_base&) = delete;

	TaskExe_base(TaskExe_base&& other) : 
		_Base(other.m_priority),
		m_task_data_ptr(std::move(other.m_task_data_ptr)) {}

	TaskExe_base& operator=(TaskExe_base&& other)
	{
		if (this == &other)
			return *this;
		_Base::m_priority = other.m_priority;
		m_task_data_ptr = std::move(other.m_task_data_ptr);
		return *this;
	}

	explicit TaskExe_base(TaskDataHandle td, Priority p = MEDIUM) : 
		Executable(p), 
		m_task_data_ptr(std::move(td)) {}

protected:
	TaskDataHandle m_task_data_ptr;
};

template<class Res, class... Args>
class TaskExe final : public TaskExe_base<Res, Args...>
{
public:
	//Inherit constructors 
	using _Base = TaskExe_base<Res, Args...>;
	using _Base::_Base;

	void execute() override 
	{
		//Moving both the task and the tuple from the shared state
		//when executing the function.
		//NOTE: shared_state is basically now invalid.
		_Base::m_task_data_ptr->promise.set_value(
			move_apply(std::move(_Base::m_task_data_ptr->task), 
				std::move(_Base::m_task_data_ptr->arguments)));
	}
};

template<class... Args>
class TaskExe<void, Args...> final : public TaskExe_base<void, Args...>
{
public:
	//Inherit constructors
	using _Base = TaskExe_base<void, Args...>;
	using _Base::_Base;

	void execute() override
	{
		//Execute the function and notify the shared_state
		move_apply(std::move(_Base::m_task_data_ptr->task), 
			std::move(_Base::m_task_data_ptr->arguments));
		_Base::m_task_data_ptr->promise.set_value();
	}

};

//Base class for TaskExecutor and TaskExecutorPool.
//Implements common parts
class Executor_base
{
public:
	template <class Res, class... Args>
	TaskFuture<Res> schedule(PackagedTask<Res(Args...)>&& task, Priority p = MEDIUM)
	{
		//Get hold of the handle to task_data and obtain the future
		auto task_data_ptr = task.get_data_handle();
		TaskFuture<Res> future = task_data_ptr->promise.get_future();

		//Move the handle into a TaskExe and enque it
		m_queue.enqueue( std::make_unique<TaskExe<Res, Args...>>
			(std::move(task_data_ptr), p));

		//If a thread is asleep and waiting, wake it up
		m_cv.notify_one();

		//Set the executor_base in the future, so .then() can be implemented
		future.set_executor_base(this);

		return future;
	}

	//moves the task to the schedule implementation. Used to avoid having to use
	//std::move every time schedule is called. Task becomes invalid either way.
	template <class Res, class... Args>
	TaskFuture<Res> schedule(PackagedTask<Res(Args...)>& task, Priority p = MEDIUM)
	{
		return schedule(std::move(task), p);
	}

	//Another utility function. It lets you schedule anything that can be deduced
	//TODO: schedule needs some template magic to support variadic parameters
	//with a Priority at the end. 
	template <class Fn>
	decltype(auto) schedule(Fn&& fn, Priority p = MEDIUM)
	{
		return schedule(std::move(make_packaged_task(std::forward<Fn>(fn))));
	}

#ifndef DISABLE_CONTINUATIONS
	template <class ContRes, class Res2>
	TaskFuture<ContRes> schedule_continuation(TaskFuture<Res2>& task, 
		Task<ContRes(TaskFuture<Res2>)>&& continuation, 
		Priority p = MEDIUM)
	{
		//Get hold of the continuation handle to task_data and obtain the future
		auto task_data_ptr = continuation.get_data_handle();
		TaskFuture<ContRes> future = task_data_ptr->promise.get_future();

		//Set the continuation argument (eg. task);
		std::get<0>(task_data_ptr->arguments) =  std::move(task) ;

		//Set continuation in the task future 
		task._Base::m_state_ptr->set_continuation(
			std::make_unique<TaskExe<ContRes, TaskFuture<Res2>>>
			(std::move(task_data_ptr), p));

		//Set the executor_base in the future, so .then() can be implemented
		future.set_executor_base(this);

		return future;
	}

	//moves the task to the schedule implementation. Used to avoid having to use
	//std::move every time schedule is called. Task becomes invalid either way.
	template <class ContRes, class Res2>
	TaskFuture<ContRes> schedule_continuation(TaskFuture<Res2>& task,
		Task<ContRes(TaskFuture<Res2>)>& continuation,
		Priority p = MEDIUM)
	{
		return schedule_continuation(task, std::move(continuation), p);
	}

	///Test purposes. Ignore this
	template <class ContRes, class Res2>
	TaskFuture<ContRes> schedule_continuation(TaskFuture<Res2>& task,
		Task<ContRes(Res2)>&& continuation,
		Priority p = MEDIUM)
	{
		//Extract the function from task_data and put it in a wrapper class
		//Probably needs specializations for void.
		auto tsk = std::move(continuation.get_data_handle()->task);
		auto wrapper = [task = std::move(tsk)](TaskFuture<Res2> future) { return task(future.get()); };
		
		return schedule_continuation(task, make_task(wrapper), p);
	}

	//Another utility function. It lets you schedule anything that can be deduced
	//TODO: schedule needs some template magic to support variadic parameters
	//with a Priority at the end. 
	template <class Res, class Fn>
	decltype(auto) schedule_continuation(TaskFuture<Res>& task, Fn&& fn, Priority p = MEDIUM)
	{
		return schedule_continuation(task, make_task(std::forward<Fn>(fn)), p);
	}

	//Unpack the future automatically if the task does not take a future as param

private:
	template<class T> friend class shared_state_base;
	
	void m_schedule_continuation(std::unique_ptr<Executable> executable)
	{
		m_queue.enqueue(std::move(executable));
	}
#endif

protected:
	void notify_threads()
	{
		//Wake up all the threads, so they can be terminated
		m_cv.notify_all();
	}

protected:
	//std::threads will run this function
	static void run(Executor_base* owner, std::atomic_bool& alive);

private:
	concurrent_priority_queue<std::unique_ptr<Executable>> m_queue;
	std::mutex m_mutex;
	std::condition_variable m_cv;
	 
};

class TaskExecutor : public Executor_base
{
public:
	TaskExecutor()
	{
		//Pass a ref to thread as the constructor performs a copy
		m_executor = std::thread(_Base::run, this, std::ref(m_alive));
	}

	~TaskExecutor()
	{
		m_alive = false;
		_Base::notify_threads();
		m_executor.join();
	}

private:
	using _Base = Executor_base;
	std::thread m_executor;
	std::atomic_bool m_alive{ true };
};


//TODO: Probably want the thread pool to shrink and grow dynamically.
//Not supported yet though. a max size must be provided

template <size_t MAX_SIZE>
class TaskExecutorPool : public Executor_base
{
public:
	TaskExecutorPool() 
	{
		for (auto x = 0u; x < MAX_SIZE; ++x)
		{
			m_alive_threads[x] = true;
			m_executor_pool[x] = std::thread(
				_Base::run, this, std::ref(m_alive_threads[x]));
		}
	}

	~TaskExecutorPool()
	{
		for (auto&& alive : m_alive_threads)
			alive = false;

		_Base::notify_threads();

		for (auto&& executor : m_executor_pool)
			executor.join();
	}

private:
	using _Base = Executor_base;
	std::thread m_executor_pool[MAX_SIZE];
	std::atomic_bool m_alive_threads[MAX_SIZE];
};



inline void Executor_base::run(Executor_base* owner, std::atomic_bool& alive)
{
	std::unique_ptr<Executable> task;

	while (true)
	{
		//Put the thread to sleep while there are no tasks 
		//to be executed. Thread is also woken on Executor distructor
		{
			std::unique_lock<std::mutex> lk(owner->m_mutex);
			while (owner->m_queue.empty() && alive)
				owner->m_cv.wait(lk);

			if (!alive) break;

			//Dequeue the task before unlocking
			task = owner->m_queue.dequeue();
		}

		task->execute();
	}
}
