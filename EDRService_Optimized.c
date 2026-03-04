#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <time.h>
#include <wininet.h>
#include <strsafe.h>
#include "../Common/EDRCommon.h"

#pragma comment(lib, "wininet.lib")

// ============================================================================
// CONFIGURATION AND CONSTANTS
// ============================================================================

#define BATCH_SIZE 100
#define BATCH_TIMEOUT_MS 30000
#define LOG_DIR "C:\\ProgramData\\MinimalEDR"
#define LOG_FILE LOG_DIR "\\events.log"
#define CONFIG_FILE LOG_DIR "\\config.ini"
#define METRICS_FILE LOG_DIR "\\metrics.log"
#define MAX_PATH_LEN 512
#define MAX_BATCH_RETRIES 3
#define UPLOAD_RETRY_DELAY_MS 5000
#define EDR_SERVICE_VERSION "1.1.0"

// ============================================================================
// STRUCTURE DEFINITIONS
// ============================================================================

typedef struct _EDR_CONFIG {
    BOOL TrackProcesses;
    BOOL TrackNetwork;
    BOOL TrackDns;
    BOOL TrackCrossProcess;
    BOOL EnableUpload;
    BOOL EnableLocalStorage;
    DWORD MaxQueueSize;
    DWORD BatchSize;
    DWORD BatchTimeoutMs;
    DWORD LogRotationSizeMB;
    CHAR S3Bucket[256];
    CHAR DatabaseConnection[512];
} EDR_CONFIG;

typedef struct _SERVICE_METRICS {
    UINT64 EventsProcessed;
    UINT64 EventsBatched;
    UINT64 EventsDropped;
    UINT64 UploadFailures;
    UINT64 UploadSuccesses;
    UINT64 BytesProcessed;
    DWORD AverageLatencyMs;
    FILETIME LastUpdateTime;
} SERVICE_METRICS;

typedef struct _EVENT_BATCH {
    EDR_EVENT Events[BATCH_SIZE];
    DWORD Count;
    DWORD StartTime;
    DWORD RetryCount;
} EVENT_BATCH;

typedef struct _SERVICE_CONTEXT {
    SERVICE_STATUS ServiceStatus;
    SERVICE_STATUS_HANDLE StatusHandle;
    HANDLE StopEvent;
    HANDLE DriverHandle;
    EDR_CONFIG Config;
    SERVICE_METRICS Metrics;
    HANDLE MetricsLock;
} SERVICE_CONTEXT;

// ============================================================================
// GLOBAL STATE
// ============================================================================

SERVICE_CONTEXT g_ServiceContext = { 0 };

// ============================================================================
// LOGGING AND DIAGNOSTICS
// ============================================================================

VOID LogEvent(LPCSTR Level, LPCSTR Format, ...)
{
    va_list Args;
    CHAR Buffer[1024];
    CHAR LogBuffer[1024];
    SYSTEMTIME SystemTime;
    HANDLE LogFile;

    GetLocalTime(&SystemTime);

    va_start(Args, Format);
    StringCchVPrintfA(Buffer, sizeof(Buffer), Format, Args);
    va_end(Args);

    StringCchPrintfA(LogBuffer, sizeof(LogBuffer),
        "[%04d-%02d-%02d %02d:%02d:%02d] [%s] %s\n",
        SystemTime.wYear, SystemTime.wMonth, SystemTime.wDay,
        SystemTime.wHour, SystemTime.wMinute, SystemTime.wSecond,
        Level, Buffer);

    // Console output (for testing)
    printf("%s", LogBuffer);

    // File output
    LogFile = CreateFileA(LOG_FILE,
        FILE_APPEND_DATA,
        FILE_SHARE_READ,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (LogFile != INVALID_HANDLE_VALUE) {
        DWORD Written;
        WriteFile(LogFile, LogBuffer, (DWORD)strlen(LogBuffer), &Written, NULL);
        CloseHandle(LogFile);
    }
}

VOID UpdateMetrics(VOID)
{
    HANDLE MetricsFile;
    CHAR Buffer[2048];

    WaitForSingleObject(g_ServiceContext.MetricsLock, INFINITE);

    GetSystemTimeAsFileTime(&g_ServiceContext.Metrics.LastUpdateTime);

    StringCchPrintfA(Buffer, sizeof(Buffer),
        "EventsProcessed: %llu\n"
        "EventsBatched: %llu\n"
        "EventsDropped: %llu\n"
        "UploadFailures: %llu\n"
        "UploadSuccesses: %llu\n"
        "BytesProcessed: %llu\n"
        "AverageLatencyMs: %u\n",
        g_ServiceContext.Metrics.EventsProcessed,
        g_ServiceContext.Metrics.EventsBatched,
        g_ServiceContext.Metrics.EventsDropped,
        g_ServiceContext.Metrics.UploadFailures,
        g_ServiceContext.Metrics.UploadSuccesses,
        g_ServiceContext.Metrics.BytesProcessed,
        g_ServiceContext.Metrics.AverageLatencyMs);

    MetricsFile = CreateFileA(METRICS_FILE,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (MetricsFile != INVALID_HANDLE_VALUE) {
        DWORD Written;
        WriteFile(MetricsFile, Buffer, (DWORD)strlen(Buffer), &Written, NULL);
        CloseHandle(MetricsFile);
    }

    ReleaseMutex(g_ServiceContext.MetricsLock);
}

// ============================================================================
// CONFIGURATION MANAGEMENT
// ============================================================================

BOOL LoadConfigFromFile(VOID)
{
    // Default configuration
    g_ServiceContext.Config.TrackProcesses = TRUE;
    g_ServiceContext.Config.TrackNetwork = TRUE;
    g_ServiceContext.Config.TrackDns = TRUE;
    g_ServiceContext.Config.TrackCrossProcess = TRUE;
    g_ServiceContext.Config.EnableUpload = FALSE;
    g_ServiceContext.Config.EnableLocalStorage = TRUE;
    g_ServiceContext.Config.MaxQueueSize = 10000;
    g_ServiceContext.Config.BatchSize = BATCH_SIZE;
    g_ServiceContext.Config.BatchTimeoutMs = BATCH_TIMEOUT_MS;
    g_ServiceContext.Config.LogRotationSizeMB = 100;
    StringCchCopyA(g_ServiceContext.Config.S3Bucket, sizeof(g_ServiceContext.Config.S3Bucket), "");

    // TODO: Implement INI file parsing
    LogEvent("INFO", "Configuration loaded with defaults (custom config: not implemented yet)");
    return TRUE;
}

BOOL ValidateConfiguration(VOID)
{
    if (g_ServiceContext.Config.BatchSize == 0 ||
        g_ServiceContext.Config.BatchSize > BATCH_SIZE) {
        LogEvent("ERROR", "Invalid batch size: %u", g_ServiceContext.Config.BatchSize);
        return FALSE;
    }

    if (g_ServiceContext.Config.BatchTimeoutMs == 0) {
        LogEvent("ERROR", "Invalid batch timeout");
        return FALSE;
    }

    if (g_ServiceContext.Config.MaxQueueSize == 0) {
        LogEvent("ERROR", "Invalid max queue size");
        return FALSE;
    }

    return TRUE;
}

// ============================================================================
// DRIVER COMMUNICATION
// ============================================================================

BOOL ConnectToDriver(VOID)
{
    g_ServiceContext.DriverHandle = CreateFileA(
        EDR_USER_DEVICE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (g_ServiceContext.DriverHandle == INVALID_HANDLE_VALUE) {
        LogEvent("ERROR", "Failed to create file handle to driver. Error: %lu", GetLastError());
        return FALSE;
    }

    LogEvent("INFO", "Successfully connected to EDR driver");
    return TRUE;
}

VOID DisconnectFromDriver(VOID)
{
    if (g_ServiceContext.DriverHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_ServiceContext.DriverHandle);
        g_ServiceContext.DriverHandle = INVALID_HANDLE_VALUE;
        LogEvent("INFO", "Disconnected from EDR driver");
    }
}

// ============================================================================
// EVENT COLLECTION AND VALIDATION
// ============================================================================

BOOL ValidateEvent(CONST EDR_EVENT* Event)
{
    if (Event == NULL) {
        return FALSE;
    }

    // Validate event type
    if (Event->EventType < EventTypeProcessCreate || Event->EventType > EventTypeDnsQuery) {
        LogEvent("WARN", "Invalid event type: %d", Event->EventType);
        return FALSE;
    }

    // Validate timestamp
    if (Event->ProcessEvent.Timestamp.QuadPart == 0) {
        LogEvent("WARN", "Event has zero timestamp");
        return FALSE;
    }

    return TRUE;
}

BOOL CollectEvents(EVENT_BATCH* Batch)
{
    if (Batch->Count >= g_ServiceContext.Config.BatchSize) {
        return FALSE;
    }

    EDR_EVENT Event = { 0 };
    DWORD BytesReturned = 0;

    BOOL Result = DeviceIoControl(
        g_ServiceContext.DriverHandle,
        IOCTL_EDR_GET_EVENT,
        NULL,
        0,
        &Event,
        sizeof(EDR_EVENT),
        &BytesReturned,
        NULL);

    if (!Result) {
        DWORD Error = GetLastError();
        if (Error != ERROR_NO_MORE_ITEMS && Error != ERROR_TIMEOUT) {
            LogEvent("WARN", "DeviceIoControl failed: %lu", Error);
        }
        return FALSE;
    }

    if (BytesReturned != sizeof(EDR_EVENT)) {
        LogEvent("WARN", "Invalid bytes returned from driver: %lu", BytesReturned);
        return FALSE;
    }

    if (!ValidateEvent(&Event)) {
        InterlockedIncrement64((LONG64*)&g_ServiceContext.Metrics.EventsDropped);
        return FALSE;
    }

    memcpy(&Batch->Events[Batch->Count], &Event, sizeof(EDR_EVENT));
    Batch->Count++;
    InterlockedIncrement64((LONG64*)&g_ServiceContext.Metrics.EventsProcessed);

    return TRUE;
}

// ============================================================================
// EVENT SERIALIZATION
// ============================================================================

BOOL EventToJsonSafe(CONST EDR_EVENT* Event, CHAR* Buffer, DWORD BufferSize)
{
    if (!Event || !Buffer || BufferSize < 256) {
        return FALSE;
    }

    HRESULT hr = E_FAIL;

    switch (Event->EventType) {
    case EventTypeProcessCreate: {
        CONST EDR_PROCESS_EVENT* pe = &Event->ProcessEvent;
        hr = StringCchPrintfA(Buffer, BufferSize,
            "{\"type\":\"process_create\",\"timestamp\":%lld,\"pid\":%u,\"ppid\":%u,"
            "\"image\":\"%S\",\"parent_image\":\"%S\",\"cmdline\":\"%S\","
            "\"has_cert\":%s,\"cert_subject\":\"%S\"}",
            pe->Timestamp.QuadPart,
            pe->ProcessId,
            pe->ParentProcessId,
            pe->ImagePath,
            pe->ParentImagePath,
            pe->CommandLine,
            pe->HasCertificate ? "true" : "false",
            pe->CertSubject);
        break;
    }

    case EventTypeNetworkTCP:
    case EventTypeNetworkUDP: {
        CONST EDR_NETWORK_EVENT* ne = &Event->NetworkEvent;
        hr = StringCchPrintfA(Buffer, BufferSize,
            "{\"type\":\"%s\",\"timestamp\":%lld,\"pid\":%u,\"direction\":\"%s\","
            "\"local_addr\":\"%u.%u.%u.%u\",\"local_port\":%u,"
            "\"remote_addr\":\"%u.%u.%u.%u\",\"remote_port\":%u,\"process\":\"%S\"}",
            ne->Protocol == 6 ? "network_tcp" : "network_udp",
            ne->Timestamp.QuadPart,
            ne->ProcessId,
            ne->Direction == DirectionInbound ? "inbound" : "outbound",
            (ne->LocalAddress >> 24) & 0xFF, (ne->LocalAddress >> 16) & 0xFF,
            (ne->LocalAddress >> 8) & 0xFF, ne->LocalAddress & 0xFF,
            ne->LocalPort,
            (ne->RemoteAddress >> 24) & 0xFF, (ne->RemoteAddress >> 16) & 0xFF,
            (ne->RemoteAddress >> 8) & 0xFF, ne->RemoteAddress & 0xFF,
            ne->RemotePort,
            ne->ProcessPath);
        break;
    }

    case EventTypeDnsQuery: {
        CONST EDR_DNS_EVENT* de = &Event->DnsEvent;
        hr = StringCchPrintfA(Buffer, BufferSize,
            "{\"type\":\"dns_query\",\"timestamp\":%lld,\"pid\":%u,"
            "\"query\":\"%S\",\"query_type\":%u,\"process\":\"%S\"}",
            de->Timestamp.QuadPart,
            de->ProcessId,
            de->QueryName,
            de->QueryType,
            de->ProcessPath);
        break;
    }

    case EventTypeCrossProcess: {
        CONST EDR_CROSSPROCESS_EVENT* ce = &Event->CrossProcessEvent;
        hr = StringCchPrintfA(Buffer, BufferSize,
            "{\"type\":\"cross_process\",\"timestamp\":%lld,\"source_pid\":%u,\"target_pid\":%u,"
            "\"base_addr\":\"0x%p\",\"size\":%zu,\"protection\":0x%X,"
            "\"source_process\":\"%S\",\"target_process\":\"%S\"}",
            ce->Timestamp.QuadPart,
            ce->SourceProcessId,
            ce->TargetProcessId,
            ce->BaseAddress,
            ce->RegionSize,
            ce->Protection,
            ce->SourceProcessPath,
            ce->TargetProcessPath);
        break;
    }

    default:
        LogEvent("WARN", "Unknown event type: %d", Event->EventType);
        return FALSE;
    }

    return SUCCEEDED(hr);
}

// ============================================================================
// STORAGE
// ============================================================================

BOOL EnsureStorageDirectory(VOID)
{
    if (!CreateDirectoryA(LOG_DIR, NULL)) {
        DWORD Error = GetLastError();
        if (Error != ERROR_ALREADY_EXISTS) {
            LogEvent("ERROR", "Failed to create storage directory. Error: %lu", Error);
            return FALSE;
        }
    }
    return TRUE;
}

BOOL WriteToLocalStorage(CONST EVENT_BATCH* Batch)
{
    if (!g_ServiceContext.Config.EnableLocalStorage || Batch->Count == 0) {
        return TRUE;
    }

    if (!EnsureStorageDirectory()) {
        return FALSE;
    }

    HANDLE LogFile = CreateFileA(LOG_FILE,
        FILE_APPEND_DATA,
        FILE_SHARE_READ,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (LogFile == INVALID_HANDLE_VALUE) {
        LogEvent("ERROR", "Failed to open log file for writing. Error: %lu", GetLastError());
        return FALSE;
    }

    CHAR JsonBuffer[8192];
    DWORD Written = 0;
    BOOL Success = TRUE;

    for (DWORD i = 0; i < Batch->Count; i++) {
        if (EventToJsonSafe(&Batch->Events[i], JsonBuffer, sizeof(JsonBuffer))) {
            DWORD LineLen = (DWORD)strlen(JsonBuffer);
            CHAR LineBuffer[8256];

            StringCchPrintfA(LineBuffer, sizeof(LineBuffer), "%s\n", JsonBuffer);

            if (!WriteFile(LogFile, LineBuffer, (DWORD)strlen(LineBuffer), &Written, NULL)) {
                LogEvent("ERROR", "Failed to write event to log file. Error: %lu", GetLastError());
                Success = FALSE;
                break;
            }

            InterlockedAdd64((LONG64*)&g_ServiceContext.Metrics.BytesProcessed, Written);
        }
        else {
            LogEvent("WARN", "Failed to serialize event %u", i);
            InterlockedIncrement64((LONG64*)&g_ServiceContext.Metrics.EventsDropped);
        }
    }

    CloseHandle(LogFile);
    return Success;
}

BOOL UploadToS3(CONST EVENT_BATCH* Batch)
{
    if (!g_ServiceContext.Config.EnableUpload || Batch->Count == 0) {
        return TRUE;
    }

    // This is a placeholder - implement AWS SDK integration
    // For now, save batch to file as a staging area
    CHAR Filename[256];
    time_t Now = time(NULL);

    StringCchPrintfA(Filename, sizeof(Filename),
        "%s\\batch_%lld.json", LOG_DIR, (long long)Now);

    HANDLE BatchFile = CreateFileA(Filename,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (BatchFile == INVALID_HANDLE_VALUE) {
        InterlockedIncrement64((LONG64*)&g_ServiceContext.Metrics.UploadFailures);
        LogEvent("ERROR", "Failed to create batch file: %s", Filename);
        return FALSE;
    }

    CHAR JsonBuffer[8192];
    DWORD Written = 0;
    const CHAR OpenBracket[] = "[\n";
    const CHAR CloseBracket[] = "\n]\n";
    const CHAR Comma[] = ",\n";
    const CHAR Newline[] = "\n";

    WriteFile(BatchFile, OpenBracket, (DWORD)strlen(OpenBracket), &Written, NULL);

    for (DWORD i = 0; i < Batch->Count; i++) {
        if (EventToJsonSafe(&Batch->Events[i], JsonBuffer, sizeof(JsonBuffer))) {
            WriteFile(BatchFile, "  ", 2, &Written, NULL);
            WriteFile(BatchFile, JsonBuffer, (DWORD)strlen(JsonBuffer), &Written, NULL);

            if (i < Batch->Count - 1) {
                WriteFile(BatchFile, Comma, (DWORD)strlen(Comma), &Written, NULL);
            }
            else {
                WriteFile(BatchFile, Newline, (DWORD)strlen(Newline), &Written, NULL);
            }
        }
    }

    WriteFile(BatchFile, CloseBracket, (DWORD)strlen(CloseBracket), &Written, NULL);
    CloseHandle(BatchFile);

    InterlockedIncrement64((LONG64*)&g_ServiceContext.Metrics.UploadSuccesses);
    LogEvent("INFO", "Batch uploaded: %s", Filename);
    return TRUE;
}

// ============================================================================
// BATCH PROCESSING
// ============================================================================

BOOL ProcessBatch(EVENT_BATCH* Batch)
{
    if (Batch->Count == 0) {
        return TRUE;
    }

    DWORD StartTick = GetTickCount();

    LogEvent("INFO", "Processing batch of %u events", Batch->Count);

    InterlockedAdd64((LONG64*)&g_ServiceContext.Metrics.EventsBatched, Batch->Count);

    // Write to local storage
    BOOL LocalSuccess = WriteToLocalStorage(Batch);
    if (!LocalSuccess) {
        LogEvent("ERROR", "Failed to write batch to local storage");
        if (Batch->RetryCount < MAX_BATCH_RETRIES) {
            Batch->RetryCount++;
            Sleep(UPLOAD_RETRY_DELAY_MS);
            return ProcessBatch(Batch);
        }
    }

    // Upload to S3
    BOOL UploadSuccess = UploadToS3(Batch);
    if (!UploadSuccess) {
        LogEvent("WARN", "Failed to upload batch to S3");
    }

    DWORD ElapsedMs = GetTickCount() - StartTick;
    LogEvent("INFO", "Batch processing completed in %u ms", ElapsedMs);

    return LocalSuccess;
}

// ============================================================================
// SERVICE CONTROL
// ============================================================================

VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode)
{
    switch (CtrlCode) {
    case SERVICE_CONTROL_STOP:
        if (g_ServiceContext.ServiceStatus.dwCurrentState != SERVICE_RUNNING) {
            break;
        }

        g_ServiceContext.ServiceStatus.dwControlsAccepted = 0;
        g_ServiceContext.ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        g_ServiceContext.ServiceStatus.dwWin32ExitCode = 0;
        g_ServiceContext.ServiceStatus.dwCheckPoint = 4;

        SetServiceStatus(g_ServiceContext.StatusHandle, &g_ServiceContext.ServiceStatus);
        SetEvent(g_ServiceContext.StopEvent);
        LogEvent("INFO", "Service stop requested");
        break;

    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(g_ServiceContext.StatusHandle, &g_ServiceContext.ServiceStatus);
        break;

    default:
        break;
    }
}

DWORD WINAPI ServiceWorkerThread(LPVOID lpParam)
{
    UNREFERENCED_PARAMETER(lpParam);

    LogEvent("INFO", "Service worker thread started");

    if (!ConnectToDriver()) {
        LogEvent("ERROR", "Failed to connect to driver at startup");
        return ERROR_INVALID_HANDLE;
    }

    EVENT_BATCH Batch = { 0 };
    Batch.StartTime = GetTickCount();

    while (WaitForSingleObject(g_ServiceContext.StopEvent, 0) != WAIT_OBJECT_0) {
        // Collect events
        BOOL EventCollected = FALSE;
        while (CollectEvents(&Batch)) {
            EventCollected = TRUE;
        }

        DWORD Elapsed = GetTickCount() - Batch.StartTime;

        // Process batch if full or timeout reached
        if (Batch.Count >= g_ServiceContext.Config.BatchSize ||
            (Batch.Count > 0 && Elapsed > g_ServiceContext.Config.BatchTimeoutMs)) {
            ProcessBatch(&Batch);
            memset(&Batch, 0, sizeof(Batch));
            Batch.StartTime = GetTickCount();
        }

        // Update metrics periodically
        static DWORD LastMetricsUpdate = 0;
        DWORD CurrentTick = GetTickCount();
        if (CurrentTick - LastMetricsUpdate > 60000) {  // Update every 60 seconds
            UpdateMetrics();
            LastMetricsUpdate = CurrentTick;
        }

        Sleep(100);
    }

    // Process any remaining events
    if (Batch.Count > 0) {
        ProcessBatch(&Batch);
    }

    DisconnectFromDriver();
    UpdateMetrics();

    LogEvent("INFO", "Service worker thread ending");
    return ERROR_SUCCESS;
}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    LogEvent("INFO", "ServiceMain called for MinimalEDR Service v%s", EDR_SERVICE_VERSION);

    g_ServiceContext.StatusHandle = RegisterServiceCtrlHandler(
        (LPSTR)"MinimalEDRService", ServiceCtrlHandler);

    if (g_ServiceContext.StatusHandle == NULL) {
        LogEvent("ERROR", "RegisterServiceCtrlHandler failed");
        return;
    }

    ZeroMemory(&g_ServiceContext.ServiceStatus, sizeof(g_ServiceContext.ServiceStatus));
    g_ServiceContext.ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceContext.ServiceStatus.dwControlsAccepted = 0;
    g_ServiceContext.ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceContext.ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceContext.ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceContext.ServiceStatus.dwCheckPoint = 0;

    if (!SetServiceStatus(g_ServiceContext.StatusHandle, &g_ServiceContext.ServiceStatus)) {
        LogEvent("ERROR", "SetServiceStatus failed");
        return;
    }

    // Load configuration
    if (!LoadConfigFromFile() || !ValidateConfiguration()) {
        LogEvent("ERROR", "Configuration loading failed");
        g_ServiceContext.ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceContext.ServiceStatus.dwWin32ExitCode = ERROR_INVALID_PARAMETER;
        SetServiceStatus(g_ServiceContext.StatusHandle, &g_ServiceContext.ServiceStatus);
        return;
    }

    // Create stop event
    g_ServiceContext.StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_ServiceContext.StopEvent == NULL) {
        LogEvent("ERROR", "CreateEvent failed");
        g_ServiceContext.ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceContext.ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_ServiceContext.StatusHandle, &g_ServiceContext.ServiceStatus);
        return;
    }

    // Create metrics lock
    g_ServiceContext.MetricsLock = CreateMutex(NULL, FALSE, NULL);
    if (g_ServiceContext.MetricsLock == NULL) {
        LogEvent("ERROR", "CreateMutex failed");
        CloseHandle(g_ServiceContext.StopEvent);
        g_ServiceContext.ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceContext.ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_ServiceContext.StatusHandle, &g_ServiceContext.ServiceStatus);
        return;
    }

    // Service running
    g_ServiceContext.ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    g_ServiceContext.ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceContext.ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceContext.ServiceStatus.dwCheckPoint = 0;

    if (!SetServiceStatus(g_ServiceContext.StatusHandle, &g_ServiceContext.ServiceStatus)) {
        LogEvent("ERROR", "SetServiceStatus failed at running state");
        CloseHandle(g_ServiceContext.StopEvent);
        CloseHandle(g_ServiceContext.MetricsLock);
        return;
    }

    LogEvent("INFO", "Service successfully started");

    // Start worker thread
    HANDLE hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);
    if (hThread == NULL) {
        LogEvent("ERROR", "CreateThread failed");
        g_ServiceContext.ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceContext.ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_ServiceContext.StatusHandle, &g_ServiceContext.ServiceStatus);
        CloseHandle(g_ServiceContext.StopEvent);
        CloseHandle(g_ServiceContext.MetricsLock);
        return;
    }

    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
    CloseHandle(g_ServiceContext.StopEvent);
    CloseHandle(g_ServiceContext.MetricsLock);

    g_ServiceContext.ServiceStatus.dwControlsAccepted = 0;
    g_ServiceContext.ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    g_ServiceContext.ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceContext.ServiceStatus.dwCheckPoint = 3;

    SetServiceStatus(g_ServiceContext.StatusHandle, &g_ServiceContext.ServiceStatus);

    LogEvent("INFO", "Service stopped");
}

// ============================================================================
// ENTRY POINT
// ============================================================================

int main(int argc, char* argv[])
{
    if (argc > 1 && strcmp(argv[1], "--console") == 0) {
        // Console mode for testing
        printf("MinimalEDR Service v%s - Console Mode\n", EDR_SERVICE_VERSION);
        printf("Connecting to driver...\n");

        ZeroMemory(&g_ServiceContext, sizeof(g_ServiceContext));

        if (!EnsureStorageDirectory()) {
            printf("Failed to create storage directory\n");
            return 1;
        }

        if (!LoadConfigFromFile() || !ValidateConfiguration()) {
            printf("Failed to load configuration\n");
            return 1;
        }

        g_ServiceContext.MetricsLock = CreateMutex(NULL, FALSE, NULL);
        if (g_ServiceContext.MetricsLock == NULL) {
            printf("Failed to create metrics lock\n");
            return 1;
        }

        if (!ConnectToDriver()) {
            printf("Failed to connect to driver\n");
            CloseHandle(g_ServiceContext.MetricsLock);
            return 1;
        }

        printf("Connected. Collecting events (press Ctrl+C to stop)...\n");

        g_ServiceContext.StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

        EVENT_BATCH Batch = { 0 };
        Batch.StartTime = GetTickCount();

        while (TRUE) {
            if (CollectEvents(&Batch)) {
                printf("Event collected. Batch: %u/%u\n", Batch.Count, g_ServiceContext.Config.BatchSize);

                DWORD Elapsed = GetTickCount() - Batch.StartTime;
                if (Batch.Count >= g_ServiceContext.Config.BatchSize ||
                    (Batch.Count > 0 && Elapsed > g_ServiceContext.Config.BatchTimeoutMs)) {
                    ProcessBatch(&Batch);
                    memset(&Batch, 0, sizeof(Batch));
                    Batch.StartTime = GetTickCount();
                }
            }
            Sleep(100);
        }

        DisconnectFromDriver();
        CloseHandle(g_ServiceContext.MetricsLock);
        if (g_ServiceContext.StopEvent != INVALID_HANDLE_VALUE) {
            CloseHandle(g_ServiceContext.StopEvent);
        }
        return 0;
    }

    // Service mode
    SERVICE_TABLE_ENTRY ServiceTable[] = {
        { (LPSTR)"MinimalEDRService", (LPSERVICE_MAIN_FUNCTION)ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcher(ServiceTable)) {
        printf("StartServiceCtrlDispatcher failed: %lu\n", GetLastError());
        return GetLastError();
    }

    return 0;
}
