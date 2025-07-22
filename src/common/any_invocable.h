/*
 * @Author: DI JUNKUN
 * @Date: 2025-03-14
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ANY_INVOCABLE_H_
#define _ANY_INVOCABLE_H_

#include <functional>
#include <iostream>
#include <memory>
#include <type_traits>

namespace minirtc {

template <typename Signature>
class AnyInvocable;

template <typename R, typename... Args>
class AnyInvocable<R(Args...)> {
 public:
  AnyInvocable() = default;

  AnyInvocable(std::nullptr_t) noexcept : callable_(nullptr) {}

  template <typename Callable, typename = std::enable_if_t<!std::is_same_v<
                                   std::decay_t<Callable>, std::nullptr_t>>>
  AnyInvocable(Callable&& callable)
      : callable_(std::make_unique<CallableWrapper<Callable>>(
            std::forward<Callable>(callable))) {}

  R operator()(Args... args) {
    if (!callable_) {
      throw std::bad_function_call();
    }
    if constexpr (std::is_void_v<R>) {
      callable_->Invoke(std::forward<Args>(args)...);
    } else {
      return callable_->Invoke(std::forward<Args>(args)...);
    }
  }

  AnyInvocable(AnyInvocable&&) = default;
  AnyInvocable& operator=(AnyInvocable&&) = default;

  explicit operator bool() const { return static_cast<bool>(callable_); }

 private:
  struct CallableBase {
    virtual ~CallableBase() = default;
    virtual R Invoke(Args&&... args) = 0;
  };

  template <typename Callable>
  struct CallableWrapper : public CallableBase {
    CallableWrapper(Callable&& callable)
        : callable_(std::forward<Callable>(callable)) {}

    R Invoke(Args&&... args) override {
      if constexpr (std::is_void_v<R>) {
        callable_(std::forward<Args>(args)...);
      } else {
        return callable_(std::forward<Args>(args)...);
      }
    }

    Callable callable_;
  };

  std::unique_ptr<CallableBase> callable_;
};

template <typename R, typename... Args>
AnyInvocable<R(Args...)> MakeMoveOnlyFunction(std::function<R(Args...)>&& f) {
  return AnyInvocable<R(Args...)>(std::move(f));
}
}  // namespace minirtc

#endif  // _ANY_INVOCABLE_H_