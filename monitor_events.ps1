# MinimalEDR Event Monitor
# PowerShell script to query and display events in real-time

param(
    [Parameter()]
    [string]$LogPath = "C:\ProgramData\MinimalEDR\events.log",
    
    [Parameter()]
    [switch]$Follow,
    
    [Parameter()]
    [ValidateSet("process", "network", "dns", "crossprocess", "all")]
    [string]$Type = "all",
    
    [Parameter()]
    [int]$Last = 0
)

function Format-Event {
    param($event)
    
    $timestamp = [DateTime]::FromFileTime($event.timestamp).ToString("yyyy-MM-dd HH:mm:ss")
    
    switch ($event.type) {
        "process_create" {
            Write-Host "[" -NoNewline
            Write-Host $timestamp -ForegroundColor Cyan -NoNewline
            Write-Host "] " -NoNewline
            Write-Host "PROCESS" -ForegroundColor Green -NoNewline
            Write-Host " PID:" -NoNewline
            Write-Host $event.pid -ForegroundColor Yellow -NoNewline
            Write-Host " PPID:" -NoNewline
            Write-Host $event.ppid -ForegroundColor Yellow
            Write-Host "    Image: " -NoNewline
            Write-Host $event.image -ForegroundColor White
            if ($event.cmdline -and $event.cmdline -ne "") {
                Write-Host "    Cmdline: " -NoNewline
                Write-Host $event.cmdline -ForegroundColor Gray
            }
            Write-Host "    Parent: " -NoNewline
            Write-Host $event.parent_image -ForegroundColor DarkGray
            if ($event.has_cert) {
                Write-Host "    Signed: " -NoNewline
                Write-Host $event.cert_subject -ForegroundColor Green
            } else {
                Write-Host "    Signed: " -NoNewline
                Write-Host "No" -ForegroundColor Red
            }
        }
        
        "network_tcp" {
            Write-Host "[" -NoNewline
            Write-Host $timestamp -ForegroundColor Cyan -NoNewline
            Write-Host "] " -NoNewline
            Write-Host "NETWORK TCP" -ForegroundColor Blue -NoNewline
            Write-Host " PID:" -NoNewline
            Write-Host $event.pid -ForegroundColor Yellow
            Write-Host "    " -NoNewline
            Write-Host $event.local_addr -NoNewline
            Write-Host ":" -NoNewline
            Write-Host $event.local_port -NoNewline
            Write-Host " -> " -NoNewline
            Write-Host $event.remote_addr -NoNewline
            Write-Host ":" -NoNewline
            Write-Host $event.remote_port -ForegroundColor Yellow
            Write-Host "    Direction: " -NoNewline
            $color = if ($event.direction -eq "outbound") { "Magenta" } else { "DarkYellow" }
            Write-Host $event.direction -ForegroundColor $color
            Write-Host "    Process: " -NoNewline
            Write-Host $event.process -ForegroundColor White
        }
        
        "network_udp" {
            Write-Host "[" -NoNewline
            Write-Host $timestamp -ForegroundColor Cyan -NoNewline
            Write-Host "] " -NoNewline
            Write-Host "NETWORK UDP" -ForegroundColor Blue -NoNewline
            Write-Host " PID:" -NoNewline
            Write-Host $event.pid -ForegroundColor Yellow
            Write-Host "    " -NoNewline
            Write-Host $event.local_addr -NoNewline
            Write-Host ":" -NoNewline
            Write-Host $event.local_port -NoNewline
            Write-Host " -> " -NoNewline
            Write-Host $event.remote_addr -NoNewline
            Write-Host ":" -NoNewline
            Write-Host $event.remote_port -ForegroundColor Yellow
            Write-Host "    Process: " -NoNewline
            Write-Host $event.process -ForegroundColor White
        }
        
        "dns_query" {
            Write-Host "[" -NoNewline
            Write-Host $timestamp -ForegroundColor Cyan -NoNewline
            Write-Host "] " -NoNewline
            Write-Host "DNS QUERY" -ForegroundColor DarkCyan -NoNewline
            Write-Host " PID:" -NoNewline
            Write-Host $event.pid -ForegroundColor Yellow
            Write-Host "    Query: " -NoNewline
            Write-Host $event.query -ForegroundColor White
            Write-Host "    Process: " -NoNewline
            Write-Host $event.process -ForegroundColor DarkGray
        }
        
        "cross_process" {
            Write-Host "[" -NoNewline
            Write-Host $timestamp -ForegroundColor Cyan -NoNewline
            Write-Host "] " -NoNewline
            Write-Host "CROSS-PROCESS" -ForegroundColor Red -NoNewline
            Write-Host " SRC:" -NoNewline
            Write-Host $event.source_pid -ForegroundColor Yellow -NoNewline
            Write-Host " -> TGT:" -NoNewline
            Write-Host $event.target_pid -ForegroundColor Yellow
            Write-Host "    Source: " -NoNewline
            Write-Host $event.source_process -ForegroundColor White
            Write-Host "    Target: " -NoNewline
            Write-Host $event.target_process -ForegroundColor White
            Write-Host "    Address: " -NoNewline
            Write-Host $event.base_addr -NoNewline
            Write-Host " Size: " -NoNewline
            Write-Host $event.size
        }
    }
    Write-Host ""
}

function Get-EDREvents {
    param([int]$count)
    
    if (-not (Test-Path $LogPath)) {
        Write-Host "Error: Log file not found: $LogPath" -ForegroundColor Red
        return
    }
    
    $lines = Get-Content $LogPath -ErrorAction SilentlyContinue
    
    if ($count -gt 0 -and $lines.Count -gt $count) {
        $lines = $lines | Select-Object -Last $count
    }
    
    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        
        try {
            $event = $line | ConvertFrom-Json
            
            if ($Type -ne "all") {
                $typeMatch = switch ($Type) {
                    "process" { "process_create" }
                    "network" { "network_tcp", "network_udp" }
                    "dns" { "dns_query" }
                    "crossprocess" { "cross_process" }
                }
                
                if ($event.type -notin $typeMatch) {
                    continue
                }
            }
            
            Format-Event $event
            
        } catch {
            Write-Host "Warning: Failed to parse event: $_" -ForegroundColor Yellow
        }
    }
}

function Watch-EDREvents {
    Write-Host "Monitoring EDR events (press Ctrl+C to stop)..." -ForegroundColor Green
    Write-Host ""
    
    $lastPosition = (Get-Item $LogPath).Length
    
    while ($true) {
        Start-Sleep -Milliseconds 500
        
        $currentLength = (Get-Item $LogPath).Length
        
        if ($currentLength -gt $lastPosition) {
            $stream = [System.IO.File]::Open($LogPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
            $stream.Position = $lastPosition
            $reader = New-Object System.IO.StreamReader($stream)
            
            while ($line = $reader.ReadLine()) {
                if ([string]::IsNullOrWhiteSpace($line)) {
                    continue
                }
                
                try {
                    $event = $line | ConvertFrom-Json
                    
                    if ($Type -ne "all") {
                        $typeMatch = switch ($Type) {
                            "process" { "process_create" }
                            "network" { "network_tcp", "network_udp" }
                            "dns" { "dns_query" }
                            "crossprocess" { "cross_process" }
                        }
                        
                        if ($event.type -notin $typeMatch) {
                            continue
                        }
                    }
                    
                    Format-Event $event
                    
                } catch {
                    # Silently skip malformed lines
                }
            }
            
            $lastPosition = $stream.Position
            $reader.Close()
            $stream.Close()
        }
    }
}

# Main execution
Write-Host "MinimalEDR Event Monitor" -ForegroundColor Cyan
Write-Host "=======================" -ForegroundColor Cyan
Write-Host ""

if ($Follow) {
    Watch-EDREvents
} else {
    Get-EDREvents -count $Last
}
