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

#include "kudu/security/kerberos_util.h"
#include "kudu/gutil/strings/split.h"
#include "kudu/gutil/strings/stringpiece.h"

#include <array>
#include <utility>

namespace kudu {
namespace security {

std::array<StringPiece, 3> SplitKerberosPrincipal(StringPiece principal) {
  std::pair<StringPiece, StringPiece> user_realm =
      strings::Split(principal, "@");
  std::pair<StringPiece, StringPiece> princ_host =
      strings::Split(user_realm.first, "/");
  return {{princ_host.first, princ_host.second, user_realm.second}};
}

} // namespace security
} // namespace kudu
