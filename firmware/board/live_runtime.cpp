#include "live_runtime.hpp"
#include "work_schedule.hpp"
#include "bottie/network_policy.hpp"

#include <cstdio>
#include <cstring>

#include "esp_timer.h"

namespace bottie {
namespace {
uint64_t now_ms() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }
}

bool LiveRuntime::start() {
    if (started_) return false;
    started_ = true;
    running_ = true;
    stopping_ = false;
    consumer_ = xTaskGetCurrentTaskHandle();
    mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
    stopped_ = xSemaphoreCreateBinaryStatic(&stopped_storage_);
    if (!mutex_ || !stopped_) { running_ = false; return false; }
    auto task = xTaskCreateStatic(entry, "companion", stack_bytes, this, 3, stack_.data(), &task_storage_);
    if (!task) { running_ = false; return false; }
    return true;
}

bool LiveRuntime::stop(uint32_t timeout_ms) {
    if (!running_) return true;
    stopping_ = true;
    client_.cancel();
    if (auto task = worker_.load()) xTaskNotifyGive(task);
    return xSemaphoreTake(stopped_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void LiveRuntime::entry(void* self) {
    auto& runtime = *static_cast<LiveRuntime*>(self);
    runtime.worker_ = xTaskGetCurrentTaskHandle();
    runtime.work();
    runtime.client_.stop();
    runtime.running_ = false;
    xSemaphoreGive(runtime.stopped_);
    vTaskDelete(nullptr);
}

void LiveRuntime::publish(bool online, const Snapshot* snapshot, const Artwork* art, uint64_t observed_ms, const TextMask* mask) {
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE) return;
    if (online && snapshot) shared_.capture(*snapshot, observed_ms, art, mask);
    else shared_.offline();
    ++published_;
    xSemaphoreGive(mutex_);
    if (consumer_) xTaskNotifyGive(consumer_);
}

void LiveRuntime::work() {
    if (!client_.start()) {
        status_ = client_.status();
        stack_free_ = uxTaskGetStackHighWaterMark(nullptr);
        std::puts("Companion transport not started; check local configuration");
        publish(false);
        return;
    }
    WorkSchedule schedule;
    network::policy::Backoff detail_backoff;
    uint64_t observed = 0;
    while (!stopping_) {
        if (refresh_requested_.exchange(false)) schedule.refresh(now_ms());
        // Capture before checking the mailbox, closing the enqueue-to-request race.
        const auto ticket = client_.asset_ticket();
        Intent intent;
        bool send = false, pending = true;
        if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
            send = controls_.take(worker_model_, now_ms(), intent);
            pending = controls_.pending();
            xSemaphoreGive(mutex_);
        }
        if (send && !stopping_) {
            publish_outcome({intent,{FeedbackPhase::sending},now_ms()});
            char json[256];
            ControlReply reply;
            if (encode_intent(intent, json, sizeof json)) {
                if(client_.control(json,std::strlen(json))) ++sent_;
                else ++control_errors_;
                reply=client_.control_reply();
            } else { ++control_errors_; reply.phase=FeedbackPhase::failed; }
            publish_outcome({intent,reply,now_ms()});
            schedule.control_attempted(now_ms());
            continue;
        }
        const auto work = schedule.next(now_ms(), pending);
        if (work == WorkerWork::metadata) {
            size_t length = 0;
            const auto started = now_ms();
            bool accepted = false;
            if (client_.playback(body_.data(), body_.size(), length)) {
                Snapshot snapshot;
                observed = now_ms();
                if (parse_snapshot(body_.data(), length, snapshot) == Error::none &&
                    worker_model_.apply_snapshot(snapshot, observed) == Error::none) {
                    const auto mask = client_.text_mask(snapshot);
                    publish(true, &snapshot, client_.artwork(), observed, &mask);
                    accepted = true;
                }
            }
            if (!accepted) { worker_model_.disconnected(); publish(false); }
            ++polls_;
            if (accepted) ++accepted_; else ++rejected_;
            poll_ms_ = static_cast<uint32_t>(now_ms() - started);
            schedule.polled(now_ms(),worker_model_.effective_state(now_ms()) == Playback::playing);
        } else if (work == WorkerWork::asset) {
            bool need_detail=false;
            if(xSemaphoreTake(mutex_,pdMS_TO_TICKS(20))==pdTRUE) {
                if(!worker_detail_.same(requested_detail_)) {
                    worker_detail_=requested_detail_; detail_backoff.clear();
                }
                need_detail=worker_detail_.matches(worker_model_.snapshot) && !shared_detail_.ready && detail_backoff.ready(now_ms());
                xSemaphoreGive(mutex_);
            }
            if(need_detail && worker_model_.effective_state(now_ms())!=Playback::unavailable) {
                const bool fetched=client_.fetch_detail(worker_detail_,ticket);
                if(fetched && xSemaphoreTake(mutex_,pdMS_TO_TICKS(20))==pdTRUE) {
                    if(worker_detail_.same(requested_detail_) && worker_detail_.matches(worker_model_.snapshot) &&
                       shared_detail_.capture(worker_detail_,client_.detail_bytes(),detail_capacity)) ++detail_published_;
                    xSemaphoreGive(mutex_);
                    if(consumer_) xTaskNotifyGive(consumer_);
                }
                if(!fetched) detail_backoff.fail(now_ms());
            } else if (worker_model_.effective_state(now_ms()) != Playback::unavailable &&
                client_.fetch_asset(worker_model_.snapshot,ticket) &&
                worker_model_.effective_state(now_ms()) != Playback::unavailable) {
                const auto mask = client_.text_mask(worker_model_.snapshot);
                publish(true,&worker_model_.snapshot,client_.artwork(),observed,&mask);
            }
            schedule.asset_attempted(now_ms());
        } else ulTaskNotifyTake(pdTRUE, std::max<TickType_t>(1,pdMS_TO_TICKS(pending ? 20 : schedule.wait_ms(now_ms()))));
        status_ = client_.status();
        stack_free_ = uxTaskGetStackHighWaterMark(nullptr);
    }
    publish(false);
}

void LiveRuntime::consume(Model& model) {
    if (!mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) return;
    bool changed = published_ != consumed_;
    if (changed) { displayed_ = shared_; consumed_ = published_; }
    if(detail_published_!=detail_consumed_) { displayed_detail_=shared_detail_; detail_consumed_=detail_published_; }
    xSemaphoreGive(mutex_);
    if (!changed) return;
    if (!displayed_.online || model.apply_snapshot(displayed_.snapshot, displayed_.observed_ms) != Error::none) {
        displayed_.offline();
        model.disconnected();
        const auto status=status_.load();
        model.snapshot.offline_reason=status==network::Status::offline || status==network::Status::connecting ?
            OfflineReason::wifi_disconnected : OfflineReason::gateway_unreachable;
    }
}

bool LiveRuntime::enqueue(const Intent& intent) {
    if (!intent.control_target[0] || !running_ || stopping_ || !mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    const bool accepted = controls_.put(intent, now_ms());
    if (accepted) {
        requested_detail_.active=false; shared_detail_.clear(); ++detail_published_;
        client_.interrupt_assets();
    }
    xSemaphoreGive(mutex_);
    if (accepted) if (auto task = worker_.load()) xTaskNotifyGive(task);
    return accepted;
}
void LiveRuntime::refresh() {
    refresh_requested_ = true;
    client_.interrupt_assets();
    if (auto task = worker_.load()) xTaskNotifyGive(task);
}

const Artwork* LiveRuntime::artwork() {
    displayed_view_ = displayed_.artwork();
    return displayed_view_.pixels ? &displayed_view_ : nullptr;
}
void LiveRuntime::publish_outcome(const ControlOutcome& result) {
    if(xSemaphoreTake(mutex_,pdMS_TO_TICKS(20))!=pdTRUE) return;
    outcome_=result; ++outcome_published_;
    xSemaphoreGive(mutex_);
    if(consumer_) xTaskNotifyGive(consumer_);
}
bool LiveRuntime::outcome(ControlOutcome& result) {
    if(!mutex_ || xSemaphoreTake(mutex_,0)!=pdTRUE) return false;
    const bool changed=outcome_consumed_!=outcome_published_;
    if(changed) { result=outcome_; outcome_consumed_=outcome_published_; }
    xSemaphoreGive(mutex_); return changed;
}
void LiveRuntime::select_detail(const DetailSelection& selection) {
    if(!mutex_ || xSemaphoreTake(mutex_,pdMS_TO_TICKS(10))!=pdTRUE) return;
    const bool changed=requested_detail_.active!=selection.active ||
        (selection.active && !requested_detail_.same(selection));
    if(changed) {
        requested_detail_=selection; shared_detail_.clear(); ++detail_published_;
        client_.interrupt_assets();
    }
    xSemaphoreGive(mutex_);
    if(changed) if(auto task=worker_.load()) xTaskNotifyGive(task);
}
DetailPage LiveRuntime::detail(const Snapshot& snapshot,const DetailSelection& selection) const {
    return displayed_detail_.view(snapshot,selection);
}
TextMask LiveRuntime::text_mask() const { return displayed_.text_mask(); }

RuntimeHealth LiveRuntime::health() const {
    return {status_.load(), polls_.load(), accepted_.load(), rejected_.load(), stack_free_.load(), poll_ms_.load(), sent_.load(), control_errors_.load()};
}
} // namespace bottie
