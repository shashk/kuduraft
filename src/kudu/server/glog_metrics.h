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
#ifndef KUDU_SERVER_GLOG_METRICS_H
#define KUDU_SERVER_GLOG_METRICS_H

#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/ref_counted.h"

namespace google {
class LogSink;
} // namespace google

namespace kudu {
class MetricEntity;

// Attaches GLog metrics to the given entity, for the duration of this
// scoped object's lifetime.
//
// NOTE: the metrics are collected process-wide, not confined to any set of
// threads, etc.
class ScopedGLogMetrics {
 public:
  explicit ScopedGLogMetrics(const scoped_refptr<MetricEntity>& entity);
  ~ScopedGLogMetrics();

 private:
  gscoped_ptr<google::LogSink> sink_;
};

// Registers glog-related metrics.
// This can be called multiple times on different entities, though the resulting
// metrics will be identical, since the GLog tracking is process-wide.
void RegisterGLogMetrics(const scoped_refptr<MetricEntity>& entity);

} // namespace kudu
#endif /* KUDU_SERVER_GLOG_METRICS_H */
