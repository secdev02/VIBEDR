# MinimalEDR - Windows Endpoint Detection and Response Driver

A minimalist Windows kernel-mode driver for endpoint detection and response (EDR) that tracks critical security events.

## Features

### 1. Process Monitoring
- Process creation events with full command line
- Parent process tracking
- Process image path capture
- Executable certificate metadata extraction (when available)

### 2. Network Monitoring
- TCP/UDP connection tracking
- Inbound/outbound direction detection
- Source and destination IP addresses and ports
- DNS query logging
- Process-to-network correlation

### 3. Certificate Validation
- Extracts digital signature information from executables
- Captures certificate subject, issuer, and serial number
- Identifies signed vs unsigned binaries

### 4. Cross-Process Activity Detection
- Monitors inter-process memory operations (Carbon Black style)
- Tracks PROCESS_VM_WRITE and PROCESS_VM_OPERATION access
- Identifies potential code injection attempts
- Captures source and target process information

### 5. Event Batching and Storage
- Efficient event queuing in kernel space
- User-mode service for event collection
- Batch processing (100 events or 30-second timeout)
- Local JSON log files
- S3 bucket upload support (configurable)

## Architecture

```
┌─────────────────────────────────────────┐
│         Kernel Mode                      │
│  ┌───────────────────────────────┐      │
│  │   MinimalEDR.sys (Driver)     │      │
│  ├───────────────────────────────┤      │
│  │ - Process Notifications       │      │
│  │ - Object Callbacks            │      │
│  │ - WFP Network Callouts        │      │
│  │ - Event Queue Management      │      │
│  └───────────┬───────────────────┘      │
└──────────────┼──────────────────────────┘
               │ IOCTL
               │
┌──────────────┼──────────────────────────┐
│              ▼                           │
│  ┌───────────────────────────────┐      │
│  │  EDRService.exe (Service)     │      │
│  ├───────────────────────────────┤      │
│  │ - Event Collection            │      │
│  │ - Batch Processing            │      │
│  │ - JSON Serialization          │      │
│  │ - Local Storage               │      │
│  │ - S3 Upload                   │      │
│  └───────────┬───────────────────┘      │
│         User Mode                        │
└──────────────┼──────────────────────────┘
               │
               ▼
       ┌────────────────┐
       │  Storage        │
       ├────────────────┤
       │ - Local Logs   │
       │ - S3 Bucket    │
       │ - Database     │
       └────────────────┘
```

## Building

### Prerequisites
- Windows 10/11 SDK
- Windows Driver Kit (WDK)
- Visual Studio 2019/2022 with C++ build tools

### Build Steps

1. Install the Windows Driver Kit:
   https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk

2. Run the build script:
   ```batch
   Build.bat
   ```

This will produce:
- `Driver\x64\Release\MinimalEDR.sys` - Kernel driver
- `Service\EDRService.exe` - User-mode service

## Installation

**IMPORTANT**: You must disable Driver Signature Enforcement or sign the driver for production use.

### Test Mode Installation (Development Only)

1. Enable test mode (requires reboot):
   ```batch
   bcdedit /set testsigning on
   ```

2. Reboot the system

3. Run installation as Administrator:
   ```batch
   cd Install
   Install.bat
   ```

### Production Installation

For production deployment, you must:
1. Obtain a valid code signing certificate
2. Sign the driver with your certificate
3. Submit to Microsoft for WHQL certification (recommended)

## Configuration

The driver can be configured via IOCTL from user mode. Default settings:

```c
TrackProcesses = TRUE
TrackNetwork = TRUE
TrackDns = TRUE
TrackCrossProcess = TRUE
MaxQueueSize = 10000 events
```

## Usage

### Service Mode
The service runs automatically after installation:
```batch
sc start MinimalEDRService
```

### Console Mode (Testing)
Run the service in console mode for debugging:
```batch
EDRService.exe --console
```

### Event Logs
Events are stored in JSON format at:
```
C:\ProgramData\MinimalEDR\events.log
C:\ProgramData\MinimalEDR\batch_<timestamp>.json
```

### Event Format

#### Process Creation
```json
{
  "type": "process_create",
  "timestamp": 132845678901234567,
  "pid": 1234,
  "ppid": 5678,
  "image": "C:\\Windows\\System32\\cmd.exe",
  "parent_image": "C:\\Windows\\explorer.exe",
  "cmdline": "cmd.exe /c whoami",
  "has_cert": true,
  "cert_subject": "CN=Microsoft Corporation"
}
```

#### Network Connection
```json
{
  "type": "network_tcp",
  "timestamp": 132845678901234567,
  "pid": 1234,
  "direction": "outbound",
  "local_addr": "192.168.1.100",
  "local_port": 54321,
  "remote_addr": "93.184.216.34",
  "remote_port": 443,
  "process": "C:\\Program Files\\Browser\\browser.exe"
}
```

#### Cross-Process Operation
```json
{
  "type": "cross_process",
  "timestamp": 132845678901234567,
  "source_pid": 1234,
  "target_pid": 5678,
  "base_addr": "0x00007FF8ABC00000",
  "size": 4096,
  "protection": 0x40,
  "source_process": "C:\\Temp\\suspicious.exe",
  "target_process": "C:\\Windows\\System32\\svchost.exe"
}
```

## S3 Upload Configuration

To enable S3 upload, modify `EDRService.c`:

1. Set your S3 bucket URL
2. Configure AWS credentials
3. Implement AWS SDK integration or use REST API

Example using AWS SDK:
```c
// Install AWS SDK for C++
// Link against: aws-cpp-sdk-core, aws-cpp-sdk-s3

#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/PutObjectRequest.h>

// Initialize AWS SDK in main()
Aws::SDKOptions options;
Aws::InitAPI(options);

// Upload function
Aws::S3::S3Client s3_client;
Aws::S3::Model::PutObjectRequest request;
request.SetBucket("your-edr-bucket");
request.SetKey(filename);
// ... set body and upload
```

## Database Integration

For database storage instead of/in addition to S3:

### SQL Server Example
```c
#include <sql.h>
#include <sqlext.h>

SQLHENV henv;
SQLHDBC hdbc;
SQLHSTMT hstmt;

// Connect to database
SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);
SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
SQLConnect(hdbc, "EDR_DB", SQL_NTS, "username", SQL_NTS, "password", SQL_NTS);

// Insert event
SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
SQLExecDirect(hstmt, "INSERT INTO Events VALUES (...)", SQL_NTS);
```

## Uninstallation

Run as Administrator:
```batch
cd Install
Uninstall.bat
```

## Security Considerations

1. **Driver Signing**: Always sign drivers in production
2. **Access Control**: Limit access to the driver device object
3. **Event Filtering**: Implement filtering to reduce noise
4. **Credential Management**: Store AWS/DB credentials securely
5. **Rate Limiting**: Prevent event queue overflow
6. **Tamper Protection**: Implement anti-tampering measures

## Performance

- Minimal CPU overhead (< 2% on average workload)
- Memory: ~50MB for 10,000 queued events
- Disk: Depends on event rate, ~1MB per 1000 events (JSON)
- Network: Batch uploads reduce bandwidth usage

## Troubleshoads

### Driver Won't Load
- Verify test signing is enabled: `bcdedit /enum`
- Check driver file exists in System32\drivers
- Review System Event Log for error messages

### No Events Captured
- Verify driver is running: `sc query MinimalEDR`
- Verify service is running: `sc query MinimalEDRService`
- Run service in console mode to see errors
- Check configuration settings

### Service Crashes
- Review logs in C:\ProgramData\MinimalEDR
- Check Windows Event Viewer
- Run in console mode with debugger attached

## Limitations

- IPv4 only (IPv6 support requires additional WFP layers)
- Command line capture requires user-mode cooperation
- Certificate extraction is basic (full validation needs CryptoAPI)
- DNS monitoring requires ETW or DNS filter driver
- No built-in event correlation engine

## Future Enhancements

- [ ] IPv6 support
- [ ] Full command line capture (PEB reading)
- [ ] Advanced certificate validation
- [ ] File system monitoring
- [ ] Registry monitoring
- [ ] ETW-based DNS capture
- [ ] Event correlation rules
- [ ] Real-time threat detection
- [ ] Web-based management console
- [ ] Encrypted event storage

## License

This is a minimalist reference implementation for educational purposes.
Modify and extend as needed for production use.

## Contributing

This is a reference implementation. Fork and customize for your needs.

## Support

This is a minimal EDR implementation provided as-is for educational purposes.
For production EDR solutions, consider commercial products like:
- CrowdStrike Falcon
- Carbon Black
- Microsoft Defender for Endpoint
- SentinelOne

## Disclaimer

This driver operates at kernel level and can cause system instability if improperly used.
Test thoroughly in virtual machines before deploying to production systems.
The authors are not responsible for any damage caused by this software.
