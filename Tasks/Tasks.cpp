// Tasks.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "Task.h"
#include <iostream>
#include <string>
#include <chrono>
#include <typeinfo>
#include "TaskExecutor.h"
#include "TaskFuture.h"

class Expensive
{
public:
	explicit Expensive(int m) : m_x(m)
	{
		std::cout << "int constructor" << std::endl;
	}


	Expensive(const Expensive& other) : m_x(other.m_x)
	{
		std::cout << "copy constructor" << std::endl;
	}

	Expensive(Expensive&& other)
	{
		using std::swap;
		swap(m_x, other.m_x);
		std::cout << "move constructor" << std::endl;
	}

	Expensive& operator=(const Expensive& other)
	{
		std::cout << "copy assignment" << std::endl;
		if (this == &other)
			return *this;
		m_x = other.m_x;
		return *this;
	}

	Expensive& operator=(int x)
	{
		std::cout << "assignment int" << std::endl;
		m_x = x;
		return *this;
	}
	Expensive& operator=(Expensive&& other)
	{
		std::cout << "move assignment" << std::endl;
		if (this == &other)
			return *this;
		using std::swap;
		swap(m_x, other.m_x);
		return *this;
	}

	Expensive operator+ (const Expensive& other) const
	{
		return Expensive{ m_x + other.m_x };
	}
public:
	int m_x;
};

Expensive expensive_fn(const Expensive& a, const Expensive& b)
{
	return (a + b);
}

template <class... Lambdas>
struct overloaded_lambdas : public Lambdas...
{
	overloaded_lambdas(Lambdas... lambdas) : Lambdas(lambdas)...{}
};

template <class... Lambdas>
auto lambda_overload(Lambdas... lambdas)
{
	return overloaded_lambdas<Lambdas...> {lambdas...};
}

void exe_loop(Executor_base* executor)
{
	static int times = 0;
	for (int x = 0; x < 10; ++x)
		std::cout << "executing for x = " << x << "times" << times << std::endl;
	++times;

	if (times < 10)
	{
		PackagedTask<void(Executor_base*)> tmp(exe_loop, executor);
		executor->schedule(tmp);
	}
}
int main()
{
	
	auto lo = lambda_overload(
		[](int x, int y) { std::cout << "lowest priority" << std::endl; return x + y; },
		[](double x, double y) {return x - y; },
		[](auto x, auto y) {return x * y; }
	);

	std::cout << "sizeof overload is " << sizeof(lo) << std::endl;
	auto w = lo(3.333, 3.3);
	{
		using namespace std::literals;

		TaskExecutorPool<3> tp;
		auto t1 = make_packaged_task(lo, 2, 5);
		PackagedTask<double(int, int)> t2(foo1, 2, 30000000);
		auto task = Task<int(double)>([](double x)->int { std::cout << "I got : " << x << std::endl; return -1; });
		auto futr= tp.schedule(t2).then(std::move(task)).then([](int t) ->std::string
		{
			std::cout << "and I got" << t << std::endl;
			return ":)";
		});


		auto sched3 = tp.schedule([]() {return "hello "s; }).then([](std::string s) { return s + "Bruno!"; }).then([](std::string s) { std::cout << s << std::endl; });

		

		for (int x = 0; x < 100; ++x) std::cout << "bla bla bla" << std::endl;
		std::cout << futr.get() << std::endl;

		sched3.get();
		//PackagedTask<double(int, int)> t3(foo1, 3, 70000000);
		//PackagedTask<double(int, int)> t4(foo1, 4, 10000000);
		//PackagedTask<double(int, int)> t5(foo1, 5, 20000000);
		//PackagedTask<void(int, int)> t6(foo2, 6, 10000000);
		//PackagedTask<int(int)> t7(foo1, 11);
		//auto t8 = make_packaged_task(foo3);

		////auto tmp = make_packaged_task(foo1, 12);

		//auto fut2 = tp.schedule(t2, LOW);
		//auto fut3 = tp.schedule(t3, LOW);
		//auto fut4 = tp.schedule(t4);
		//auto fut5 = tp.schedule(t5);
		//auto fut6 = tp.schedule([] { std::cout << "hello :)\n" << std::endl; });
		//auto fut7 = tp.schedule(t8, CRITICAL);
		////fut7.then(make_packaged_task([](int x)->void {std::cout << x << std::endl; }, 0));
		//auto fut8 = tp.schedule(make_packaged_task(exe_loop, &tp));

		//auto task = make_task(foo4);

		//tp.schedule(
		//	make_packaged_task([] { std::cout << "bye bye\n" << std::endl; }), 
		//	LAST_TO_EXECUTE
		//).get();



		//fut2.get(), fut3.get();// , fut4.get(), fut5.get(), fut6.get();
	}
	system("PAUSE");
    return 0;
}

