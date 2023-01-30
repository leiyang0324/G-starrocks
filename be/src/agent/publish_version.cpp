// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Inc.

#include "publish_version.h"

#include <bvar/bvar.h>

#include "common/compiler_util.h"
#include "common/tracer.h"
#include "fmt/format.h"
#include "gutil/strings/join.h"
#include "storage/data_dir.h"
#include "storage/storage_engine.h"
#include "storage/tablet.h"
#include "storage/tablet_manager.h"
#include "storage/txn_manager.h"
#include "util/starrocks_metrics.h"
#include "util/threadpool.h"
#include "util/time.h"

namespace starrocks {

const uint32_t PUBLISH_VERSION_SUBMIT_MAX_RETRY = 10;
bvar::LatencyRecorder g_publish_latency("be", "publish");

struct TabletPublishVersionTask {
    // input params
    int64_t txn_id{0};
    int64_t partition_id{0};
    int64_t tablet_id{0};
    int64_t version{0}; // requested publish version
    RowsetSharedPtr rowset;
    // output params
    Status st;
    // max continuous version after publish is done
    // or 0 which means tablet not found or publish task cannot be submitted
    int64_t max_continuous_version{0};
};

void run_publish_version_task(ThreadPoolToken* token, const PublishVersionAgentTaskRequest& publish_version_task,
                              TFinishTaskRequest& finish_task, std::unordered_set<DataDir*>& affected_dirs) {
    int64_t start_ts = MonotonicMillis();
    auto& publish_version_req = publish_version_task.task_req;
    int64_t transaction_id = publish_version_req.transaction_id;

    Span span = Tracer::Instance().start_trace_or_add_span("run_publish_version_task",
                                                           publish_version_req.txn_trace_parent);
    span->SetAttribute("txn_id", transaction_id);
    auto scoped = trace::Scope(span);

    size_t num_partition = publish_version_req.partition_version_infos.size();
    size_t num_active_tablet = 0;
    std::vector<std::map<TabletInfo, RowsetSharedPtr>> partitions(num_partition);
    for (size_t i = 0; i < publish_version_req.partition_version_infos.size(); i++) {
        StorageEngine::instance()->txn_manager()->get_txn_related_tablets(
                transaction_id, publish_version_req.partition_version_infos[i].partition_id, &partitions[i]);
        num_active_tablet += partitions[i].size();
    }
    span->SetAttribute("num_partition", num_partition);
    span->SetAttribute("num_tablet", num_active_tablet);
    std::vector<TabletPublishVersionTask> tablet_tasks(num_active_tablet);
    size_t tablet_idx = 0;
    for (size_t i = 0; i < publish_version_req.partition_version_infos.size(); i++) {
        for (auto& itr : partitions[i]) {
            auto& task = tablet_tasks[tablet_idx++];
            task.txn_id = transaction_id;
            task.partition_id = publish_version_req.partition_version_infos[i].partition_id;
            task.tablet_id = itr.first.tablet_id;
            task.version = publish_version_req.partition_version_infos[i].version;
            task.rowset = std::move(itr.second);
        }
    }
    std::mutex affected_dirs_lock;
    for (auto& tablet_task : tablet_tasks) {
        uint32_t retry_time = 0;
        Status st;
        while (retry_time++ < PUBLISH_VERSION_SUBMIT_MAX_RETRY) {
            st = token->submit_func([&]() {
                auto& task = tablet_task;
                auto tablet_span = Tracer::Instance().add_span("tablet_publish_txn", span);
                auto scoped_tablet_span = trace::Scope(tablet_span);
                tablet_span->SetAttribute("txn_id", transaction_id);
                tablet_span->SetAttribute("tablet_id", task.tablet_id);
                tablet_span->SetAttribute("version", task.version);
                if (!task.rowset) {
                    task.st = Status::NotFound(
                            fmt::format("rowset not found  of tablet: {}, txn_id: {}", task.tablet_id, task.txn_id));
                    LOG(WARNING) << task.st;
                    return;
                }
                TabletSharedPtr tablet = StorageEngine::instance()->tablet_manager()->get_tablet(task.tablet_id);
                if (!tablet) {
                    // tablet may get dropped, it's ok to ignore this situation
                    LOG(WARNING) << fmt::format(
                            "publish_version tablet not found tablet_id: {}, version: {} txn_id: {}", task.tablet_id,
                            task.version, task.txn_id);
                    return;
                }
                {
                    std::lock_guard lg(affected_dirs_lock);
                    affected_dirs.insert(tablet->data_dir());
                }
                task.st = StorageEngine::instance()->txn_manager()->publish_txn(task.partition_id, tablet, task.txn_id,
                                                                                task.version, task.rowset);
                if (!task.st.ok()) {
                    LOG(WARNING) << "Publish txn failed tablet:" << tablet->tablet_id() << " version:" << task.version
                                 << " partition:" << task.partition_id << " txn_id: " << task.txn_id
                                 << " rowset:" << task.rowset->rowset_id();
                    tablet_span->SetStatus(trace::StatusCode::kError, task.st.get_error_msg());
                } else {
                    LOG(INFO) << "Publish txn success tablet:" << tablet->tablet_id() << " version:" << task.version
                              << " tablet_max_version:" << tablet->max_continuous_version()
                              << " partition:" << task.partition_id << " txn_id: " << task.txn_id
                              << " rowset:" << task.rowset->rowset_id();
                }
            });
            if (st.is_service_unavailable()) {
                int64_t retry_sleep_ms = 50 * retry_time;
                LOG(WARNING) << "publish version threadpool is busy, retry in  " << retry_sleep_ms
                             << "ms. txn_id: " << transaction_id << ", tablet:" << tablet_task.tablet_id;
                // In general, publish version is fast. A small sleep is needed here.
                auto wait_span = Tracer::Instance().add_span("retry_wait", span);
                SleepFor(MonoDelta::FromMilliseconds(retry_sleep_ms));
                continue;
            } else {
                break;
            }
        }
        if (!st.ok()) {
            tablet_task.st = std::move(st);
        }
    }
    span->AddEvent("all_task_submitted");
    token->wait();
    span->AddEvent("all_task_finished");

    Status st;
    finish_task.__isset.tablet_versions = true;
    auto& error_tablet_ids = finish_task.error_tablet_ids;
    auto& tablet_versions = finish_task.tablet_versions;
    tablet_versions.reserve(tablet_tasks.size());
    for (auto& task : tablet_tasks) {
        if (!task.st.ok()) {
            error_tablet_ids.push_back(task.tablet_id);
            if (st.ok()) {
                st = task.st;
            }
        }
    }
    // return tablet and its version which has already finished.
    int total_tablet_cnt = 0;
    for (size_t i = 0; i < publish_version_req.partition_version_infos.size(); i++) {
        const auto& partition_version = publish_version_req.partition_version_infos[i];
        std::vector<TabletInfo> tablet_infos;
        StorageEngine::instance()->tablet_manager()->get_tablets_by_partition(partition_version.partition_id,
                                                                              tablet_infos);
        total_tablet_cnt += tablet_infos.size();
        for (const auto& tablet_info : tablet_infos) {
            TabletSharedPtr tablet = StorageEngine::instance()->tablet_manager()->get_tablet(tablet_info.tablet_id);
            if (!tablet) {
                // tablet may get dropped, it's ok to ignore this situation
                LOG(WARNING) << fmt::format("publish_version tablet not found tablet_id: {}, version: {} txn_id: {}",
                                            tablet_info.tablet_id, partition_version.version, transaction_id);
            } else {
                const int64_t max_continuous_version = tablet->max_continuous_version();
                if (max_continuous_version > 0) {
                    auto& pair = tablet_versions.emplace_back();
                    pair.__set_tablet_id(tablet_info.tablet_id);
                    pair.__set_version(max_continuous_version);
                }
            }
        }
    }

    // TODO: add more information to status, rather than just first error tablet
    st.to_thrift(&finish_task.task_status);
    finish_task.__set_task_type(publish_version_task.task_type);
    finish_task.__set_signature(publish_version_task.signature);
    if (!error_tablet_ids.empty()) {
        finish_task.__isset.error_tablet_ids = true;
    }

    auto publish_latency = MonotonicMillis() - start_ts;
    g_publish_latency << publish_latency;
    if (st.ok()) {
        LOG(INFO) << "publish_version success. txn_id: " << transaction_id << " #partition:" << num_partition
                  << " #tablet:" << tablet_tasks.size() << " time:" << publish_latency << "ms"
                  << " #already_finished:" << total_tablet_cnt - num_active_tablet;
    } else {
        StarRocksMetrics::instance()->publish_task_failed_total.increment(1);
        LOG(WARNING) << "publish_version has error. txn_id: " << transaction_id << " #partition:" << num_partition
                     << " #tablet:" << tablet_tasks.size() << " error_tablets(" << error_tablet_ids.size()
                     << "):" << JoinInts(error_tablet_ids, ",") << " time:" << publish_latency << "ms"
                     << " #already_finished:" << total_tablet_cnt - num_active_tablet;
    }
}

} // namespace starrocks
