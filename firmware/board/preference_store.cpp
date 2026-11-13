#include "preference_store.hpp"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <algorithm>

namespace bottie {
namespace {
constexpr const char* partition = "nvs";
constexpr const char* namespace_name = "bottie_ui_v1";
constexpr const char* key = "prefs";
StoreResult translate(esp_err_t result) {
    if (result == ESP_OK) return StoreResult::ok;
    if (result == ESP_ERR_NVS_NOT_FOUND) return StoreResult::missing;
    if (result == ESP_ERR_NVS_NOT_ENOUGH_SPACE || result == ESP_ERR_NVS_NO_FREE_PAGES) return StoreResult::full;
    if (result == ESP_ERR_NVS_NEW_VERSION_FOUND) return StoreResult::unknown_version;
    return StoreResult::write_error;
}
}
StoreResult PreferenceStore::initialize() {
    const auto* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_NVS,partition);
    if (!part || part->address != 0x9000 || part->size != 0x5000 || part->encrypted) {
        return StoreResult::unavailable;
    }
    sdk_result_=nvs_flash_init_partition(partition);
    return translate(sdk_result_);
}
StoreResult PreferenceStore::read(PreferenceBytes& bytes,size_t& size) {
    size=0;
    nvs_handle_t handle{};
    auto result = nvs_open_from_partition(partition,namespace_name,NVS_READONLY,&handle);
    StoreResult loaded = translate(result);
    if (result == ESP_OK) {
        result = nvs_get_blob(handle,key,nullptr,&size);
        if (result == ESP_OK && size == bytes.size()) result = nvs_get_blob(handle,key,bytes.data(),&size);
        loaded = result == ESP_OK && size != bytes.size() ? StoreResult::unknown_version : translate(result);
        nvs_close(handle);
    }
    sdk_result_=result; return loaded;
}
void PreferenceStore::start(PreferenceState& state) {
    const auto before=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    load_preferences(*this,state);
    const auto after=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    heap_delta_=static_cast<int32_t>(before)-static_cast<int32_t>(after);
    if (!state.writable()) return;
    consumer_ = xTaskGetCurrentTaskHandle();
    mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
    if (mutex_) task_ = xTaskCreateStatic(entry,"prefs",stack_.size()*sizeof(StackType_t),this,1,stack_.data(),&task_storage_);
    if (!task_) state.loaded(StoreResult::unavailable);
}
StoreResult PreferenceStore::write(Preferences value) {
    if (!value.valid()) return StoreResult::write_error;
    nvs_handle_t handle{};
    auto result = nvs_open_from_partition(partition,namespace_name,NVS_READWRITE,&handle);
    if (result != ESP_OK) { sdk_result_=result; return translate(result); }
    const auto bytes = encode_preferences(value);
    result = nvs_set_blob(handle,key,bytes.data(),bytes.size());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    sdk_result_=result; return translate(result);
}
bool PreferenceStore::submit(const PreferenceWrite& value) {
    if (!task_ || xSemaphoreTake(mutex_,0) != pdTRUE) return false;
    const bool accepted = !busy_;
    if (accepted) { pending_ = value; busy_ = true; complete_ = false; }
    xSemaphoreGive(mutex_);
    if (accepted) xTaskNotifyGive(task_);
    return accepted;
}
bool PreferenceStore::result(PreferenceWrite& value, StoreResult& result) {
    if (!task_ || xSemaphoreTake(mutex_,0) != pdTRUE) return false;
    const bool ready = complete_;
    if (ready) { value = pending_; result = result_; busy_ = complete_ = false; }
    xSemaphoreGive(mutex_);
    return ready;
}
void PreferenceStore::entry(void* self) {
    auto& store = *static_cast<PreferenceStore*>(self);
    for (;;) {
        store.stack_free_ = uxTaskGetStackHighWaterMark(nullptr);
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        xSemaphoreTake(store.mutex_,portMAX_DELAY);
        const auto value = store.pending_.value;
        xSemaphoreGive(store.mutex_);
        const auto started=esp_timer_get_time();
        const auto result = store.write(value);
        store.last_write_us_=static_cast<uint32_t>(std::min<int64_t>(UINT32_MAX,esp_timer_get_time()-started));
        store.max_write_us_=std::max(store.max_write_us_.load(),store.last_write_us_.load());
        ++store.writes_; if(result!=StoreResult::ok) ++store.errors_;
        xSemaphoreTake(store.mutex_,portMAX_DELAY);
        store.result_ = result; store.complete_ = true;
        xSemaphoreGive(store.mutex_);
        store.stack_free_ = uxTaskGetStackHighWaterMark(nullptr);
        xTaskNotifyGive(store.consumer_);
    }
}
} // namespace bottie
