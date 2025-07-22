/*
 * @Author: DI JUNKUN
 * @Date: 2024-04-22
 * Copyright (c) 2024 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _BYTE_BUFFER_H_
#define _BYTE_BUFFER_H_

#include <cstddef>
#include <cstdint>

namespace minirtc {
class ByteBufferReader {
 public:
  ByteBufferReader(const char* bytes, size_t len);

  ByteBufferReader(const ByteBufferReader&) = delete;
  ByteBufferReader& operator=(const ByteBufferReader&) = delete;

  // Returns start of unprocessed data.
  const char* Data() const { return bytes_ + start_; }
  // Returns number of unprocessed bytes.
  size_t Length() const { return end_ - start_; }

  bool ReadBytes(char* val, size_t len);
  bool ReadUInt8(uint8_t* val);
  bool ReadUVarint(uint64_t* val);
  bool ReadUVarint(uint64_t* val, size_t* len);

  bool Consume(size_t size);

 protected:
  void Construct(const char* bytes, size_t size);

  const char* bytes_;
  size_t size_;
  size_t start_;
  size_t end_;
};
}  // namespace minirtc

#endif