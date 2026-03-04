# MinimalEDR - Architecture and Data Flow

## System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        KERNEL MODE                               │
│                                                                   │
│  ┌────────────────────────────────────────────────────────┐    │
│  │          Windows Kernel Notifications                   │    │
│  │                                                         │    │
│  │  • PsSetCreateProcessNotifyRoutineEx                    │    │
│  │  • ObRegisterCallbacks (Process Objects)               │    │
│  │  • FwpmCalloutRegister (Network)                        │    │
│  └───────────────────┬────────────────────────────────────┘    │
│                      │                                           │
│                      ▼                                           │
│  ┌────────────────────────────────────────────────────────┐    │
│  │          MinimalEDR.sys Driver                          │    │
│  │                                                         │    │
│  │  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐  │    │
│  │  │   Process    │  │   Network    │  │    Cross    │  │    │
│  │  │  Monitoring  │  │  Monitoring  │  │   Process   │  │    │
│  │  └──────┬───────┘  └──────┬───────┘  └──────┬──────┘  │    │
│  │         │                  │                  │         │    │
│  │         └──────────────────┼──────────────────┘         │    │
│  │                            ▼                            │    │
│  │                  ┌──────────────────┐                   │    │
│  │                  │  Event Queue     │                   │    │
│  │                  │  (Ring Buffer)   │                   │    │
│  │                  │  Max: 10K events │                   │    │
│  │                  └─────────┬────────┘                   │    │
│  │                            │                            │    │
│  │                            │ IOCTL_EDR_GET_EVENT        │    │
│  └────────────────────────────┼────────────────────────────┘    │
│                                │                                 │
└────────────────────────────────┼─────────────────────────────────┘
                                 │
                                 │ DeviceIoControl
                                 │
┌────────────────────────────────┼─────────────────────────────────┐
│                        USER MODE│                                 │
│                                 ▼                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │         EDRService.exe (Windows Service)                  │   │
│  │                                                           │   │
│  │  ┌─────────────────────────────────────────────────┐    │   │
│  │  │  Event Collection Loop                           │    │   │
│  │  │  • Poll driver via IOCTL                         │    │   │
│  │  │  • Batch events (100 or 30s timeout)             │    │   │
│  │  │  • Retry on failures                             │    │   │
│  │  └──────────────────┬───────────────────────────────┘    │   │
│  │                     ▼                                     │   │
│  │  ┌─────────────────────────────────────────────────┐    │   │
│  │  │  Event Processing                                │    │   │
│  │  │  • JSON serialization                            │    │   │
│  │  │  • Data enrichment                               │    │   │
│  │  │  • Filtering & normalization                     │    │   │
│  │  └──────────────────┬───────────────────────────────┘    │   │
│  │                     ▼                                     │   │
│  │  ┌─────────────────────────────────────────────────┐    │   │
│  │  │  Storage Router                                  │    │   │
│  │  │  • Local file system                             │    │   │
│  │  │  • Amazon S3                                     │    │   │
│  │  │  • SQL Database                                  │    │   │
│  │  └──────────────────┬───────────────────────────────┘    │   │
│  └────────────────────────────────────────────────────────────┘   │
│                       │                                           │
└───────────────────────┼───────────────────────────────────────────┘
                        │
         ┌──────────────┼──────────────┐
         │              │              │
         ▼              ▼              ▼
    ┌────────┐    ┌─────────┐   ┌──────────┐
    │ Local  │    │   S3    │   │ Database │
    │  File  │    │ Bucket  │   │  Server  │
    └────────┘    └─────────┘   └──────────┘
```

## Component Details

### Kernel Driver (MinimalEDR.sys)

**Responsibilities:**
- Register with Windows kernel notification mechanisms
- Capture security events with minimal latency
- Queue events in kernel memory
- Serve events to user-mode service via IOCTL
- Operate at IRQL <= DISPATCH_LEVEL

**Key Structures:**
```c
typedef struct _DRIVER_CONTEXT {
    PDEVICE_OBJECT DeviceObject;
    HANDLE FwpmEngineHandle;
    LIST_ENTRY EventQueue;
    KSPIN_LOCK QueueLock;
    KEVENT QueueEvent;
    EDR_CONFIG Config;
} DRIVER_CONTEXT;
```

**Event Flow:**
1. Windows kernel invokes callback
2. Driver collects event data
3. Event queued with spinlock protection
4. User-mode service notified via event object

### User-Mode Service (EDRService.exe)

**Responsibilities:**
- Poll driver for events via IOCTL
- Batch events for efficiency
- Serialize to JSON format
- Manage storage destinations
- Handle errors and retries

**Batch Processing Logic:**
```
while (running) {
    event = GetEventFromDriver()
    batch.Add(event)
    
    if (batch.Count >= 100 OR elapsed > 30s) {
        ProcessBatch(batch)
        batch.Clear()
    }
}
```

## Data Flow

### 1. Process Creation Flow

```
User Launches Process
      ↓
PsSetCreateProcessNotifyRoutineEx callback
      ↓
ProcessNotifyCallback() in driver
      ↓
- Get process image path (SeLocateProcessImageName)
- Get parent process info
- Extract certificate data (future)
- Read command line from PEB (future)
      ↓
QueueEvent() → Add to ring buffer
      ↓
KeSetEvent() → Signal user-mode service
      ↓
Service calls DeviceIoControl(IOCTL_EDR_GET_EVENT)
      ↓
DequeueEvent() → Remove from queue
      ↓
Return event to user-mode
      ↓
JSON serialization
      ↓
Batch aggregation
      ↓
Storage (file/S3/DB)
```

### 2. Network Connection Flow

```
Application calls connect()
      ↓
Windows Filtering Platform (WFP)
      ↓
FWPS_LAYER_ALE_AUTH_CONNECT_V4 layer
      ↓
NetworkCallout() in driver
      ↓
- Extract 5-tuple (proto, src IP, src port, dst IP, dst port)
- Get process ID from metadata
- Determine direction (inbound/outbound)
      ↓
QueueEvent()
      ↓
[Same flow as process events]
```

### 3. Cross-Process Memory Write Flow

```
Process A calls OpenProcess(PROCESS_VM_WRITE, ...)
      ↓
ObRegisterCallbacks hook
      ↓
ObjectPreCallback() in driver
      ↓
Check desired access flags
      ↓
If VM_WRITE or VM_OPERATION:
  - Get source process info
  - Get target process info
  - Record operation
      ↓
QueueEvent()
      ↓
[Same flow as process events]
```

## Event Format Specification

### Process Event
```json
{
  "type": "process_create",
  "timestamp": <FILETIME>,
  "pid": <uint32>,
  "ppid": <uint32>,
  "image": "<full_path>",
  "parent_image": "<full_path>",
  "cmdline": "<command_line>",
  "has_cert": <bool>,
  "cert_subject": "<X.509 subject>",
  "cert_issuer": "<X.509 issuer>",
  "cert_serial": "<hex_string>"
}
```

### Network Event
```json
{
  "type": "network_tcp|network_udp",
  "timestamp": <FILETIME>,
  "pid": <uint32>,
  "direction": "inbound|outbound",
  "local_addr": "<IPv4>",
  "local_port": <uint16>,
  "remote_addr": "<IPv4>",
  "remote_port": <uint16>,
  "process": "<full_path>"
}
```

### Cross-Process Event
```json
{
  "type": "cross_process",
  "timestamp": <FILETIME>,
  "source_pid": <uint32>,
  "target_pid": <uint32>,
  "base_addr": "<hex_address>",
  "size": <size_t>,
  "protection": <uint32>,
  "source_process": "<full_path>",
  "target_process": "<full_path>"
}
```

### DNS Event
```json
{
  "type": "dns_query",
  "timestamp": <FILETIME>,
  "pid": <uint32>,
  "query": "<domain_name>",
  "query_type": <uint32>,
  "process": "<full_path>"
}
```

## Performance Characteristics

### Latency
- Event capture: < 1ms (kernel callback to queue)
- Event retrieval: < 5ms (IOCTL round-trip)
- Batch processing: 100-30000ms (depending on trigger)

### Throughput
- Max event rate: ~10,000 events/second
- Queue capacity: 10,000 events
- Batch size: 100 events
- JSON serialization: ~1ms per event

### Resource Usage
- Driver memory: 5-10 MB (base) + queue
- Service memory: 20-50 MB
- CPU usage: 1-3% (normal workload)
- Disk I/O: Depends on event rate

## Security Model

### Kernel Driver
- Runs at SYSTEM privilege
- Cannot be tampered by user-mode code
- Protected by driver signing
- Access controlled via device ACL

### User-Mode Service
- Runs as LocalSystem
- Exclusive handle to driver device
- Secure credential storage (encrypted)
- Audit logging enabled

### Communication
- IOCTL interface (privileged)
- No network exposure from driver
- Service handles all external comms
- TLS for S3/database uploads

## Scalability

### Vertical Scaling
- Increase queue size (MaxQueueSize config)
- Adjust batch size for throughput
- Multiple worker threads (future)

### Horizontal Scaling
- Deploy to multiple endpoints
- Centralized storage (S3/database)
- Aggregate analysis in SIEM

### Cloud Integration
```
Endpoint 1 ──┐
Endpoint 2 ──┤
Endpoint 3 ──┼──→ S3 Bucket ──→ Lambda ──→ SIEM/Analytics
Endpoint 4 ──┤                     ↓
Endpoint 5 ──┘                  Athena
                                   ↓
                              QuickSight
```

## Extension Points

### Custom Event Types
Add new event types by:
1. Define structure in EDRCommon.h
2. Implement collector in driver
3. Add serialization in service
4. Update analysis scripts

### Custom Storage
Implement storage backends:
```c
BOOL UploadToCustomBackend(EVENT_BATCH* batch) {
    // Your implementation
    // - Format data
    // - Authenticate
    // - Upload
    // - Handle errors
}
```

### Real-Time Analysis
Add analysis in service:
```c
VOID AnalyzeEvent(EDR_EVENT* event) {
    if (IsSuspicious(event)) {
        TriggerAlert(event);
    }
}
```

## Monitoring and Observability

### Driver Metrics
- Events queued
- Events dropped
- Queue depth
- CPU time spent

### Service Metrics
- Events processed
- Batch upload latency
- Upload failures
- Storage lag

### Health Checks
```powershell
# Check driver status
sc query MinimalEDR

# Check service status
sc query MinimalEDRService

# Check event log size
Get-ChildItem C:\ProgramData\MinimalEDR\*.log | Measure-Object -Property Length -Sum

# Check event rate
Get-Content C:\ProgramData\MinimalEDR\events.log | Measure-Object -Line
```

## Deployment Strategies

### Development
1. Enable test signing
2. Install unsigned driver
3. Run service in console mode
4. Monitor with PowerShell script

### Staging
1. Sign driver with test certificate
2. Deploy to test systems
3. Configure centralized logging
4. Run automated tests

### Production
1. Sign driver with production certificate
2. Submit for WHQL (optional but recommended)
3. Deploy via GPO or SCCM
4. Configure monitoring and alerting
5. Implement automated response

## Compliance Considerations

### Data Retention
- Configure retention periods in config
- Implement log rotation
- Archive to cold storage (S3 Glacier)

### PII Handling
- Command lines may contain sensitive data
- Consider redaction rules
- Implement data masking

### Audit Requirements
- Log all administrative actions
- Track configuration changes
- Maintain chain of custody

## Disaster Recovery

### Backup Strategy
- Regular backups of configuration
- Event log backups
- Database replication (if applicable)

### Failover
- Service auto-restart on failure
- Queue persists through service restarts
- Graceful degradation on storage failure

### Recovery Procedures
1. Stop service
2. Restore configuration
3. Clear queue (if needed)
4. Restart service
5. Verify event flow

## Future Enhancements

### Short Term
- [ ] Full command line capture via PEB
- [ ] Certificate validation with CryptoAPI
- [ ] IPv6 support
- [ ] ETW-based DNS monitoring

### Medium Term
- [ ] File system monitoring (minifilter)
- [ ] Registry monitoring
- [ ] Image load monitoring
- [ ] Thread creation tracking

### Long Term
- [ ] Machine learning threat detection
- [ ] Automated incident response
- [ ] Integration with SOAR platforms
- [ ] Distributed tracing correlation
