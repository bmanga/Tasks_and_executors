#pragma once
#include <functional>
#include <tuple>
#include <utility>
#include <typeinfo>
#include <iostream>

#include "TaskFuture.h"

namespace impl
{
	constexpr bool
	are_all_true()
	{
		return true;
	};

	template<typename Bool, typename... Tail>
	constexpr std::enable_if_t<std::is_same<bool, Bool>::value, bool>
	are_all_true(Bool first, Tail... tail)
	{
		return first && are_all_true(tail...);
	};

	template <class F, class Tuple, std::size_t... I>
	decltype(auto) apply_impl(F&& f, Tuple&& t, std::index_sequence<I...>)
	{
		return std::invoke(std::forward<F>(f), std::get<I>(std::forward<Tuple>(t))...);
		// Note: std::invoke is a C++17 feature
	}

	template <class>
	struct mem_fn_signature;

	template <class Ret, class C, class... Args>
	struct mem_fn_signature <Ret (C::*) (Args...)>
	{
		using type = Ret(Args...);
	};

	template <class Ret, class C, class... Args>
	struct mem_fn_signature <Ret(C::*) (Args...) const>
	{
		using type = Ret(Args...);
	};

	template <class T>
	using mem_fn_signature_t =
		typename mem_fn_signature<T>::type;
} // namespace impl

template <class F, template<class...> class Tuple, class... Tuple_Args>
decltype(auto) apply(F&& f, Tuple<Tuple_Args...> t)
{
	return impl::apply_impl(std::forward<F>(f), (t),
		std::make_index_sequence<sizeof...(Tuple_Args)>{});

}

namespace impl
{
	template <class F, class Tuple, std::size_t... I>
	decltype(auto) move_apply_impl(F&& f, Tuple&& t, std::index_sequence<I...>)
	{
		return std::invoke(std::forward<F>(f), std::move(std::get<I>(std::forward<Tuple>(t)))...);
		 //Note: std::invoke is a C++17 feature
	}
}

//Moves all the variables from the tuple. 
template <class F, template<class...> class Tuple, class... Tuple_Args>
decltype(auto) move_apply(F&& f, Tuple<Tuple_Args...>&& t)
{
	return impl::move_apply_impl(std::forward<F>(f), std::move(t),
		std::make_index_sequence<sizeof...(Tuple_Args)>{});

}


//TODO: could adapt to use EBC optimization
template <class Res, class... Args>
struct task_data
{
	std::function<Res(Args...)> task;
	std::tuple<Args...> arguments;
	TaskPromise<Res> promise;

	template <class Fn, class... TArgs>
	task_data(Fn&& fn, TArgs&&... args) :
		task(std::forward<Fn>(fn)),
		arguments(std::forward<TArgs>(args)...) {}

	template <class Fn>
	task_data(Fn&& fn) :
		task(std::forward<Fn>(fn)),
		arguments{} 
	{}
};



///task_data specialization requires specializing a lot of stuff
///(wherever task_data::arguments is referenced). Not worth the effort
//template <class Res>
//struct task_data<Res>
//{
//	std::function<Res()> task;
//	TaskPromise<Res> promise;
//};

template <class Ret, class... Args>
class Task_data_impl
{
	friend Executor_base;
public:
	using TaskDataHandle = std::unique_ptr<task_data<Ret, Args...>>;

	TaskDataHandle get_data_handle()
	{
		m_task_data_moved = true;
		return std::move(m_task_data_ptr);
	}

public:
	explicit Task_data_impl(TaskDataHandle data) :
		m_task_data_ptr(std::move(data)) {}

private:
	TaskDataHandle m_task_data_ptr;
	bool m_task_data_moved{ false };
};

template <class, class...>
class Task;

template <class Ret, class... Args>
class Task <Ret(Args...)> : public Task_data_impl<Ret, Args...>
{
	using _Base = Task_data_impl<Ret, Args...>;

public:
	template <class Fn>
	Task(Fn&& fn) :
		_Base(std::make_unique<task_data<Ret, Args...>>
			(std::forward<Fn>(fn))) {}

	//Use function pointers to allow passing an overloaded function
	Task(Ret(*fn_ptr)(Args...)) :
		_Base(std::make_unique<task_data<Ret, Args...>>
			(fn_ptr)) {}

};

template <class Ret, class... Args>
class PackagedTask;

template <class Ret, class... Args>
class PackagedTask <Ret(Args...)> : public Task_data_impl<Ret, Args...>
{
	friend class Executor_base;
	using _Base = Task_data_impl<Ret, Args...>;

public:
	template <class Fn, class... Fn_Args>
	PackagedTask(Fn&& fn, Fn_Args&&... args) :
		_Base(std::make_unique<task_data<Ret, Args...>>
			(std::forward<Fn>(fn), std::forward<Fn_Args>(args)...))
	{
		//catch some common errors at compile time
		static_assert(sizeof...(Fn_Args) == sizeof...(Args),
			"the number of passed parameters and function parameters"
			"does not match");
		static_assert(impl::are_all_true(
			(std::is_convertible<std::decay_t<Fn_Args>, Args>::value)...
			), "no conversion available for the passed arguments");
	}

	//Use function pointers to allow passing an overloaded function
	template <class... Fn_Args>
	PackagedTask(Ret(*fn_ptr)(Args...), Fn_Args&&... args):
		_Base(std::make_unique<task_data<Ret, Args...>>
			(fn_ptr, std::forward<Fn_Args>(args)...))
	{
		//catch some common errors at compile time
		static_assert(sizeof...(Fn_Args) == sizeof...(Args),
			"the number of passed parameters and function parameters"
			"does not match");
		static_assert(impl::are_all_true(
			(std::is_convertible<std::decay_t<Fn_Args>, Args>::value)...
			), "no conversion available for the passed arguments");
	}


	template <size_t Pos, class Arg>
	void set_argument(Arg&& arg)
	{
		if (_Base::m_task_data_moved) return;

		std::get<Pos>(_Base::m_task_data_ptr->arguments) = 
			std::forward<Arg>(arg);
	}

	template <class... TArgs>
	void set_arguments(TArgs&&... args)
	{
		if (_Base::m_task_data_moved) return;

		// move assignment operator
		_Base::m_task_data_ptr->arguments =
			std::tuple<Args...>(std::forward<Args>(args)...);
	}
};

//Utility make function
template <class Fn, class... Args>
auto make_packaged_task(Fn&& fn, Args&&... args) ->
	PackagedTask<std::result_of_t<Fn(Args...)>(Args...)>
{
	return{ std::forward<Fn>(fn), std::forward<Args>(args)... };
}

template <class Fn, class Ret, class... Args>
auto make_task_impl (Fn fn, Ret(Fn::*mf)(Args...) const)
{
	return Task<Ret(Args...)>{fn};  
}
template <class Fn>
decltype(auto) make_task(Fn fn)
{
	return make_task_impl(fn,&Fn::operator());
}

template <class Ret, class... Args>
auto make_task(Ret(*fn)(Args...))->
	Task<Ret(Args...)>
{
	return {fn};
}

