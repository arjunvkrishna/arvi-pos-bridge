#ifndef NERAM_PRINT_QUEUE_H
#define NERAM_PRINT_QUEUE_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>
#include "config.h"

enum PrinterIdx { PRINTER_BILL = 0, PRINTER_KOT = 1, PRINTER_JUICE = 2, PRINTER_COUNT = 3 };

// One decoded print job waiting for (or being sent to) a printer.
struct PrintJob {
    char id[MAX_JOB_ID_LEN + 1];
    char type[16];   // BILL, KOT, JUICE_KOT, TEST_SLIP
    char host[16];
    uint16_t port;
    uint8_t* data;        // PSRAM, owned by the job
    size_t len;
    uint32_t receivedMs;
};

enum ReserveResult {
    RESERVE_NEW = 0,        // First time we see this job - process it
    RESERVE_DUP_INFLIGHT,   // Redelivered while still queued/printing - ignore
    RESERVE_DUP_DONE        // Already printed - only (re)report completion, never reprint
};

// Local job queue: one FIFO + one worker task per printer (Bill, Kitchen KOT, Juice KOT), so a
// slow/offline printer never delays the others. Also dedupes redelivered jobs and retries
// completion reports that could not reach the server.
class PrintQueue {
public:
    void begin();

    ReserveResult reserve(const char* jobId);   // Registers the ID as in-flight when NEW
    void release(const char* jobId);            // Forget (job failed / was rejected, may legitimately return)
    bool submit(PrintJob* job);                 // Takes ownership on true
    bool hasRoom() const;                       // Room in every printer queue
    uint32_t pendingCount() const;              // Waiting + printing
    bool isBusy(PrinterIdx idx) const { return m_busy[idx]; }

    void addPendingReport(const char* jobId, int bytes);
    void flushPendingReports();

    static void freeJob(PrintJob* job);
    static PrinterIdx printerForType(const char* type);

private:
    struct RegistryEntry { char id[MAX_JOB_ID_LEN + 1]; uint8_t state; uint32_t seq; };   // 0 free, 1 in-flight, 2 done
    struct PendingReport { char id[MAX_JOB_ID_LEN + 1]; int bytes; bool used; };

    static void taskEntry(void* param);
    void runWorker(PrinterIdx idx);
    void process(PrinterIdx idx, PrintJob* job);
    void markDone(const char* jobId);

    QueueHandle_t m_queue[PRINTER_COUNT] = {};
    TaskHandle_t m_task[PRINTER_COUNT] = {};
    volatile bool m_busy[PRINTER_COUNT] = {};
    std::atomic<int> m_inFlight{0};

    SemaphoreHandle_t m_regMutex = nullptr;
    RegistryEntry m_registry[JOB_REGISTRY_SIZE] = {};
    uint32_t m_seq = 0;
    PendingReport m_pending[PENDING_REPORT_SLOTS] = {};
};

extern PrintQueue g_printQueue;

#endif // NERAM_PRINT_QUEUE_H
