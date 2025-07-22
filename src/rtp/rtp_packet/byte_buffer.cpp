#include "byte_buffer.h"

#include <cstring>

namespace minirtc {

ByteBufferReader::ByteBufferReader(const char* bytes, size_t len) {
  Construct(bytes, len);
}

void ByteBufferReader::Construct(const char* bytes, size_t len) {
  bytes_ = bytes;
  size_ = len;
  start_ = 0;
  end_ = len;
}

bool ByteBufferReader::ReadBytes(char* val, size_t len) {
  if (len > Length()) {
    return false;
  } else {
    memcpy(val, bytes_ + start_, len);
    start_ += len;
    return true;
  }
}

bool ByteBufferReader::ReadUInt8(uint8_t* val) {
  if (!val) return false;

  return ReadBytes(reinterpret_cast<char*>(val), 1);
}

bool ByteBufferReader::ReadUVarint(uint64_t* val) {
  if (!val) {
    return false;
  }
  // Integers are deserialized 7 bits at a time, with each byte having a
  // continuation byte (msb=1) if there are more bytes to be read.
  uint64_t v = 0;
  for (int i = 0; i < 64; i += 7) {
    char byte;
    if (!ReadBytes(&byte, 1)) {
      return false;
    }
    // Read the first 7 bits of the byte, then offset by bits read so far.
    v |= (static_cast<uint64_t>(byte) & 0x7F) << i;
    // True if the msb is not a continuation byte.
    if (static_cast<uint64_t>(byte) < 0x80) {
      *val = v;
      return true;
    }
  }
  return false;
}

bool ByteBufferReader::ReadUVarint(uint64_t* val, size_t* len) {
  if (!val) {
    return false;
  }
  // Integers are deserialized 7 bits at a time, with each byte having a
  // continuation byte (msb=1) if there are more bytes to be read.
  uint64_t v = 0;
  for (int i = 0; i < 64; i += 7) {
    char byte;
    if (!ReadBytes(&byte, 1)) {
      return false;
    }
    // Read the first 7 bits of the byte, then offset by bits read so far.
    v |= (static_cast<uint64_t>(byte) & 0x7F) << i;
    // True if the msb is not a continuation byte.
    if (static_cast<uint64_t>(byte) < 0x80) {
      *val = v;
      if (len) {
        *len = i / 8 + (i % 8 ? 1 : 0) + 1;
      }
      return true;
    }
  }
  return false;
}

bool ByteBufferReader::Consume(size_t size) {
  if (size > Length()) return false;
  start_ += size;
  return true;
}
}  // namespace minirtc