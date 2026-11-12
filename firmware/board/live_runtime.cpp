#include "live_runtime.hpp"

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
    return xSemaphoreTake(stopped_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void LiveRuntime::entry(void* self) {
    auto& runtime = *static_cast<LiveRuntime*>(self);
    runtime.work();
    runtime.client_.stop();
    runtime.running_ = false;
    xSemaphoreGive(runtime.stopped_);
    vTaskDelete(nullptr);
}

void LiveRuntime::publish(bool online, const Snapshot* snapshot, const Artwork* art, uint64_t observed_ms) {
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE) return;
    if (online && snapshot) shared_.capture(*snapshot, observed_ms, art);
    else shared_.offline();
    ++published_;
    xSemaphoreGive(mutex_);
}

void LiveRuntime::work() {
    if (!client_.start()) {
        status_ = client_.status();
        stack_free_ = uxTaskGetStackHighWaterMark(nullptr);
        std::puts("Companion transport not started; check local configuration");
        publish(false);
        return;
    }
    while (!stopping_) {
        size_t length = 0;
        uint64_t observed = now_ms();
        bool accepted = false;
        if (client_.playback(body_.data(), body_.size(), length)) {
            Snapshot snapshot;
            if (parse_snapshot(body_.data(), length, snapshot) == Error::none &&
                worker_model_.apply_snapshot(snapshot, observed) == Error::none) {
                publish(true, &snapshot, client_.artwork(), observed);
                accepted = true;
            }
            else publish(false);
        } else publish(false);
        if (!accepted) worker_model_.disconnected();
        Intent intent;
        bool send = false;
        if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
            send = controls_.take(worker_model_, now_ms(), intent);
            xSemaphoreGive(mutex_);
        }
        if (send && !stopping_) {
            char json[128];
            if (encode_intent(intent, json, sizeof json) && client_.control(json, std::strlen(json))) ++sent_;
            else { ++control_errors_; worker_model_.disconnected(); publish(false); }
        }
        ++polls_;
        if (accepted) ++accepted_;
        else ++rejected_;
        status_ = client_.status();
        poll_ms_ = static_cast<uint32_t>(now_ms() - observed);
        stack_free_ = uxTaskGetStackHighWaterMark(nullptr);
        // Cancellation can interrupt the next request without waiting for a full poll interval.
        for (unsigned n = 0; n < 10 && !stopping_; ++n) vTaskDelay(pdMS_TO_TICKS(100));
    }
    publish(false);
}

void LiveRuntime::consume(Model& model) {
    if (!mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) return;
    bool changed = published_ != consumed_;
    if (changed) { displayed_ = shared_; consumed_ = published_; }
    xSemaphoreGive(mutex_);
    if (!changed) return;
    if (!displayed_.online || model.apply_snapshot(displayed_.snapshot, displayed_.observed_ms) != Error::none) {
        displayed_.offline();
        model.disconnected();
    }
}

bool LiveRuntime::enqueue(const Intent& intent) {
    if (!intent.control_target[0] || !running_ || stopping_ || !mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    const bool accepted = controls_.put(intent, now_ms());
    xSemaphoreGive(mutex_);
    return accepted;
}

const Artwork* LiveRuntime::artwork() {
    displayed_view_ = displayed_.artwork();
    return displayed_view_.pixels ? &displayed_view_ : nullptr;
}

RuntimeHealth LiveRuntime::health() const {
    return {status_.load(), polls_.load(), accepted_.load(), rejected_.load(), stack_free_.load(), poll_ms_.load(), sent_.load(), control_errors_.load()};
}
} // namespace bottie
