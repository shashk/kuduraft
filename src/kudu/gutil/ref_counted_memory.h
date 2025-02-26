// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef KUDU_GUTIL_REF_COUNTED_MEMORY_H_
#define KUDU_GUTIL_REF_COUNTED_MEMORY_H_

#include <cstddef>

#include <string>
#include <vector>

#include "kudu/gutil/macros.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/threading/thread_collision_warner.h"

#ifndef BASE_EXPORT
#define BASE_EXPORT
#endif

namespace kudu {

// A generic interface to memory. This object is reference counted because one
// of its two subclasses own the data they carry, and we need to have
// heterogeneous containers of these two types of memory.
class BASE_EXPORT RefCountedMemory
    : public RefCountedThreadSafe<RefCountedMemory> {
 public:
  // Retrieves a pointer to the beginning of the data we point to. If the data
  // is empty, this will return NULL.
  virtual const unsigned char* front() const = 0;

  // Size of the memory pointed to.
  virtual size_t size() const = 0;

  // Returns true if |other| is byte for byte equal.
  bool Equals(const scoped_refptr<RefCountedMemory>& other) const;

  // Handy method to simplify calling front() with a reinterpret_cast.
  template <typename T>
  const T* front_as() const {
    return reinterpret_cast<const T*>(front());
  }

 protected:
  friend class RefCountedThreadSafe<RefCountedMemory>;
  RefCountedMemory();
  virtual ~RefCountedMemory();
};

// An implementation of RefCountedMemory, where the ref counting does not
// matter.
class BASE_EXPORT RefCountedStaticMemory : public RefCountedMemory {
 public:
  RefCountedStaticMemory() : data_(nullptr), length_(0) {}
  RefCountedStaticMemory(const void* data, size_t length)
      : data_(static_cast<const unsigned char*>(length ? data : nullptr)),
        length_(length) {}

  // Overridden from RefCountedMemory:
  virtual const unsigned char* front() const override;
  virtual size_t size() const override;

 private:
  virtual ~RefCountedStaticMemory();

  const unsigned char* data_;
  size_t length_;

  DISALLOW_COPY_AND_ASSIGN(RefCountedStaticMemory);
};

// An implementation of RefCountedMemory, where we own the data in a vector.
class BASE_EXPORT RefCountedBytes : public RefCountedMemory {
 public:
  RefCountedBytes();

  // Constructs a RefCountedBytes object by _copying_ from |initializer|.
  explicit RefCountedBytes(std::vector<unsigned char> initializer);

  // Constructs a RefCountedBytes object by copying |size| bytes from |p|.
  RefCountedBytes(const unsigned char* p, size_t size);

  // Constructs a RefCountedBytes object by performing a swap. (To non
  // destructively build a RefCountedBytes, use the constructor that takes a
  // vector.)
  static RefCountedBytes* TakeVector(std::vector<unsigned char>* to_destroy);

  // Overridden from RefCountedMemory:
  virtual const unsigned char* front() const override;
  virtual size_t size() const override;

  const std::vector<unsigned char>& data() const {
    return data_;
  }
  std::vector<unsigned char>& data() {
    return data_;
  }

 private:
  virtual ~RefCountedBytes();

  std::vector<unsigned char> data_;

  DISALLOW_COPY_AND_ASSIGN(RefCountedBytes);
};

// An implementation of RefCountedMemory, where the bytes are stored in an STL
// string. Use this if your data naturally arrives in that format.
class BASE_EXPORT RefCountedString : public RefCountedMemory {
 public:
  RefCountedString();

  // Constructs a RefCountedString object by performing a swap. (To non
  // destructively build a RefCountedString, use the default constructor and
  // copy into object->data()).
  static RefCountedString* TakeString(std::string* to_destroy);

  // Overridden from RefCountedMemory:
  virtual const unsigned char* front() const override;
  virtual size_t size() const override;

  const std::string& data() const {
    return data_;
  }
  std::string& data() {
    return data_;
  }

 private:
  virtual ~RefCountedString();

  std::string data_;

  DISALLOW_COPY_AND_ASSIGN(RefCountedString);
};

// An implementation of RefCountedMemory that holds a chunk of memory
// previously allocated with malloc or calloc, and that therefore must be freed
// using free().
class BASE_EXPORT RefCountedMallocedMemory : public RefCountedMemory {
 public:
  RefCountedMallocedMemory(void* data, size_t length);

  // Overridden from RefCountedMemory:
  virtual const unsigned char* front() const override;
  virtual size_t size() const override;

 private:
  virtual ~RefCountedMallocedMemory();

  unsigned char* data_;
  size_t length_;

  DISALLOW_COPY_AND_ASSIGN(RefCountedMallocedMemory);
};

} // namespace kudu

#endif // KUDU_GUTIL_REF_COUNTED_MEMORY_H_
