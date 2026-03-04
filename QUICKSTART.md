# MinimalEDR - Quick Start Guide

## Overview
MinimalEDR is a lightweight Windows kernel-mode driver that monitors critical security events for endpoint detection and response.

## What It Monitors

### 1. Process Activity
- New process creation with full command line
- Parent process tracking
- Digital signature verification
- Certificate metadata extraction

### 2. Network Activity  
- TCP/UDP connections (inbound & outbound)
- Source/destination IPs and ports
- Process-to-network correlation
- DNS queries

### 3. Cross-Process Operations
- Inter-process memory writes (Carbon Black style)
- Process injection detection
- Source and target process identification

### 4. Event Batching
- Efficient kernel-space queuing
- User-mode batch processing (100 events or 30 seconds)
- Local JSON logging
- Optional S3/database upload

## Prerequisites

- Windows 10/11 (x64)
- Administrator privileges
- Test signing enabled (for development) OR signed driver (for production)

## Installation (Development/Testing)

### Step 1: Enable Test Signing
```batch
bcdedit /set testsigning on
```
Reboot your system after this command.

### Step 2: Build the Driver
```batch
Build.bat
```
This requires Visual Studio and Windows Driver Kit (WDK).

### Step 3: Install
```batch
cd Install
Install.bat
```

### Step 4: Verify Installation
```batch
Test.bat
```

## Usage

### View Real-Time Events (PowerShell)
```powershell
# Monitor all events
.\monitor_events.ps1 -Follow

# Monitor only process events
.\monitor_events.ps1 -Follow -Type process

# View last 50 events
.\monitor_events.ps1 -Last 50
```

### Analyze Event Logs (Python)
```bash
python analyze_events.py C:\ProgramData\MinimalEDR\events.log
```

### Console Mode (Debugging)
```batch
EDRService.exe --console
```

## Event Examples

### Process Creation
```json
{
  "type": "process_create",
  "timestamp": 132845678901234567,
  "pid": 4892,
  "ppid": 1024,
  "image": "C:\\Windows\\System32\\cmd.exe",
  "parent_image": "C:\\Windows\\explorer.exe",
  "cmdline": "cmd.exe /c whoami",
  "has_cert": true,
  "cert_subject": "CN=Microsoft Corporation"
}
```

### Network Connection
```json
{
  "type": "network_tcp",
  "timestamp": 132845678901234567,
  "pid": 4892,
  "direction": "outbound",
  "local_addr": "192.168.1.100",
  "local_port": 54321,
  "remote_addr": "93.184.216.34",
  "remote_port": 443,
  "process": "C:\\Program Files\\Firefox\\firefox.exe"
}
```

### Cross-Process Memory Write
```json
{
  "type": "cross_process",
  "timestamp": 132845678901234567,
  "source_pid": 7890,
  "target_pid": 1234,
  "base_addr": "0x00007FF8ABC00000",
  "size": 4096,
  "protection": 64,
  "source_process": "C:\\Temp\\injector.exe",
  "target_process": "C:\\Windows\\System32\\svchost.exe"
}
```

## Common Commands

```batch
REM Start services
sc start MinimalEDR
sc start MinimalEDRService

REM Stop services
sc stop MinimalEDRService
sc stop MinimalEDR

REM Check status
sc query MinimalEDR
sc query MinimalEDRService

REM View logs
type C:\ProgramData\MinimalEDR\events.log

REM Uninstall
cd Install
Uninstall.bat
```

## Configuration

Edit `C:\ProgramData\MinimalEDR\config.ini` to customize:
- Event types to track
- Batch size and timeout
- Storage destinations (local, S3, database)
- Filtering rules
- Performance limits

See `config.ini.example` for all options.

## Storage Options

### Local Files
Events are always logged to:
```
C:\ProgramData\MinimalEDR\events.log
C:\ProgramData\MinimalEDR\batch_<timestamp>.json
```

### Amazon S3
Configure in `config.ini`:
```ini
[S3]
BucketName=your-edr-bucket
Region=us-east-1
EnableS3Upload=true
```

### SQL Server
Configure in `config.ini`:
```ini
[Database]
Type=sqlserver
ConnectionString=Server=localhost;Database=EDR;
EnableDatabase=true
```

## Troubleshooting

### Driver Won't Load
```batch
REM Check test signing
bcdedit /enum | findstr testsigning

REM Should show: testsigning Yes
```

### No Events Captured
```batch
REM Check driver status
sc query MinimalEDR

REM Check service status  
sc query MinimalEDRService

REM Run service in console mode to see errors
EDRService.exe --console
```

### Service Crashes
Check logs:
```
C:\ProgramData\MinimalEDR\service.log
```

View Windows Event Log:
```
Event Viewer → Windows Logs → System
```

## Security Considerations

⚠️ **Important**: This is a development/reference implementation.

For production use:
1. Sign the driver with a valid certificate
2. Submit for WHQL certification
3. Implement proper access controls
4. Encrypt stored credentials
5. Add rate limiting and DoS protection
6. Implement tamper protection

## Performance Impact

- CPU: < 2% on typical workload
- Memory: ~50MB (10,000 queued events)
- Disk: ~1MB per 1,000 events (JSON format)

## Next Steps

1. **Monitor Events**: Run `monitor_events.ps1 -Follow` to see real-time activity
2. **Generate Traffic**: Browse web, run programs to generate events
3. **Analyze Data**: Run `analyze_events.py` to identify patterns
4. **Configure Storage**: Set up S3 or database for centralized logging
5. **Customize**: Modify filtering rules and detection logic

## Support

This is a reference implementation for educational purposes. For production EDR, consider:
- CrowdStrike Falcon
- Carbon Black
- Microsoft Defender for Endpoint
- SentinelOne

## License

Educational/reference implementation. Modify as needed.

## Disclaimer

⚠️ Kernel drivers can cause system instability. Test in VMs first.
The authors are not responsible for any damage caused by this software.
