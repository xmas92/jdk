/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_UTILITIES_EXPECTED_HPP
#define SHARE_UTILITIES_EXPECTED_HPP

#include "metaprogramming/enableIf.hpp"
#include "utilities/globalDefinitions.hpp"

#include <type_traits>

#if defined (__cpp_if_constexpr) && __cpp_if_constexpr >= 201606L
#define IF_CONSTEXPR constexpr
#else
#define IF_CONSTEXPR /* C++17 */
#endif

#if defined (__cpp_constexpr) && __cpp_constexpr >= 202002L
#define CONSTEXPR_DESTRUCTOR constexpr
#else
#define CONSTEXPR_DESTRUCTOR /* C++20 */
#endif

namespace {
  // Later C++20 <type_traits> Helpers
  template <typename T>
  struct remove_cvref { using type = std::remove_cv_t<std::remove_reference_t<T>>; };
  template <typename T>
  using remove_cvref_t = typename remove_cvref<T>::type;

  // Later C++17 <type_traits> Helpers
  template <typename T, typename U>
  static constexpr bool is_convertible_v = std::is_convertible<T, U>::value;
  template <typename T, typename U>
  static constexpr bool is_same_v = std::is_same<T, U>::value;
  template <typename T, typename... Args>
  static constexpr bool is_constructible_v = std::is_constructible<T, Args...>::value;
  template <typename T>
  static constexpr bool is_default_constructible_v = std::is_default_constructible<T>::value;
  template <typename T>
  static constexpr bool is_copy_constructible_v = std::is_copy_constructible<T>::value;
  template <typename T>
  static constexpr bool is_copy_assignable_v = std::is_copy_assignable<T>::value;
  template <typename T>
  static constexpr bool is_move_constructible_v = std::is_move_constructible<T>::value;
  template <typename T>
  static constexpr bool is_move_assignable_v = std::is_move_assignable<T>::value;
  template <typename T>
  static constexpr bool is_trivially_destructible_v = std::is_trivially_destructible<T>::value;
  template <typename T>
  static constexpr bool is_void_v = std::is_void<T>::value;

  // <utility> Header Helpers
  template <typename T>
  constexpr T&& forward(typename std::remove_reference<T>::type& t) { return static_cast<T&&>(t); }
  template <typename T>
  constexpr typename std::remove_reference<T>::type&& move(T&& t) { return static_cast<typename std::remove_reference<T>::type&&>(t); }

  // Later C++20 <memory> Header Helpers
  template <typename T, typename... Args>
  constexpr T* construct_at(T* location, Args&&... args) {
    return ::new(static_cast<void*>(location)) T(forward<Args>(args)...);
  }

  // Template Helpers
  template <typename, template <typename...> class>
  struct is_specialization_of : std::false_type {};
  template <template <typename...> class Type, typename... Args>
  struct is_specialization_of<Type<Args...>, Type> : std::true_type {};
  template <typename T, template <typename...> class Type>
  static constexpr bool is_specialization_of_v = is_specialization_of<T, Type>::value;
};

struct InPlaceMark { explicit InPlaceMark() = default; };

// C++17
//inline constexpr InPlaceType IN_PLACE {};

struct UnexpectedMark { explicit UnexpectedMark() = default; };

// C++17
//inline constexpr UnexpectedMark UNEXPECTED {};

template <typename E>
class Unexpected {
  template <class T, class G>
  friend class Expected;
private:
  E _error;

public:
  constexpr Unexpected(const Unexpected&) = default;

  template <typename G = E, ENABLE_IF(
    !is_same_v<remove_cvref_t<G>, Unexpected> &&
    !is_same_v<remove_cvref_t<G>, InPlaceMark> &&
    is_constructible_v<E, G>
  )>
  constexpr explicit Unexpected(G&& e)
    : _error(forward<G>(e)) {};

  template <typename... Args>
  constexpr explicit Unexpected(InPlaceMark, Args&&... args)
    : _error(forward<Args>(args)...) {};

  constexpr const E& error() const& { return _error; };
  constexpr E& error() & { return _error; };
  constexpr const E&& error() const&& { return move(_error); };
  constexpr E&& error() && { return move(_error); };

  // Unimplemented due to lack of <utility> / std::swap
  // constexpr void swap(Unexpected& other) { using std::swap; swap(_error, other._error); }

  template <typename T>
  friend constexpr bool operator==(Unexpected& x, Unexpected& y) {
    return x.error() == y.error();
  }

  // Unimplemented due to lack Unexpected::swap(Unexpected&)
  // friend constexpr void swap(Unexpected& x, Unexpected& y) { x.swap(y); }
};

// C++17 Class template argument deduction guide
// template <class T> Unexpected(T) -> Unexpected<T>;

template <typename T, typename E>
class Expected {
public:
  using value_type = T;
  using error_type = E;
  using unexpected_type = Unexpected<E>;
  template <typename U>
  using rebind = Expected<U, error_type>;

private:
  union {
      T _value;
      E _error;
  };
  bool _has_value;

  template <typename U, typename G>
  static constexpr bool disallowed_expected = (
    is_constructible_v<T, Expected<U, G>&> ||
    is_constructible_v<T, Expected<U, G>> ||
    is_constructible_v<T, const Expected<U, G>&> ||
    is_constructible_v<T, const Expected<U, G>> ||
    is_convertible_v<Expected<U, G>&, T> ||
    is_convertible_v<Expected<U, G>, T> ||
    is_convertible_v<const Expected<U, G>&, T> ||
    is_convertible_v<const Expected<U, G>, T> ||
    is_constructible_v<Unexpected<E>, Expected<U, G>&> ||
    is_constructible_v<Unexpected<E>, Expected<U, G>> ||
    is_constructible_v<Unexpected<E>, const Expected<U, G>&> ||
    is_constructible_v<Unexpected<E>, const Expected<U, G>>
  );
  template <typename U, typename G>
  static constexpr bool is_copy_constructible_expected = (
    !(is_same_v<T, U> && is_same_v<E, G>) &&
    is_constructible_v<T, const U&> &&
    is_constructible_v<E, const G&> &&
    !disallowed_expected<U, G>
  );
  template <typename U, typename G>
  static constexpr bool is_move_constructible_expected = (
    !(is_same_v<T, U> && is_same_v<E, G>) &&
    is_constructible_v<T, U> &&
    is_constructible_v<E, G> &&
    !disallowed_expected<U, G>
  );

public:
  template <ENABLE_IF(is_default_constructible_v<T>)>
  constexpr Expected() : _value(), _has_value(true) {};

  // Trivial = default optimization requires requires (C++20)
  template <ENABLE_IF(
    is_copy_constructible_v<T> &&
    is_copy_constructible_v<E>
  )>
  constexpr Expected(const Expected& other)
    : _has_value(other.has_value()) {
    if (has_value()) {
      construct_at(&_value, other._value);
    } else {
      construct_at(&_error, other._error);
    }
  }
  // requires requires (C++20)
  // constexpr Expected(const Expected&) = delete;

  // Trivial = default optimization requires requires (C++20)
  template <ENABLE_IF(
    is_move_constructible_v<T> &&
    is_move_constructible_v<E>
  )>
  constexpr Expected(Expected&& other)
    : _has_value(other.has_value()) {
    if (has_value()) {
      construct_at(&_value, move(other._value));
    } else {
      construct_at(&_error, move(other._error));
    }
  }
  // requires requires (C++20)
  // constexpr Expected(Expected&&) = delete;

  template <typename U, typename G, ENABLE_IF(
    is_copy_constructible_expected<U, G> &&
    (!is_convertible_v<const U&, T> || !is_convertible_v<const G&, E>)
  )>
  constexpr explicit Expected(const Expected<U, G>& other)
    : _has_value(other.has_value()) {
    if (has_value()) {
      construct_at(&_value, other._value);
    } else {
      construct_at(&_error, other._error);
    }
  }
  template <typename U, typename G, ENABLE_IF(
    is_copy_constructible_expected<U, G> &&
    (is_convertible_v<const U&, T> && is_convertible_v<const G&, E>)
  )>
  constexpr Expected(const Expected<U, G>& other)
    : _has_value(other.has_value()) {
    if (has_value()) {
      construct_at(&_value, other._value);
    } else {
      construct_at(&_error, other._error);
    }
  }

  template <typename U, typename G, ENABLE_IF(
    is_move_constructible_expected<U, G> &&
    (!is_convertible_v<U, T> || !is_convertible_v<G, E>)
  )>
  constexpr explicit Expected(Expected<U, G>&& other)
    : _has_value(other.has_value()) {
    if (has_value()) {
      construct_at(&_value, move(other._value));
    } else {
      construct_at(&_error, move(other._error));
    }
  }

  template <typename U, typename G, ENABLE_IF(
    is_move_constructible_expected<U, G> &&
    (is_convertible_v<U, T> && is_convertible_v<G, E>)
  )>
  constexpr Expected(Expected<U, G>&& other)
    : _has_value(other.has_value()) {
    if (has_value()) {
      construct_at(&_value, move(other._value));
    } else {
      construct_at(&_error, move(other._error));
    }
  }

  template <typename U = std::remove_cv_t<T>, ENABLE_IF(
    !is_same_v<remove_cvref_t<U>, InPlaceMark> &&
    !is_same_v<remove_cvref_t<U>, UnexpectedMark> &&
    !is_same_v<Expected, remove_cvref_t<U>> &&
    !is_constructible_v<E, U> &&
    !is_specialization_of_v<remove_cvref_t<U>, Unexpected> &&
    (!is_same_v<std::remove_cv_t<T>, bool> || !is_specialization_of_v<remove_cvref_t<U>, Expected>) &&
    !is_convertible_v<U, T>
  )>
  constexpr explicit Expected(U&& v) : _value(forward<U>(v)), _has_value(true) {};

  template <typename U = std::remove_cv_t<T>, ENABLE_IF(
    !is_same_v<remove_cvref_t<U>, InPlaceMark> &&
    !is_same_v<remove_cvref_t<U>, UnexpectedMark> &&
    !is_same_v<Expected, remove_cvref_t<U>> &&
    !is_constructible_v<E, U> &&
    !is_specialization_of_v<remove_cvref_t<U>, Unexpected> &&
    (!is_same_v<std::remove_cv_t<T>, bool> || !is_specialization_of_v<remove_cvref_t<U>, Expected>) &&
    is_convertible_v<U, T>
  )>
  constexpr Expected(U&& v) : _value(forward<U>(v)), _has_value(true) {};

  template <typename G, ENABLE_IF(
    is_constructible_v<E, const G&> &&
    !is_convertible_v<const G&, E>
  )>
  constexpr explicit Expected(const Unexpected<G>& e) : _error(e._error), _has_value(false) {}
  template <typename G, ENABLE_IF(
    is_constructible_v<E, const G&> &&
    is_convertible_v<const G&, E>
  )>
  constexpr Expected(const Unexpected<G>& e) : _error(e._error), _has_value(false) {}

  template <typename G, ENABLE_IF(
    is_constructible_v<E, G> &&
    !is_convertible_v<G, E>
  )>
  constexpr explicit Expected(Unexpected<G>&& e) : _error(move(e._error)), _has_value(false) {}
  template <typename G, ENABLE_IF(
    is_constructible_v<E, G> &&
    is_convertible_v<G, E>
  )>
  constexpr Expected(Unexpected<G>&& e) : _error(move(e._error)), _has_value(false) {}

  template <typename... Args, ENABLE_IF(is_constructible_v<T, Args...>)>
  constexpr explicit Expected(InPlaceMark, Args&&... args) : _value(forward<Args>(args)...), _has_value(true) {}

  template <typename... Args, ENABLE_IF(is_constructible_v<E, Args...>)>
  constexpr explicit Expected(UnexpectedMark, Args&&... args) : _error(forward<Args>(args)...), _has_value(false) {}

  // Trivial = default optimization requires requires (C++20)
  CONSTEXPR_DESTRUCTOR ~Expected() {
    if (has_value()) {
      if IF_CONSTEXPR (!is_trivially_destructible_v<T>) {
        _value.~T();
      }
    } else {
      if IF_CONSTEXPR (!is_trivially_destructible_v<E>) {
        _error.~E();
      }
    }
  }

private:
  template <typename U, typename V, typename... Args>
  static constexpr void reinit_expected(U& newval, V& oldval, Args&&... args) {
    if IF_CONSTEXPR (!is_trivially_destructible_v<V>) {
      oldval.~V();
    }
    construct_at(&newval, forward<Args>(args)...);
  }

public:
  template <ENABLE_IF(
    is_copy_assignable_v<T> &&
    is_copy_constructible_v<T> &&
    is_copy_assignable_v<E> &&
    is_copy_constructible_v<E>
  )>
  constexpr Expected& operator=(const Expected& other) {
    if (has_value() && other.has_value()) {
      _value = other.value();
    } else if (has_value()) {
      reinit_expected(_error, _value, other.error());
    } else if (other.has_value()) {
      reinit_expected(_value, _error, other.value());
    } else {
      _error = other.error();
    }
  }
  Expected& operator=(const Expected&) = delete;

  template <ENABLE_IF(
    is_move_assignable_v<T> &&
    is_move_constructible_v<T> &&
    is_move_assignable_v<E> &&
    is_move_constructible_v<E>
  )>
  constexpr Expected& operator=(Expected&& other) {
    if (has_value() && other.has_value()) {
      _value = move(other.value());
    } else if (has_value()) {
      reinit_expected(_error, _value, move(other.error()));
    } else if (other.has_value()) {
      reinit_expected(_value, _error, move(other.value()));
    } else {
      _error = move(other.error());
    }
    _has_value = other.has_value();
  }
  Expected& operator=(Expected&& other) = delete;

  template <typename U = std::remove_cv_t<T>>
  constexpr Expected& operator=(U&& v) {
    if (has_value()) {
      _value = forward<U>(v);
    } else {
      reinit_expected(_value, _error, forward<U>(v));
      _has_value = true;
    }
  }

  template <typename G>
  constexpr Expected& operator=(const Unexpected<G>& e) {
    if (has_value()) {
      reinit_expected(_error, _value, e.error());
      _has_value = false;
    } else {
      _error = e.error();
    }
  }
  template <typename G>
  constexpr Expected& operator=(Unexpected<G>&& e) {
    if (has_value()) {
      reinit_expected(_error, _value, move(e.error()));
      _has_value = false;
    } else {
      _error = move(e.error());
    }
  }

  constexpr bool has_value() const { return _has_value; }

  constexpr T& value() & { precond(has_value()); return _value; }
  constexpr const T& value() const& { precond(has_value()); return _value; }
  constexpr T&& value() && { precond(has_value()); return move(_value); }
  constexpr const T&& value() const&& { precond(has_value()); return move(_value); }

  constexpr E& error() & { precond(!has_value()); return _error; }
  constexpr const E& error() const& { precond(!has_value()); return _error; }
  constexpr E&& error() && { precond(!has_value()); return move(_error); }
  constexpr const E&& error() const&& { precond(!has_value()); return move(_error); }

  template <typename U = std::remove_cv_t<T>>
  constexpr T value_or(U&& default_value) const& {
    return has_value() ? this->value() : static_cast<T>(forward<U>(default_value));
  }
  template <typename U = std::remove_cv_t<T>>
  constexpr T value_or(U&& default_value) && {
    return has_value() ? move(this->value()) : static_cast<T>(forward<U>(default_value));
  }

  template <typename G = E>
  constexpr E error_or(G&& default_value) const& {
    return has_value() ? forward<G>(default_value) : error();
  }
  template <typename G = E>
  constexpr E error_or(G&& default_value) && {
    return has_value() ? forward<G>(default_value) : move(error());
  }

  template <typename F, ENABLE_IF(is_constructible_v<E, E&>)>
  constexpr auto and_then(F&& f) & {
    // TODO: C++17 Use std::invoke_result_t
    using U = std::remove_cv_t<std::result_of_t<F(T&)>>;
    if (has_value()) {
      return f(_value);
    }
    return U{UnexpectedMark{}, _error};
  }
  template <typename F, ENABLE_IF(is_constructible_v<E, const E&>)>
  constexpr auto and_then(F&& f) const& {
    // TODO: C++17 Use std::invoke_result_t
    using U = std::remove_cv_t<std::result_of_t<F(const T&)>>;
    if (has_value()) {
      return f(_value);
    }
    return U{UnexpectedMark{}, _error};
  }
  template <typename F, ENABLE_IF(is_constructible_v<E, E>)>
  constexpr auto and_then(F&& f) && {
    // TODO: C++17 Use std::invoke_result_t
    using U = std::remove_cv_t<std::result_of_t<F(T)>>;
    if (has_value()) {
      return f(move(_value));
    }
    return U{UnexpectedMark{}, move(_error)};
  }
  template <typename F, ENABLE_IF(is_constructible_v<E, const E>)>
  constexpr auto and_then(F&& f) const&& {
    // TODO: C++17 Use std::invoke_result_t
    using U = std::remove_cv_t<std::result_of_t<F(const T)>>;
    if (has_value()) {
      return f(move(_value));
    }
    return U{UnexpectedMark{}, move(_error)};
  }

  template <typename F, ENABLE_IF(is_constructible_v<T, T&>)>
  constexpr auto or_else(F&& f) & {
    // TODO: C++17 Use std::invoke_result_t
    using G = std::remove_cv_t<std::result_of_t<F(E&)>>;
    if (!has_value()) {
      return f(_error);
    }
    return G{InPlaceMark{}, _value};
  }
  template <typename F, ENABLE_IF(is_constructible_v<T, const T&>)>
  constexpr auto or_else(F&& f) const& {
    // TODO: C++17 Use std::invoke_result_t
    using G = std::remove_cv_t<std::result_of_t<F(const E&)>>;
    if (!has_value()) {
      return f(_error);
    }
    return G{InPlaceMark{}, _value};
  }
  template <typename F, ENABLE_IF(is_constructible_v<T, T>)>
  constexpr auto or_else(F&& f) && {
    // TODO: C++17 Use std::invoke_result_t
    using G = std::remove_cv_t<std::result_of_t<F(E)>>;
    if (!has_value()) {
      return f(move(_error));
    }
    return G{InPlaceMark{}, move(_value)};
  }
  template <typename F, ENABLE_IF(is_constructible_v<T, const T>)>
  constexpr auto or_else(F&& f) const&& {
    // TODO: C++17 Use std::invoke_result_t
    using G = std::remove_cv_t<std::result_of_t<F(const E)>>;
    if (!has_value()) {
      return f(move(_error));
    }
    return G{InPlaceMark{}, move(_value)};
  }

private:
  template <typename U, typename V, typename F, ENABLE_IF(!is_void_v<U>)>
  constexpr Expected<U, E> transform_invoke_helper(F&& f, V&& v) {
    // TODO: Maybe create constructor that invokes the function on the inside
    return Expected<U, E>{InPlaceMark{}, f(forward<V>(v))};
  }
  template <typename U, typename V, typename F, ENABLE_IF(is_void_v<U>)>
  constexpr Expected<void, E> transform_invoke_helper(F&& f, V&& v) {
    f(forward<V>(v));
    return Expected<void, E>{};
  }

public:
  template <typename F, ENABLE_IF(is_constructible_v<E, E&>)>
  constexpr auto transform(F&& f) & {
    using U = std::remove_cv_t<std::result_of_t<F(T&)>>;
    if (has_value()) {
      return transform_invoke_helper<U>(f, _value);
    }
    return Expected<U, E>{UnexpectedMark{}, _error};
  }
  template <typename F, ENABLE_IF(is_constructible_v<E, const E&>)>
  constexpr auto transform(F&& f) const& {
    using U = std::remove_cv_t<std::result_of_t<F(const T&)>>;
    if (has_value()) {
      return transform_invoke_helper<U>(f, _value);
    }
    return Expected<U, E>{UnexpectedMark{}, _error};
  }
  template <typename F, ENABLE_IF(is_constructible_v<E, E>)>
  constexpr auto transform(F&& f) && {
    using U = std::remove_cv_t<std::result_of_t<F(T)>>;
    if (has_value()) {
      return transform_invoke_helper<U>(f, move(_value));
    }
    return Expected<U, E>{UnexpectedMark{}, move(_error)};
  }
  template <typename F, ENABLE_IF(is_constructible_v<E, const E>)>
  constexpr auto transform(F&& f) const&& {
    using U = std::remove_cv_t<std::result_of_t<F(const T)>>;
    if (has_value()) {
      return transform_invoke_helper<U>(f, move(_value));
    }
    return Expected<U, E>{UnexpectedMark{}, move(_error)};
  }

  template <typename F, ENABLE_IF(is_constructible_v<T, T&>)>
  constexpr auto transform_error(F&& f) & {
    using G = std::remove_cv_t<std::result_of_t<F(E&)>>;
    if (!has_value()) {
      return Expected<T, G>{UnexpectedMark{}, f(_error)};
    }
    return Expected<T, G>{InPlaceMark{}, _value};
  }
  template <typename F, ENABLE_IF(is_constructible_v<T, const T&>)>
  constexpr auto transform_error(F&& f) const& {
    using G = std::remove_cv_t<std::result_of_t<F(const E&)>>;
    if (!has_value()) {
      return Expected<T, G>{UnexpectedMark{}, f(_error)};
    }
    return Expected<T, G>{InPlaceMark{}, _value};
  }
  template <typename F, ENABLE_IF(is_constructible_v<T, T>)>
  constexpr auto transform_error(F&& f) && {
    using G = std::remove_cv_t<std::result_of_t<F(E)>>;
    if (!has_value()) {
      return Expected<T, G>{UnexpectedMark{}, f(move(_error))};
    }
    return Expected<T, G>{InPlaceMark{}, move(_value)};
  }
  template <typename F, ENABLE_IF(is_constructible_v<T, const T>)>
  constexpr auto transform_error(F&& f) const&& {
    using G = std::remove_cv_t<std::result_of_t<F(const E)>>;
    if (!has_value()) {
      return Expected<T, G>{UnexpectedMark{}, f(move(_error))};
    }
    return Expected<T, G>{InPlaceMark{}, move(_value)};
  }

  template <typename T2, typename E2>
  friend constexpr bool operator==(const Expected& lhs, const Expected<T2, E2>& rhs) {
    // Replace with C++20 requires
    STATIC_ASSERT(!is_void_v<T2>);
    return lhs.has_value() != rhs.has_value() ? false :
        (lhs.has_value() ? lhs.value() == rhs.value() : lhs.error() == rhs.error());
  }
  template <typename E2>
  friend constexpr bool operator==(const Expected& lhs, const Unexpected<E2>& error) {
    return !lhs.has_value() && static_cast<bool>(lhs.error() == error.error());
  }
  template <typename T2>
  friend constexpr bool operator==(const Expected& lhs, const T2& value) {
    return lhs.has_value() && static_cast<bool>(lhs.value() == value);
  }
};

template <typename E>
class Expected<void, E> {
private:
  E _error;
  bool _has_value;


  // Template Helpers
  template <typename U, typename G>
  static constexpr bool disallowed_expected = (
    is_constructible_v<Unexpected<E>, Expected<U, G>&> ||
    is_constructible_v<Unexpected<E>, Expected<U, G>> ||
    is_constructible_v<Unexpected<E>, const Expected<U, G>&> ||
    is_constructible_v<Unexpected<E>, const Expected<U, G>>
  );
  template <typename U, typename G>
  static constexpr bool is_copy_constructible_expected = (
    !(is_same_v<void, U> && is_same_v<E, G>) &&
    is_constructible_v<E, const G&> &&
    !disallowed_expected<U, G>
  );
  template <typename U, typename G>
  static constexpr bool is_move_constructible_expected = (
    !(is_same_v<void, U> && is_same_v<E, G>) &&
    is_constructible_v<E, G> &&
    !disallowed_expected<U, G>
  );


public:
  using value_type = void;
  using error_type = E;
  using unexpected_type = Unexpected<E>;
  template <typename U>
  using rebind = Expected<U, error_type>;

  constexpr Expected() : _has_value(true) {};

  // Trivial = default optimization requires requires (C++20)
  template <ENABLE_IF(
    is_copy_constructible_v<E>
  )>
  constexpr Expected(const Expected& other)
    : _has_value(other.has_value()) {
    if (!has_value()) {
      construct_at(&_error, other._error);
    }
  }
  // requires requires (C++20)
  // constexpr Expected(const Expected&) = delete;

  // Trivial = default optimization requires requires (C++20)
  template <ENABLE_IF(
    is_move_constructible_v<E>
  )>
  constexpr Expected(Expected&& other)
    : _has_value(other.has_value()) {
    if (!has_value()) {
      construct_at(&_error, move(other._error));
    }
  }
  // requires requires (C++20)
  // constexpr Expected(Expected&&) = delete;

  template <typename U, typename G, ENABLE_IF(
    is_copy_constructible_expected<U, G> &&
    is_void_v<U> &&
    !is_convertible_v<const G&, E>
  )>
  constexpr explicit Expected(const Expected<U, G>& other)
    : _has_value(other.has_value()) {
    if (!has_value()) {
      construct_at(&_error, other._error);
    }
  }
  template <typename U, typename G, ENABLE_IF(
    is_copy_constructible_expected<U, G> &&
    is_void_v<U> &&
    is_convertible_v<const G&, E>
  )>
  constexpr Expected(const Expected<U, G>& other)
    : _has_value(other.has_value()) {
    if (!has_value()) {
      construct_at(&_error, other._error);
    }
  }

  template <typename U, typename G, ENABLE_IF(
    is_move_constructible_expected<U, G> &&
    is_void_v<U> &&
    !is_convertible_v<G, E>
  )>
  constexpr explicit Expected(Expected<U, G>&& other)
    : _has_value(other.has_value()) {
    if (!has_value()) {
      construct_at(&_error, move(other._error));
    }
  }

  template <typename U, typename G, ENABLE_IF(
    is_move_constructible_expected<U, G> &&
    is_void_v<U> &&
    is_convertible_v<G, E>
  )>
  constexpr Expected(Expected<U, G>&& other)
    : _has_value(other.has_value()) {
    if (!has_value()) {
      construct_at(&_error, move(other._error));
    }
  }

  template <typename G, ENABLE_IF(
    is_constructible_v<E, const G&> &&
    !is_convertible_v<const G&, E>
  )>
  constexpr explicit Expected(const Unexpected<G>& e) : _error(e._error), _has_value(false) {}
  template <typename G, ENABLE_IF(
    is_constructible_v<E, const G&> &&
    is_convertible_v<const G&, E>
  )>
  constexpr Expected(const Unexpected<G>& e) : _error(e._error), _has_value(false) {}

  template <typename G, ENABLE_IF(
    is_constructible_v<E, G> &&
    !is_convertible_v<G, E>
  )>
  constexpr explicit Expected(Unexpected<G>&& e) : _error(move(e._error)), _has_value(false) {}
  template <typename G, ENABLE_IF(
    is_constructible_v<E, G> &&
    is_convertible_v<G, E>
  )>
  constexpr Expected(Unexpected<G>&& e) : _error(move(e._error)), _has_value(false) {}

  constexpr explicit Expected(InPlaceMark) : _has_value(true) {}

  template <typename... Args, ENABLE_IF(is_constructible_v<E, Args...>)>
  constexpr explicit Expected(UnexpectedMark, Args&&... args) : _error(forward<Args>(args)...), _has_value(false) {}

  constexpr bool has_value() const { return _has_value; }

  constexpr E& error() & { precond(!has_value()); return _error; }
  constexpr const E& error() const& { precond(!has_value()); return _error; }
  constexpr E&& error() && { precond(!has_value()); return move(_error); }
  constexpr const E&& error() const&& { precond(!has_value()); return move(_error); }

  template <typename G = E>
  constexpr E error_or(G&& default_value) const& {
    return has_value() ? forward<G>(default_value) : error();
  }
  template <typename G = E>
  constexpr E error_or(G&& default_value) && {
    return has_value() ? forward<G>(default_value) : move(error());
  }

  template <typename F, ENABLE_IF(is_constructible_v<E, E&>)>
  constexpr auto and_then(F&& f) & {
    // TODO: C++17 Use std::invoke_result_t
    using U = std::remove_cv_t<std::result_of_t<F()>>;
    if (has_value()) {
      return f();
    }
    return U{UnexpectedMark{}, _error};
  }
  template <typename F, ENABLE_IF(is_constructible_v<E, const E&>)>
  constexpr auto and_then(F&& f) const& {
    // TODO: C++17 Use std::invoke_result_t
    using U = std::remove_cv_t<std::result_of_t<F()>>;
    if (has_value()) {
      return f();
    }
    return U{UnexpectedMark{}, _error};
  }
  template <typename F, ENABLE_IF(is_constructible_v<E, E>)>
  constexpr auto and_then(F&& f) && {
    // TODO: C++17 Use std::invoke_result_t
    using U = std::remove_cv_t<std::result_of_t<F()>>;
    if (has_value()) {
      return f();
    }
    return U{UnexpectedMark{}, move(_error)};
  }
  template <typename F, ENABLE_IF(is_constructible_v<E, const E>)>
  constexpr auto and_then(F&& f) const&& {
    // TODO: C++17 Use std::invoke_result_t
    using U = std::remove_cv_t<std::result_of_t<F()>>;
    if (has_value()) {
      return f();
    }
    return U{UnexpectedMark{}, move(_error)};
  }

  template <typename F>
  constexpr auto or_else(F&& f) & {
    // TODO: C++17 Use std::invoke_result_t
    using G = std::remove_cv_t<std::result_of_t<F(E&)>>;
    if (!has_value()) {
      return f(_error);
    }
    return G{};
  }
  template <typename F>
  constexpr auto or_else(F&& f) const& {
    // TODO: C++17 Use std::invoke_result_t
    using G = std::remove_cv_t<std::result_of_t<F(const E&)>>;
    if (!has_value()) {
      return f(_error);
    }
    return G{};
  }
  template <typename F>
  constexpr auto or_else(F&& f) && {
    // TODO: C++17 Use std::invoke_result_t
    using G = std::remove_cv_t<std::result_of_t<F(E)>>;
    if (!has_value()) {
      return f(move(_error));
    }
    return G{};
  }
  template <typename F>
  constexpr auto or_else(F&& f) const&& {
    // TODO: C++17 Use std::invoke_result_t
    using G = std::remove_cv_t<std::result_of_t<F(const E)>>;
    if (!has_value()) {
      return f(move(_error));
    }
    return G{};
  }

private:
  template <typename U, typename F, ENABLE_IF(!is_void_v<U>)>
  constexpr Expected<U, E> transform_invoke_helper(F&& f) {
    // TODO: Maybe create constructor that invokes the function on the inside
    return Expected<U, E>{InPlaceMark{}, f()};
  }
  template <typename U, typename F, ENABLE_IF(is_void_v<U>)>
  constexpr Expected<void, E> transform_invoke_helper(F&& f) {
    f();
    return Expected<void, E>{};
  }

public:
  template <typename F, ENABLE_IF(is_constructible_v<E, E&>)>
  constexpr auto transform(F&& f) & {
    using U = std::remove_cv_t<std::result_of_t<F()>>;
    if (has_value()) {
      return transform_invoke_helper<U>(f);
    }
    return Expected<U, E>{UnexpectedMark{}, _error};
  }
  template <typename F, ENABLE_IF(is_constructible_v<E, const E&>)>
  constexpr auto transform(F&& f) const& {
    using U = std::remove_cv_t<std::result_of_t<F()>>;
    if (has_value()) {
      return transform_invoke_helper<U>(f);
    }
    return Expected<U, E>{UnexpectedMark{}, _error};
  }
  template <typename F, ENABLE_IF(is_constructible_v<E, E>)>
  constexpr auto transform(F&& f) && {
    using U = std::remove_cv_t<std::result_of_t<F()>>;
    if (has_value()) {
      return transform_invoke_helper<U>(f);
    }
    return Expected<U, E>{UnexpectedMark{}, move(_error)};
  }
  template <typename F, ENABLE_IF(is_constructible_v<E, const E>)>
  constexpr auto transform(F&& f) const&& {
    using U = std::remove_cv_t<std::result_of_t<F()>>;
    if (has_value()) {
      return transform_invoke_helper<U>(f);
    }
    return Expected<U, E>{UnexpectedMark{}, move(_error)};
  }

  template <typename F>
  constexpr auto transform_error(F&& f) & {
    using G = std::remove_cv_t<std::result_of_t<F(E&)>>;
    if (!has_value()) {
      return Expected<void, G>{UnexpectedMark{}, f(_error)};
    }
    return Expected<void, G>{};
  }
  template <typename F>
  constexpr auto transform_error(F&& f) const& {
    using G = std::remove_cv_t<std::result_of_t<F(const E&)>>;
    if (!has_value()) {
      return Expected<void, G>{UnexpectedMark{}, f(_error)};
    }
    return Expected<void, G>{};
  }
  template <typename F>
  constexpr auto transform_error(F&& f) && {
    using G = std::remove_cv_t<std::result_of_t<F(E)>>;
    if (!has_value()) {
      return Expected<void, G>{UnexpectedMark{}, f(move(_error))};
    }
    return Expected<void, G>{};
  }
  template <typename F>
  constexpr auto transform_error(F&& f) const&& {
    using G = std::remove_cv_t<std::result_of_t<F(const E)>>;
    if (!has_value()) {
      return Expected<void, G>{UnexpectedMark{}, f(move(_error))};
    }
    return Expected<void, G>{};
  }

  template <typename T2, typename E2>
  friend constexpr bool operator==(const Expected& lhs, const Expected<void, E2>& rhs) {
    // Replace with C++20 requires
    return lhs.has_value() != rhs.has_value() ? false :
        (lhs.has_value() ? true : lhs.error() == rhs.error());
  }
  template <typename E2>
  friend constexpr bool operator==(const Expected& lhs, const Unexpected<E2>& error) {
    return !lhs.has_value() && static_cast<bool>(lhs.error() == error.error());
  }

};

// Annoying specializations. Using C++20 requires fixes this. Is there another way?
// template <typename E> class Expected<const void, E>;
// template <typename E> class Expected<volatile void, E>;
// template <typename E> class Expected<const volatile void, E>;

#endif // SHARE_UTILITIES_EXPECTED_HPP
