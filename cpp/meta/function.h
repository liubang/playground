//=====================================================================
//
// function.h -
//
// Created by liubang on 2023/10/09 23:26
// Last Modified: 2023/10/09 23:26
//
//=====================================================================

#pragma once

#include "cpp/meta/traits.h"
#include <memory>
#include <utility>

namespace pl {

template <typename Fn> struct Function {
    static_assert(!is_same_v<Fn, Fn>, "invalid function.");
};

template <typename Ret, typename... Args> struct Function<Ret(Args...)> {
public:
    Function() = default;

    template <typename F, typename = enable_if_t<std::is_invocable_r_v<Ret, F&, Args...>>>
    Function(F f) : func_(std::make_shared<FuncImpl<F>>(std::move(f))) {}

    Ret operator()(Args... args) const {
        if (!func_) [[unlikely]] {
            throw std::runtime_error("function not initialized");
        }
        return func_->call(std::forward<Args>(args)...);
    }

private:
    struct FuncInter {
        virtual Ret call(Args... args) = 0;
        virtual ~FuncInter() = default;
    };

    template <typename F> struct FuncImpl : FuncInter {
        F f_;

        FuncImpl(F f) : f_(std::move(f)) {}

        Ret call(Args... args) override { return f_(std::forward<Args>(args)...); }
    };

private:
    std::shared_ptr<FuncInter> func_;
};

} // namespace pl
