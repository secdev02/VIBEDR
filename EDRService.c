#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <time.h>
#include <wininet.h>
#include "../Common/EDRCommon.h"

#pragma comment(lib, "wininet.lib")

// Service globals
SERVICE_STATUS g_ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;
HANDLE g_DriverHandle = INVALID_HANDLE_VALUE;

// Batch configuration
#define BATCH_SIZE 100
#define BATCH_TIMEOUT_MS 30000  // 30 seconds
#define LOG_FILE_PATH "C:\\ProgramData\\MinimalEDR\\events.log"
#define UPLOAD_ENABLED TRUE

typedef struct _EVENT_BATCH {
    EDR_EVENT Events[BATCH_SIZE];
    DWORD Count;
    DWORD StartTime;
} EVENT_BATCH;

// Forward declarations
VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv);
VOID WINAPI ServiceCtrlHandler(DWORD);
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam);
BOOL ConnectToDriver();
VOID DisconnectFromDriver();
BOOL CollectEvents(EVENT_BATCH* batch);
BOOL ProcessBatch(EVENT_BATCH* batch);
BOOL WriteToLocalStorage(EVENT_BATCH* batch);
BOOL UploadToS3(EVENT_BATCH* batch);
VOID EventToJson(EDR_EVENT* event, CHAR* buffer, DWORD bufferSize);

// Service entry point
int main(int argc, char* argv[])
{
    if (argc > 1 && strcmp(argv[1], "--console") == 0) {
        // Console mode for testing
        printf("MinimalEDR Service - Console Mode\n");
        printf("Connecting to driver...\n");
        
        if (!ConnectToDriver()) {
            printf("Failed to connect to driver\n");
            return 1;
        }

        printf("Connected. Collecting events (press Ctrl+C to stop)...\n");
        
        EVENT_BATCH batch = { 0 };
        batch.StartTime = GetTickCount();

        while (TRUE) {
            if (CollectEvents(&batch)) {
                if (batch.Count >= BATCH_SIZE || 
                    (GetTickCount() - batch.StartTime) > BATCH_TIMEOUT_MS) {
                    ProcessBatch(&batch);
                    memset(&batch, 0, sizeof(batch));
                    batch.StartTime = GetTickCount();
                }
            }
            Sleep(100);
        }

        DisconnectFromDriver();
        return 0;
    }

    // Service mode
    SERVICE_TABLE_ENTRY ServiceTable[] = {
        { (LPSTR)"MinimalEDRService", (LPSERVICE_MAIN_FUNCTION)ServiceMain },
        { NULL, NULL }
    };

    if (StartServiceCtrlDispatcher(ServiceTable) == FALSE) {
        return GetLastError();
    }

    return 0;
}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv)
{
    DWORD Status = E_FAIL;

    g_StatusHandle = RegisterServiceCtrlHandler((LPSTR)"MinimalEDRService", ServiceCtrlHandler);
    if (g_StatusHandle == NULL) {
        return;
    }

    ZeroMemory(&g_ServiceStatus, sizeof(g_ServiceStatus));
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;

    if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
        return;
    }

    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_ServiceStopEvent == NULL) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;

    if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE) {
        return;
    }

    HANDLE hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);

    WaitForSingleObject(hThread, INFINITE);

    CloseHandle(g_ServiceStopEvent);

    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 3;

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode)
{
    switch (CtrlCode) {
    case SERVICE_CONTROL_STOP:
        if (g_ServiceStatus.dwCurrentState != SERVICE_RUNNING)
            break;

        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        g_ServiceStatus.dwWin32ExitCode = 0;
        g_ServiceStatus.dwCheckPoint = 4;

        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        SetEvent(g_ServiceStopEvent);
        break;

    default:
        break;
    }
}

DWORD WINAPI ServiceWorkerThread(LPVOID lpParam)
{
    if (!ConnectToDriver()) {
        return ERROR_INVALID_HANDLE;
    }

    EVENT_BATCH batch = { 0 };
    batch.StartTime = GetTickCount();

    while (WaitForSingleObject(g_ServiceStopEvent, 0) != WAIT_OBJECT_0) {
        if (CollectEvents(&batch)) {
            DWORD elapsed = GetTickCount() - batch.StartTime;
            
            if (batch.Count >= BATCH_SIZE || elapsed > BATCH_TIMEOUT_MS) {
                ProcessBatch(&batch);
                memset(&batch, 0, sizeof(batch));
                batch.StartTime = GetTickCount();
            }
        }
        Sleep(100);
    }

    // Process remaining events
    if (batch.Count > 0) {
        ProcessBatch(&batch);
    }

    DisconnectFromDriver();
    return ERROR_SUCCESS;
}

BOOL ConnectToDriver()
{
    g_DriverHandle = CreateFile(
        EDR_USER_DEVICE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    return g_DriverHandle != INVALID_HANDLE_VALUE;
}

VOID DisconnectFromDriver()
{
    if (g_DriverHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_DriverHandle);
        g_DriverHandle = INVALID_HANDLE_VALUE;
    }
}

BOOL CollectEvents(EVENT_BATCH* batch)
{
    if (batch->Count >= BATCH_SIZE) {
        return FALSE;
    }

    EDR_EVENT event;
    DWORD bytesReturned;

    BOOL result = DeviceIoControl(
        g_DriverHandle,
        IOCTL_EDR_GET_EVENT,
        NULL,
        0,
        &event,
        sizeof(EDR_EVENT),
        &bytesReturned,
        NULL
    );

    if (result && bytesReturned == sizeof(EDR_EVENT)) {
        memcpy(&batch->Events[batch->Count], &event, sizeof(EDR_EVENT));
        batch->Count++;
        return TRUE;
    }

    return FALSE;
}

BOOL ProcessBatch(EVENT_BATCH* batch)
{
    if (batch->Count == 0) {
        return TRUE;
    }

    printf("Processing batch of %d events\n", batch->Count);

    // Write to local storage first
    BOOL success = WriteToLocalStorage(batch);

    // Optionally upload to S3
    if (UPLOAD_ENABLED) {
        UploadToS3(batch);
    }

    return success;
}

BOOL WriteToLocalStorage(EVENT_BATCH* batch)
{
    // Ensure directory exists
    CreateDirectoryA("C:\\ProgramData\\MinimalEDR", NULL);

    FILE* file = fopen(LOG_FILE_PATH, "a");
    if (!file) {
        return FALSE;
    }

    CHAR jsonBuffer[8192];
    
    for (DWORD i = 0; i < batch->Count; i++) {
        EventToJson(&batch->Events[i], jsonBuffer, sizeof(jsonBuffer));
        fprintf(file, "%s\n", jsonBuffer);
    }

    fclose(file);
    return TRUE;
}

BOOL UploadToS3(EVENT_BATCH* batch)
{
    // This is a placeholder for S3 upload functionality
    // In production, you would:
    // 1. Use AWS SDK or REST API
    // 2. Batch events into JSON
    // 3. Upload with proper credentials
    // 4. Handle retries and errors

    CHAR url[512];
    time_t now = time(NULL);
    sprintf(url, "https://your-bucket.s3.amazonaws.com/edr-events/%lld.json", (long long)now);

    // For demonstration, we'll just show how to prepare the data
    CHAR* jsonData = (CHAR*)malloc(batch->Count * 4096);
    if (!jsonData) {
        return FALSE;
    }

    strcpy(jsonData, "[\n");
    CHAR eventJson[8192];
    
    for (DWORD i = 0; i < batch->Count; i++) {
        EventToJson(&batch->Events[i], eventJson, sizeof(eventJson));
        strcat(jsonData, "  ");
        strcat(jsonData, eventJson);
        if (i < batch->Count - 1) {
            strcat(jsonData, ",\n");
        } else {
            strcat(jsonData, "\n");
        }
    }
    strcat(jsonData, "]\n");

    // In production, use AWS SDK or WinHTTP to upload
    // For now, just save to file as demonstration
    CHAR filename[256];
    sprintf(filename, "C:\\ProgramData\\MinimalEDR\\batch_%lld.json", (long long)now);
    FILE* file = fopen(filename, "w");
    if (file) {
        fwrite(jsonData, 1, strlen(jsonData), file);
        fclose(file);
    }

    free(jsonData);
    return TRUE;
}

VOID EventToJson(EDR_EVENT* event, CHAR* buffer, DWORD bufferSize)
{
    buffer[0] = '\0';
    
    switch (event->EventType) {
        case EventTypeProcessCreate: {
            EDR_PROCESS_EVENT* pe = &event->ProcessEvent;
            sprintf(buffer,
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
                pe->CertSubject
            );
            break;
        }
        
        case EventTypeNetworkTCP:
        case EventTypeNetworkUDP: {
            EDR_NETWORK_EVENT* ne = &event->NetworkEvent;
            sprintf(buffer,
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
                ne->ProcessPath
            );
            break;
        }
        
        case EventTypeDnsQuery: {
            EDR_DNS_EVENT* de = &event->DnsEvent;
            sprintf(buffer,
                "{\"type\":\"dns_query\",\"timestamp\":%lld,\"pid\":%u,"
                "\"query\":\"%S\",\"query_type\":%u,\"process\":\"%S\"}",
                de->Timestamp.QuadPart,
                de->ProcessId,
                de->QueryName,
                de->QueryType,
                de->ProcessPath
            );
            break;
        }
        
        case EventTypeCrossProcess: {
            EDR_CROSSPROCESS_EVENT* ce = &event->CrossProcessEvent;
            sprintf(buffer,
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
                ce->TargetProcessPath
            );
            break;
        }
    }
}
