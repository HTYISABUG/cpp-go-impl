#ifndef GO_CHAN
#define GO_CHAN

#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

namespace go {

#define DECLARE_INPUT_CHANNEL_FRIEND                                 \
  template <template <typename> typename CH, typename U, typename V> \
  friend const std::shared_ptr<CH<U>>& operator<<(                   \
      const std::shared_ptr<CH<U>>& pCh, V&& value);                 \
  template <template <typename> typename CH, typename U, typename V> \
  friend void send(const std::shared_ptr<CH<U>>& pCh, V&& value);    \
  template <template <typename> typename CH, typename U, typename V> \
  friend bool trySend(const std::shared_ptr<CH<U>>& pCh, V&& value); \
  template <template <typename> typename CH, typename U>             \
  friend void close(const std::shared_ptr<CH<U>>& pCh);              \
  template <template <typename> typename CH, typename U>             \
  friend size_t len(const std::shared_ptr<CH<U>>& pCh);              \
  template <template <typename> typename CH, typename U>             \
  friend size_t cap(const std::shared_ptr<CH<U>>& pCh);

#define DECLARE_OUTPUT_CHANNEL_FRIEND                                         \
  template <template <typename> typename CH, typename U>                      \
  friend U& operator<<(U& value, const std::shared_ptr<CH<U>>& pCh);          \
  template <template <typename> typename CH, typename U>                      \
  friend void receive(const std::shared_ptr<CH<U>>& pCh, U& value);           \
  template <template <typename> typename CH, typename U>                      \
  friend void receive(const std::shared_ptr<CH<U>>& pCh, U& value, bool& ok); \
  template <template <typename> typename CH, typename U>                      \
  friend bool tryReceive(const std::shared_ptr<CH<U>>& pCh, U& value);        \
  template <template <typename> typename CH, typename U>                      \
  friend bool tryReceive(const std::shared_ptr<CH<U>>& pCh, U& value,         \
                         bool& ok);                                           \
  template <template <typename> typename CH, typename U>                      \
  friend size_t len(const std::shared_ptr<CH<U>>& pCh);                       \
  template <template <typename> typename CH, typename U>                      \
  friend size_t cap(const std::shared_ptr<CH<U>>& pCh);

// InputChannel is a channel that can only send values in type T.
template <typename T>
class InputChannel {
  DECLARE_INPUT_CHANNEL_FRIEND

 protected:
  virtual bool send(const T& value, bool blocking = true) = 0;
  virtual bool send(T&& value, bool blocking = true) = 0;

  virtual void close() = 0;

  virtual size_t len() const = 0;
  virtual size_t cap() const = 0;
};

// OutputChannel is a channel that can only receive values in type T.
template <typename T>
class OutputChannel {
  DECLARE_OUTPUT_CHANNEL_FRIEND

 protected:
  virtual bool receive(T& value, bool& ok, bool blocking = true) = 0;

  virtual size_t len() const = 0;
  virtual size_t cap() const = 0;
};

template <template <typename> typename CH, typename U, typename V>
const std::shared_ptr<CH<U>>& operator<<(const std::shared_ptr<CH<U>>& pCh,
                                         V&& value) {
  pCh->send(std::forward<V>(value));
  return pCh;
}

template <template <typename> typename CH, typename U, typename V>
void send(const std::shared_ptr<CH<U>>& pCh, V&& value) {
  pCh->send(std::forward<V>(value));
}

template <template <typename> typename CH, typename U, typename V>
bool trySend(const std::shared_ptr<CH<U>>& pCh, V&& value) {
  return pCh->trySend(std::forward<V>(value), false);
}

template <template <typename> typename CH, typename U>
void close(const std::shared_ptr<CH<U>>& pCh) {
  pCh->close();
}

template <template <typename> typename CH, typename U>
U& operator<<(U& value, const std::shared_ptr<CH<U>>& pCh) {
  bool ok;
  pCh->receive(value, ok);
  return value;
}

template <template <typename> typename CH, typename U>
void receive(const std::shared_ptr<CH<U>>& pCh, U& value) {
  bool ok;
  pCh->receive(value, ok);
}

template <template <typename> typename CH, typename U>
void receive(const std::shared_ptr<CH<U>>& pCh, U& value, bool& ok) {
  pCh->receive(value, ok);
}

template <template <typename> typename CH, typename U>
bool tryReceive(const std::shared_ptr<CH<U>>& pCh, U& value) {
  bool ok;
  return pCh->receive(value, ok, false);
}

template <template <typename> typename CH, typename U>
bool tryReceive(const std::shared_ptr<CH<U>>& pCh, U& value, bool& ok) {
  return pCh->receive(value, ok, false);
}

template <template <typename> typename CH, typename U>
size_t len(const std::shared_ptr<CH<U>>& pCh) {
  return pCh->len();
}

template <template <typename> typename CH, typename U>
size_t cap(const std::shared_ptr<CH<U>>& pCh) {
  return pCh->cap();
}

/**
 * @brief Channel provides a mechanism for concurrently executing functions to
 * communicate by sending and receiving values of a specified element type.
 *
 * Channel acts as first-in-first-out queues. For example, if one goroutine
 * sends values on a channel and a second goroutine receives them, the values
 * are received in the order sent.
 *
 * Channel does not guarantee the order in which values are sent or received by
 * multiple threads. For example, if multiple threads send values to a Channel ,
 * the receiving order is unknown, since the sending order cannot be guaranteed.
 */
template <typename T>
class Channel : public InputChannel<T>, public OutputChannel<T> {
  DECLARE_INPUT_CHANNEL_FRIEND
  DECLARE_OUTPUT_CHANNEL_FRIEND

 public:
  template <typename... Args>
  static std::shared_ptr<Channel> make(Args&&... args) {
    struct Wrapper : public Channel {
      explicit Wrapper(Args&&... args) : Channel(std::forward<Args>(args)...) {}
    };

    return std::make_shared<Wrapper>(std::forward<Args>(args)...);
  }

 protected:
  template <typename U>
  bool _send(U&& value, bool blocking) {
    Lock lk(mMutex);

    bool ret = true;

    if (mIsClosed) {
      throw std::runtime_error("send on closed channel");
    } else if (!mOutputQ.empty()) {
      assert(mBufferQ.empty());

      auto pIO = mOutputQ.front();
      mOutputQ.pop();
      pIO->mValue = std::forward<U>(value);
      pIO->mCond.notify_one();
    } else if (mBufferQ.size() < mCap) {
      assert(mInputQ.empty());

      mBufferQ.push(std::forward<U>(value));
    } else {
      if (blocking) {
        auto pIO = std::make_shared<WaitingIO>(std::forward<U>(value));
        mInputQ.push(pIO);  // popped by receiver
        pIO->mCond.wait(lk);
      } else {
        ret = false;
      }

      if (mIsClosed) {
        throw std::runtime_error("send on closed channel");
      }
    }

    return ret;
  }

  bool send(const T& value, bool blocking) override {
    return _send(value, blocking);
  }

  bool send(T&& value, bool blocking) override {
    return _send(std::move(value), blocking);
  }

  bool receive(T& value, bool& ok, bool blocking) override {
    Lock lk(mMutex);

    bool ret = true;

    ok = true;

    if (!mBufferQ.empty()) {
      assert(mOutputQ.empty());

      value = std::move(mBufferQ.front());
      mBufferQ.pop();

      if (!mInputQ.empty()) {
        auto pIO = mInputQ.front();
        mInputQ.pop();
        mBufferQ.push(std::move(pIO->mValue));
        pIO->mCond.notify_one();
      }
    } else if (mIsClosed) {
      value = {};
      ok = false;
    } else if (!mInputQ.empty()) {
      assert(mCap == 0);

      auto pIO = mInputQ.front();
      mInputQ.pop();
      value = std::move(pIO->mValue);
      pIO->mCond.notify_one();
    } else {
      if (blocking) {
        auto pIO = std::make_shared<WaitingIO>();
        mOutputQ.push(pIO);  // popped by sender
        pIO->mCond.wait(lk);

        if (mIsClosed) {
          value = {};
          ok = false;
        } else {
          value = std::move(pIO->mValue);
        }
      } else {
        ret = false;
      }
    }

    return ret;
  }

  void close() override {
    LockGuard _(mMutex);

    mIsClosed = true;

    while (!mOutputQ.empty()) {
      assert(mBufferQ.empty());

      auto& pIO = mOutputQ.front();
      pIO->mValue = {};
      pIO->mCond.notify_one();
      mOutputQ.pop();
    }

    while (!mInputQ.empty()) {
      auto& pIO = mInputQ.front();
      pIO->mCond.notify_one();
      mInputQ.pop();
    }
  }

  size_t len() const override {
    LockGuard _(mMutex);
    return mBufferQ.size();
  }

  size_t cap() const override { return mCap; }

 private:
  using Lock = std::unique_lock<std::mutex>;
  using LockGuard = std::lock_guard<std::mutex>;

  Channel() = default;
  Channel(size_t cap) : mCap(cap) {}

  Channel(const Channel&) = delete;
  Channel(Channel&&) = delete;
  Channel& operator=(const Channel&) = delete;
  Channel& operator=(Channel&&) = delete;

  const size_t mCap = 0;

  struct WaitingIO {
    WaitingIO() = default;

    template <typename U>
    explicit WaitingIO(U&& value) : mValue(std::forward<U>(value)) {}

    T mValue;
    std::condition_variable mCond;
  };

  using WaitingIOPtr = std::shared_ptr<WaitingIO>;

  mutable std::mutex mMutex;
  std::queue<T> mBufferQ;
  std::queue<WaitingIOPtr> mInputQ;
  std::queue<WaitingIOPtr> mOutputQ;

  bool mIsClosed = false;
};

}  // namespace go

#endif  // GO_CHAN
