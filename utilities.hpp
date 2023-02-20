#ifndef GO_UTILITIES
#define GO_UTILITIES

#include <algorithm>
#include <functional>
#include <memory>
#include <random>

#include "Chan.hpp"

namespace go {

template <typename T>
class IChan;

template <typename T>
class OChan;

template <typename T>
class Chan;

class Select;

template <class T>
struct type_identity {
  using type = T;
};

template <class T>
using type_identity_t = typename type_identity<T>::type;

class Case {
  friend class Select;

 public:
  template <typename PtrCH, typename V>
  Case(const PtrCH& pCh, V&& value, const std::function<void()>& f) {
    mTask = [=, &value = std::forward<V>(value)]() {
      bool ret;

      if (ret = trySend(pCh, std::forward<V>(value))) {
        f();
      }

      return ret;
    };
  }

  template <template <typename> typename CH, typename U>
  Case(const std::shared_ptr<CH<U>>& pCh,
       const std::function<void(type_identity_t<U>&&, bool)>& f) {
    mTask = [=]() {
      bool ret, ok;
      U value;

      if (ret = tryReceive(pCh, value, ok)) {
        f(std::move(value), ok);
      }

      return ret;
    };
  }

  template <template <typename> typename CH, typename U>
  Case(const std::shared_ptr<CH<U>>& pCh,
       const std::function<void(type_identity_t<U>&&)>& f) {
    mTask = [=]() {
      bool ret;
      U value;

      if (ret = tryReceive(pCh, value)) {
        f(std::move(value));
      }

      return ret;
    };
  }

  bool operator()() const { return mTask(); }

 private:
  Case() = delete;

  std::function<bool()> mTask;
};

class Default {
  friend class Select;

 public:
  Default(const std::function<void()>& f) : mTask(f) {}

  void operator()() const { mTask(); }

 private:
  Default() = default;

  std::function<void()> mTask;
};

template <class...>
struct conjunction : std::true_type {};

template <class B1>
struct conjunction<B1> : B1 {};

template <class B1, class... Bn>
struct conjunction<B1, Bn...>
    : std::conditional_t<bool(B1::value), conjunction<Bn...>, B1> {};

template <class... B>
constexpr bool conjunction_v = conjunction<B...>::value;

class Select {
 public:
  template <typename... Args>
  Select(Args&&... args) {
    init(std::forward<Args>(args)...);
  }

 private:
  Select() = delete;

  Select(const Select&) = delete;
  Select(Select&&) = delete;
  Select& operator=(const Select&) = delete;
  Select& operator=(Select&&) = delete;

  void init(Case&& c) {
    mCases.emplace_back(std::move(c));
    exec();
  }

  void init(Case&& c, Default&& d) {
    mCases.emplace_back(std::move(c));
    mHasDefault = true;
    mDefault = std::move(d);
    exec();
  }

  template <typename... Args>
  void init(Case&& c, Args&&... args) {
    mCases.emplace_back(std::move(c));
    init(std::forward<Args>(args)...);
  }

  template <typename... Args>
  void init(Default&& d, Args&&... args) {
    static_assert(conjunction_v<std::is_same<Case, Args>...>,
                  "multiple defaults");

    mHasDefault = true;
    mDefault = std::move(d);
    init(std::forward<Args>(args)...);
  }

  void exec() {
    std::random_device rd;
    std::mt19937 gen(rd());

    std::shuffle(mCases.begin(), mCases.end(), gen);

    for (const auto& c : mCases) {
      if (c()) {
        return;
      }
    }

    if (mHasDefault) {
      mDefault();
    }
  }

  bool mHasDefault = false;

  std::vector<Case> mCases;
  Default mDefault;
};

}  // namespace go

#endif  // GO_UTILITIES
