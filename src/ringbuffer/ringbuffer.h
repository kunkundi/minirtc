#ifndef _RINGBUFFER_H_
#define _RINGBUFFER_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace minirtc {

int RingBufferDummy();

class Data {
 public:
  Data() = default;

  Data(const char* data, uint32_t size) {
    data_ = new char[size];
    memcpy(data_, data, size);
    size_ = size;
  }

  Data(const Data& obj) {
    data_ = new char[obj.size_];
    memcpy(data_, obj.data_, obj.size_);
    size_ = obj.size_;
  }

  Data& operator=(const Data& obj) {
    data_ = new char[obj.size_];
    memcpy(data_, obj.data_, obj.size_);
    size_ = obj.size_;
    return *this;
  }

  ~Data() {
    if (data_) {
      delete data_;
      data_ = nullptr;
    }
    size_ = 0;
  }

  uint32_t size() const { return size_; }
  char* data() const { return data_; }

 public:
  char* data_ = nullptr;
  uint32_t size_ = 0;
};

template <typename T>
class RingBuffer {
 public:
  RingBuffer(unsigned size = 1280) : m_size(size), m_front(0), m_rear(0) {
    m_data = new T[size];
  }

  ~RingBuffer() {
    delete[] m_data;
    m_data = nullptr;
  }

  bool isEmpty() const { return m_front == m_rear; }

  bool isFull() const { return m_front == (m_rear + 1) % m_size; }

  bool push(T value) {
    if (isFull()) {
      return false;
    }
    if (!m_data) {
      return false;
    }
    m_data[m_rear] = std::move(value);
    m_rear = (m_rear + 1) % m_size;
    return true;
  }

  std::optional<T> pop() {
    if (isEmpty()) {
      return std::nullopt;
    }
    std::optional<T> value = std::move(m_data[m_front]);
    m_front = (m_front + 1) % m_size;
    return value;
  }

  unsigned int front() const { return m_front; }

  unsigned int rear() const { return m_rear; }

  unsigned int size() const { return m_size; }

  bool clear() {
    m_front = 0;
    m_rear = 0;
    return true;
  }

 private:
  unsigned int m_size;
  unsigned int m_front;
  unsigned int m_rear;
  T* m_data;
};
}  // namespace minirtc

#endif