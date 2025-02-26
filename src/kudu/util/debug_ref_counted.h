// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef KUDU_UTIL_DEBUG_REF_COUNTED_H_
#define KUDU_UTIL_DEBUG_REF_COUNTED_H_

#include <glog/logging.h>

#include "kudu/gutil/ref_counted.h"
#include "kudu/util/debug-util.h"

namespace kudu {

// For use in debugging. Change a ref-counted class to inherit from this,
// instead of RefCountedThreadSafe, and fill your logs with stack traces.
template <class T, typename Traits = DefaultRefCountedThreadSafeTraits<T>>
class DebugRefCountedThreadSafe : public RefCountedThreadSafe<T, Traits> {
 public:
  DebugRefCountedThreadSafe() {}

  void AddRef() const {
    RefCountedThreadSafe<T, Traits>::AddRef();
    LOG(INFO) << "Incremented ref on " << this << ":\n" << GetStackTrace();
  }

  void Release() const {
    LOG(INFO) << "Decrementing ref on " << this << ":\n" << GetStackTrace();
    RefCountedThreadSafe<T, Traits>::Release();
  }

 protected:
  ~DebugRefCountedThreadSafe() {}

 private:
  friend struct DefaultRefCountedThreadSafeTraits<T>;

  DISALLOW_COPY_AND_ASSIGN(DebugRefCountedThreadSafe);
};

} // namespace kudu

#endif // KUDU_UTIL_DEBUG_REF_COUNTED_H_
