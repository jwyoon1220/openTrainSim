#pragma once
// A value or an error message: the subset of C++23 std::expected<T, std::string> the loaders
// use (the project is C++20). Moving to C++23 turns this file into one alias.
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace ots {

// Wraps an error message so `return Unexpected(...)` reads like std::unexpected.
struct Unexpected {
    std::string message;
    explicit Unexpected(std::string m) : message(std::move(m)) {}
};

template <class T>
class [[nodiscard]] Expected {
public:
    Expected(T value) : v_(std::in_place_index<0>, std::move(value)) {}
    Expected(Unexpected e) : v_(std::in_place_index<1>, std::move(e.message)) {}
    template <class U>
        requires(std::is_constructible_v<T, U&&> && !std::is_same_v<std::remove_cvref_t<U>, Expected> &&
                 !std::is_same_v<std::remove_cvref_t<U>, Unexpected> && !std::is_same_v<std::remove_cvref_t<U>, T>)
    Expected(U&& value) : v_(std::in_place_index<0>, T(std::forward<U>(value))) {}

    [[nodiscard]] bool has_value() const { return v_.index() == 0; }
    explicit operator bool() const { return has_value(); }
    T& operator*() & { return std::get<0>(v_); }
    const T& operator*() const& { return std::get<0>(v_); }
    T&& operator*() && { return std::get<0>(std::move(v_)); }
    T* operator->() { return &std::get<0>(v_); }
    const T* operator->() const { return &std::get<0>(v_); }
    [[nodiscard]] const std::string& error() const { return std::get<1>(v_); }

private:
    std::variant<T, std::string> v_;
};

// Success or an error message.
template <>
class [[nodiscard]] Expected<void> {
public:
    Expected() = default;
    Expected(Unexpected e) : error_(std::move(e.message)), ok_(false) {}
    [[nodiscard]] bool has_value() const { return ok_; }
    explicit operator bool() const { return ok_; }
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    std::string error_;
    bool ok_ = true;
};

} // namespace ots
