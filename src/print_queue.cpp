#include "print_queue.h"
#include "cloud_client.h"
#include "printer_client.h"
#include "system_state.h"
#include "rgb_led_manager.h"
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>

PrintQueue g_printQueue;

void PrintQueue::begin() {
    m_regMutex = xSemaphoreCreateMutex();
    for (int i = 0; i < PRINTER_COUNT; i++) {
        m_queue[i] = xQueueCreate(PRINT_QUEUE_LEN, sizeof(PrintJob*));
    }
    // One worker per printer; kitchen/juice tickets get priority over the bill.
    xTaskCreatePinnedToCore(taskEntry, "PrintBill", PRINT_TASK_STACK, (void*)(intptr_t)PRINTER_BILL, 2, &m_task[PRINTER_BILL], NETWORK_TASK_CORE);
    xTaskCreatePinnedToCore(taskEntry, "PrintKot", PRINT_TASK_STACK, (void*)(intptr_t)PRINTER_KOT, 3, &m_task[PRINTER_KOT], NETWORK_TASK_CORE);
    xTaskCreatePinnedToCore(taskEntry, "PrintJuice", PRINT_TASK_STACK, (void*)(intptr_t)PRINTER_JUICE, 3, &m_task[PRINTER_JUICE], NETWORK_TASK_CORE);
}

void PrintQueue::freeJob(PrintJob* job) {
    if (!job) return;
    if (job->data) heap_caps_free(job->data);
    delete job;
}

// ------------------------------------------------------------------------------
// Job registry (dedupe on server redelivery)
// ------------------------------------------------------------------------------
ReserveResult PrintQueue::reserve(const char* jobId) {
    ReserveResult result = RESERVE_NEW;
    xSemaphoreTake(m_regMutex, portMAX_DELAY);

    RegistryEntry* freeSlot = nullptr;
    RegistryEntry* oldestDone = nullptr;
    for (auto& e : m_registry) {
        if (e.state != 0 && strcmp(e.id, jobId) == 0) {
            result = (e.state == 1) ? RESERVE_DUP_INFLIGHT : RESERVE_DUP_DONE;
            xSemaphoreGive(m_regMutex);
            return result;
        }
        if (e.state == 0 && !freeSlot) freeSlot = &e;
        if (e.state == 2 && (!oldestDone || e.seq < oldestDone->seq)) oldestDone = &e;
    }

    RegistryEntry* slot = freeSlot ? freeSlot : oldestDone;
    if (!slot) {
        // Registry full of in-flight jobs: cannot happen with the queue limits, but fail safe.
        xSemaphoreGive(m_regMutex);
        return RESERVE_DUP_INFLIGHT;
    }
    strlcpy(slot->id, jobId, sizeof(slot->id));
    slot->state = 1;
    slot->seq = ++m_seq;
    xSemaphoreGive(m_regMutex);
    return RESERVE_NEW;
}

void PrintQueue::release(const char* jobId) {
    xSemaphoreTake(m_regMutex, portMAX_DELAY);
    for (auto& e : m_registry) {
        if (e.state != 0 && strcmp(e.id, jobId) == 0) { e.state = 0; break; }
    }
    xSemaphoreGive(m_regMutex);
}

void PrintQueue::markDone(const char* jobId) {
    xSemaphoreTake(m_regMutex, portMAX_DELAY);
    for (auto& e : m_registry) {
        if (e.state != 0 && strcmp(e.id, jobId) == 0) { e.state = 2; e.seq = ++m_seq; break; }
    }
    xSemaphoreGive(m_regMutex);
}

// ------------------------------------------------------------------------------
// Queue
// ------------------------------------------------------------------------------
bool PrintQueue::submit(PrintJob* job) {
    PrinterIdx idx = printerForType(job->type);
    if (xQueueSend(m_queue[idx], &job, 0) != pdTRUE) return false;
    StateLock lock(20);
    if (lock.ok()) g_systemState.queueCount = pendingCount();
    return true;
}

bool PrintQueue::hasRoom() const {
    for (int i = 0; i < PRINTER_COUNT; i++) {
        if (uxQueueSpacesAvailable(m_queue[i]) == 0) return false;
    }
    return true;
}

uint32_t PrintQueue::pendingCount() const {
    uint32_t n = (uint32_t)m_inFlight.load();
    for (int i = 0; i < PRINTER_COUNT; i++) n += uxQueueMessagesWaiting(m_queue[i]);
    return n;
}

PrinterIdx PrintQueue::printerForType(const char* type) {
    if (strstr(type, "JUICE")) return PRINTER_JUICE;   // Check before "KOT": the type is "JUICE_KOT"
    if (strstr(type, "KOT")) return PRINTER_KOT;
    return PRINTER_BILL;                               // BILL, TEST_SLIP
}

void PrintQueue::taskEntry(void* param) {
    g_printQueue.runWorker((PrinterIdx)(intptr_t)param);
}

void PrintQueue::runWorker(PrinterIdx idx) {
    esp_task_wdt_add(nullptr);
    Serial.printf("[PRINT] Worker started for %s printer\n", idx == PRINTER_KOT ? "KOT" : (idx == PRINTER_JUICE ? "JUICE" : "BILL"));
    while (true) {
        esp_task_wdt_reset();
        PrintJob* job = nullptr;
        if (xQueueReceive(m_queue[idx], &job, pdMS_TO_TICKS(1000)) == pdTRUE && job) {
            process(idx, job);
        }
    }
}

void PrintQueue::process(PrinterIdx idx, PrintJob* job) {
    m_busy[idx] = true;
    m_inFlight++;
    {
        StateLock lock;
        if (lock.ok()) {
            g_systemState.state = STATE_PRINTING;
            g_systemState.activeJobId = job->id;
            g_systemState.activeJobType = job->type;
            g_systemState.activeTargetHost = job->host;
            g_systemState.activeTargetPort = job->port;
            g_systemState.activeJobBytes = job->len;
            g_systemState.queueCount = pendingCount();
        }
    }
    g_rgbLed.triggerPrinting(true);

    bool printed = false;
    String failMsg;

    if (millis() - job->receivedMs > JOB_STALE_MS) {
        // The server lease is about to lapse; hand the job back instead of risking a double print.
        failMsg = "Job expired waiting in bridge queue";
        g_cloudClient.reportFail(job->id, failMsg, false, 0);
        release(job->id);
    } else {
        const char* jobId = job->id;
        PrintResult r = g_printerClient.sendRawPrintJob(String(job->host), job->port, job->data, job->len, [jobId]() {
            g_cloudClient.reportSent(jobId);   // Server stops auto-retrying the job once bytes may reach the printer
        });

        if (r.outcome == PRINT_OK) {
            printed = true;
            markDone(job->id);
            Serial.printf("[PRINT] %s %s: %u bytes in %ums to %s:%u\n", job->type, job->id,
                          (unsigned)job->len, (unsigned)r.durationMs, job->host, job->port);
            if (!g_cloudClient.reportComplete(job->id, (int)job->len)) {
                addPendingReport(job->id, (int)job->len);
            }
        } else {
            failMsg = r.error;
            bool ambiguous = (r.outcome == PRINT_WRITE_FAILED);
            Serial.printf("[PRINT] FAILED %s %s: %s\n", job->type, job->id, r.error.c_str());
            release(job->id);
            g_cloudClient.reportFail(job->id, r.error, ambiguous, (int)r.bytesSent);
        }
    }

    if (printed) {
        clearAlertPrefix("PRINTER FAIL");
        g_rgbLed.triggerPrintSuccess();
    } else {
        setAlert("PRINTER FAIL", String(job->host) + ":" + String(job->port));
        g_rgbLed.triggerPrintFailed();
    }

    freeJob(job);
    m_busy[idx] = false;
    int remaining = --m_inFlight;
    {
        StateLock lock;
        if (lock.ok()) {
            if (printed) g_systemState.totalJobsPrinted++; else g_systemState.totalJobsFailed++;
            g_systemState.queueCount = pendingCount();
            if (remaining == 0) {
                g_systemState.state = STATE_IDLE;
                g_systemState.activeJobId = "";
            }
        }
    }
    if (remaining == 0) g_rgbLed.triggerPrinting(false);
}

// ------------------------------------------------------------------------------
// Completion reports that could not reach the server
// ------------------------------------------------------------------------------
void PrintQueue::addPendingReport(const char* jobId, int bytes) {
    xSemaphoreTake(m_regMutex, portMAX_DELAY);
    PendingReport* slot = nullptr;
    for (auto& p : m_pending) {
        if (p.used && strcmp(p.id, jobId) == 0) { slot = &p; break; }
        if (!p.used && !slot) slot = &p;
    }
    if (!slot) slot = &m_pending[0];   // Full: overwrite the first (the server lease sweep is the backstop)
    strlcpy(slot->id, jobId, sizeof(slot->id));
    slot->bytes = bytes;
    slot->used = true;
    xSemaphoreGive(m_regMutex);
}

void PrintQueue::flushPendingReports() {
    for (int i = 0; i < PENDING_REPORT_SLOTS; i++) {
        char id[MAX_JOB_ID_LEN + 1];
        int bytes = 0;
        xSemaphoreTake(m_regMutex, portMAX_DELAY);
        bool has = m_pending[i].used;
        if (has) { strlcpy(id, m_pending[i].id, sizeof(id)); bytes = m_pending[i].bytes; }
        xSemaphoreGive(m_regMutex);
        if (!has) continue;

        if (g_cloudClient.reportComplete(id, bytes, 2)) {
            xSemaphoreTake(m_regMutex, portMAX_DELAY);
            if (m_pending[i].used && strcmp(m_pending[i].id, id) == 0) m_pending[i].used = false;
            xSemaphoreGive(m_regMutex);
        }
    }
}
