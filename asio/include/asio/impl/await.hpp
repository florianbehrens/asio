//
// impl/await.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_IMPL_AWAIT_HPP
#define ASIO_IMPL_AWAIT_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <tuple>
#include <utility>
#include "asio/async_result.hpp"
#include "asio/detail/type_traits.hpp"
#include "asio/dispatch.hpp"
#include "asio/post.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

// Promise object for coroutine at top of thread-of-execution "stack".
class awaiter
{
public:
  awaiter* get_return_object()
  {
    return this;
  }

  auto initial_suspend()
  {
    return std::experimental::suspend_never();
  }

  auto final_suspend()
  {
    return std::experimental::suspend_always();
  }

  void unhandled_exception()
  {
    pending_exception_ = std::current_exception();
  }

  void set_exception(std::exception_ptr ex)
  {
    pending_exception_ = ex;
  }

  void return_void()
  {
  }

  awaiter* add_ref()
  {
    ++ref_count_;
    return this;
  }

  void release()
  {
    if (--ref_count_ == 0)
      coroutine_handle<awaiter>::from_promise(*this).destroy();
  }

  void rethrow_exception()
  {
    if (pending_exception_)
    {
      std::exception_ptr ex = std::exchange(pending_exception_, nullptr);
      std::rethrow_exception(ex);
    }
  }

private:
  std::size_t ref_count_ = 0;
  std::exception_ptr pending_exception_;
};

struct awaiter_delete
{
public:
  void operator()(awaiter* a)
  {
    if (a)
      a->release();
  }
};

typedef std::unique_ptr<awaiter, awaiter_delete> awaiter_ptr;

class awaitee_base
{
public:
  auto initial_suspend()
  {
    return std::experimental::suspend_never();
  }

  struct final_suspender
  {
    awaitee_base* this_;

    bool await_ready()
    {
      return false;
    }

    void await_suspend(coroutine_handle<void>)
    {
      this_->wake_caller();
    }

    void await_resume()
    {
    }
  };

  auto final_suspend()
  {
    return final_suspender{this};
  }

  void unhandled_exception()
  {
    pending_exception_ = std::current_exception();
    ready_ = true;
  }

  void set_exception(std::exception_ptr e)
  {
    pending_exception_ = e;
    ready_ = true;
  }

  void wake_caller()
  {
    if (caller_)
      caller_.resume();
  }

  bool ready()
  {
    return ready_;
  }

  void set_caller(coroutine_handle<void> h)
  {
    caller_ = h;
  }

protected:
  void rethrow_exception()
  {
    if (pending_exception_)
    {
      std::exception_ptr ex = std::exchange(pending_exception_, nullptr);
      std::rethrow_exception(ex);
    }
  }

  template <typename> friend class awaitable;
  awaiter* awaiter_ = nullptr;
  coroutine_handle<void> caller_ = nullptr;
  std::exception_ptr pending_exception_ = nullptr;
  bool ready_ = false;
};

template <typename T>
class awaitee
  : public awaitee_base
{
public:
  ~awaitee()
  {
    if (initialised_)
    {
      T* p = static_cast<T*>(static_cast<void*>(buf_));
      p->~T();
    }
  }

  awaitable<T> get_return_object()
  {
    return awaitable<T>(this);
  };

  template <typename U>
  void return_value(U&& u)
  {
    T* p = static_cast<T*>(static_cast<void*>(buf_));
    new (p) T(std::forward<U>(u));
    initialised_ = true;
  }

  T value()
  {
    rethrow_exception();
    return std::move(*static_cast<T*>(static_cast<void*>(buf_)));
  }

private:
  template <typename> friend class awaitable;
  alignas(T) unsigned char buf_[sizeof(T)];
  bool initialised_ = false;
};

template <>
class awaitee<void>
  : public awaitee_base
{
public:
  awaitable<void> get_return_object()
  {
    return awaitable<void>(this);
  };

  void return_void()
  {
  }

  void value()
  {
    rethrow_exception();
  }
};

#if defined(_MSC_VER)
# pragma warning(push)
# pragma warning(disable:4033)
#endif // defined(_MSC_VER)

template <typename T> T dummy_return()
{
  return static_cast<T&&>(*static_cast<T*>(nullptr));
}

template <> void dummy_return<void>()
{
}

template <typename T>
awaitable<T> make_dummy_awaitable()
{
  for (;;) co_await std::experimental::suspend_always();
  co_return dummy_return<T>();
}

#if defined(_MSC_VER)
# pragma warning(pop)
#endif // defined(_MSC_VER)

template <typename Executor>
class destroy_awaiter
{
public:
  typedef Executor executor_type;

  destroy_awaiter(Executor ex, awaiter_ptr a)
    : ex_(ex),
      awaiter_(std::move(a))
  {
  }

  destroy_awaiter(destroy_awaiter&& other)
    : ex_(std::move(other.ex_)),
      awaiter_(std::move(other.awaiter_))
  {
  }

  executor_type get_executor() const noexcept
  {
    return ex_;
  }

  void operator()()
  {
    awaiter_ptr(std::move(awaiter_));
  }

protected:
  Executor ex_;
  awaiter_ptr awaiter_;
};

template <typename Executor>
class awaiter_task
{
public:
  typedef Executor executor_type;

  awaiter_task(Executor ex, awaiter* a)
    : ex_(ex),
      awaiter_(a->add_ref())
  {
  }

  awaiter_task(awaiter_task&& other)
    : ex_(std::move(other.ex_)),
      awaiter_(std::move(other.awaiter_))
  {
  }

  ~awaiter_task()
  {
    if (awaiter_)
      (post)(destroy_awaiter<Executor>(ex_, std::move(awaiter_)));
  }

  executor_type get_executor() const noexcept
  {
    return ex_;
  }

protected:
  Executor ex_;
  awaiter_ptr awaiter_;
};

template <typename Executor>
class spawn_handler : public awaiter_task<Executor>
{
public:
  using awaiter_task<Executor>::awaiter_task;

  void operator()()
  {
    awaiter_ptr ptr(std::move(this->awaiter_));
    coroutine_handle<awaiter>::from_promise(*ptr).resume();
  }
};

template <typename Executor, typename T>
class await_handler_base : public awaiter_task<Executor>
{
public:
  typedef awaitable<T> awaitable_type;

  await_handler_base(basic_unsynchronized_await_context<Executor> ctx)
    : awaiter_task<Executor>(ctx.get_executor(), ctx.awaiter_),
      awaitee_(nullptr)
  {
  }

  awaitable<T> make_awaitable()
  {
    awaitable<T> a(make_dummy_awaitable<T>());
    awaitee_ = a.awaitee_;
    return a;
  }

protected:
  awaitee<T>* awaitee_;
};

template <typename Executor, typename... Args>
class await_handler;

template <typename Executor>
class await_handler<Executor>
  : public await_handler_base<Executor, void>
{
public:
  using await_handler_base<Executor, void>::await_handler_base;

  void operator()()
  {
    awaiter_ptr ptr(std::move(this->awaiter_));
    this->awaitee_->return_void();
    this->awaitee_->wake_caller();
    ptr->rethrow_exception();
  }
};

template <typename Executor>
class await_handler<Executor, error_code>
  : public await_handler_base<Executor, void>
{
public:
  typedef void return_type;

  using await_handler_base<Executor, void>::await_handler_base;

  void operator()(error_code ec)
  {
    awaiter_ptr ptr(std::move(this->awaiter_));
    if (ec)
      this->awaitee_->set_exception(std::make_exception_ptr(system_error(ec)));
    else
      this->awaitee_->return_void();
    this->awaitee_->wake_caller();
    ptr->rethrow_exception();
  }
};

template <typename Executor>
class await_handler<Executor, std::exception_ptr>
  : public await_handler_base<Executor, void>
{
public:
  using await_handler_base<Executor, void>::await_handler_base;

  void operator()(std::exception_ptr ex)
  {
    awaiter_ptr ptr(std::move(this->awaiter_));
    if (ex)
      this->awaitee_->set_exception(ex);
    else
      this->awaitee_->return_void();
    this->awaitee_->wake_caller();
    ptr->rethrow_exception();
  }
};

template <typename Executor, typename T>
class await_handler<Executor, T>
  : public await_handler_base<Executor, T>
{
public:
  using await_handler_base<Executor, T>::await_handler_base;

  void operator()(T t)
  {
    awaiter_ptr ptr(std::move(this->awaiter_));
    this->awaitee_->return_value(std::forward<T>(t));
    this->awaitee_->wake_caller();
    ptr->rethrow_exception();
  }
};

template <typename Executor, typename T>
class await_handler<Executor, error_code, T>
  : public await_handler_base<Executor, T>
{
public:
  using await_handler_base<Executor, T>::await_handler_base;

  void operator()(error_code ec, T t)
  {
    awaiter_ptr ptr(std::move(this->awaiter_));
    if (ec)
      this->awaitee_->set_exception(std::make_exception_ptr(system_error(ec)));
    else
      this->awaitee_->return_value(std::forward<T>(t));
    this->awaitee_->wake_caller();
    ptr->rethrow_exception();
  }
};

template <typename Executor, typename T>
class await_handler<Executor, std::exception_ptr, T>
  : public await_handler_base<Executor, T>
{
public:
  using await_handler_base<Executor, T>::await_handler_base;

  void operator()(std::exception_ptr ex, T t)
  {
    awaiter_ptr ptr(std::move(this->awaiter_));
    if (ex)
      this->awaitee_->set_exception(ex);
    else
      this->awaitee_->return_value(std::forward<T>(t));
    this->awaitee_->wake_caller();
    ptr->rethrow_exception();
  }
};

template <typename Executor>
class make_await_context
{
public:
  explicit make_await_context(Executor ex)
    : ex_(ex)
  {
  }

  bool await_ready()
  {
    return false;
  }

  void await_suspend(coroutine_handle<detail::awaiter> h)
  {
    awaiter_ = &h.promise();
  }

  basic_unsynchronized_await_context<Executor> await_resume()
  {
    return basic_unsynchronized_await_context<Executor>(ex_, awaiter_);
  }

private:
  Executor ex_;
  awaiter* awaiter_ = nullptr;
};

template <typename T>
struct awaitable_signature;

template <typename T>
struct awaitable_signature<awaitable<T>>
{
  typedef void type(std::exception_ptr, T);
};

template <>
struct awaitable_signature<awaitable<void>>
{
  typedef void type(std::exception_ptr);
};

template <typename T, typename WorkGuard, typename Handler,
    typename Executor, typename F, typename... Args>
awaiter* spawn_entry_point(awaitable<T>*, WorkGuard work_guard,
    Handler handler, Executor ex, F f, Args... args)
{
  bool done = false;

  try
  {
    T t = co_await std::invoke(std::move(f), std::move(args)...,
        co_await make_await_context<Executor>(ex));

    done = true;

    (dispatch)(work_guard.get_executor(),
        [handler = std::move(handler), t = std::move(t)]() mutable
        {
          handler(std::exception_ptr(), std::move(t));
        });
  }
  catch (...)
  {
    if (done)
      throw;

    (dispatch)(work_guard.get_executor(),
        [handler = std::move(handler), e = std::current_exception()]() mutable
        {
          handler(e, T());
        });
  }
}

template <typename WorkGuard, typename Handler,
    typename Executor, typename F, typename... Args>
awaiter* spawn_entry_point(awaitable<void>*, WorkGuard work_guard,
    Handler handler, Executor ex, F f, Args... args)
{
  std::exception_ptr e = nullptr;

  try
  {
    co_await std::invoke(std::move(f), std::move(args)...,
        co_await make_await_context<Executor>(ex));
  }
  catch (...)
  {
    e = std::current_exception();
  }

  (dispatch)(work_guard.get_executor(),
      [handler = std::move(handler), e]() mutable
      {
        handler(e);
      });
}

template <typename CompletionToken,
    typename Executor, typename F, typename... Args>
auto spawn(CompletionToken&& token, const Executor& ex, F&& f, Args&&... args)
{
  typedef decltype(
      std::invoke(std::declval<std::decay_t<F>>(),
        std::declval<std::decay_t<Args>>()...,
        std::declval<basic_unsynchronized_await_context<Executor>>()))
    awaitable_type;

  typedef typename awaitable_signature<awaitable_type>::type signature_type;

  async_completion<CompletionToken, signature_type> completion(token);

  auto work_guard = make_work_guard(completion.handler, ex);
  awaiter* a = (spawn_entry_point)(static_cast<awaitable_type*>(nullptr),
      std::move(work_guard), std::move(completion.handler), ex,
      std::forward<F>(f), std::forward<Args>(args)...);

  (post)(spawn_handler<Executor>(ex, a));

  return completion.result.get();
}

template <typename ArgTypes, typename Executor,
    typename F, typename CompletionToken>
inline auto spawn_reorder(std::index_sequence<> index_seq,
    const Executor& ex, F&& f, CompletionToken&& token)
{
  return (spawn)(std::forward<CompletionToken>(token), ex, std::forward<F>(f));
}

template <typename ArgTypes, std::size_t... Index,
    typename Executor, typename F, typename CompletionToken,
    typename = typename enable_if<sizeof...(Index)>::type>
inline auto spawn_reorder(std::index_sequence<Index...> index_seq,
    const Executor& ex, F&& f, std::tuple_element_t<Index, ArgTypes>&&... args,
    CompletionToken&& token)
{
  return (spawn)(std::forward<CompletionToken>(token), ex, std::move(f),
      std::forward<std::tuple_element_t<Index, ArgTypes>>(args)...);
}

} // namespace detail

template <typename T>
awaitable<T>::~awaitable()
{
  if (awaitee_)
  {
    detail::coroutine_handle<
      detail::awaitee<T>>::from_promise(
        *awaitee_).destroy();
  }
}

template <typename T>
inline bool awaitable<T>::await_ready()
{
  return awaitee_->ready();
}

template <typename T>
inline void awaitable<T>::await_suspend(
    detail::coroutine_handle<detail::awaiter> h)
{
  awaitee_->set_caller(h);
}

template <typename T> template <typename U>
inline void awaitable<T>::await_suspend(
    detail::coroutine_handle<detail::awaitee<U>> h)
{
  awaitee_->set_caller(h);
}

template <typename T>
inline T awaitable<T>::await_resume()
{
  awaitee_->set_caller(nullptr);
  return awaitee_->value();
}

template <typename Executor, typename R, typename... Args>
struct handler_type<basic_unsynchronized_await_context<Executor>, R(Args...)>
{
  typedef detail::await_handler<
    Executor, typename decay<Args>::type...> type;
};

template <typename Executor, typename... Args>
class async_result<detail::await_handler<Executor, Args...>>
{
public:
  typedef typename detail::await_handler<
    Executor, Args...>::awaitable_type type;

  async_result(detail::await_handler<Executor, Args...>& h)
    : awaitable_(h.make_awaitable())
  {
  }

  type get()
  {
    return std::move(awaitable_);
  }

private:
  type awaitable_;
};

template <typename Executor, typename F, typename... Args, typename>
inline auto spawn(const Executor& ex, F&& f, Args&&... args)
{
  static_assert(sizeof...(Args) > 0, "CompletionToken required");

  return detail::spawn_reorder<std::tuple<Args...>>(
      std::make_index_sequence<sizeof...(Args) - 1>(),
      ex, std::forward<F>(f), std::forward<Args>(args)...);
}

template <typename ExecutionContext, typename F, typename... Args, typename>
inline auto spawn(ExecutionContext& ctx, F&& f, Args&&... args)
{
  return (spawn)(ctx.get_executor(), std::forward<F>(f),
      std::forward<Args>(args)...);
}

template <typename Executor, typename F, typename... Args>
inline auto spawn(const basic_unsynchronized_await_context<Executor>& ctx,
    F&& f, Args&&... args)
{
  return (spawn)(ctx.get_executor(), std::forward<F>(f),
      std::forward<Args>(args)...);
}

} // namespace asio

namespace std {
namespace experimental {

template <typename... Args>
struct coroutine_traits<asio::detail::awaiter*, Args...>
{
  typedef asio::detail::awaiter promise_type;
};

template <typename T, typename... Args>
struct coroutine_traits<asio::awaitable<T>, Args...>
{
  typedef asio::detail::awaitee<T> promise_type;
};

} // namespace experimental
} // namespace std

#include "asio/detail/pop_options.hpp"

#endif // ASIO_IMPL_AWAIT_HPP
