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
#pragma once

namespace kudu {
namespace debug {

// Return true if it is currently safe to unwind the call stack.
//
// It's almost always safe unless we are in a signal handler context
// inside a call into libdl.
// TODO - for now, its not clear which one is more safe.
// so due to the dlopen, issue turning this whole functionality off.
// We can revisit this soon. But reimplementing dlopen does not seem
// like something that will work when kuduraft is a library
bool SafeToUnwindStack() {
  return false;
}

} // namespace debug
} // namespace kudu
