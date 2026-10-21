#ifndef NERAM_PRINTER_CLIENT_H
#define NERAM_PRINTER_CLIENT_H

#include <Arduino.h>
#include <WiFiClient.h>
#include <functional>

struct PrinterCheckResult {
    bool reachable;
    uint32_t latencyMs;
    String errorMessage;
};

enum PrintOutcome {
    PRINT_OK = 0,
    PRINT_REJECTED,        // Target/payload failed validation - nothing was sent
    PRINT_CONNECT_FAILED,  // Never connected - nothing was sent, safe to retry
    PRINT_WRITE_FAILED     // Failed after connecting - paper may have printed (ambiguous)
};

struct PrintResult {
    PrintOutcome outcome = PRINT_REJECTED;
    size_t bytesSent = 0;
    uint32_t durationMs = 0;
    String error;
};

class PrinterClient {
public:
    PrinterClient();

    // Only private-LAN IPv4 targets on the printer port range are allowed.
    // Blocks loopback, own IP, gateway, subnet broadcast and public addresses.
    bool isAllowedTarget(const String& host, int port, String& why);

    PrinterCheckResult checkPrinter(const String& host, int port, uint32_t timeoutMs = 250);

    // Streams ESC/POS bytes. Sends to the same host:port are serialized so two jobs never interleave.
    // onConnected (optional) runs once the TCP connection is up, right before the first byte is written.
    PrintResult sendRawPrintJob(const String& host, int port, const uint8_t* data, size_t length,
                                const std::function<void()>& onConnected = nullptr);

    // Probe the configured printers (Bill, Kitchen KOT, Juice KOT); a busy printer is skipped (keeps its last result).
    void checkPrinters(bool skipBill = false, bool skipKot = false, bool skipJuice = false);

private:
    SemaphoreHandle_t lockFor(uint32_t ip, uint16_t port);
    struct Slot { uint32_t ip = 0; uint16_t port = 0; SemaphoreHandle_t mutex = nullptr; };
    Slot m_slots[4];
    SemaphoreHandle_t m_tableMutex;
};

extern PrinterClient g_printerClient;

#endif // NERAM_PRINTER_CLIENT_H
